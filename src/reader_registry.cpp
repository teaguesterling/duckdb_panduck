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
    CASE WHEN panduck_ensure_extension('duck_block_utils')
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
    {DEFAULT_SCHEMA,
     "panduck_source_list",
     {"src", nullptr},
     {{nullptr, nullptr}},
     "CASE WHEN typeof(src) LIKE '%[]' "
     "     THEN flatten(list_transform(src::VARCHAR[], "
     "                  lambda p: CASE WHEN panduck_is_glob(p) THEN panduck_glob(p) ELSE [p] END)) "
     "     WHEN panduck_is_glob(src::VARCHAR) THEN panduck_glob(src::VARCHAR) "
     "     ELSE [src::VARCHAR] END"},

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
     "    WHEN output_format = 'md' AND panduck_ensure_extension('markdown')"
     "      THEN 'SELECT duck_blocks_to_md(panduck_read_blocks(' || panduck_quote(src) ||"
     "           ', format := ' || panduck_quote(format) || ')) AS r'"
     "    WHEN output_format = 'html' AND panduck_ensure_extension('webbed')"
     "      THEN 'SELECT duck_blocks_to_html(panduck_read_blocks(' || panduck_quote(src) ||"
     "           ', format := ' || panduck_quote(format) || ')) AS r'"
     "    WHEN output_format = 'text' AND panduck_ensure_extension('duck_block_utils')"
     "      THEN 'SELECT duck_blocks_to_text(panduck_read_blocks(' || panduck_quote(src) ||"
     "           ', format := ' || panduck_quote(format) || ')) AS r'"
     "    ELSE error('panduck: doc_render supports md, html and text; ' || output_format ||"
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
const DefaultTableMacro DOC_CONTAINER_MACRO = {
    DEFAULT_SCHEMA,
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

const DefaultTableMacro DOC_SECTION_MACRO = {
    DEFAULT_SCHEMA,
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

const DefaultTableMacro READ_PDF_BLOCKS_MACRO = {
    DEFAULT_SCHEMA,
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
        first_page := CASE WHEN pages = '' THEN 1
                           WHEN regexp_matches(pages, '^[0-9]+(-[0-9]+)?$')
                               THEN split_part(pages, '-', 1)::INTEGER
                           ELSE error('panduck: pages must be N or N-M, got ' || pages) END,
        last_page := CASE WHEN pages = '' THEN 2147483647
                          WHEN regexp_matches(pages, '^[0-9]+$') THEN pages::INTEGER
                          WHEN regexp_matches(pages, '^[0-9]+-[0-9]+$')
                              THEN split_part(pages, '-', 2)::INTEGER
                          ELSE error('panduck: pages must be N or N-M, got ' || pages) END)
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

const DefaultTableMacro READ_DOC_MACRO = {DEFAULT_SCHEMA,
                                          "read_panduck_doc",
                                          {"src", nullptr},
                                          {{"format", "'auto'"}, {"pages", "''"}, {nullptr, nullptr}},
                                          R"SQL(
SELECT * FROM query(
    CASE
        -- `pages` WAS ACCEPTED AND IGNORED BY EVERY FORMAT. It has been declared here
        -- since this macro was written and was never referenced in the body, so
        -- pages := '2' and pages := 'utter nonsense' both returned the whole document.
        -- PDF now honours it; nothing else has pages to honour, and saying so is the
        -- point -- silently ignoring a parameter is how it came to read as a feature.
        WHEN pages <> '' AND panduck_resolved_format(src, format) <> 'pdf'
            THEN error('panduck: pages applies only to paginated formats (pdf); ' ||
                       panduck_resolved_format(src, format) || ' has no pages')

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
        -- PDF delegates to read_pdf_blocks, which is where `pages` actually means
        -- something. The gate stays here so dispatch keeps its named error; the function
        -- itself cannot carry one, because a guard would have to run before the binder
        -- resolves read_pdf_elements.
        WHEN panduck_resolved_format(src, format) = 'pdf'
            THEN CASE WHEN panduck_ensure_extension('pdf')
                 THEN 'SELECT * FROM read_pdf_blocks(' || panduck_quote(src) ||
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

        -- ONE ZIM ARTICLE IS A DOCUMENT. zim://<archive>.zim/<path> resolves through the
        -- zim extension to the article's HTML, which then reads exactly like any other
        -- HTML -- so this branch composes the two extensions rather than adding a reader.
        --
        -- The archive ends at `.zim`, which is what separates it from the article path: an
        -- archive may itself live under directories, so splitting on the first slash would
        -- take `wiki.zim` out of `zim://books/wiki.zim/A/Page` and leave `books`.
        WHEN panduck_resolved_format(src, format) = 'zim_article'
            THEN CASE WHEN panduck_ensure_extension('zim') AND panduck_ensure_extension('webbed')
                 THEN 'SELECT ' || panduck_block_cols() ||
                      ' FROM (SELECT unnest(html_to_duck_blocks(zim_get_content(' ||
                      panduck_quote(substr(src, 7, position('.zim' IN src) - 3)) || ', ' ||
                      panduck_quote(substr(src, position('.zim' IN src) + 5)) ||
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

	TableFunction registry("panduck_reader_registry", {}, RegistryScan, RegistryBind, RegistryGlobalState::Init);
	loader.RegisterFunction(registry);

	auto list_of_varchar = LogicalType::LIST(LogicalType::VARCHAR);
	TableFunction reg_doc("panduck_register_doc_reader", {LogicalType::VARCHAR, LogicalType::VARCHAR, list_of_varchar},
	                      RegisterScan, RegisterBind<DOC_KIND>, RegisterGlobalState::Init);
	loader.RegisterFunction(reg_doc);

	for (auto *tm : {&READ_DOC_MACRO, &READ_TABLE_MACRO, &DOC_TOC_MACRO, &READ_PDF_BLOCKS_MACRO,
	                 &DOC_SECTION_MACRO, &DOC_CONTAINER_MACRO}) {
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
	loader.RegisterFunction(reg_tbl);
}

} // namespace duckdb
