#include "reader_registry.hpp"

#include "supported_extensions.hpp"

#include "duckdb/catalog/default/default_functions.hpp"
#include "duckdb/catalog/default/default_table_functions.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/extension_helper.hpp"

#include <cctype>

namespace duckdb {
namespace readers {

namespace {

//! Lowercase, dot-prefixed. Accepts "rtf", ".rtf" or ".RTF" so a caller registering a
//! reader does not have to guess which spelling the registry wants.
std::string NormalizeExt(const std::string &raw) {
	std::string s;
	for (char c : raw) {
		s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
	}
	if (!s.empty() && s[0] != '.') {
		s.insert(s.begin(), '.');
	}
	return s;
}

} // namespace

//! The extension of a path, normalized. Pure string work -- no I/O, so panduck_can_read()
//! answers for a file that does not exist.
std::string ExtOfPath(const std::string &path) {
	auto slash = path.find_last_of("/\\");
	auto base = slash == std::string::npos ? path : path.substr(slash + 1);
	auto dot = base.find_last_of('.');
	if (dot == std::string::npos || dot + 1 >= base.size()) {
		return "";
	}
	return NormalizeExt(base.substr(dot));
}

ReaderRegistry &ReaderRegistry::Get() {
	static ReaderRegistry instance;
	return instance;
}

ReaderRegistry::ReaderRegistry() {
	struct Seed {
		const char *ext, *format, *reader_ext, *function, *kind;
	};
	// Claims for extensions that cannot describe themselves. panduck's OWN formats are
	// NOT here -- they are derived from panduck_supported_extensions() below, so a reader
	// landing in this extension needs no edit to this table.
	static const Seed SEEDS[] = {
	    {".md", "markdown", "markdown", "read_markdown_blocks", KIND_DOC},
	    {".markdown", "markdown", "markdown", "read_markdown_blocks", KIND_DOC},
	    {".html", "html", "webbed", "", KIND_DOC},
	    {".htm", "html", "webbed", "", KIND_DOC},
	    {".pdf", "pdf", "pdf", "", KIND_DOC},
	    {".json", "pandoc_ast", "duck_block_utils", "", KIND_DOC},
	    // Config trees: a nested key-value document. Not prose, but not rows either.
	    {".toml", "toml", "toml", "", KIND_DOC},
	    {".yaml", "yaml", "yaml", "", KIND_DOC},
	    {".yml", "yaml", "yaml", "", KIND_DOC},
	    // Genuinely tabular. Claimed so they do not fall through to the code fallback;
	    // read_panduck_doc refuses them by name and points at read_panduck_table.
	    {".csv", "data", "core", "", KIND_TABLE},
	    {".tsv", "data", "core", "", KIND_TABLE},
	    {".parquet", "data", "core", "", KIND_TABLE},
	    {".arrow", "data", "core", "", KIND_TABLE},
	    {".jsonl", "data", "core", "", KIND_TABLE},
	    {".ndjson", "data", "core", "", KIND_TABLE},
	    {".xlsx", "data", "excel", "", KIND_TABLE},
	};
	for (auto &s : SEEDS) {
		entries.push_back(ReaderEntry {s.ext, s.format, s.reader_ext, s.function, s.kind, SOURCE_BUILTIN});
	}
	// panduck's own implemented readers, DERIVED from its self-description rather than
	// restated. A 'planned' format has reader == nullptr and is skipped, so dispatch can
	// never route to a function that does not exist.
	for (size_t i = 0; i < FORMAT_COUNT; i++) {
		const auto &f = FORMATS[i];
		if (!f.reader || std::string(f.status) != std::string(STATUS_IMPLEMENTED)) {
			continue;
		}
		for (size_t j = 0; f.extensions[j] != nullptr; j++) {
			entries.push_back(
			    ReaderEntry {NormalizeExt(f.extensions[j]), f.format, "panduck", f.reader, KIND_DOC, SOURCE_BUILTIN});
		}
	}
}

std::vector<ReaderEntry> ReaderRegistry::Entries() {
	std::lock_guard<std::mutex> guard(lock);
	return entries;
}

bool ReaderRegistry::Lookup(const std::string &ext, ReaderEntry &out) {
	std::lock_guard<std::mutex> guard(lock);
	for (auto &e : entries) {
		if (e.ext == ext) {
			out = e;
			return true;
		}
	}
	return false;
}

void ReaderRegistry::Register(const ReaderEntry &entry) {
	std::lock_guard<std::mutex> guard(lock);
	// Replace, never append. "One reader per extension" holds BY CONSTRUCTION rather than
	// being asserted after the fact -- a user registration overrides, it does not compete.
	for (auto &e : entries) {
		if (e.ext == entry.ext) {
			e = entry;
			return;
		}
	}
	entries.push_back(entry);
}

} // namespace readers

namespace {

using readers::ExtOfPath;
using readers::ReaderEntry;
using readers::ReaderRegistry;

// ------------------------------------------------------------------ scalar lookups
//
// These are C++ rather than SQL macros for one reason: a macro body that consults the
// registry is a subquery, and query() rejects subqueries in its argument -- which is
// exactly the expression dispatch has to build. See reader_registry.hpp.

enum class Field { FORMAT, FUNCTION, READER_EXT, KIND };

template <Field F>
void RegistryFieldFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::ExecuteWithNulls<string_t, string_t>(
	    args.data[0], result, args.size(), [&](string_t path, ValidityMask &mask, idx_t idx) {
		    ReaderEntry entry;
		    if (!ReaderRegistry::Get().Lookup(ExtOfPath(path.GetString()), entry)) {
			    mask.SetInvalid(idx);
			    return string_t();
		    }
		    const std::string *value = &entry.format;
		    switch (F) {
		    case Field::FUNCTION:
			    value = &entry.function;
			    break;
		    case Field::READER_EXT:
			    value = &entry.reader_ext;
			    break;
		    case Field::KIND:
			    value = &entry.kind;
			    break;
		    case Field::FORMAT:
			    break;
		    }
		    if (value->empty()) {
			    mask.SetInvalid(idx);
			    return string_t();
		    }
		    return StringVector::AddString(result, *value);
	    });
}

void CanReadFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, bool>(args.data[0], result, args.size(), [&](string_t path) {
		ReaderEntry entry;
		return ReaderRegistry::Get().Lookup(ExtOfPath(path.GetString()), entry);
	});
}

void EnsureExtensionFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	UnaryExecutor::Execute<string_t, bool>(args.data[0], result, args.size(), [&](string_t name) {
		return ExtensionHelper::TryAutoLoadExtension(context, name.GetString());
	});
}

// ------------------------------------------------------------------ registry table fn

struct RegistryBindData : public TableFunctionData {
	std::vector<ReaderEntry> rows;
};

struct RegistryGlobalState : public GlobalTableFunctionState {
	idx_t offset = 0;
	static unique_ptr<GlobalTableFunctionState> Init(ClientContext &, TableFunctionInitInput &) {
		return make_uniq<RegistryGlobalState>();
	}
};

unique_ptr<FunctionData> RegistryBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                      vector<string> &names) {
	names = {"ext", "format", "reader_ext", "function", "kind", "source"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
	auto result = make_uniq<RegistryBindData>();
	result->rows = ReaderRegistry::Get().Entries();
	return std::move(result);
}

void RegistryScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<RegistryBindData>();
	auto &state = input.global_state->Cast<RegistryGlobalState>();
	idx_t count = 0;
	while (state.offset < data.rows.size() && count < STANDARD_VECTOR_SIZE) {
		const auto &e = data.rows[state.offset];
		output.SetValue(0, count, Value(e.ext));
		output.SetValue(1, count, Value(e.format));
		output.SetValue(2, count, Value(e.reader_ext));
		output.SetValue(3, count, e.function.empty() ? Value(LogicalType::VARCHAR) : Value(e.function));
		output.SetValue(4, count, Value(e.kind));
		output.SetValue(5, count, Value(e.source));
		state.offset++;
		count++;
	}
	output.SetCardinality(count);
}

// ------------------------------------------------------------------ CALL registration

struct RegisterBindData : public TableFunctionData {
	std::vector<std::string> exts;
	std::string reader_ext, function, kind;
};

struct RegisterGlobalState : public GlobalTableFunctionState {
	bool done = false;
	static unique_ptr<GlobalTableFunctionState> Init(ClientContext &, TableFunctionInitInput &) {
		return make_uniq<RegisterGlobalState>();
	}
};

