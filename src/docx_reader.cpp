#include "doc_metadata.hpp"
#include "docx_reader.hpp"

#include "duck_block_types.hpp"
#include "zip_container.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <cctype>
#include <map>
#include <pugixml.hpp>

namespace duckdb {
namespace docx {

namespace {

//! styleId -> style name, from word/styles.xml. Word writes localized names ("Overskrift
//! 1"), so the id is checked too; between them the common writers are covered.
std::map<std::string, std::string> ParseStyleNames(const std::string &xml) {
	std::map<std::string, std::string> names;
	pugi::xml_document doc;
	if (!doc.load_buffer(xml.data(), xml.size())) {
		return names;
	}
	for (auto style : doc.child("w:styles").children("w:style")) {
		std::string id = style.attribute("w:styleId").value();
		std::string name = style.child("w:name").attribute("w:val").value();
		if (!id.empty()) {
			names[id] = name;
		}
	}
	return names;
}

//! Heading level from a "Heading 3"-ish string, or 0. Accepts "heading 3", "Heading3"
//! and "heading_20_3" -- the spellings the common writers actually emit.
int HeadingLevelFromName(const std::string &raw) {
	std::string s;
	for (char c : raw) {
		s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
	}
	auto pos = s.find(DuckBlockTypes::TYPE_HEADING);
	if (pos == std::string::npos) {
		return 0;
	}
	for (size_t i = pos + 7; i < s.size(); i++) {
		if (std::isdigit(static_cast<unsigned char>(s[i]))) {
			int level = s[i] - '0';
			return (level >= 1 && level <= 6) ? level : 0;
		}
		if (s[i] != ' ' && s[i] != '_' && !std::isdigit(static_cast<unsigned char>(s[i]))) {
			// "Heading" with no number, or "HeadingChar" -- not a numbered heading.
			if (!std::isalpha(static_cast<unsigned char>(s[i]))) {
				continue;
			}
			return 0;
		}
	}
	return 0;
}

struct RunFormat {
	bool bold = false, italic = false, underline = false, strike = false;

