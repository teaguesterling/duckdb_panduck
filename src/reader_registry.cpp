#include "reader_registry.hpp"
#include "panduck_duckdb_compat.hpp"

#include "supported_extensions.hpp"

#include "duckdb/catalog/default/default_functions.hpp"
#include "duckdb/catalog/default/default_table_functions.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/extension_helper.hpp"

#include <algorithm>
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
	// A URL SCHEME IS AN EXTENSION FOR REGISTRY PURPOSES. `zim://wiki.zim/A/Article` names
	// ONE ARTICLE inside an archive, and its trailing segment usually has no dot at all --
	// so extension lookup answers NULL and the source falls through to `code`. Keying on
	// the scheme makes the registry answer honestly for a shape that is a document.
	//
	// It is a DIFFERENT format from `.zim`: the archive is a corpus with no single-document
	// reading and is refused, while an article is HTML and reads like any other. One suffix
	// and one scheme, two answers, which is why this cannot be a single row.
	if (path.rfind("zim://", 0) == 0) {
		return "zim://";
	}
	auto slash = path.find_last_of("/\\");
	auto base = slash == std::string::npos ? path : path.substr(slash + 1);
	auto dot = base.find_last_of('.');
	if (dot == std::string::npos || dot + 1 >= base.size()) {
		return "";
	}
	return NormalizeExt(base.substr(dot));
}

//! A reader parameter name must be a bare identifier.
//!
//! CHECKED AT REGISTRATION, not at render time, so a malformed row cannot be STORED at
//! all. The difference matters because a registry entry is process-wide and permanent: a
//! row accepted now would be rendered into every later read_panduck_doc in the process,
//! and the caller who then hits the error has no way to tell which registration is
//! responsible. Refusing at the point of registration puts the error where the mistake is.
bool IsIdentifier(const std::string &s) {
	if (s.empty() || (!std::isalpha(static_cast<unsigned char>(s[0])) && s[0] != '_')) {
		return false;
	}
	for (char c : s) {
		if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
			return false;
		}
	}
	return true;
}

//! Does `arg` actually hold a value of `arg_type`?
//!
//! BOOLEAN AND INTEGER ARE RENDERED BARE -- unquoted -- so an unchecked `arg` under either
//! type is a direct SQL injection: arg_type 'BOOLEAN' with arg "true) UNION SELECT ..."
//! would be emitted verbatim into the reader call. Only VARCHAR is quoted, and quoting is
//! what makes it safe. So the bare types are constrained to values that cannot be anything
//! but a literal, and this runs BOTH at registration (so the row cannot be stored) and
//! again in the renderer (so no future path can reach emission unchecked). The redundancy
//! is deliberate: this is the check the whole no-stored-fragment argument rests on.
bool ArgMatchesType(const std::string &arg_type, const std::string &arg) {
	if (arg_type == "VARCHAR") {
		return true; // rendered quoted and escaped; any content is a literal
	}
	if (arg_type == "BOOLEAN") {
		return arg == "true" || arg == "false";
	}
	if (arg_type == "INTEGER") {
		if (arg.empty()) {
			return false;
		}
		size_t i = (arg[0] == '-' || arg[0] == '+') ? 1 : 0;
		if (i >= arg.size()) {
			return false;
		}
		for (; i < arg.size(); i++) {
			if (!std::isdigit(static_cast<unsigned char>(arg[i]))) {
				return false;
			}
		}
		return true;
	}
	return false;
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
	    // webbed shipped read_html_blocks + parse_html_blocks in v2.8.1 (2026-08-30),
	    // measured against community build 093856b. Until then it exposed only the SCALAR
	    // html_to_duck_blocks(html), which returns a LIST and so needed unpacking in a
	    // special-case branch of READ_DOC_MACRO. That branch is gone and these rows now
	    // name a function, so html takes the same generic path markdown does.
	    //
	    // This row was stale for five days and nothing noticed, which is the same
	    // two-clocks failure as the db_* -> duck_* rename: the registry is a COMPILE-TIME
	    // claim about a sibling's RUNTIME surface. check-vocabulary catches drift in the
	    // vendored constants and nothing catches drift here.
	    {".html", "html", "webbed", "read_html_blocks", KIND_DOC},
	    {".htm", "html", "webbed", "read_html_blocks", KIND_DOC},
	    {".pdf", "pdf", "pdf", "", KIND_DOC},
	    // A .zim is a CORPUS, not a document -- an archive of many articles, closer to a
	    // .zip than to a .docx. It is declared here so it stops FALLING THROUGH to `code`
	    // and being handed to sitting_duck as source: a binary archive parsed as a
	    // programming language is a silently wrong answer, which is worse than an honest
	    // refusal. Raised by duckeye, who routes .zim to duckdb_zim directly and needs
	    // panduck to answer honestly rather than plausibly.
	    {".zim", "zim", "zim", "", KIND_DOC},
	    // The SCHEME, not a suffix: one article, which is a document.
	    {"zim://", "zim_article", "zim", "", KIND_DOC},
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
	// WRITTEN WITH Vector::SetValue RATHER THAN UnaryExecutor, because the executor's null
	// protocol is not stable across DuckDB versions: v1.5.5 offers ExecuteWithNulls with a
	// (value, ValidityMask &, idx) lambda; v2.0 removed it in favour of a lambda returning
	// optional<T>. SetValue has the same signature in both, and a default-constructed Value
	// IS the NULL -- so one spelling covers both the value and the null case with no shim.
	//
	// This is a registry lookup over a handful of paths, not a hot loop, so the cost of
	// going through Value rather than the flat array does not matter here. Reaching for the
	// flat array WOULD need a shim, since v2.0 renamed the mutable accessors.
	UnifiedVectorFormat input;
	args.data[0].ToUnifiedFormat(args.size(), input);
	auto paths = UnifiedVectorFormat::GetData<string_t>(input);

	for (idx_t i = 0; i < args.size(); i++) {
		auto idx = input.sel->get_index(i);
		ReaderEntry entry;
		if (!input.validity.RowIsValid(idx) ||
		    !ReaderRegistry::Get().Lookup(ExtOfPath(paths[idx].GetString()), entry)) {
			result.SetValue(i, Value());
			continue;
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
		result.SetValue(i, value->empty() ? Value() : Value(*value));
	}
}

void CanReadFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, bool>(args.data[0], result, args.size(), [&](string_t path) {
		ReaderEntry entry;
		return ReaderRegistry::Get().Lookup(ExtOfPath(path.GetString()), entry);
	});
}

//! The sentinel meaning "the caller asked for nothing". Dispatch's option-bearing named
//! parameters default to it, and it renders to the empty string WITHOUT consulting the
//! registry -- which is what keeps a default read byte-identical to the SQL panduck
//! generated before options existed, for every format, including the ones that map no
//! options at all and would otherwise raise.
static constexpr const char *OPTION_DEFAULT = "default";