template <const char *KIND>
unique_ptr<FunctionData> RegisterBind(ClientContext &, TableFunctionBindInput &input, vector<LogicalType> &return_types,
                                      vector<string> &names) {
	names = {"ext", "reader_ext", "function", "kind"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
	auto result = make_uniq<RegisterBindData>();
	result->reader_ext = input.inputs[0].GetValue<string>();
	result->function = input.inputs[1].GetValue<string>();
	result->kind = KIND;
	for (auto &v : ListValue::GetChildren(input.inputs[2])) {
		result->exts.push_back(v.GetValue<string>());
	}
	if (result->exts.empty()) {
		throw InvalidInputException("panduck: register requires at least one file extension");
	}
	return std::move(result);
}

void RegisterScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<RegisterBindData>();
	auto &state = input.global_state->Cast<RegisterGlobalState>();
	if (state.done) {
		output.SetCardinality(0);
		return;
	}
	idx_t count = 0;
	for (auto &raw : data.exts) {
		ReaderEntry entry;
		entry.ext = readers::ExtOfPath("x" + (raw[0] == '.' ? raw : "." + raw));
		// A user reader's "format" is the extension that provides it, so panduck_format_for
		// names something a human can act on rather than a generic "custom".
		entry.format = data.reader_ext;
		entry.reader_ext = data.reader_ext;
		entry.function = data.function;
		entry.kind = data.kind;
		entry.source = readers::SOURCE_USER;
		ReaderRegistry::Get().Register(entry);

		output.SetValue(0, count, Value(entry.ext));
		output.SetValue(1, count, Value(entry.reader_ext));
		output.SetValue(2, count, Value(entry.function));
		output.SetValue(3, count, Value(entry.kind));
		count++;
	}
	state.done = true;
	output.SetCardinality(count);
}

// ------------------------------------------------------------------ SQL surface
//
// Dispatch is a SQL macro over query(), and its CASE is built entirely from C++ scalars,
// so no subquery ever reaches query()'s argument. The generic branch -- "the registry
// names a function, so call it" -- covers builtin flat readers and user-registered
// readers with the same code path; only formats needing a bespoke shape are special-cased.

const DefaultMacro SCALAR_MACROS[] = {
    {DEFAULT_SCHEMA,
     "panduck_quote",
     {"s", nullptr},
     {{nullptr, nullptr}},
     "'''' || replace(coalesce(s, ''), '''', '''''') || ''''"},

    // The unpack column list, defined once. Only LIST-producing branches use it; flat
    // branches pass SELECT * through, which has no positional list to transpose.
    {DEFAULT_SCHEMA,
     "panduck_block_cols",
     {nullptr},
     {{nullptr, nullptr}},
     "'b.kind, b.element_type, b.content, b.level, b.encoding, b.attributes, b.element_order'"},

    // Pure expressions over C++ scalars -- safe to inline into query()'s argument.
    {DEFAULT_SCHEMA,
     "panduck_resolved_format",
     {"src", "fmt", nullptr},
     {{nullptr, nullptr}},
     "coalesce(nullif(fmt, 'auto'), panduck_format_for(src))"},

    {DEFAULT_SCHEMA,
     "panduck_supported_paths",
     {nullptr},
     {{nullptr, nullptr}},
     "(SELECT list(r.ext ORDER BY r.ext) FROM panduck_reader_registry() r)"},

    {DEFAULT_SCHEMA,
     "panduck_read_blocks",
     {"src", nullptr},
     {{"format", "'auto'"}, {"pages", "''"}, {nullptr, nullptr}},
     // No ::duck_block cast: that named type belongs to duck_block_utils, and panduck
     // must answer without it. The struct is structurally identical, so it still casts
     // implicitly wherever a duck_block[] is wanted.
     "(SELECT list(b) FROM read_panduck_doc(src, format := format, pages := pages) b)"},

    {nullptr, nullptr, {nullptr}, {{nullptr, nullptr}}, nullptr}};

