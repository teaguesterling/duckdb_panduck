#include "supported_extensions.hpp"

#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {
namespace readers {

// Extension lists, nullptr-terminated. Lowercase, no leading dot -- see the convention
// note in supported_extensions.hpp.
static const char *const EXT_DOCX[] = {"docx", nullptr};
static const char *const EXT_ODT[] = {"odt", nullptr};
static const char *const EXT_EPUB[] = {"epub", nullptr};
static const char *const EXT_LATEX[] = {"tex", "latex", nullptr};
static const char *const EXT_RST[] = {"rst", nullptr};
static const char *const EXT_ORG[] = {"org", nullptr};
static const char *const EXT_MEDIAWIKI[] = {"wiki", "mediawiki", nullptr};
static const char *const EXT_RTF[] = {"rtf", nullptr};

// The formats panduck claims. `format` is pandoc's own name for it, so a reader added
// here lines up with the `--from` value a human would reach for.
//
// WHAT IS DELIBERATELY ABSENT: markdown (.md) and HTML (.html/.htm). Pandoc reads both,
// and panduck could. But `duckdb_markdown` and `duckdb_webbed` already read them into
// duck_block, and this table is a *self-description*, not a routing table -- a row here
// is panduck asserting "I read this", which a dispatcher is entitled to act on. Two
// registries claiming .md is exactly the ambiguity a derived dispatcher is supposed to
// eliminate. When panduck later owns path->blocks routing (`panduck_read()`), it will
// route .md onward to `duckdb_markdown` by reading *that* extension's self-description,
// not by hardcoding a claim about it here. Second-hand knowledge about another
// extension's formats is precisely what went stale in duck_block_utils'
// doc_default_extension_mappings(), and panduck has no test that could catch it going
// stale here either.
const FormatReader FORMATS[] = {
    {"docx", EXT_DOCX, "read_docx_blocks", STATUS_IMPLEMENTED,
     "ZIP via miniz + word/document.xml via pugixml. Headings from BOTH w:outlineLvl and a "
     "w:pStyle resolved through styles.xml -- pandoc writes one, LibreOffice the other. "
     "Tables, lists, images and footnotes are not read yet"},
    {"odt", EXT_ODT, "read_odt_blocks", STATUS_IMPLEMENTED,
     "ZIP via miniz + content.xml via pugixml, sharing ZipContainer with docx. ODF has a "
     "dedicated text:h element with text:outline-level, so unlike RTF and DOCX there is no "
     "heading ambiguity. Lists, tables, images and footnotes are not read yet"},
    {"epub", EXT_EPUB, "read_epub_blocks", STATUS_IMPLEMENTED,
     "ZIP via ZipContainer, then META-INF/container.xml -> the .opf package document -> the "
     "SPINE, which is the only statement of reading order. Content documents are XHTML, so "
     "pugixml reads them directly and no HTML parser is needed. Headings, paragraphs, list "
     "items, blockquotes, divs, links and images; run formatting resolves through CSS "
     "classes because LibreOffice's export emits no semantic markup at all. Tables and "
     "footnotes are not read yet"},
    {"latex", EXT_LATEX, "read_latex_blocks", STATUS_IMPLEMENTED,
     "a tokenizer plus a macro DISPOSITION table -- semantic, transparent, dropped, text -- "
     "rather than a grammar, because LaTeX has no document model to parse against. "
     "TRANSPARENT is what makes a pandoc file and a handwritten one read alike: pandoc "
     "buries every \\section inside \\hypertarget{id}{%...}, and descending into a content "
     "argument reads that without a pandoc-specific rule. Sectioning (ranked against the "
     "\\documentclass), paragraphs, lists (bullet and ordered, tight/loose), block quotes, "
     "verbatim, inline formatting with genuine nesting, links, images and footnotes. Math "
     "($..$, $$..$$, \\(..\\), \\[..\\]) is read OPAQUE -- the TeX between the shifts is kept "
     "verbatim as content, never parsed or macro-expanded. Tables and \\newcommand "
     "expansion are not read yet"},
    {"rst", EXT_RST, nullptr, STATUS_PLANNED, "roadmap phase 5"},
    {"org", EXT_ORG, "read_org_blocks", STATUS_IMPLEMENTED,
     "line-scanned, not tokenized: Org's block structure is entirely line prefixes and "
     "only inline markup is character-level. TODO keywords, tags, property drawers, "
     "timestamps, footnotes, #+INCLUDE and babel are OUT OF SCOPE -- Org's agenda and "
     "literate-programming layers, which are real Org and are not document structure. "
     "Drawers are DROPPED rather than left to fall through as prose; scoping a construct "
     "out has to mean dropped, not leaked"},
    {"mediawiki", EXT_MEDIAWIKI, nullptr, STATUS_PLANNED, "roadmap phase 5"},
    {"rtf", EXT_RTF, "read_rtf_blocks", STATUS_IMPLEMENTED,
     "headings via \\outlinelevel or a {\\stylesheet} \\sN reference; paragraphs and inline "
     "bold/italic/underline/strikethrough. Lists, tables and footnotes are not read yet"},
};

const size_t FORMAT_COUNT = sizeof(FORMATS) / sizeof(FORMATS[0]);

} // namespace readers

namespace {

struct SupportedExtensionsBindData : public TableFunctionData {};

struct SupportedExtensionsGlobalState : public GlobalTableFunctionState {
	idx_t offset = 0;

	static unique_ptr<GlobalTableFunctionState> Init(ClientContext &, TableFunctionInitInput &) {
		return make_uniq<SupportedExtensionsGlobalState>();
	}
};

unique_ptr<FunctionData> SupportedExtensionsBind(ClientContext &, TableFunctionBindInput &,
                                                 vector<LogicalType> &return_types, vector<string> &names) {
	names = {"format", "extensions", "reader", "status", "notes"};
	return_types = {LogicalType::VARCHAR, LogicalType::LIST(LogicalType::VARCHAR), LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::VARCHAR};
	return make_uniq<SupportedExtensionsBindData>();
}

void SupportedExtensionsScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<SupportedExtensionsGlobalState>();

	idx_t count = 0;
	while (state.offset < readers::FORMAT_COUNT && count < STANDARD_VECTOR_SIZE) {
		const auto &f = readers::FORMATS[state.offset];

		vector<Value> extensions;
		for (auto ext = f.extensions; *ext; ext++) {
			extensions.push_back(Value(*ext));
		}

		output.SetValue(0, count, Value(f.format));
		output.SetValue(1, count, Value::LIST(LogicalType::VARCHAR, extensions));
		// A format with no reader in this build has no function to name -- NULL, not "".
		output.SetValue(2, count, f.reader ? Value(f.reader) : Value(LogicalType::VARCHAR));
		output.SetValue(3, count, Value(f.status));
		output.SetValue(4, count, Value(f.notes));

		state.offset++;
		count++;
	}
	output.SetCardinality(count);
}

} // namespace

void RegisterSupportedExtensionsFunction(ExtensionLoader &loader) {
	TableFunction fn("panduck_supported_extensions", {}, SupportedExtensionsScan, SupportedExtensionsBind,
	                 SupportedExtensionsGlobalState::Init);
	loader.RegisterFunction(fn);
}

} // namespace duckdb
