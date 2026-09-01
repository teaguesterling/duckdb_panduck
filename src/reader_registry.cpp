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
	    // A .zim is a CORPUS, not a document -- an archive of many articles, closer to a
	    // .zip than to a .docx. It is declared here so it stops FALLING THROUGH to `code`
	    // and being handed to sitting_duck as source: a binary archive parsed as a
	    // programming language is a silently wrong answer, which is worse than an honest
	    // refusal. Raised by duckeye, who routes .zim to duckdb_zim directly and needs
	    // panduck to answer honestly rather than plausibly.
	    {".zim", "zim", "zim", "", KIND_DOC},
	    {".json", "data", "json", "", KIND_TABLE},
	    // Config trees: a nested key-value document. Not prose, but not rows either.
	    {".toml", "toml", "toml", "", KIND_DOC},
	    {".yaml", "yaml", "yaml", "", KIND_DOC},
	    {".yml", "yaml", "yaml", "", KIND_DOC},
	    // .json is DATA, not a Pandoc AST. panduck used to route it to duck_block_utils'
	    // pandoc_ast_to_blocks, which made the IO engine depend on the helper layer -- the
	    // wrong direction. Anyone holding a Pandoc AST calls pandoc_ast_to_blocks(content)
	    // directly, which is the standalone usefulness duck_block_utils is meant to have.
	    // Reading Pandoc JSON natively belongs in panduck eventually; borrowing it does not.
	    //
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

// ------------------------------------------------------------------ doc_* namespace
//
// doc_* takes a PATH; db_* takes blocks you already hold. That split is why this
// namespace belongs to panduck: taking a path is the IO engine's job, and it restores
// doc_toc('README.md') -- an ergonomic loss duck_block_utils recorded as an accepted
// cost when dispatch moved out.
//
// These load duck_block_utils on demand, exactly as reading .md loads markdown and .html
// loads webbed. panduck's CORE never needs it: every reader, the registry and both
// dispatchers work with duck_block_utils absent. Only this sugar depends on it, and it
// says so by name when it is missing.
//
// LIMITED TO WHAT IS ACTUALLY REACHABLE. duck_block_utils exposes two shapes:
//
//   C++ scalars, available at LOAD    db_blocks_toc, db_blocks_to_text   -> usable here
//   macros behind PRAGMA duck_block_* db_toc, db_section, db_ansi        -> NOT usable
//
// A macro created by a pragma cannot be reached from a panduck macro: it is invisible to
// the statement that loads the extension, and panduck cannot invoke a pragma from inside
// a macro. So doc_section and doc_sections_like are absent until duck_block_utils
// registers db_* at LOAD (DefaultTableMacro) rather than behind its pragma -- the same
// change panduck made for its own registry. Likewise doc_render has no 'ansi' arm yet;
// db_ansi is a macro.