const DefaultTableMacro READ_DOC_MACRO = {DEFAULT_SCHEMA,
                                          "read_panduck_doc",
                                          {"src", nullptr},
                                          {{"format", "'auto'"}, {"pages", "''"}, {nullptr, nullptr}},
                                          R"SQL(
SELECT * FROM query(
    CASE
        WHEN panduck_resolved_format(src, format) = 'data'
            THEN error('panduck: ' || src || ' is a data format, not a document. ' ||
                       'Use read_panduck_table instead.')

        -- GENERIC: the registry names a table function that emits duck_blocks, so call
        -- it. One path for builtin flat readers (rtf, markdown) and for anything a user
        -- registered with panduck_register_doc_reader. SELECT * because those readers
        -- already emit the canonical schema -- no column list, nothing to transpose.
        WHEN panduck_reader_function_for(src) IS NOT NULL
             AND panduck_resolved_format(src, format) = panduck_format_for(src)
            THEN CASE WHEN panduck_ensure_extension(panduck_reader_extension_for(src))
                 THEN 'SELECT * FROM ' || panduck_reader_function_for(src) ||
                      '(' || panduck_quote(src) || ')'
                 ELSE error('panduck: ' || src || ' needs the ' ||
                            panduck_reader_extension_for(src) || ' extension') END

        -- LIST-producing branches: these unpack BY NAME.
        WHEN panduck_resolved_format(src, format) = 'html'
            THEN CASE WHEN panduck_ensure_extension('webbed')
                 THEN 'SELECT ' || panduck_block_cols() ||
                      ' FROM (SELECT unnest(html_to_duck_blocks(html)) AS b FROM read_html_objects(' ||
                      panduck_quote(src) || '))'
                 ELSE error('panduck: html needs the webbed extension') END

        WHEN panduck_resolved_format(src, format) = 'pdf'
            THEN CASE WHEN panduck_ensure_extension('pdf') AND panduck_ensure_extension('markdown')
                 THEN 'SELECT ' || panduck_block_cols() ||
                      ' FROM (SELECT unnest(parse_markdown_to_duck_blocks(pdf_to_markdown(' ||
                      panduck_quote(src) || '))) AS b)'
                 ELSE error('panduck: pdf needs the pdf and markdown extensions') END

        WHEN panduck_resolved_format(src, format) = 'pandoc_ast'
            THEN CASE WHEN panduck_ensure_extension('duck_block_utils')
                 THEN 'SELECT ' || panduck_block_cols() ||
                      ' FROM (SELECT unnest(pandoc_ast_to_blocks(content)) AS b FROM read_text(' ||
                      panduck_quote(src) || '))'
                 ELSE error('panduck: pandoc AST needs the duck_block_utils extension') END

        -- A config tree is entirely document metadata, so it becomes ONE metadata block
        -- carrying the parsed document as JSON rather than being flattened to a string or
        -- refused. When kind='value' lands in duck_block_utils this should become value
        -- elements -- MetaMap is the shape a nested key-value tree actually wants.
        WHEN panduck_resolved_format(src, format) = 'toml'
            THEN CASE WHEN panduck_ensure_extension('toml')
                 THEN 'SELECT ''block'' AS kind, ''metadata'' AS element_type, ' ||
                      'parse_toml(content)::VARCHAR AS content, NULL::INTEGER AS level, ' ||
                      '''json'' AS encoding, MAP {''source_type'': ''toml''} AS attributes, ' ||
                      '0 AS element_order FROM read_text(' || panduck_quote(src) || ')'
                 ELSE error('panduck: toml needs the toml extension') END

        -- Anything unclaimed falls through to source code. The exclusion rule is BY
        -- CONSTRUCTION: .md is in the registry, so it can never arrive here.
        -- sitting_duck's ast_to_blocks is a TABLE MACRO emitting (file_path,
        -- element_order, block STRUCT(...)) -- a third output shape, neither flat columns
        -- nor LIST(duck_block). It unpacks by name like the LIST branches do.
        --
        -- KNOWN LIMITATION, measured: panduck_ensure_extension loads sitting_duck, but a
        -- MACRO registered into the catalog at load time is not visible to the statement
        -- that triggered the load -- the binder's catalog view predates it. A C++ function
        -- is (read_markdown_blocks resolves on the first call; ast_to_blocks does not).
        -- So the first code-format read in a fresh session fails with a catalog error and
        -- the second succeeds. Eagerly loading sitting_duck whenever panduck loads would
        -- fix it at the cost of pulling in tree-sitter for every .rtf read, which is a
        -- worse trade. LOAD sitting_duck once, or call twice.
        ELSE CASE WHEN panduck_ensure_extension('sitting_duck')
             THEN 'SELECT ' || panduck_block_cols() ||
                  ' FROM (SELECT block AS b FROM ast_to_blocks(' || panduck_quote(src) || '))'
             ELSE error('panduck: no reader for ' || src ||
                        ' and sitting_duck (the fallback) is not installed') END
    END
)
)SQL"};

// read_panduck_table -- duckeye's --raw. DuckDB's replacement scan already reads csv,
// parquet, json and xlsx from a bare path, so mostly this gets out of the way.
const DefaultTableMacro READ_TABLE_MACRO = {DEFAULT_SCHEMA,
                                            "read_panduck_table",
                                            {"src", nullptr},
                                            {{nullptr, nullptr}},
                                            R"SQL(
SELECT * FROM query(
    CASE
        WHEN panduck_reader_kind_for(src) = 'table' AND panduck_reader_function_for(src) IS NOT NULL
            THEN CASE WHEN panduck_ensure_extension(panduck_reader_extension_for(src))
                 THEN 'SELECT * FROM ' || panduck_reader_function_for(src) ||
                      '(' || panduck_quote(src) || ')'
                 ELSE error('panduck: ' || src || ' needs the ' ||
                            panduck_reader_extension_for(src) || ' extension') END
        WHEN panduck_format_for(src) = 'toml'
            THEN CASE WHEN panduck_ensure_extension('toml')
                 THEN 'SELECT parse_toml(content) AS toml FROM read_text(' || panduck_quote(src) || ')'
                 ELSE error('panduck: toml needs the toml extension') END
        ELSE 'SELECT * FROM ' || panduck_quote(src)
    END
)
)SQL"};

const char DOC_KIND[] = "doc";
const char TABLE_KIND[] = "table";

} // namespace

void RegisterReaderRegistry(ExtensionLoader &loader) {
	loader.RegisterFunction(
	    ScalarFunction("panduck_ensure_extension", {LogicalType::VARCHAR}, LogicalType::BOOLEAN, EnsureExtensionFun));
	loader.RegisterFunction(ScalarFunction("panduck_format_for", {LogicalType::VARCHAR}, LogicalType::VARCHAR,
	                                       RegistryFieldFun<Field::FORMAT>));
	loader.RegisterFunction(ScalarFunction("panduck_reader_function_for", {LogicalType::VARCHAR}, LogicalType::VARCHAR,
	                                       RegistryFieldFun<Field::FUNCTION>));
	loader.RegisterFunction(ScalarFunction("panduck_reader_extension_for", {LogicalType::VARCHAR}, LogicalType::VARCHAR,
	                                       RegistryFieldFun<Field::READER_EXT>));
	loader.RegisterFunction(ScalarFunction("panduck_reader_kind_for", {LogicalType::VARCHAR}, LogicalType::VARCHAR,
	                                       RegistryFieldFun<Field::KIND>));
	loader.RegisterFunction(
	    ScalarFunction("panduck_can_read", {LogicalType::VARCHAR}, LogicalType::BOOLEAN, CanReadFun));

	TableFunction registry("panduck_reader_registry", {}, RegistryScan, RegistryBind, RegistryGlobalState::Init);
	loader.RegisterFunction(registry);

	auto list_of_varchar = LogicalType::LIST(LogicalType::VARCHAR);
	TableFunction reg_doc("panduck_register_doc_reader", {LogicalType::VARCHAR, LogicalType::VARCHAR, list_of_varchar},
	                      RegisterScan, RegisterBind<DOC_KIND>, RegisterGlobalState::Init);
	loader.RegisterFunction(reg_doc);

	for (auto *tm : {&READ_DOC_MACRO, &READ_TABLE_MACRO}) {
		auto info = DefaultTableFunctionGenerator::CreateTableMacroInfo(*tm);
		loader.RegisterFunction(*info);
	}
	for (idx_t i = 0; SCALAR_MACROS[i].name != nullptr; i++) {
		auto info = DefaultFunctionGenerator::CreateInternalMacroInfo(SCALAR_MACROS[i]);
		loader.RegisterFunction(*info);
	}

	TableFunction reg_tbl("panduck_register_table_reader",
	                      {LogicalType::VARCHAR, LogicalType::VARCHAR, list_of_varchar}, RegisterScan,
	                      RegisterBind<TABLE_KIND>, RegisterGlobalState::Init);
	loader.RegisterFunction(reg_tbl);
}

} // namespace duckdb