//! panduck_reader_option_for(path, intent, value) -> "param := literal", or ''.
//!
//! THE RENDERING IS BY TYPE, and this is the whole security boundary. `param` is re-checked
//! as an identifier and `arg` re-checked against `arg_type` even though registration
//! already refused anything else: this function is the only place registration data becomes
//! SQL text, so it does not delegate its own safety to a check that ran somewhere else. A
//! VARCHAR goes through the same doubling panduck_quote uses; BOOLEAN and INTEGER are
//! emitted bare and are therefore constrained to values that cannot be anything but a
//! literal. Nothing registrant-supplied reaches the generated SQL as SQL.
//!
//! AN UNMAPPED INTENT RAISES rather than being dropped. `pages` spent the life of this
//! macro being accepted and silently ignored, which is how it came to read as a working
//! feature at the call site; a reader that cannot honour `attributes := 'all'` must say so
//! rather than return a document quietly missing what was asked for.
void ReaderOptionForFun(DataChunk &args, ExpressionState &state, Vector &result) {
	// SetValue rather than a TernaryExecutor, for the same version-neutrality reason
	// RegistryFieldFun gives: the executors' null protocol moved between v1.5.5 and v2.0.
	UnifiedVectorFormat path_fmt, intent_fmt, value_fmt;
	args.data[0].ToUnifiedFormat(args.size(), path_fmt);
	args.data[1].ToUnifiedFormat(args.size(), intent_fmt);
	args.data[2].ToUnifiedFormat(args.size(), value_fmt);
	auto paths = UnifiedVectorFormat::GetData<string_t>(path_fmt);
	auto intents = UnifiedVectorFormat::GetData<string_t>(intent_fmt);
	auto values = UnifiedVectorFormat::GetData<string_t>(value_fmt);

	for (idx_t i = 0; i < args.size(); i++) {
		auto v_idx = value_fmt.sel->get_index(i);
		// A NULL VALUE IS NOT THE SENTINEL, and conflating the two reopens the hole the
		// IS DISTINCT FROM guards in READ_DOC_MACRO close. Those guards only cover the
		// formats that map NO option; a format that DOES map one reaches the generic branch
		// and arrives here, so read_panduck_doc('x.htmltest', attributes := NULL) would have
		// rendered nothing and read the document without the option -- accepted and ignored,
		// by passing NULL instead of a value, on exactly the formats where the option works.
		if (!value_fmt.validity.RowIsValid(v_idx)) {
			auto i_idx_err = intent_fmt.sel->get_index(i);
			auto named = intent_fmt.validity.RowIsValid(i_idx_err) ? intents[i_idx_err].GetString() : "an option";
			throw InvalidInputException("panduck: %s must name a value; NULL is not one", named);
		}
		if (values[v_idx].GetString() == OPTION_DEFAULT) {
			result.SetValue(i, Value(""));
			continue;
		}
		auto p_idx = path_fmt.sel->get_index(i);
		auto i_idx = intent_fmt.sel->get_index(i);
		if (!path_fmt.validity.RowIsValid(p_idx) || !intent_fmt.validity.RowIsValid(i_idx)) {
			result.SetValue(i, Value());
			continue;
		}
		auto path = paths[p_idx].GetString();
		auto intent = intents[i_idx].GetString();
		auto value = values[v_idx].GetString();

		ReaderEntry entry;
		ReaderRegistry::Get().Lookup(ExtOfPath(path), entry);
		const readers::ReaderOption *match = nullptr;
		for (auto &o : entry.options) {
			if (o.intent == intent && o.value == value) {
				match = &o;
				break;
			}
		}
		if (!match) {
			throw InvalidInputException("panduck: the reader for %s has no mapping for %s = '%s'", path, intent, value);
		}
		if (!readers::IsIdentifier(match->param) || !readers::ArgMatchesType(match->arg_type, match->arg)) {
			throw InvalidInputException("panduck: refusing to render option %s for %s", match->param, path);
		}
		std::string rendered = match->param + " := ";
		if (match->arg_type == "VARCHAR") {
			rendered += "'";
			for (char c : match->arg) {
				if (c == '\'') {
					rendered += "''";
				} else {
					rendered.push_back(c);
				}
			}
			rendered += "'";
		} else {
			rendered += match->arg;
		}
		result.SetValue(i, Value(rendered));
	}
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
                                      panduck::BindNames &names) {
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
	std::vector<readers::ReaderOption> options;
};

struct RegisterGlobalState : public GlobalTableFunctionState {
	bool done = false;
	static unique_ptr<GlobalTableFunctionState> Init(ClientContext &, TableFunctionInitInput &) {
		return make_uniq<RegisterGlobalState>();
	}
};