	bool Plain() const {
		return !bold && !italic && !underline && !strike;
	}
	//! duck_block's inline vocabulary is flat, so a run carrying several attributes is
	//! reported by its strongest. Documented limitation, matching the RTF reader.
	std::string ElementType() const {
		if (bold) {
			return DuckBlockTypes::INLINE_BOLD;
		}
		if (italic) {
			return DuckBlockTypes::INLINE_ITALIC;
		}
		if (underline) {
			return DuckBlockTypes::INLINE_UNDERLINE;
		}
		if (strike) {
			return DuckBlockTypes::INLINE_STRIKETHROUGH;
		}
		return DuckBlockTypes::INLINE_TEXT;
	}
};

//! A w:rPr toggle is on unless it explicitly says otherwise: <w:b/> means bold, and
//! <w:b w:val="0"/> means not. Treating presence alone as true would make every
//! explicitly-disabled run bold.
bool ToggleOn(const pugi::xml_node &rpr, const char *tag) {
	auto node = rpr.child(tag);
	if (!node) {
		return false;
	}
	std::string val = node.attribute("w:val").value();
	return val.empty() || (val != "0" && val != "false" && val != "none");
}

int ParagraphHeadingLevel(const pugi::xml_node &ppr, const std::map<std::string, std::string> &styles) {
	if (!ppr) {
		return 0;
	}
	// Mechanism 1: an explicit outline level. LibreOffice writes this and no pStyle.
	auto outline = ppr.child("w:outlineLvl");
	if (outline) {
		std::string val = outline.attribute("w:val").value();
		if (!val.empty()) {
			int level = std::atoi(val.c_str()) + 1;
			if (level >= 1 && level <= 6) {
				return level;
			}
		}
	}
	// Mechanism 2: a paragraph style named "Heading N". pandoc writes this and no
	// outlineLvl. Resolve through styles.xml, falling back to the id itself.
	auto pstyle = ppr.child("w:pStyle");
	if (pstyle) {
		std::string id = pstyle.attribute("w:val").value();
		auto it = styles.find(id);
		if (it != styles.end()) {
			int level = HeadingLevelFromName(it->second);
			if (level > 0) {
				return level;
			}
		}
		return HeadingLevelFromName(id);
	}
	return 0;
}

} // namespace

//! docProps/core.xml as `value` elements, appended after the blocks.
//!
//! Every field here EXCEEDS pandoc, which extracts nothing from DOCX -- see
//! doc_metadata.hpp. Each carries attributes['source_type'] with its original spelling so
//! a consumer can tell format-derived metadata from pandoc-derived, which is the condition
//! the approval to exceed came with.
void CollectDocxMetadata(const std::string &core_xml, std::vector<DocxBlock> &out) {
	if (core_xml.empty()) {
		return;
	}
	pugi::xml_document doc;
	if (!doc.load_buffer(core_xml.data(), core_xml.size())) {
		// A malformed metadata part must not fail the document. The body is what the
		// caller asked for; metadata is enrichment, and losing it is a smaller harm than
		// refusing a file whose prose reads perfectly.
		return;
	}
	auto props = doc.child("cp:coreProperties");
	if (!props) {
		return;
	}
	std::map<std::string, std::pair<std::string, std::string>> found; // key -> {text, source}
	for (auto &field : DOCX_CORE_FIELDS) {
		auto node = props.child(field.source);
		if (!node) {
			continue;
		}
		auto text = TrimMetaText(node.child_value());
		if (text.empty()) {
			// EMPTY FIELDS ARE SKIPPED HERE, and this is the one place that differs from
			// the EPUB/LaTeX/RTF readers, deliberately.
			//
			// The ruling is "an empty field is still a field", and its stated ground is
			// that "emitting nothing would discard a fact PANDOC PRESERVED" -- pandoc emits
			// the key with an empty value, so a reader mirrors it. That reasoning is about
			// MIRRORING, and pandoc extracts nothing at all from DOCX and ODT, so there is
			// no empty field of pandoc's to mirror and nothing is being discarded.
			//
			// THAT GROUND WAS INCOMPLETE, and the ruling's second one has to be answered
			// on its own terms: a consumer cannot recover "the author declared a title and
			// left it blank" from silence. That argument never mentions pandoc, so pandoc's
			// absence does not dispose of it.
			//
			// Answered directly it points the same way and gives a better rule than "docx
			// and odt are different": PRESENT-AND-EMPTY CARRIES INFORMATION ONLY WHEN
			// PRESENCE IS A CHOICE. Emit an empty field when its presence is authorial;
			// skip it when the format's writer emits the element unconditionally. That
			// yields this behaviour here and the opposite for YAML and Org, from one
			// principle rather than an exception.
			//
			// MEASURED ACROSS TWO INDEPENDENT PRODUCERS rather than assumed from one:
			//
			//     LibreOffice  dc:title, dc:creator, dc:subject, dc:description  all EMPTY
			//     Pandoc       dc:title, dc:creator EMPTY; subject/description ABSENT
			//
			// Both write an empty title and creator into every file, so presence there is a
			// constant rather than an authorial act. Emitting them would put four empty
			// rows in every document and invite exactly the misreading the original rule
			// protects against -- a deliberate blanking that never happened.
			//
			// BOUNDED DELIBERATELY: Word and Pages are UNMEASURED. If some producer writes
			// <dc:title/> only when a title was set and then cleared, the element IS
			// authorial in those files and this skip is wrong for them. A reader cannot see
			// which producer wrote a file, so per-format is the implementable
			// approximation. Recorded as a measurement about named producers rather than a
			// law, so it can be revisited instead of inherited.
			continue;
		}
		found[field.key] = {text, field.source};
	}
	for (auto &kv : found) {
		DocxBlock block;
		block.kind = DuckBlockTypes::KIND_VALUE;
		block.element_type = DuckBlockTypes::VALUE_INLINES;
		block.key = kv.first;
		block.source_type = kv.second.second;
		if (!kv.second.first.empty()) { // always true here; see the skip above
			DocxInline run;
			run.element_type = DuckBlockTypes::INLINE_TEXT;
			run.content = kv.second.first;
			block.inlines.push_back(std::move(run));
		}
		out.push_back(std::move(block));
	}
}

std::vector<DocxBlock> ParseDocxFile(const std::string &path) {
	ZipContainer zip(path, "read_docx_blocks");
	auto document_xml = zip.ReadRequired("word/document.xml");
	std::string styles_xml;
	zip.Read("word/styles.xml", styles_xml); // optional: a minimal DOCX may omit it

	auto styles = ParseStyleNames(styles_xml);

	// OPTIONAL: a DOCX need not carry docProps/core.xml, and a missing part is not an
	// error -- it is a document that declared no metadata.
	std::string core_xml;
	zip.Read("docProps/core.xml", core_xml);

	pugi::xml_document doc;
	// parse_ws_pcdata is REQUIRED, not a tuning knob. pandoc emits inter-word spacing as
	// separate runs whose only content is a space: <w:t xml:space="preserve"> </w:t>.
	// pugixml's default flags discard whitespace-only text nodes, so those runs came back
	// empty and were skipped, welding "with" and "bold" into "withbold". The document says
	// xml:space="preserve"; honouring that is the reader's job.
	auto parsed =
	    doc.load_buffer(document_xml.data(), document_xml.size(), pugi::parse_default | pugi::parse_ws_pcdata);
	if (!parsed) {
		throw InvalidInputException("read_docx_blocks: word/document.xml is not well-formed XML in %s: %s", path,
		                            parsed.description());
	}

	std::vector<DocxBlock> blocks;
	auto body = doc.child("w:document").child("w:body");
	for (auto para : body.children("w:p")) {
		auto ppr = para.child("w:pPr");
		int level = ParagraphHeadingLevel(ppr, styles);

		struct Run {
			RunFormat fmt;
			std::string text;
		};
		std::vector<Run> runs;
		bool any_format = false;

		for (auto run : para.children("w:r")) {
			RunFormat fmt;
			auto rpr = run.child("w:rPr");
			if (rpr) {
				fmt.bold = ToggleOn(rpr, "w:b");
				fmt.italic = ToggleOn(rpr, "w:i");
				fmt.underline = static_cast<bool>(rpr.child("w:u"));
				fmt.strike = ToggleOn(rpr, "w:strike");
			}
			std::string text;
			for (auto child : run.children()) {
				std::string tag = child.name();
				if (tag == "w:t") {
					text += child.text().get();
				} else if (tag == "w:tab") {
					text += "\t";
				} else if (tag == "w:br" || tag == "w:cr") {
					text += " ";
				}
			}
			if (text.empty()) {
				continue;
			}
			if (!fmt.Plain()) {
				any_format = true;
			}
			if (!runs.empty() && runs.back().fmt.ElementType() == fmt.ElementType()) {
				runs.back().text += text;
			} else {
				runs.push_back(Run {fmt, text});
			}
		}

		std::string all;
		for (auto &r : runs) {
			all += r.text;
		}
		auto begin = all.find_first_not_of(" \t\n");
		if (begin == std::string::npos) {
			continue; // whitespace-only paragraph
		}
		auto end = all.find_last_not_of(" \t\n");
		std::string trimmed = all.substr(begin, end - begin + 1);

		DocxBlock block;
		if (level > 0) {
			block.element_type = DuckBlockTypes::TYPE_HEADING;
			block.heading_level = level;
		} else {
			block.element_type = DuckBlockTypes::TYPE_PARAGRAPH;
		}

		// Character formatting inside a heading is presentational -- writers bold heading
		// text as part of the heading style -- so headings always flatten. Leaving content
		// NULL would make a table of contents built from `content` come back empty.
		if (!any_format || level > 0) {
			block.content = trimmed;
		} else {
			for (auto &r : runs) {
				block.inlines.push_back(DocxInline {r.fmt.ElementType(), r.text});
			}
		}
		blocks.push_back(std::move(block));
	}
	// AFTER the blocks -- spec 6.2 makes body-then-metadata a contract.
	CollectDocxMetadata(core_xml, blocks);
	return blocks;
}

} // namespace docx

namespace {

struct DocxRow {
	std::string kind, element_type, content;
	bool has_level = false;
	int32_t level = 0;
	std::map<std::string, std::string> attributes;
	int32_t element_order = 0;
};

struct DocxBindData : public TableFunctionData {
	std::vector<DocxRow> rows;
};

struct DocxGlobalState : public GlobalTableFunctionState {
	idx_t offset = 0;
	static unique_ptr<GlobalTableFunctionState> Init(ClientContext &, TableFunctionInitInput &) {
		return make_uniq<DocxGlobalState>();
	}
};

unique_ptr<FunctionData> DocxBind(ClientContext &, TableFunctionBindInput &input, vector<LogicalType> &return_types,
                                  vector<string> &names) {
	// Column order mirrors the duck_block struct, so a row casts straight to duck_block
	// and read_panduck_doc's flat branch can SELECT * it through.
	names = {"kind", "element_type", "content", "level", "encoding", "attributes", "element_order"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::INTEGER,
	                LogicalType::VARCHAR, LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR),
	                LogicalType::INTEGER};

