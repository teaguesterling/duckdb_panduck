#include "pandoc_ast_map.hpp"

#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {
namespace pandoc_ast {

// The complete Block and Inline constructor set of pandoc-types 1.23 (pandoc 3.x),
// paired with the duck_block element_type each one corresponds to.
//
// element_type values are NOT invented here -- they are the vocabulary already fixed by
// duck_block_utils (src/include/block_types.hpp) and documented in its
// docs/pandoc_ast_spec.md. Panduck's readers must emit these exact strings for the
// existing round-trip to work.
const Mapping MAPPINGS[] = {
    // ---- Blocks (15 constructors) ----
    {"Header", "block", "heading", STATUS_MAPPED, "heading_level 1-6 in attributes; id from Attr"},
    {"Para", "block", "paragraph", STATUS_MAPPED, "text-only runs flatten to content; rich runs emit inline children"},
    {"Plain", "block", "paragraph", STATUS_MAPPED, "same as Para, with plain=true in attributes"},
    {"CodeBlock", "block", "code", STATUS_MAPPED, "language taken from the first Attr class"},
    {"BlockQuote", "block", "blockquote", STATUS_MAPPED, "nested blocks flattened, depth carried in level"},
    {"BulletList", "block", "list", STATUS_MAPPED, "list_type=bullet"},
    {"OrderedList", "block", "list", STATUS_MAPPED, "list_type=ordered, plus start/style/delimiter"},
    {"Table", "block", "table", STATUS_MAPPED, "full structure preserved as json encoding"},
    {"HorizontalRule", "block", "hr", STATUS_MAPPED, ""},
    {"RawBlock", "block", "raw", STATUS_MAPPED, "source format recorded in attributes"},
    {"Div", "block", "div", STATUS_MAPPED, "Attr preserved, children recursed"},
    {"LineBlock", "block", "pandoc:lineblock", STATUS_PLANNED,
     "spec'd in duck_block_utils docs but no code path; currently dropped by the else branch"},
    {"DefinitionList", "block", "pandoc:deflist", STATUS_PLANNED,
     "spec'd in duck_block_utils docs but no code path; currently dropped"},
    {"Figure", "block", "pandoc:figure", STATUS_PLANNED, "pandoc 3.0+; spec'd but no code path; currently dropped"},
    {"Null", "block", nullptr, STATUS_DROPPED, "empty block, intentionally yields no element"},

    // ---- Inlines (20 constructors) ----
    {"Str", "inline", "text", STATUS_MAPPED, "literal text"},
    {"Space", "inline", "space", STATUS_MAPPED, ""},
    {"SoftBreak", "inline", "softbreak", STATUS_MAPPED, ""},
    {"LineBreak", "inline", "linebreak", STATUS_MAPPED, ""},
    {"Strong", "inline", "bold", STATUS_MAPPED, ""},
    {"Emph", "inline", "italic", STATUS_MAPPED, ""},
    {"Strikeout", "inline", "strikethrough", STATUS_MAPPED, ""},
    {"Superscript", "inline", "superscript", STATUS_MAPPED, ""},
    {"Subscript", "inline", "subscript", STATUS_MAPPED, ""},
    {"SmallCaps", "inline", "smallcaps", STATUS_MAPPED, ""},
    {"Code", "inline", "code", STATUS_MAPPED, ""},
    {"Math", "inline", "math", STATUS_MAPPED, "InlineMath vs DisplayMath recorded in attributes"},
    {"Link", "inline", "link", STATUS_MAPPED, "href/title from Target"},
    {"Image", "inline", "image", STATUS_MAPPED, "src/title from Target"},
    {"RawInline", "inline", "raw", STATUS_MAPPED, "source format recorded in attributes"},
    {"Quoted", "inline", "quoted", STATUS_MAPPED, "SingleQuote or DoubleQuote"},
    {"Cite", "inline", "cite", STATUS_MAPPED, ""},
    {"Note", "inline", "note", STATUS_MAPPED, "footnote body as nested blocks"},
    {"Span", "inline", "span", STATUS_MAPPED, ""},
    {"Underline", "inline", "underline", STATUS_PLANNED,
     "block_types.hpp defines INLINE_UNDERLINE but pandoc_inline_convert.cpp never "
     "matches it; falls through to text with literal content \"[Underline]\""},
};

const size_t MAPPING_COUNT = sizeof(MAPPINGS) / sizeof(MAPPINGS[0]);

} // namespace pandoc_ast

namespace {

struct PandocAstMapBindData : public TableFunctionData {};

struct PandocAstMapGlobalState : public GlobalTableFunctionState {
	idx_t offset = 0;

	static unique_ptr<GlobalTableFunctionState> Init(ClientContext &, TableFunctionInitInput &) {
		return make_uniq<PandocAstMapGlobalState>();
	}
};

unique_ptr<FunctionData> PandocAstMapBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                          vector<string> &names) {
	names = {"pandoc_type", "kind", "element_type", "status", "notes"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR};
	return make_uniq<PandocAstMapBindData>();
}

void PandocAstMapScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<PandocAstMapGlobalState>();

	idx_t count = 0;
	while (state.offset < pandoc_ast::MAPPING_COUNT && count < STANDARD_VECTOR_SIZE) {
		const auto &m = pandoc_ast::MAPPINGS[state.offset];

		output.SetValue(0, count, Value(m.pandoc_type));
		output.SetValue(1, count, Value(m.kind));
		// A dropped constructor has no duck_block counterpart -- NULL, not "".
		output.SetValue(2, count, m.element_type ? Value(m.element_type) : Value(LogicalType::VARCHAR));
		output.SetValue(3, count, Value(m.status));
		output.SetValue(4, count, Value(m.notes));

		state.offset++;
		count++;
	}
	output.SetCardinality(count);
}

} // namespace

void RegisterPandocAstMapFunction(ExtensionLoader &loader) {
	TableFunction fn("panduck_pandoc_ast_map", {}, PandocAstMapScan, PandocAstMapBind, PandocAstMapGlobalState::Init);
	loader.RegisterFunction(fn);
}

} // namespace duckdb