template <const char *KIND>
unique_ptr<FunctionData> RegisterBind(ClientContext &, TableFunctionBindInput &input, vector<LogicalType> &return_types,
                                      panduck::BindNames &names) {
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

	// OPTIONS ARE VALIDATED HERE, IN THE BIND, so an ill-formed row never reaches the
	// registry. RegisterScan runs after this returns and is the only writer, so a throw
	// from here leaves the registry untouched -- which is the property the test asserts by
	// counting rows for the rejected extension afterwards, not merely by catching an error.
	// See ReaderOption for why the fields are validated rather than trusted.
	auto opt_entry = input.named_parameters.find("options");
	if (opt_entry != input.named_parameters.end() && !opt_entry->second.IsNull()) {
		for (auto &row : ListValue::GetChildren(opt_entry->second)) {
			if (row.IsNull()) {
				throw InvalidInputException("panduck: options contains a NULL entry");
			}
			// READ BY NAME rather than by position. The named parameter's declared STRUCT
			// type fixes the order today, but a by-index read silently mis-assigns every
			// field if that declaration is ever reordered -- and the field that would land
			// in `param` decides what gets emitted as an identifier.
			//
			// EVERY FIELD IS REQUIRED AND NULL IS NOT A VALUE. A field the caller omits
			// arrives here as NULL (the cast fills it in; it is not a bind error), and
			// folding NULL to "" made a row with no `arg` at all storable under
			// arg_type 'VARCHAR' -- it rendered `p := ''`, measured. The design's claim is
			// that a malformed row cannot be STORED, and a row missing a field is malformed,
			// so the NULL is refused here rather than absorbed. An explicitly EMPTY `arg` is
			// still accepted: `p := ''` is a legitimate literal to want, and it is
			// distinguishable from an absent one.
			readers::ReaderOption o;
			auto &fields = StructValue::GetChildren(row);
			auto &field_types = StructType::GetChildTypes(row.type());
			for (idx_t i = 0; i < fields.size(); i++) {
				auto &name = field_types[i].first;
				if (fields[i].IsNull()) {
					throw InvalidInputException("panduck: an option needs a non-NULL '%s'", name);
				}
				auto text = fields[i].GetValue<string>();
				if (name == "intent") {
					o.intent = std::move(text);
				} else if (name == "value") {
					o.value = std::move(text);
				} else if (name == "param") {
					o.param = std::move(text);
				} else if (name == "arg") {
					o.arg = std::move(text);
				} else if (name == "arg_type") {
					o.arg_type = std::move(text);
				}
			}
			if (!readers::IsIdentifier(o.param)) {
				throw InvalidInputException("panduck: param must be an identifier, got '%s'", o.param);
			}
			if (o.arg_type != "VARCHAR" && o.arg_type != "BOOLEAN" && o.arg_type != "INTEGER") {
				throw InvalidInputException("panduck: unknown arg_type '%s' (VARCHAR, BOOLEAN or INTEGER)", o.arg_type);
			}
			if (!readers::ArgMatchesType(o.arg_type, o.arg)) {
				throw InvalidInputException("panduck: arg '%s' is not a %s", o.arg, o.arg_type);
			}
			if (o.intent.empty()) {
				throw InvalidInputException("panduck: an option needs an intent");
			}
			result->options.push_back(std::move(o));
		}
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
		entry.options = data.options;
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
//   C++ scalars, available at LOAD    duck_blocks_toc, duck_blocks_to_text -> usable here
//   macros behind PRAGMA duck_block_* duck_block_ansi and friends          -> NOT usable
//
// A macro created by a pragma cannot be reached from a panduck macro: it is invisible to
// the statement that loads the extension, and panduck cannot invoke a pragma from inside
// a macro. So doc_section and doc_sections_like are absent, and doc_render has no 'ansi'
// arm, until the pieces they need are reachable at LOAD.
//
// THE SPELLING HERE IS duck_*, NOT db_*, AND THAT CHANGED UNDER US. duck_block_utils
// renamed its whole surface (db_ reads as DATABASE everywhere else in SQL) and published
// it. Measured in an empty extension_directory, so the shared ~/.duckdb profile could not
// colour the result: community build 3f2a0f0 exports 0 db_* and 174 duck_*, with no
// back-compat aliases. These are RUNTIME name lookups against whatever build is
// installed, so they broke the moment that landed -- see doc_namespace.test, which is the
// alarm for exactly this and must be flipped in the same commit as these call sites.
//
// PARTLY UNBLOCKED, recorded as a measurement and not acted on: that same build registers
// duck_blocks_render_ansi and duck_block_section as C++ SCALARS at LOAD. Wiring doc_render
// 'ansi' or doc_section to them means checking their signatures against what those macros
// need, which is a separate change from this rename.

const DefaultTableMacro DOC_TOC_MACRO = {DEFAULT_SCHEMA,
                                         "doc_toc",
                                         {"src", nullptr},
                                         {{"format", "'auto'"}, {nullptr, nullptr}},
                                         R"SQL(
SELECT * FROM query(
    -- GUARDED HERE, NOT INHERITED FROM read_panduck_doc, because this macro does not pass
    -- `format` along as a VALUE -- it renders it into SQL text through panduck_quote, which
    -- coalesces NULL to ''. So dispatch never saw a NULL to refuse: it saw the string '',
    -- resolved the format to '' and raised "resolves to format '' ... This is a panduck
    -- bug", blaming panduck for the caller's NULL. MEASURED. A misdiagnosis is worse than a
    -- silent default, because it sends the next person to read the wrong code.
    CASE WHEN format IS NULL
    THEN error('panduck: cannot use NULL as argument for "format"')
    WHEN panduck_ensure_extension('duck_block_utils')
    THEN 'SELECT (t).level AS level, (t).title AS title, (t).id AS id, ' ||
         '(t).indent AS indent, (t).element_order AS element_order ' ||
         'FROM (SELECT unnest(duck_blocks_toc(panduck_read_blocks(' || panduck_quote(src) ||
         ', format := ' || panduck_quote(format) || '))) AS t)'
    ELSE error('panduck: doc_toc needs the duck_block_utils extension (INSTALL duck_block_utils)')
    END
)
)SQL"};

// The macro TABLE is version-neutral -- see panduck::PanduckMacro. DuckDB's own
// DefaultMacro changed shape between v1.5.5 and v2.0, and the SQL below is the part worth
// keeping in one place; only the conversion is version-aware.
const panduck::PanduckMacro SCALAR_MACROS[] = {
    {DEFAULT_SCHEMA,
     "panduck_quote",
     {"s", nullptr},
     {{nullptr, nullptr}},
     "'''' || replace(coalesce(s, ''), '''', '''''') || ''''"},

    // Is this source string a PATTERN or a PATH? Only these three characters make a glob in
    // DuckDB's matcher. A plain path deliberately does NOT go through the filesystem: a
    // caller naming one file must keep today's behaviour, including the reader's own error
    // if it is missing, rather than getting a glob-shaped error from panduck.
    {DEFAULT_SCHEMA,
     "panduck_is_glob",
     {"s", nullptr},
     {{nullptr, nullptr}},
     "contains(s, '*') OR contains(s, '?') OR contains(s, '[')"},

    // Every source form becomes one VARCHAR[] here, so nothing downstream has to branch on
    // which form the caller used. flatten() preserves argument order for a list, which is
    // what makes read_panduck_doc(['a','b']) deterministic independent of the filesystem.
    //
    // NO LONGER NEUTRAL, reversing the original Task 2 ruling. The plan's first cut said
    // this primitive stays neutral and dispatch alone owns the empty-match raise -- but
    // dispatch only ever sees the FLATTENED list, and flatten() throws away which element
    // produced which paths. read_panduck_doc(['a.odt', '*.nomatch']) flattens to just
    // ['a.odt'], and a length check on THAT tells dispatch "one path resolved", which is
    // true and useless: it can no longer see that the second element matched nothing.  A
    // caller silently loses a source that named itself in the call.  So each glob is
    // checked as it expands, here, before flatten() erases the boundary between elements --
    // the only place in the pipeline where that boundary still exists.  Dispatch's own
    // len(...) = 0 check on the whole list stays; it is now redundant for a list but still
    // the one that catches a BARE glob matching nothing.
    {DEFAULT_SCHEMA,
     "panduck_source_list",
     {"src", nullptr},
     {{nullptr, nullptr}},
     "CASE WHEN typeof(src) LIKE '%[]' "
     "     THEN flatten(list_transform(src::VARCHAR[], "
     "                  lambda p: CASE WHEN panduck_is_glob(p) "
     "                                 THEN CASE WHEN len(panduck_glob(p)) = 0 "
     "                                      THEN error('panduck: no files matched ' || p) "
     "                                      ELSE panduck_glob(p) END "
     "                                 ELSE [p] END)) "
     "     WHEN panduck_is_glob(src::VARCHAR) "
     "          THEN CASE WHEN len(panduck_glob(src::VARCHAR)) = 0 "
     "               THEN error('panduck: no files matched ' || src::VARCHAR) "
     "               ELSE panduck_glob(src::VARCHAR) END "
     "     ELSE [src::VARCHAR] END"},

    // Build the UNION ALL that dispatch runs. One arm per path, joined in list order.
    //
    // PROVENANCE IS PROJECTED, NOT REQUESTED. `SELECT *, '<path>' AS filename` puts the
    // column AFTER the canonical seven by construction, because SELECT * emits those first.
    // That matters more than it looks: duck_block spec 6.4 keys 8-field acceptance on the
    // exact type with filename LAST, every consumer reads the struct by index, and BOTH
    // shipped sibling producers got this wrong by emitting the column first (webbed
    // a865d37, markdown 340c0cd) -- refused at the binder rather than misread, but refused.
    // A projection cannot make that mistake.
    //
    // It also means panduck needs nothing from a sibling to have provenance: the path is
    // already in hand at dispatch time.
    //
    // THREE DEFECTS FIXED HERE across two rounds of review, all because this builder assumed
    // every path behaves like the generic branch's plain flat readers and none of those
    // assumptions held.
    //
    // 1. NO EXTENSION GATE. panduck_ensure_extension does not merely produce a nicer error --
    // it is TryAutoLoadExtension (reader_registry.cpp), so skipping it meant a community
    // reader (webbed's read_html_blocks, say) that the single-path branch would have loaded
    // automatically instead surfaced a raw Catalog Error through a plural call. Each arm now
    // wraps its SELECT in an ensure-or-named-error CASE, so the two paths agree both in
    // behaviour and in wording.
    //
    // 2. SILENT DATA LOSS (round 1). panduck_reader_function_for(p) is NULL for every
    // registry entry whose `function` is deliberately empty -- pdf, toml, yaml, zim, zim://,
    // and every KIND_TABLE format (csv, json, ...), all served by READ_DOC_MACRO's own
    // special-cased branches rather than a plain table function. Concatenating a NULL reader
    // name nulled the whole arm, and array_to_string SKIPS NULL elements -- so
    // read_panduck_doc(['a.odt', 'x.pdf']) silently returned only a.odt's rows, no error.
    // Refusing loudly is the correct behaviour for now; teaching this builder the special
    // branches is a separate change.
    //
    // 3. SILENT DATA LOSS AGAIN, THROUGH THE ROUND-1 FIX ITSELF (round 2). `error(NULL) IS
    // NULL` is true -- error() does not throw on a NULL argument, it silently RETURNS NULL.
    // The round-1 fix's own named-error arm, `error('...' || panduck_reader_extension_for(p)
    // || '...')`, concatenates NULL whenever a reader is registered with an empty
    // reader_ext (panduck_register_doc_reader('', fn, exts) -- meaning "no companion
    // extension needed"), so panduck_reader_extension_for(p) answers NULL,
    // panduck_ensure_extension(NULL) answers NULL (not TRUE), and the ELSE below used to
    // build error(NULL) -- itself NULL, skipped by array_to_string exactly like defect 2.
    // Reproduced end to end: register such a reader, put it in a list with an ODT, and the
    // second document silently vanishes with no error. FIXED by hoisting the NULL check into
    // the WHEN rather than the message: a NULL reader_ext takes the SAME path a real
    // extension that loaded fine would (nothing to ensure), so the ELSE is only ever reached
    // when reader_extension_for(p) is proven NON-NULL, and no coalesce is needed there.
    //
    // A NULL PATH ELEMENT gets the same structural treatment, checked FIRST: a caller can
    // write read_panduck_doc(['a.odt', NULL]), and panduck_reader_function_for(NULL) also
    // answers NULL (not "no function for this format" but "no input to look up at all"),
    // which used to fall into defect 2's own arm with p itself NULL -- naming a NULL path in
    // the message and nulling it right back out through the same error(NULL) trap. Checked
    // ahead of the function-lookup so every later arm can assume p is non-NULL.
    //
    // OPTIONS ARE CARRIED AS AN INTENT, NOT AS A FRAGMENT. `opt_intent`/`opt_value` are
    // panduck's own vocabulary ('attributes', 'all'); the reader's spelling for it comes
    // from the registry and is RENDERED by panduck_reader_option_for, which validates
    // before emitting. Nothing a registrant supplied is interpolated as SQL. See
    // ReaderOption in reader_registry.hpp for why a stored fragment was rejected.
    //
    // THIS IS THE ONE IMPLEMENTATION, and panduck_read_arms below delegates to it with the
    // 'default' sentinel. Written the other way round -- arms_opt as a copy of arms with an
    // extra concat -- the three silent-data-loss defects documented above would have to stay
    // correct in two bodies at once, and the byte-identity property Task 4 asserts would be
    // a coincidence between two strings rather than a consequence of running one builder.
    {DEFAULT_SCHEMA,
     "panduck_read_arms_opt",
     {"paths", "with_filename", "opt_intent", "opt_value", nullptr},
     {{nullptr, nullptr}},
     // THE NULL `with_filename` REFUSAL IS REPEATED HERE rather than left to dispatch, and
     // in dispatch's exact words. This builder is directly callable and directly tested, so
     // a guard living only in READ_DOC_MACRO would leave the two call sites disagreeing
     // about the same argument -- which is the defect shape fixed for `pages` one round
     // earlier, where dispatch coalesced a NULL that read_pdf_blocks refused.
     "CASE WHEN with_filename IS NULL "
     "     THEN error('panduck: cannot use NULL as argument for \"filename\"') "
     "     ELSE "
     "array_to_string(list_transform(paths, lambda p: "
     "  CASE WHEN p IS NULL "
     "       THEN error('panduck: a multi-document source contains a NULL path') "
     "       WHEN panduck_reader_function_for(p) IS NULL "
     "       THEN error('panduck: ' || p || ' (' || coalesce(panduck_format_for(p), 'unknown') || "
     "                  ') cannot be read in a multi-document call yet') "
     "       ELSE CASE WHEN panduck_reader_extension_for(p) IS NULL "
     "                 OR panduck_ensure_extension(panduck_reader_extension_for(p)) "
     "            THEN 'SELECT *' || CASE WHEN with_filename THEN ', ' || panduck_quote(p) || ' AS filename' "
     "                               ELSE '' END || "
     "                 ' FROM ' || panduck_reader_function_for(p) || '(' || panduck_quote(p) || "
     "                 CASE WHEN panduck_reader_option_for(p, opt_intent, opt_value) = '' THEN '' "
     "                      ELSE ', ' || panduck_reader_option_for(p, opt_intent, opt_value) END || ')' "
     "            ELSE error('panduck: ' || p || ' needs the ' || panduck_reader_extension_for(p) || "
     "                       ' extension') END "
     "       END), "
     "  ' UNION ALL ') END"},

    // Arms with no option requested. Kept as its own name because it is what every caller
    // that has nothing to ask for should read like, and because the suite asserts its
    // output as a string; 'default' is the sentinel that makes the option render to ''.
    {DEFAULT_SCHEMA,
     "panduck_read_arms",
     {"paths", "with_filename", nullptr},
     {{nullptr, nullptr}},
     "panduck_read_arms_opt(paths, with_filename, NULL, 'default')"},

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
    // duck_blocks_to_text, which is a C++ scalar and therefore reachable; 'ansi' is not
    // wired up -- see the reachability note above the doc_toc macro.
    {DEFAULT_SCHEMA,
     "doc_render",
     {"src", "output_format", nullptr},
     {{"format", "'auto'"}, {nullptr, nullptr}},
     "(SELECT r FROM query("
     "  CASE"
     // Same as doc_toc: `format` is rendered into SQL text through panduck_quote, which
     // coalesces NULL to '', so a NULL never reaches read_panduck_doc as a NULL and
     // surfaced as the "This is a panduck bug" arm instead. Measured.
     "    WHEN format IS NULL"
     "      THEN error('panduck: cannot use NULL as argument for \"format\"')"
     "    WHEN output_format = 'md' AND panduck_ensure_extension('markdown')"
     "      THEN 'SELECT duck_blocks_to_md(panduck_read_blocks(' || panduck_quote(src) ||"
     "           ', format := ' || panduck_quote(format) || ')) AS r'"
     "    WHEN output_format = 'html' AND panduck_ensure_extension('webbed')"
     "      THEN 'SELECT duck_blocks_to_html(panduck_read_blocks(' || panduck_quote(src) ||"
     "           ', format := ' || panduck_quote(format) || ')) AS r'"
     "    WHEN output_format = 'text' AND panduck_ensure_extension('duck_block_utils')"
     "      THEN 'SELECT duck_blocks_to_text(panduck_read_blocks(' || panduck_quote(src) ||"
     "           ', format := ' || panduck_quote(format) || ')) AS r'"
     // output_format IS COALESCED (round-2 error() audit): a caller can write
     // doc_render(src, NULL), which matches none of the WHEN clauses above (each requires
     // output_format to literally equal a string, and NULL = 'md' is NULL, not TRUE) and
     // falls to this ELSE with output_format itself NULL. Without the coalesce the
     // concatenation is NULL and error(NULL) silently returns NULL instead of raising.
     "    ELSE error('panduck: doc_render supports md, html and text; ' || coalesce(output_format, '<NULL>') ||"
     "               ' is unsupported or its extension is not installed')"
     "  END))"},

    {nullptr, nullptr, {nullptr}, {{nullptr, nullptr}}, nullptr}};

// read_pdf_blocks(src, pages := '') -- PDF into duck_blocks, with real page selection.
//
// WHY THE ELEMENTS ROUTE AND NOT pdf_to_markdown. read_panduck_doc's pdf branch goes
// through parse_markdown_to_duck_blocks(pdf_to_markdown(src)), and pdf_to_markdown takes
// a PATH AND NOTHING ELSE -- measured: [col0] VARCHAR. So that route cannot select pages
// at all. pdf_pages(src, dest, spec) can, but it WRITES A NEW PDF and returns its path,
// which is a filesystem side effect inside a SELECT and does not belong in a read path.
// read_pdf_elements takes first_page/last_page natively, so it is the only route where
// `pages` can mean anything.
//
// THE TRADE IS REAL AND MEASURED, NOT HIDDEN. Against the same document read as ODT:
// the elements route KEEPS list structure that pdf_to_markdown flattens (two list_items
// vs "• bullet one • bullet two" jammed into one paragraph), and LOSES inline emphasis
// that pdf_to_markdown keeps (**bold** survives markdown, not elements). Neither route
// dominates. Page selection is the thing a caller asked for by name, so it wins here;
// the emphasis loss is a divergence, recorded as one.
//
// HEADING LEVELS ARE RANKED OVER THE WHOLE DOCUMENT, THEN THE CONTENT IS SLICED, and the
// order matters. PDF has no heading levels -- only font sizes -- so level is dense_rank
// over distinct heading font sizes. Rank within the SLICE and a section cut out of the
// middle of a document comes back with its headings promoted to level 1, because it is
// the largest thing in its own slice. Measured on a two-page fixture: ranked per-slice,
// "Page Two Heading" reads heading_level=1 alone and 2 in the full document -- the same
// heading changing depth depending on what you asked for. Ranked document-wide it stays
// 2 either way. The cost is that the level scan reads the whole document even when one
// page is wanted, so `pages` selects CONTENT rather than saving work.
//
// NO ensure_extension GATE HERE, deliberately. A guard would have to run before the
// binder resolves read_pdf_elements, which it cannot, so the only way to keep the named
// error is to build the whole body as a query() string -- and this body has enough
// quoting in it that doing so trades a clear error message for a real chance of a subtly
// wrong regex. read_panduck_doc keeps its gate and delegates here, so dispatch still says
// "panduck: pdf needs the pdf and markdown extensions"; calling this function directly
// without pdf installed gives a catalog error, exactly as calling webbed's
// read_html_blocks or markdown's read_markdown_blocks directly would.
// doc_section(src, section, format := 'auto') -- the blocks under one heading.
//
// PATH IN, BLOCKS OUT, which is why this does not wrap duck_block_utils' section
// functions even though they exist and this file's docs said it would. Measured on
// build 3f2a0f0: duck_blocks_get_section(blocks, pattern, output_format) returns VARCHAR
// -- a RENDERED string -- for every output_format including 'blocks', and
// duck_blocks_sections_like returns (section, start_order, content), also rendered. Both
// are useful and neither gives back duck_blocks, so wrapping either would make doc_section
// the only doc_* that reads a path and hands back text instead of a queryable table.
// Slicing panduck's own stream keeps the composition -- the result feeds doc_toc, the
// pandoc writer, and every duck_block consumer, because it IS duck_blocks. It also drops
// a dependency: those functions need the json extension loaded, and this needs nothing.
//
// THE BOUNDARY IS "the next heading at the same level or higher", not "the next heading".
// A section CONTAINS its subsections, which is what makes doc_section('Chapter 2') mean
// what a reader expects rather than stopping at the first sub-heading. Verified against a
// fixture built for exactly this: sections.html is h1/h2/h1/h2 so a slice must actually
// STOP. The first fixture tried had every section running to end-of-document, which would
// have let a missing boundary pass -- 41 and 40 rows out of 54, both simply the tail.
//
// MATCHES CONTENT OR id, because a heading's TEXT is what a person names and its id is
// what a link names, and an HTML document has both. try_cast on heading_level rather than
// a cast: the attribute is absent on non-headings and need not be numeric, and a hard cast
// would turn a malformed attribute into an error for the whole document.
// doc_container(src, id, format := 'auto') -- the blocks inside one identified container.
//
// SIBLING OF doc_section, DIFFERENT AXIS. doc_section walks HEADINGS and its boundary is
// heading_level, so it answers "the prose under this title". doc_container walks the
// STRUCTURAL nesting in `level` and answers "what is inside this box" -- a div, a section,
// an article, a list, a blockquote. Those are different questions and a document can
// disagree about them: a <div> can hold three headings, and a heading's section can run
// across several divs.
//
// THE BOUNDARY IS `level`, WHICH READERS ALREADY MAINTAIN. A container at depth N owns the
// blocks after it while depth > N, ending at the next block whose depth is <= N. Nothing
// format-specific is involved, so this works for any reader that nests -- it is HTML today
// only because HTML is where ids come from.
//
// MATCHES ANY BLOCK CARRYING THE id, not only containers, and that is deliberate: a caller
// naming an id should get what that id labels, whether the emitter called it a div, a
// section or a figure. A leaf with an id yields just itself, which is the correct answer
// rather than an empty one.
//
// `id` IS NOT CANONICAL VOCABULARY -- measured: the duck_block vocabulary publishes seven
// ATTR_ constants (role, key, heading_level, list_type, source_type, pandoc_ast, ordered)
// and identity is not among them, though the spec's per-element attribute table does
// describe id. A proposal to publish ATTR_ID is with duck_block_utils. Until it lands this
// reads a key by literal, exactly as webbed writes it by literal, and the two agree by
// convention rather than by contract. doc_section already carries the same exposure.
//
// WHICH BLOCKS CARRY AN id DEPENDS ON THE INSTALLED webbed, not on panduck. Before
// webbed#142/#143 only div, section/article and heading kept it, so <ul id="steps"> was
// unaddressable; after, every block keeps it. This macro is correct either way and simply
// finds more.
const DefaultTableMacro DOC_CONTAINER_MACRO = {DEFAULT_SCHEMA,
                                               "doc_container",
                                               {"src", "id", nullptr},
                                               {{"format", "'auto'"}, {nullptr, nullptr}},
                                               R"SQL(
WITH b AS (SELECT * FROM read_panduck_doc(src, format := format)),
c AS (SELECT element_order AS o, level AS lv
      FROM b WHERE attributes['id'] = id
      ORDER BY element_order LIMIT 1),
stop AS (SELECT min(b.element_order) AS o
         FROM b, c
         WHERE b.element_order > c.o AND b.level <= c.lv)
SELECT b.kind, b.element_type, b.content, b.level, b.encoding, b.attributes, b.element_order
FROM b, c
WHERE b.element_order >= c.o
  AND (b.element_order < (SELECT o FROM stop) OR (SELECT o FROM stop) IS NULL)
ORDER BY b.element_order
)SQL"};

const DefaultTableMacro DOC_SECTION_MACRO = {DEFAULT_SCHEMA,
                                             "doc_section",
                                             {"src", "section", nullptr},
                                             {{"format", "'auto'"}, {nullptr, nullptr}},
                                             R"SQL(
WITH b AS (SELECT * FROM read_panduck_doc(src, format := format)),
h AS (SELECT element_order AS o,
             coalesce(try_cast(attributes['heading_level'] AS INTEGER), 1) AS lvl
      FROM b
      WHERE element_type = 'heading' AND (content = section OR attributes['id'] = section)
      ORDER BY element_order LIMIT 1),
stop AS (SELECT min(b.element_order) AS o
         FROM b, h
         WHERE b.element_type = 'heading' AND b.element_order > h.o
           AND coalesce(try_cast(b.attributes['heading_level'] AS INTEGER), 1) <= h.lvl)
SELECT b.kind, b.element_type, b.content, b.level, b.encoding, b.attributes, b.element_order
FROM b, h
WHERE b.element_order >= h.o
  AND (b.element_order < (SELECT o FROM stop) OR (SELECT o FROM stop) IS NULL)
ORDER BY b.element_order
)SQL"};

const DefaultTableMacro READ_PDF_BLOCKS_MACRO = {DEFAULT_SCHEMA,
                                                 "read_pdf_blocks",
                                                 {"src", nullptr},
                                                 {{"pages", "''"}, {nullptr, nullptr}},
                                                 R"SQL(
WITH lvl AS (
    SELECT font_size, dense_rank() OVER (ORDER BY font_size DESC) AS hl
    FROM (SELECT DISTINCT font_size FROM read_pdf_elements(src) WHERE element_type = 'heading')
), raw AS (
    SELECT page_number, element_idx, element_type, text, font_size
    FROM read_pdf_elements(
        src,
        -- pages IS COALESCED IN BOTH error() MESSAGES BELOW (round-2 error() audit): a
        -- caller can write read_pdf_blocks(src, pages := NULL) despite the '' default,
        -- which matches neither the '' branch nor either regexp_matches branch (NULL
        -- compared or matched against anything is NULL, not TRUE) and falls to ELSE with
        -- pages itself NULL. Without the coalesce the concatenation is NULL and error(NULL)
        -- silently returns NULL instead of raising, passing a NULL first_page/last_page
        -- into read_pdf_elements rather than the named "pages must be N or N-M" refusal.
        first_page := CASE WHEN pages = '' THEN 1
                           WHEN regexp_matches(pages, '^[0-9]+(-[0-9]+)?$')
                               THEN split_part(pages, '-', 1)::INTEGER
                           ELSE error('panduck: pages must be N or N-M, got ' || coalesce(pages, '<NULL>')) END,
        last_page := CASE WHEN pages = '' THEN 2147483647
                          WHEN regexp_matches(pages, '^[0-9]+$') THEN pages::INTEGER
                          WHEN regexp_matches(pages, '^[0-9]+-[0-9]+$')
                              THEN split_part(pages, '-', 2)::INTEGER
                          ELSE error('panduck: pages must be N or N-M, got ' || coalesce(pages, '<NULL>')) END)
), e AS (
    -- A page_break BLOCK opens each page, which is how duck_block_utils models pagination:
    -- duck_blocks_get_pages and duck_blocks_page_rows scan for element_type='page_break'
    -- and read page_number OFF THE BREAK (doc_macros.cpp:201, :324), treating everything
    -- between breaks as that page's content. Blocks do not carry their own page there.
    --
    -- This shipped without breaks first, and duck_blocks_page_rows returned 0 rows for a
    -- two-page PDF -- the pagination function could not see the pagination. Renaming the
    -- per-block key from 'page' to 'page_number' did not fix it either, because the
    -- mismatch was never the key: it was two different MODELS of the same concept.
    -- TYPE_PAGE = "page_break" is canonical vocabulary and panduck's own EPUB reader
    -- already emits it (epub_reader.cpp:629), so this reader was the outlier.
    --
    -- The per-block page_number is KEPT as well, because filtering rows by page directly is
    -- what a caller reaches for first. It is NOT canonical -- the vocabulary publishes no
    -- ATTR_ constant for it, which is exactly why the original divergence was invisible to
    -- check-vocabulary -- so it is a panduck convention pending a ruling from
    -- duck_block_utils rather than something to rely on across extensions.
    SELECT page_number, element_idx, element_type, text, font_size,
           row_number() OVER (ORDER BY page_number, element_idx) - 1 AS ord
    FROM (SELECT page_number, -1 AS element_idx, 'page_break' AS element_type,
                 NULL AS text, NULL::DOUBLE AS font_size
          FROM (SELECT DISTINCT page_number FROM raw)
          UNION ALL SELECT page_number, element_idx, element_type, text, font_size FROM raw)
)
SELECT 'block' AS kind,
       CASE e.element_type WHEN 'heading' THEN 'heading'
                           WHEN 'list_item' THEN 'list_item'
                           WHEN 'page_break' THEN 'page_break'
                           ELSE 'paragraph' END AS element_type,
       regexp_replace(e.text, '^\s*(•|\d+\.)\s+', '') AS content,
       1 AS level,
       'text' AS encoding,
       -- 'page_number', NOT 'page'. duck_block_utils' duck_blocks_get_pages and
       -- duck_blocks_page_rows both read attributes['page_number'] (doc_macros.cpp:201,
       -- :324). This shipped as 'page' first and duck_blocks_page_rows returned 0 rows for
       -- a two-page PDF -- the pagination function could not see the pagination. Nothing
       -- caught it: the vocabulary publishes no ATTR_ constant for this, so check-vocabulary
       -- has nothing to compare against, and every arm of that check is about constants that
       -- ARE published rather than keys a producer invented.
       map_from_entries(list_filter([
           {k: 'page_number', v: e.page_number::VARCHAR},
           {k: 'heading_level', v: CASE WHEN e.element_type = 'heading' THEN lvl.hl::VARCHAR END},
           {k: 'list_type', v: CASE WHEN e.element_type = 'list_item'
                                    THEN CASE WHEN regexp_matches(e.text, '^\s*\d+\.')
                                              THEN 'ordered' ELSE 'bullet' END END}
       ], lambda x: x.v IS NOT NULL)) AS attributes,
       e.ord::INTEGER AS element_order
FROM e LEFT JOIN lvl ON e.font_size = lvl.font_size
ORDER BY e.ord
)SQL"};

const DefaultTableMacro READ_DOC_MACRO = {
    DEFAULT_SCHEMA,
    "read_panduck_doc",
    {"src", nullptr},
    // `attributes` IS THE FIRST OPTION INTENT to reach dispatch, and it exists here rather
    // than only in panduck_read_arms_opt's own test for a reason: an option the dispatcher
    // cannot be asked for is a feature that exists solely inside its own assertion. Its
    // default is the 'default' sentinel, which renders to nothing -- so an unchanged call
    // generates byte-identical SQL to what it generated before options existed, which is
    // the property the plural and single-path string assertions pin.
    {{"format", "'auto'"}, {"pages", "''"}, {"filename", "false"}, {"attributes", "'default'"}, {nullptr, nullptr}},
    R"SQL(
SELECT * FROM query(
    CASE
        -- NULL IS NOT A VALUE FOR A NAMED PARAMETER, and DuckDB CORE is the standard this
        -- family was aligned to: read_csv('a.csv', filename := NULL) and read_json(...) both
        -- raise `Cannot use NULL as argument for "filename"`. panduck silently defaulting
        -- was therefore two defects at once -- accept-and-ignore, and a divergence from core
        -- on the very parameter whose name, column and deliberate refusal of the
        -- string-rename form were all settled by "same as core". MEASURED before this fix:
        -- filename := NULL returned SEVEN columns from a single path, a glob and a pdf alike,
        -- with provenance silently off; format := NULL read the document by auto-detection.
        --
        -- THIS IS THE SAME DEFECT IN ITS FOURTH COSTUME. It has now appeared as a `<>`
        -- comparison (NULL falsifies the condition), as a renderer sentinel (NULL read as
        -- 'default'), as this `CASE WHEN filename` (NULL is not TRUE, so provenance is
        -- quietly off), and as `coalesce` inside panduck_resolved_format (NULL reads as
        -- 'auto'). The shape is always the same: a NULL takes the same path the DEFAULT
        -- takes, so the caller cannot tell that what they asked for did not happen. Checked
        -- deliberately across every named parameter this macro family accepts rather than
        -- only the ones already reported.
        --
        -- `pages` IS REFUSED FURTHER DOWN AND IN DIFFERENT WORDS, deliberately. `filename`
        -- and `format` are panduck's own parameters and answer to core; `pages` is parsed by
        -- read_pdf_blocks, so dispatch matches THAT function's refusal verbatim so the two
        -- paths cannot disagree about the same argument. Each matches its own authority,
        -- which is one rule, not two.
        WHEN filename IS NULL
            THEN error('panduck: cannot use NULL as argument for "filename"')

        WHEN format IS NULL
            THEN error('panduck: cannot use NULL as argument for "format"')

        -- `pages` ON A PLURAL SOURCE IS REFUSED OUTRIGHT, deliberately BEFORE the
        -- single-format `pages` guard below and BEFORE the format is ever resolved.
        -- Review round 2: the existing guard below only rejects `pages` when the
        -- resolved format is not 'pdf' -- which means read_panduck_doc('*.pdf',
        -- pages := '2') currently PASSES it, because the format resolves to 'pdf' even
        -- though the source is a glob. That is forward-safety, not redundancy: the
        -- moment PDF (or any format) gains a plural arm, that combination would start
        -- silently dropping `pages` again, the identical defect class. Subsuming the
        -- glob case here, ahead of any format resolution, closes that off pre-emptively
        -- rather than waiting for it to be found the same way Finding 3 was.
        --
        -- `IS DISTINCT FROM`, NOT `<>`, AND THAT IS THE WHOLE GUARD. `NULL <> ''` is NULL,
        -- not TRUE, so a caller writing pages := NULL skipped both of these guards entirely
        -- and got the parameter accepted and silently ignored -- measured:
        -- read_panduck_doc('constructs.odt', pages := NULL) returned all 54 rows, and
        -- read_panduck_doc('*.odt', pages := NULL) returned all 180. That is the exact
        -- defect these guards exist to close, reachable by passing NULL instead of a value,
        -- which is how the class survives a fix that only looks at non-NULL arguments. The
        -- round-2 error() audit found error(NULL) silently returning NULL; this is its
        -- twin one level up, in the CONDITION rather than the message.
        WHEN pages IS DISTINCT FROM '' AND (typeof(src) LIKE '%[]' OR panduck_is_glob(src::VARCHAR))
            THEN error('panduck: pages applies to a single document; ' ||
                       coalesce(src::VARCHAR, '<NULL>') || ' names more than one')

        -- `pages` WAS ACCEPTED AND IGNORED BY EVERY FORMAT. It has been declared here
        -- since this macro was written and was never referenced in the body, so
        -- pages := '2' and pages := 'utter nonsense' both returned the whole document.
        -- PDF now honours it; nothing else has pages to honour, and saying so is the
        -- point -- silently ignoring a parameter is how it came to read as a feature.
        --
        -- RUNS BEFORE THE PLURAL BRANCH, deliberately, so a glob inherits the same
        -- refusal a plain path gets: read_panduck_doc('*.odt', pages := 'nonsense') must
        -- raise rather than silently ignore pages, which is exactly the defect this guard
        -- exists to name. panduck_resolved_format(src::VARCHAR, format) still resolves
        -- correctly for a bare glob STRING here -- '*.odt' ends in '.odt' same as
        -- 'x.odt' does -- because this guard runs before src is expanded into a list.
        --
        -- panduck_resolved_format(...) is PROVABLY NON-NULL whenever this WHEN selects:
        -- the condition itself requires it to literally not equal 'pdf', which cannot be
        -- true for a NULL value (NULL <> 'pdf' is NULL, not TRUE) -- so no coalesce is
        -- needed on the interpolation below. Verified in the round-2 error() audit.
        --
        -- `IS DISTINCT FROM` for the same reason as the guard above; see its comment.
        WHEN pages IS DISTINCT FROM '' AND panduck_resolved_format(src::VARCHAR, format) <> 'pdf'
            THEN error('panduck: pages applies only to paginated formats (pdf); ' ||
                       panduck_resolved_format(src::VARCHAR, format) || ' has no pages')

        -- THE ONE HOLE THE TWO GUARDS ABOVE CANNOT REACH: a single PDF with pages := NULL.
        -- Both guards now fire for a NULL, but neither SELECTS here -- the first needs a
        -- plural source and the second needs a non-pdf format -- so a NULL fell through to
        -- the pdf branch below, where panduck_quote coalesces NULL to '' and the read
        -- quietly returned every page. Dispatch and the function it delegates to disagreed
        -- about the same argument: read_pdf_blocks(src, pages := NULL) raises this exact
        -- message on its own (see its first_page/last_page CASE). Fixing the guards and
        -- leaving this would have meant closing the hole for every format EXCEPT the one
        -- where `pages` actually means something. The wording is read_pdf_blocks' verbatim,
        -- so the two paths now refuse identically as well as both refusing.
        WHEN pages IS NULL
            THEN error('panduck: pages must be N or N-M, got <NULL>')

        -- src::VARCHAR IS COALESCED HERE (round-2 error() audit) because this branch is
        -- reachable with a NULL src: a caller forcing format := 'data' explicitly makes
        -- panduck_resolved_format resolve to 'data' regardless of src, e.g.
        -- read_panduck_doc(NULL, format := 'data'). Without the coalesce, the
        -- concatenation is NULL, error(NULL) returns NULL instead of raising (DuckDB
        -- behaviour: error(NULL) IS NULL is true), and the CASE silently produces NULL
        -- SQL text instead of the named refusal.
        WHEN panduck_resolved_format(src::VARCHAR, format) = 'data'
            THEN error('panduck: ' || coalesce(src::VARCHAR, '<NULL>') ||
                       ' is a data format, not a document. Use read_panduck_table instead.')

        -- PLURAL SOURCES. Resolved to a path list, then one arm per path. This branch runs
        -- only when the source is not a single plain path, so a caller naming one file
        -- generates exactly the SQL it generates today -- byte-identical, asserted.
        --
        -- Runs AFTER the pages and data-format guards above (moved here in review), so a
        -- glob string is checked by them exactly like a plain path is before ever being
        -- expanded into a list. Placed BEFORE the generic branch below, which requires a
        -- resolvable single format and would misfire on a list or a glob string.
        --
        -- CORRECTED IN ROUND 2: this comment previously claimed the bare-glob empty-match
        -- raise below was "what catches a bare glob matching nothing". That is no longer
        -- true and saying so was left standing as a stale claim. panduck_source_list now
        -- raises per element BEFORE returning (see that macro's comment), including for a
        -- bare unmatched glob -- so the error() below is *dead* for that case: the
        -- exception fires inside panduck_source_list, before len(...) is ever evaluated
        -- out here. The one case this dispatch-level check still catches on its own is a
        -- literal empty-list ARGUMENT, e.g. read_panduck_doc([]) -- flatten() over zero
        -- elements raises nothing per-element (there is nothing to iterate), so len(...) =
        -- 0 genuinely reaches here. src::VARCHAR is non-NULL whenever this fires: the only
        -- way in is a real (non-NULL) empty list, whose ::VARCHAR cast is the string '[]'.
        WHEN typeof(src) LIKE '%[]' OR panduck_is_glob(src::VARCHAR)
            THEN CASE WHEN len(panduck_source_list(src)) = 0
                 THEN error('panduck: no files matched ' || src::VARCHAR)
                 ELSE panduck_read_arms_opt(panduck_source_list(src), filename, 'attributes', attributes) END

        -- GENERIC: the registry names a table function that emits duck_blocks, so call
        -- it. One path for builtin flat readers (rtf, markdown) and for anything a user
        -- registered with panduck_register_doc_reader. panduck_read_arms with a
        -- single-element list generates exactly the same SQL the hand-built string used
        -- to, because with_filename := false is a no-op projection -- byte-identical,
        -- asserted by doc_namespace.test, reader_registry.test and html_reader.test.
        --
        -- THE EXTENSION-NULL CHECK IS HOISTED (round-2 error() audit), mirroring the
        -- identical fix in panduck_read_arms: a user can register a doc reader with
        -- panduck_register_doc_reader('', function, extensions) -- an empty reader_ext,
        -- meaning "no companion extension needed". panduck_reader_extension_for then
        -- answers NULL, panduck_ensure_extension(NULL) answers NULL (not TRUE), and the
        -- old ELSE concatenated that NULL into the message, so error(NULL) silently
        -- returned NULL instead of raising. Hoisting the IS NULL check into the WHEN means
        -- a NULL reader_ext takes the SAME path a real extension that loaded fine would,
        -- which is correct: no companion extension means nothing to ensure. Once past this
        -- hoist, reader_extension_for(src::VARCHAR) is guaranteed NON-NULL in the ELSE
        -- below (the OR would otherwise have already selected the THEN), so no coalesce is
        -- needed there.
        WHEN panduck_reader_function_for(src::VARCHAR) IS NOT NULL
             AND panduck_resolved_format(src::VARCHAR, format) = panduck_format_for(src::VARCHAR)
            THEN CASE WHEN panduck_reader_extension_for(src::VARCHAR) IS NULL
                      OR panduck_ensure_extension(panduck_reader_extension_for(src::VARCHAR))
                 THEN panduck_read_arms_opt([src::VARCHAR], filename, 'attributes', attributes)
                 ELSE error('panduck: ' || src::VARCHAR || ' needs the ' ||
                            panduck_reader_extension_for(src::VARCHAR) || ' extension') END

        -- EVERY BRANCH BELOW THIS LINE IS SPECIAL-CASED AND CANNOT CARRY AN OPTION: pdf,
        -- zim, toml/yaml, the text fallbacks and the code fallback all build their own SQL
        -- rather than going through panduck_read_arms_opt, so `attributes` would be
        -- ACCEPTED AND SILENTLY IGNORED there -- the exact shape `pages` had for the whole
        -- life of this macro, which is how it came to read as a working feature. Refusing
        -- by name costs one branch and closes the class off for every intent added later.
        -- Placed AFTER the generic branch, so a source that CAN honour the option still
        -- does; reached only when nothing above claimed the source.
        --
        -- `IS DISTINCT FROM`, NOT `<>`: `NULL <> 'default'` is NULL, not TRUE, so
        -- read_panduck_doc('two_pages.pdf', attributes := NULL) skipped this guard and
        -- returned all 45 rows -- the parameter accepted and ignored, reached by passing
        -- NULL instead of a value. Measured, and identical in shape to the `pages` guards
        -- above, which inherited it. One pattern, fixed in one place at a time.
        WHEN attributes IS DISTINCT FROM 'default'
            THEN error('panduck: attributes is not supported for ' ||
                       coalesce(panduck_resolved_format(src::VARCHAR, format), 'this source'))

        -- AND `filename` IS THE SAME DEFECT WITH A NON-NULL VALUE, found by looking for the
        -- pattern rather than for another NULL. Only the generic/plural branches (through
        -- panduck_read_arms_opt) and the pdf branch below actually PROJECT the provenance
        -- column. MEASURED: read_panduck_doc('config.toml', filename := true) returned its
        -- row with no filename column, and format := 'pandoc' with filename := true returned
        -- SEVEN columns -- the parameter accepted and silently ignored, exactly as `pages`
        -- was, and exactly as `attributes` would have been without the guard above.
        --
        -- PDF IS EXCLUDED BY NAME because it is the one special-cased branch that DOES
        -- project filename, and `IS DISTINCT FROM` rather than `<>` because
        -- panduck_resolved_format is NULL for the code fallback (no registry format and
        -- format := 'auto'), which is precisely where `NULL <> 'pdf'` would have skipped
        -- this guard and let the defect survive in the one branch nothing else covers.
        WHEN filename AND panduck_resolved_format(src::VARCHAR, format) IS DISTINCT FROM 'pdf'
            THEN error('panduck: filename is not supported for ' ||
                       coalesce(panduck_resolved_format(src::VARCHAR, format), 'this source'))

        -- PANDOC'S OWN AST, reached by format := 'pandoc' and never by extension. The
        -- generic branch above derives its reader from the file's SUFFIX, and this format
        -- deliberately claims none -- so it needs a branch of its own or it is
        -- unreachable through dispatch entirely.
        WHEN panduck_resolved_format(src::VARCHAR, format) = 'pandoc'
            THEN 'SELECT * FROM read_pandoc_blocks(' || panduck_quote(src::VARCHAR) || ')'

        -- LIST-producing branches: these unpack BY NAME.
        -- PDF delegates to read_pdf_blocks, which is where `pages` actually means
        -- something. The gate stays here so dispatch keeps its named error; the function
        -- itself cannot carry one, because a guard would have to run before the binder
        -- resolves read_pdf_elements.
        --
        -- filename is projected here too, the same way the generic branch does it --
        -- otherwise read_panduck_doc('report.pdf', filename := true) would accept the
        -- parameter and silently ignore it, which is the exact defect `pages` had before
        -- this plan, on a single row-count instead of a whole page.
        WHEN panduck_resolved_format(src::VARCHAR, format) = 'pdf'
            THEN CASE WHEN panduck_ensure_extension('pdf')
                 THEN 'SELECT *' ||
                      CASE WHEN filename THEN ', ' || panduck_quote(src::VARCHAR) || ' AS filename' ELSE '' END ||
                      ' FROM read_pdf_blocks(' || panduck_quote(src::VARCHAR) ||
                      ', pages := ' || panduck_quote(pages) || ')'
                 ELSE error('panduck: pdf needs the pdf extension') END

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
        WHEN panduck_resolved_format(src::VARCHAR, format) = 'toml'
            THEN 'SELECT ''block'' AS kind, ''metadata'' AS element_type, ' ||
                 'content AS content, 1 AS level, ' ||
                 '''toml'' AS encoding, MAP {''role'': ''document''} AS attributes, ' ||
                 '0 AS element_order FROM read_text(' || panduck_quote(src::VARCHAR) || ')'

        WHEN panduck_resolved_format(src::VARCHAR, format) = 'yaml'
            THEN 'SELECT ''block'' AS kind, ''metadata'' AS element_type, ' ||
                 'content AS content, 1 AS level, ' ||
                 '''yaml'' AS encoding, MAP {''role'': ''document''} AS attributes, ' ||
                 '0 AS element_order FROM read_text(' || panduck_quote(src::VARCHAR) || ')'

        -- ONE ZIM ARTICLE IS A DOCUMENT. zim://<archive>.zim/<path> resolves through the
        -- zim extension to the article's HTML, which then reads exactly like any other
        -- HTML -- so this branch composes the two extensions rather than adding a reader.
        --
        -- The archive ends at `.zim`, which is what separates it from the article path: an
        -- archive may itself live under directories, so splitting on the first slash would
        -- take `wiki.zim` out of `zim://books/wiki.zim/A/Page` and leave `books`.
        WHEN panduck_resolved_format(src::VARCHAR, format) = 'zim_article'
            THEN CASE WHEN panduck_ensure_extension('zim') AND panduck_ensure_extension('webbed')
                 THEN 'SELECT ' || panduck_block_cols() ||
                      ' FROM (SELECT unnest(html_to_duck_blocks(zim_get_content(' ||
                      panduck_quote(substr(src::VARCHAR, 7, position('.zim' IN src::VARCHAR) - 3)) || ', ' ||
                      panduck_quote(substr(src::VARCHAR, position('.zim' IN src::VARCHAR) + 5)) ||
                      ')::VARCHAR)) AS b)'
                 ELSE error('panduck: zim:// needs the zim and webbed extensions') END

        -- A ZIM ARCHIVE IS NOT A DOCUMENT, and this refuses it with the reason.
        --
        -- duckdb_zim emits its own schema and has no duck_block awareness, so there is
        -- nothing to route to: a .zim holds thousands of articles and "read it as a
        -- document" has no answer. The zim extension indexes the archive, searches it, and
        -- resolves a single article -- which IS a document, and reaches panduck as HTML.
        --
        -- An error naming the alternative beats both silence and a plausible wrong answer.
        --
        -- src::VARCHAR IS COALESCED (round-2 error() audit): reachable with a NULL src via
        -- read_panduck_doc(NULL, format := 'zim'), same shape as the 'data' guard above.
        WHEN panduck_resolved_format(src::VARCHAR, format) = 'zim'
            THEN error('panduck: ' || coalesce(src::VARCHAR, '<NULL>') ||
                       ' is a ZIM archive -- a corpus of many ' ||
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
        -- src::VARCHAR IS COALESCED BELOW (round-2 error() audit): this branch is reachable
        -- with a NULL src through its own first clause -- read_panduck_doc(NULL) with the
        -- default format := 'auto' satisfies (format_for(NULL) IS NULL AND
        -- nullif('auto','auto') IS NULL), both true, independent of sitting_duck being
        -- installed. Without the coalesce, a fresh session without sitting_duck would
        -- concatenate NULL and error(NULL) would silently return NULL instead of raising.
        WHEN (panduck_format_for(src::VARCHAR) IS NULL AND nullif(format, 'auto') IS NULL)
             OR panduck_resolved_format(src::VARCHAR, format) = 'code'
            THEN CASE WHEN panduck_ensure_extension('sitting_duck')
             THEN 'SELECT ' || panduck_block_cols() ||
                  ' FROM (SELECT block AS b FROM ast_to_blocks(' || panduck_quote(src::VARCHAR) || '))'
             ELSE error('panduck: no reader for ' || coalesce(src::VARCHAR, '<NULL>') ||
                        ' and sitting_duck (the fallback) is not installed') END

        -- src::VARCHAR IS COALESCED HERE TOO (round-2 error() audit): reachable with a NULL
        -- src whenever an explicit, unrecognised `format` is also given -- e.g.
        -- read_panduck_doc(NULL, format := 'bogus') skips every WHEN above (each compares
        -- against a specific resolved format that 'bogus' never matches) and lands here.
        -- panduck_resolved_format(...) itself is provably non-NULL by this point: were it
        -- NULL, that would require format = 'auto' AND format_for(src::VARCHAR) IS NULL,
        -- which is exactly the fallback branch's own first clause above -- so anything
        -- reaching this final ELSE already has a non-NULL resolved format. src::VARCHAR
        -- does not have that guarantee, hence the coalesce.
        ELSE error('panduck: ' || coalesce(src::VARCHAR, '<NULL>') || ' resolves to format ''' ||
                   panduck_resolved_format(src::VARCHAR, format) ||
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
        -- THE EXTENSION-NULL CHECK IS HOISTED (round-2 error() audit), the same fix as
        -- READ_DOC_MACRO's generic branch and panduck_read_arms: panduck_register_table_reader
        -- shares RegisterBind with the doc-reader registration and accepts an empty
        -- reader_ext the same way, so panduck_reader_extension_for(src) can answer NULL here
        -- too. Without the hoist, panduck_ensure_extension(NULL) answers NULL, the ELSE
        -- concatenates that NULL into the message, and error(NULL) silently returns NULL
        -- instead of raising. src itself is guaranteed non-NULL whenever this WHEN selects:
        -- panduck_reader_function_for(NULL) answers NULL, which fails the IS NOT NULL half.
        WHEN panduck_reader_kind_for(src) = 'table' AND panduck_reader_function_for(src) IS NOT NULL
            THEN CASE WHEN panduck_reader_extension_for(src) IS NULL
                      OR panduck_ensure_extension(panduck_reader_extension_for(src))
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

// panduck_glob(pattern) -- expand a filesystem pattern to a sorted list of paths.
//
// WHY THIS EXISTS IN C++ AT ALL, since one new primitive in a design that is otherwise
// macro SQL deserves a reason. DuckDB's `glob` is a TABLE function, and dispatch cannot
// consume a table function: a table function's arguments must be LITERALS, so
// `LATERAL read_odt_blocks(g.file)` is refused ("does not support lateral join column
// parameters") and `query((SELECT ... FROM glob(...)))` is refused ("Table function cannot
// contain subqueries"). A SCALAR returning a list can be consumed by the scalar expression
// that builds dispatch's SQL string. That is the entire reason.
//
// Sorted, because the UNION ALL built from this must be deterministic across runs and
// platforms; the file system's enumeration order is not.
//
// Empty list rather than an error when nothing matches: the primitive stays neutral and
// read_panduck_doc owns the policy, so the raise has exactly one site.
void PanduckGlobFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	auto &fs = FileSystem::GetFileSystem(context);
	UnifiedVectorFormat input;
	args.data[0].ToUnifiedFormat(args.size(), input);
	auto patterns = UnifiedVectorFormat::GetData<string_t>(input);
	for (idx_t i = 0; i < args.size(); i++) {
		auto idx = input.sel->get_index(i);
		if (!input.validity.RowIsValid(idx)) {
			result.SetValue(i, Value(LogicalType::LIST(LogicalType::VARCHAR)));
			continue;
		}
		auto files = fs.GlobFiles(patterns[idx].GetString(), FileGlobOptions::ALLOW_EMPTY);
		std::vector<std::string> paths;
		paths.reserve(files.size());
		for (auto &f : files) {
			paths.push_back(f.path);
		}
		std::sort(paths.begin(), paths.end());
		vector<Value> out;
		out.reserve(paths.size());
		for (auto &p : paths) {
			out.push_back(Value(p));
		}
		result.SetValue(i, Value::LIST(LogicalType::VARCHAR, std::move(out)));
	}
}

} // namespace

void RegisterReaderRegistry(ExtensionLoader &loader) {
	loader.RegisterFunction(
	    ScalarFunction("panduck_ensure_extension", {LogicalType::VARCHAR}, LogicalType::BOOLEAN, EnsureExtensionFun));
	loader.RegisterFunction(ScalarFunction("panduck_glob", {LogicalType::VARCHAR},
	                                       LogicalType::LIST(LogicalType::VARCHAR), PanduckGlobFun));
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
	// SPECIAL_HANDLING, and it is load-bearing rather than a formality. Under DuckDB's
	// default the executor short-circuits any row with a NULL argument to NULL without
	// calling the function at all -- so panduck_read_arms, which delegates with a NULL
	// intent because it is asking for nothing, rendered NULL, concatenated NULL through the
	// whole arm, and array_to_string dropped it: every single-path read produced no SQL.
	// This function defines its own NULL behaviour (a NULL VALUE means "nothing requested"
	// and renders '') and has to be told it may see one.
	ScalarFunction option_for("panduck_reader_option_for",
	                          {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::VARCHAR,
	                          ReaderOptionForFun);
	option_for.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	loader.RegisterFunction(option_for);

	TableFunction registry("panduck_reader_registry", {}, RegistryScan, RegistryBind, RegistryGlobalState::Init);
	loader.RegisterFunction(registry);

	auto list_of_varchar = LogicalType::LIST(LogicalType::VARCHAR);
	// The option row's shape is DECLARED here, so DuckDB casts whatever the caller wrote to
	// it before RegisterBind sees it.
	//
	// WHAT THAT CAST ACTUALLY DOES, measured rather than assumed -- an earlier version of
	// this comment claimed a missing or misspelled field became "a bind error naming the
	// field", and that is FALSE. DuckDB casts a struct BY NAME: fields written in any order
	// land correctly ({arg_type, arg, param, value, intent} registers fine), and a field
	// that is absent or misspelled is filled with NULL rather than refused. So a misspelled
	// `parm:` reaches RegisterBind as param = NULL. It is caught there -- every field is
	// required and NULL is refused -- but the cast is not what catches it, and a comment
	// that misdescribes a security check is worse than none, because the next person trusts
	// it instead of testing it.
	//
	// The by-name read in RegisterBind is therefore belt-and-braces rather than the thing
	// standing between a reordered declaration and a mis-assigned `param`; both hold today.
	auto option_list = LogicalType::LIST(LogicalType::STRUCT({{"intent", LogicalType::VARCHAR},
	                                                          {"value", LogicalType::VARCHAR},
	                                                          {"param", LogicalType::VARCHAR},
	                                                          {"arg", LogicalType::VARCHAR},
	                                                          {"arg_type", LogicalType::VARCHAR}}));
	TableFunction reg_doc("panduck_register_doc_reader", {LogicalType::VARCHAR, LogicalType::VARCHAR, list_of_varchar},
	                      RegisterScan, RegisterBind<DOC_KIND>, RegisterGlobalState::Init);
	reg_doc.named_parameters["options"] = option_list;
	loader.RegisterFunction(reg_doc);

	for (auto *tm : {&READ_DOC_MACRO, &READ_TABLE_MACRO, &DOC_TOC_MACRO, &READ_PDF_BLOCKS_MACRO, &DOC_SECTION_MACRO,
	                 &DOC_CONTAINER_MACRO}) {
		auto info = DefaultTableFunctionGenerator::CreateTableMacroInfo(*tm);
		loader.RegisterFunction(*info);
	}
	for (idx_t i = 0; SCALAR_MACROS[i].name != nullptr; i++) {
		// `definition` must outlive the call: on v2.0 the built DefaultMacro points into it.
		std::string definition;
		auto macro = panduck::MakeDefaultMacro(SCALAR_MACROS[i], definition);
		auto info = DefaultFunctionGenerator::CreateInternalMacroInfo(macro);
		loader.RegisterFunction(*info);
	}

	TableFunction reg_tbl("panduck_register_table_reader",
	                      {LogicalType::VARCHAR, LogicalType::VARCHAR, list_of_varchar}, RegisterScan,
	                      RegisterBind<TABLE_KIND>, RegisterGlobalState::Init);
	reg_tbl.named_parameters["options"] = option_list;
	loader.RegisterFunction(reg_tbl);
}

} // namespace duckdb