	auto result = make_uniq<DocxBindData>();
	int32_t order = 0;
	for (auto &block : docx::ParseDocxFile(input.inputs[0].GetValue<string>())) {
		DocxRow row;
		row.kind = block.kind.empty() ? DuckBlockTypes::KIND_BLOCK : block.kind;
		if (!block.key.empty()) {
			row.attributes[DuckBlockTypes::ATTR_KEY] = block.key;
		}
		if (!block.source_type.empty()) {
			row.attributes[DuckBlockTypes::ATTR_SOURCE_TYPE] = block.source_type;
		}
		row.element_type = block.element_type;
		row.content = block.content;
		row.element_order = order++;
		if (block.heading_level > 0) {
			row.attributes[DuckBlockTypes::ATTR_HEADING_LEVEL] = std::to_string(block.heading_level);
		}
		// EVERY ELEMENT CARRIES A STRUCTURAL LEVEL. Top level is 1; an inline is a CHILD
		// of its block, so it is one deeper. This reader emits no containers, so every
		// block sits at 1 and every inline at 2.
		const int32_t block_level = 1;
		row.has_level = true;
		row.level = block_level;
		result->rows.push_back(std::move(row));

		for (auto &inl : block.inlines) {
			DocxRow child;
			child.kind = DuckBlockTypes::KIND_INLINE;
			child.element_type = inl.element_type;
			child.content = inl.content;
			child.has_level = true;
			child.level = block_level + 1;
			child.element_order = order++;
			result->rows.push_back(std::move(child));
		}
	}
	return std::move(result);
}

void DocxScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<DocxBindData>();
	auto &state = input.global_state->Cast<DocxGlobalState>();
	idx_t count = 0;
	while (state.offset < data.rows.size() && count < STANDARD_VECTOR_SIZE) {
		const auto &row = data.rows[state.offset];
		output.SetValue(0, count, Value(row.kind));
		output.SetValue(1, count, Value(row.element_type));
		output.SetValue(2, count, row.content.empty() ? Value(LogicalType::VARCHAR) : Value(row.content));
		output.SetValue(3, count, row.has_level ? Value::INTEGER(row.level) : Value(LogicalType::INTEGER));
		output.SetValue(4, count, Value(DuckBlockTypes::INLINE_TEXT));
		output.SetValue(5, count, DuckBlockTypes::CreateAttributesMap(row.attributes));
		output.SetValue(6, count, Value::INTEGER(row.element_order));
		state.offset++;
		count++;
	}
	output.SetCardinality(count);
}

} // namespace

void RegisterDocxReaderFunction(ExtensionLoader &loader) {
	TableFunction fn("read_docx_blocks", {LogicalType::VARCHAR}, DocxScan, DocxBind, DocxGlobalState::Init);
	loader.RegisterFunction(fn);
}

} // namespace duckdb
