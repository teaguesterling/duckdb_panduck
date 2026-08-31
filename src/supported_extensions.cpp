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
    {"odt", EXT_ODT, nullptr, STATUS_PLANNED, "roadmap phase 2: same container/XML machinery as docx"},
    {"epub", EXT_EPUB, nullptr, STATUS_PLANNED, "roadmap phase 3: container.xml -> .opf spine, toc.ncx / nav.xhtml"},
    {"latex", EXT_LATEX, nullptr, STATUS_PLANNED,
     "roadmap phase 4: streaming tokenizer for macros, environments, math"},
    {"rst", EXT_RST, nullptr, STATUS_PLANNED, "roadmap phase 5"},
    {"org", EXT_ORG, nullptr, STATUS_PLANNED, "roadmap phase 5"},
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