const DefaultTableMacro DOC_TOC_MACRO = {DEFAULT_SCHEMA,
                                         "doc_toc",
                                         {"src", nullptr},
                                         {{"format", "'auto'"}, {nullptr, nullptr}},
                                         R"SQL(
SELECT * FROM query(
    CASE WHEN panduck_ensure_extension('duck_block_utils')
    THEN 'SELECT (t).level AS level, (t).title AS title, (t).id AS id, ' ||
         '(t).indent AS indent, (t).element_order AS element_order ' ||
         'FROM (SELECT unnest(db_blocks_toc(panduck_read_blocks(' || panduck_quote(src) ||
         ', format := ' || panduck_quote(format) || '))) AS t)'
    ELSE error('panduck: doc_toc needs the duck_block_utils extension (INSTALL duck_block_utils)')
    END
)
)SQL"};

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
     // ORDER BY element_order is not decoration. doc_section slices this list by
     // position, so the order has to be GUARANTEED rather than incidental -- a bare
     // list() over a table function preserves emission order today but nothing in the
     // contract says it must, and a reordering would surface as a slicing bug far from
     // its cause.
     "(SELECT list(b ORDER BY b.element_order) FROM read_panduck_doc(src, format := format, pages := pages) b)"},

    // doc_render(src, format) -- render a document to a FORMAT. duck_block_utils deleted
    // its doc_render when it stopped depending on format extensions; panduck is the right
    // home because rendering to md/html IS format IO. 'text' delegates to
    // db_blocks_to_text, which is a C++ scalar and therefore reachable; 'ansi' cannot be
    // added until db_ansi registers at LOAD instead of behind a pragma.
    {DEFAULT_SCHEMA,
     "doc_render",
     {"src", "output_format", nullptr},
     {{"format", "'auto'"}, {nullptr, nullptr}},
     "(SELECT r FROM query("
     "  CASE"
     "    WHEN output_format = 'md' AND panduck_ensure_extension('markdown')"
     "      THEN 'SELECT duck_blocks_to_md(panduck_read_blocks(' || panduck_quote(src) ||"
     "           ', format := ' || panduck_quote(format) || ')) AS r'"
     "    WHEN output_format = 'html' AND panduck_ensure_extension('webbed')"
     "      THEN 'SELECT duck_blocks_to_html(panduck_read_blocks(' || panduck_quote(src) ||"
     "           ', format := ' || panduck_quote(format) || ')) AS r'"
     "    WHEN output_format = 'text' AND panduck_ensure_extension('duck_block_utils')"
     "      THEN 'SELECT db_blocks_to_text(panduck_read_blocks(' || panduck_quote(src) ||"
     "           ', format := ' || panduck_quote(format) || ')) AS r'"
     "    ELSE error('panduck: doc_render supports md, html and text; ' || output_format ||"
     "               ' is unsupported or its extension is not installed')"
     "  END))"},

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

        -- PANDOC'S OWN AST, reached by format := 'pandoc' and never by extension. The
        -- generic branch above derives its reader from the file's SUFFIX, and this format
        -- deliberately claims none -- so it needs a branch of its own or it is
        -- unreachable through dispatch entirely.
        WHEN panduck_resolved_format(src, format) = 'pandoc'
            THEN 'SELECT * FROM read_pandoc_blocks(' || panduck_quote(src) || ')'

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

        -- A config tree is entirely document metadata, so it becomes ONE metadata block.
        --
        -- This carried a deferral -- "when kind='value' lands this should become value
        -- elements, MetaMap is the shape a nested key-value tree wants" -- and the deferral
        -- was OVERTAKEN rather than met. kind='value' did land, and the answer went the
        -- other way: the ruling is that panduck keeps the blob verbatim and does not parse
        -- it at all, so there is no tree to shape. Removed rather than left standing,
        -- because a deferral pointing at a decision already made differently is worse than
        -- a stale one: it reads as the plan.
        --
        -- `level` is 1 and not NULL. It was NULL until 2026-09-01, which duck_blocks_validate
        -- rejects outright: "level is NULL; every element carries an explicit structural
        -- depth". Nothing here objected because there was no .toml or .yaml fixture, so the
        -- only two branches in this file that build a block by hand were also the only two
        -- the test suite never read. This is the same defect duck_block_utils records
        -- against duckdb_webbed, whose metadata blocks carried a NULL level for three major
        -- spec versions -- and for the same reason: the element a producer synthesises
        -- itself is the one no reader test covers.
        --
        -- THE BLOB IS KEPT VERBATIM. These parsed the file -- parse_toml(), yaml_to_json()
        -- -- and emitted the result as JSON. Teague's ruling, now spec 6.2: a `metadata`
        -- blob is "a verbatim blob you must not reinterpret", and parsing full YAML or
        -- TOML in this extension violates panduck's isolation.
        --
        -- The dependency is the concrete cost, and it was severe: each branch called
        -- panduck_ensure_extension() and ERRORED without it, so panduck could not read a
        -- .toml file AT ALL unless a third-party extension was installed -- a hard
        -- dependency acquired purely to do work the vocabulary does not want done. Now it
        -- is read_text and nothing else.
        --
        -- role='document' rather than 'frontmatter': the distinction is STRUCTURAL, not
        -- about file extension. Frontmatter PRECEDES a body; here the blob IS the whole
        -- document and there is no body. A .yaml file read whole is `document` even though
        -- a frontmatter block is also YAML.
        --
        -- encoding carries the syntax, and source_type is deliberately GONE. It was
        -- recording 'toml', which `encoding` now says properly -- ENCODING_TOML was
        -- declared for this case. source_type stays for what encoding cannot express, such
        -- as `generic`'s original type name. Two fields saying one thing is how they drift
        -- apart.
        --
        -- These values are string literals because this is a raw SQL macro body and the
        -- vocabulary's C++ constants cannot be interpolated into it -- so the drift check,
        -- which works by finding constant USAGE, is blind to them. That is the same hole
        -- duck_block_utils just closed on its own `role` literals. The guard here is
        -- test/sql/reader_registry.test, which asserts these against
        -- duck_block_encoding_names() and the declared role, not against a copy.
        WHEN panduck_resolved_format(src, format) = 'toml'
            THEN 'SELECT ''block'' AS kind, ''metadata'' AS element_type, ' ||
                 'content AS content, 1 AS level, ' ||
                 '''toml'' AS encoding, MAP {''role'': ''document''} AS attributes, ' ||
                 '0 AS element_order FROM read_text(' || panduck_quote(src) || ')'

        WHEN panduck_resolved_format(src, format) = 'yaml'
            THEN 'SELECT ''block'' AS kind, ''metadata'' AS element_type, ' ||
                 'content AS content, 1 AS level, ' ||
                 '''yaml'' AS encoding, MAP {''role'': ''document''} AS attributes, ' ||
                 '0 AS element_order FROM read_text(' || panduck_quote(src) || ')'

        -- A ZIM ARCHIVE IS NOT A DOCUMENT, and this refuses it with the reason.
        --
        -- duckdb_zim emits its own schema and has no duck_block awareness, so there is
        -- nothing to route to: a .zim holds thousands of articles and "read it as a
        -- document" has no answer. The zim extension indexes the archive, searches it, and
        -- resolves a single article -- which IS a document, and reaches panduck as HTML.
        --
        -- An error naming the alternative beats both silence and a plausible wrong answer.
        WHEN panduck_resolved_format(src, format) = 'zim'
            THEN error('panduck: ' || src || ' is a ZIM archive -- a corpus of many ' ||
                       'articles, not one document. Use the zim extension to index or ' ||
                       'search it, and read a single article once resolved.')

        -- Anything unclaimed falls through to source code. The exclusion rule is BY
        -- CONSTRUCTION: .md is in the registry, so it can never arrive here.
        -- sitting_duck's ast_to_blocks is a TABLE MACRO emitting (file_path,
        -- element_order, block STRUCT(...)) -- a third output shape, neither flat columns
        -- nor LIST(duck_block). It unpacks by name like the LIST branches do.
        --
        -- KNOWN LIMITATION. Autoloading mid-statement is governed by TWO effects that
        -- compose, both measured:
        --
        --   1. BIND TIME. A statement binds fully before it executes, so anything it
        --      names DIRECTLY must already exist -- panduck_ensure_extension runs during
        --      execution, too late. This is why every branch here goes through query():
        --      query() binds its inner SQL at execution, after the ensure has run.
        --        CASE WHEN ensure('markdown') THEN md_to_html('# H') END  -> Catalog Error
        --        query(CASE WHEN ensure('markdown') THEN '<that sql>' END) -> works
        --
        --   2. CATALOG VISIBILITY. Even inside query(), a MACRO created by the load is
        --      not visible, while a C++ function is:
        --        read_markdown_blocks (C++ table function) -> resolves first call
        --        md_to_html           (C++ scalar)         -> resolves first call
        --        ast_to_blocks        (table macro)        -> does NOT; second call works
        --
        -- ast_to_blocks is a macro, so the first code-format read in a fresh session
        -- fails and the second succeeds. Eagerly loading sitting_duck whenever panduck
        -- loads would fix it at the cost of pulling tree-sitter into every .rtf read.
        -- LOAD sitting_duck once, or call twice.
        -- Guarded on format_for IS NULL rather than a bare ELSE. A catch-all would
        -- swallow any format the registry CLAIMS but this CASE has no branch for,
        -- routing it to sitting_duck and returning a parse tree instead of a document --
        -- silently wrong rather than loudly missing. That is precisely how doc_search
        -- silently degraded md/html/blocks/pandoc to text in duck_block_utils, and it is
        -- how .yaml behaved here until this branch existed.
        WHEN (panduck_format_for(src) IS NULL AND nullif(format, 'auto') IS NULL)
             OR panduck_resolved_format(src, format) = 'code'
            THEN CASE WHEN panduck_ensure_extension('sitting_duck')
             THEN 'SELECT ' || panduck_block_cols() ||
                  ' FROM (SELECT block AS b FROM ast_to_blocks(' || panduck_quote(src) || '))'
             ELSE error('panduck: no reader for ' || src ||
                        ' and sitting_duck (the fallback) is not installed') END

        ELSE error('panduck: ' || src || ' resolves to format ''' ||
                   panduck_resolved_format(src, format) ||
                   ''' but read_panduck_doc has no branch for it -- the registry claims ' ||
                   'the format and dispatch does not handle it. This is a panduck bug.')
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
        -- DuckDB's replacement scan reads csv/parquet/json/xlsx from a bare path, but
        -- only once the extension providing the format is loaded. Ensure it first:
        -- 'core' names no loadable extension, and ensure returns false harmlessly there.
        ELSE CASE WHEN panduck_reader_extension_for(src) IS NULL
                    OR panduck_reader_extension_for(src) = 'core'
                    OR panduck_ensure_extension(panduck_reader_extension_for(src))
             THEN 'SELECT * FROM ' || panduck_quote(src)
             ELSE error('panduck: ' || src || ' needs the ' ||
                        panduck_reader_extension_for(src) || ' extension') END
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

	for (auto *tm : {&READ_DOC_MACRO, &READ_TABLE_MACRO, &DOC_TOC_MACRO}) {
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
