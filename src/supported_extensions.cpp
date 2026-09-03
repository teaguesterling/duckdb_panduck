#include "supported_extensions.hpp"
#include "panduck_duckdb_compat.hpp"

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
static const char *const EXT_IPYNB[] = {"ipynb", nullptr};
//! NO EXTENSION, deliberately. A Pandoc AST file is a .json, and .json is already claimed
//! as a DATA format -- dispatch cannot tell an AST from any other JSON by its suffix, and
//! the overwhelming majority of .json files in the world are data. Auto-routing would
//! silently change what every existing .json query returns to serve a rare case.
static const char *const EXT_PANDOC[] = {nullptr};
static const char *const EXT_ORG[] = {"org", nullptr};
static const char *const EXT_MEDIAWIKI[] = {"wiki", "mediawiki", nullptr};
static const char *const EXT_TEXTILE[] = {"textile", nullptr};
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
     "Lists come from w:numPr, but ONLY when the numId resolves in "
     "numbering.xml -- numId 0 means NO numbering rather than numbering zero, and "
     "LibreOffice writes exactly that. Blockquotes from a quote pStyle or w:ind "
     "w:left>=720, which is how LibreOffice marks one with no style at all. Tables, "
     "IMAGES resolve <a:blip r:embed> through document.xml.rels, so the src is a path "
     "rather than a relationship id; FOOTNOTES carry their body from footnotes.xml, "
     "skipping the separator/continuation notes Word stores alongside real ones. "
     "Tables read, into the spec 5.0 native "
     "schema, with the header row taken from w:tblHeader and only from there"},
    {"odt", EXT_ODT, "read_odt_blocks", STATUS_IMPLEMENTED,
     "ZIP via miniz + content.xml via pugixml, sharing ZipContainer with docx. ODF has a "
     "dedicated text:h element with text:outline-level, so unlike RTF and DOCX there is no "
     "heading ambiguity. Lists come from text:list nesting with orderedness resolved PER "
     "LEVEL -- ODF declares all ten levels of a list style up front and routinely mixes "
     "bullet and number among them. Blockquotes from the Block_20_Text / Quotations "
     "styles, followed through style:parent-style-name. Tables, images and footnotes "
     "are read: images via xlink:href, which ODF points straight at the file, and "
     "footnotes from text:note-body rather than the note-citation marker. Tables read "
     "with header rows taken from ODF's structural "
     "table:table-header-rows rather than by promoting the first row"},
    {"epub", EXT_EPUB, "read_epub_blocks", STATUS_IMPLEMENTED,
     "ZIP via ZipContainer, then META-INF/container.xml -> the .opf package document -> the "
     "SPINE, which is the only statement of reading order. Content documents are XHTML, so "
     "pugixml reads them directly and no HTML parser is needed. Headings, paragraphs, list "
     "items, blockquotes, divs, links and images; run formatting resolves through CSS "
     "classes because LibreOffice's export emits no semantic markup at all. Tables and "
     "footnotes are not read -- pandoc's RTF path carries none either. An IMAGE is "
     "reported with NO src: RTF embeds rather than references, so there is no path to "
     "give, and {\\pict} bytes are skipped while the element is kept. Tables read: RTF has no table element, so a "
     "table is "
     "a run of \\intbl paragraphs with \\cell and \\row as terminators, accumulated "
     "rather than descended into"},
    {"pandoc", EXT_PANDOC, "read_pandoc_blocks", STATUS_IMPLEMENTED,
     "pandoc's own JSON AST, and the widest interface panduck has: `json` is what "
     "`pandoc -t json` emits, so this one reader makes ALL 43 of pandoc's input formats "
     "reachable with no per-format code, for anyone who has pandoc installed. Reached ONLY "
     "by format := 'pandoc' or by calling read_pandoc_blocks directly -- it claims no "
     "extension, because .json is already a data format and dispatch cannot tell an AST "
     "from any other JSON by its suffix. Does NOT make pandoc a dependency: the seven "
     "native readers still need no external binary, and a user piping `pandoc -t json` has "
     "made that choice explicitly, once, rather than having it made per document"},
    {"ipynb", EXT_IPYNB, "read_ipynb_blocks", STATUS_IMPLEMENTED,
     "a Jupyter notebook, parsed with the yyjson DuckDB already vendors -- so .ipynb needs "
     "NO third-party extension, unlike the toml and yaml paths. Each cell is a `div` "
     "carrying its kind in attributes['source_type']; a code cell's OUTPUTS are content and "
     "are kept, because a notebook read without them is a script. A markdown cell is held "
     "RAW with encoding='markdown' -- a DEFERRAL rather than a resting place, since that "
     "content would be duck_blocks, discharged by a post-parse helper for embedded formats "
     "rather than by markdown parsing landing here. Notebook title, authors and kernel are "
     "recovered as kind='value', which EXCEEDS pandoc: it puts the whole of a notebook's "
     "metadata into one opaque `jupyter` blob"},
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
    {"rst", EXT_RST, "read_rst_blocks", STATUS_IMPLEMENTED,
     "heading level is set by the ORDER OF FIRST APPEARANCE of an adornment character, "
     "not by which character it is -- measured, and the opposite of the usual assumption "
     "that `=` is level 1. A field list is a DEFINITION LIST and not metadata, so this is "
     "the only panduck format with no kind='value' rows. Directives are an OPEN set: "
     "code-block becomes `code`, everything else becomes `div` with the directive name in "
     "attributes['source_type'], and the body is descended into rather than dropped. "
     "Substitutions, citations, footnotes, roles and .. include:: are out of scope and "
     "DROPPED rather than left to fall through as prose"},
    {"org", EXT_ORG, "read_org_blocks", STATUS_IMPLEMENTED,
     "line-scanned, not tokenized: Org's block structure is entirely line prefixes and "
     "only inline markup is character-level. TODO keywords, tags, property drawers, "
     "timestamps, footnotes, #+INCLUDE and babel are OUT OF SCOPE -- Org's agenda and "
     "literate-programming layers, which are real Org and are not document structure. "
     "Drawers are DROPPED rather than left to fall through as prose; scoping a construct "
     "out has to mean dropped, not leaked"},
    {"mediawiki", EXT_MEDIAWIKI, "read_mediawiki_blocks", STATUS_IMPLEMENTED,
     "the scanner cannot be purely line-local, because a `|` starting a line is a table "
     "cell while a `|` inside {{...}} is an argument separator. Templates NEST, so it "
     "balances braces rather than matching them and hands the reader a whole call as one "
     "line. Templates are held RAW with their name in attributes['template_name'] -- "
     "unresolvable by construction, and MediaWiki's own parser renders an undefined one as "
     "a red link, producing no content either. Behavior switches (__TOC__) are also raw, "
     "with source_type='behavior_switch': MediaWiki CONSUMES them, so pandoc's literal "
     "Str \"__TOC__\" puts a token in the document no reader ever sees. A leading space is "
     "a `code` block, because MediaWiki renders it <pre> -- measured against "
     "maintenance/parse.php, where pandoc approximates it as inline Code inside a "
     "paragraph. No document metadata: like RST, this format has none"},
    {"textile", EXT_TEXTILE, "read_textile_blocks", STATUS_IMPLEMENTED,
     "a pure line scanner -- textile has no construct whose interior changes how a later "
     "line is read, so unlike mediawiki it needs no brace balancing. A block marker is "
     "PARSED rather than matched, since attributes sit between the name and the dot: "
     "p{color:red}. and h2(cls). are markers. `*` and `**` both map to bold and `_`/`__` "
     "both to italic -- textile distinguishes <strong> from <b> and duck_block does not. "
     "TWO DIVERGENCES from pandoc, both settled against python-textile: a bullet list "
     "adjacent to an ordered one stays a LIST here, where pandoc turns it into a paragraph "
     "containing a literal asterisk; and notextile. is honoured, where pandoc keeps the "
     "marker as prose and parses the body anyway. No document metadata, like rst and "
     "mediawiki. A leading space is NOT preformatted, unlike mediawiki"},
    {"rtf", EXT_RTF, "read_rtf_blocks", STATUS_IMPLEMENTED,
     "headings via \\outlinelevel or a {\\stylesheet} \\sN reference; paragraphs and inline "
     "bold/italic/underline/strikethrough. Lists come from \\ls and \\ilvl, with "
     "{\\listtext} -- the RENDERED bullet glyph -- suppressed as the presentation it is. "
     "Orderedness IS read, from {\\*\\listtable} resolved through {\\*\\listoverridetable} -- "
     "\\levelnfc 23 is a bullet, 255 is no number, anything else numbers the items. A list "
     "whose definition is absent stays bullet rather than guessing. Tables and "
     "footnotes are not read -- pandoc's RTF path carries none either. An IMAGE is "
     "reported with NO src: RTF embeds rather than references, so there is no path to "
     "give, and {\\pict} bytes are skipped while the element is kept. Tables read: RTF has no table element, so a "
     "table is "
     "a run of \\intbl paragraphs with \\cell and \\row as terminators, accumulated "
     "rather than descended into"},
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
                                                 vector<LogicalType> &return_types, panduck::BindNames &names) {
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
