#include "doc_metadata.hpp"
#include "odt_reader.hpp"

#include "duck_block_types.hpp"
#include "zip_container.hpp"

#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <functional>
#include <map>
#include <pugixml.hpp>

namespace duckdb {
namespace odt {

namespace {

struct RunFormat {
	bool bold = false, italic = false, underline = false, strike = false;

	bool Plain() const {
		return !bold && !italic && !underline && !strike;
	}
	//! duck_block's inline vocabulary is flat, so a run carrying several attributes is
	//! reported by its strongest. Same documented limitation as the RTF and DOCX readers.
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

//! ODT resolves inline formatting indirectly: a run names a style
//! (<text:span text:style-name="T1">) and the style carries the properties. This is the
//! ODF analogue of DOCX's styles.xml, except the styles that matter live in content.xml's
//! own <office:automatic-styles>, generated per document.
std::map<std::string, RunFormat> ParseAutomaticStyles(const pugi::xml_node &root) {
	std::map<std::string, RunFormat> styles;
	for (auto style : root.child("office:automatic-styles").children("style:style")) {
		std::string name = style.attribute("style:name").value();
		if (name.empty()) {
			continue;
		}
		auto props = style.child("style:text-properties");
		if (!props) {
			continue;
		}
		RunFormat fmt;
		fmt.bold = std::string(props.attribute("fo:font-weight").value()) == "bold";
		fmt.italic = std::string(props.attribute("fo:font-style").value()) == "italic";
		// A line-through or underline is present unless it says "none" -- ODF spells the
		// style rather than toggling a flag, so absence and "none" both mean off.
		std::string strike_style = props.attribute("style:text-line-through-style").value();
		fmt.strike = !strike_style.empty() && strike_style != "none";
		std::string underline_style = props.attribute("style:text-underline-style").value();
		fmt.underline = !underline_style.empty() && underline_style != "none";
		styles[name] = fmt;
	}
	return styles;
}

struct Run {
	RunFormat fmt;
	std::string text;
};

//! Walk the mixed content of a text:p / text:h in document order. ODT interleaves raw
//! text with elements, so the order of children is the order of the sentence.
void CollectRuns(const pugi::xml_node &node, const RunFormat &inherited, const std::map<std::string, RunFormat> &styles,
                 std::vector<Run> &runs) {
	for (auto child : node.children()) {
		if (child.type() == pugi::node_pcdata) {
			std::string text = child.value();
			if (text.empty()) {
				continue;
			}
			if (!runs.empty() && runs.back().fmt.ElementType() == inherited.ElementType()) {
				runs.back().text += text;
			} else {
				runs.push_back(Run {inherited, text});
			}
			continue;
		}
		std::string tag = child.name();
		if (tag == "text:span") {
			auto it = styles.find(child.attribute("text:style-name").value());
			CollectRuns(child, it != styles.end() ? it->second : inherited, styles, runs);
		} else if (tag == "text:a") {
			// A link's own text still belongs to the sentence; the href is not modelled
			// yet, so the anchor text is collected with the surrounding formatting.
			CollectRuns(child, inherited, styles, runs);
		} else if (tag == "text:s") {
			// <text:s text:c="3"/> is a run of spaces; ODF collapses literal ones.
			int count = child.attribute("text:c").as_int(1);
			std::string spaces(count < 1 ? 1 : count, ' ');
			if (!runs.empty() && runs.back().fmt.ElementType() == inherited.ElementType()) {
				runs.back().text += spaces;
			} else {
				runs.push_back(Run {inherited, spaces});
			}
		} else if (tag == "text:tab") {
			runs.push_back(Run {inherited, "\t"});
		} else if (tag == "text:line-break") {
			runs.push_back(Run {inherited, " "});
		}
		// text:bookmark-start / -end, text:sequence-decls and friends carry no content.
	}
}

} // namespace

//! meta.xml as `value` elements, appended after the blocks.
//!
//! Every field EXCEEDS pandoc, which extracts nothing from ODT -- see doc_metadata.hpp --
//! so each carries attributes['source_type'] with its original spelling, marking it as
//! format-derived rather than pandoc-derived.
void CollectOdtMetadata(const std::string &meta_xml, std::vector<OdtBlock> &out) {
	if (meta_xml.empty()) {
		return;
	}
	pugi::xml_document doc;
	if (!doc.load_buffer(meta_xml.data(), meta_xml.size())) {
		// A malformed metadata part must not fail the document -- the body is what the
		// caller asked for.
		return;
	}
	auto meta = doc.child("office:document-meta").child("office:meta");
	if (!meta) {
		return;
	}
	std::map<std::string, std::pair<std::string, std::string>> found; // key -> {text, source}
	for (auto &field : ODT_META_FIELDS) {
		auto node = meta.child(field.source);
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
	if (!found.count("date")) {
		// meta:creation-date ONLY when dc:date is absent. Both carry the same instant in
		// every fixture measured, and two sources feeding one key is how the two drift
		// apart -- so the Dublin Core spelling wins and this is the fallback, not a peer.
		auto created = meta.child("meta:creation-date");
		auto created_text = created ? TrimMetaText(created.child_value()) : std::string();
		if (!created_text.empty()) {
			found["date"] = {created_text, "meta:creation-date"};
		}
	}
	for (auto &kv : found) {
		OdtBlock block;
		block.kind = DuckBlockTypes::KIND_VALUE;
		block.element_type = DuckBlockTypes::VALUE_INLINES;
		block.key = kv.first;
		block.source_type = kv.second.second;
		if (!kv.second.first.empty()) { // always true here; see the skip above
			OdtInline run;
			run.element_type = DuckBlockTypes::INLINE_TEXT;
			run.content = kv.second.first;
			block.inlines.push_back(std::move(run));
		}
		out.push_back(std::move(block));
	}
}

std::vector<OdtBlock> ParseOdtFile(const std::string &path) {
	ZipContainer zip(path, "read_odt_blocks");
	auto content_xml = zip.ReadRequired("content.xml");
	// OPTIONAL: a missing meta.xml is a document that declared no metadata, not an error.
	std::string meta_xml;
	zip.Read("meta.xml", meta_xml);

	pugi::xml_document doc;
	// parse_ws_pcdata for the same reason DOCX needs it: a whitespace-only text node
	// between two spans is real inter-word spacing, and the default flags discard it.
	auto parsed = doc.load_buffer(content_xml.data(), content_xml.size(), pugi::parse_default | pugi::parse_ws_pcdata);
	if (!parsed) {
		throw InvalidInputException("read_odt_blocks: content.xml is not well-formed XML in %s: %s", path,
		                            parsed.description());
	}

	auto root = doc.child("office:document-content");
	auto styles = ParseAutomaticStyles(root);
	auto body = root.child("office:body").child("office:text");

	// ODF nests list content: text:list > text:list-item > text:p. Skipping text:list
	// wholesale loses the TEXT, not just the list structure -- the differential validator
	// caught exactly that, with pandoc reporting "bullet one bullet two" where panduck
	// reported nothing. Flattening the paragraphs out preserves every word while leaving
	// list structure unmodelled, which is the same position RTF and DOCX are in. Losing
	// structure is a gap; losing text is a bug.
	std::vector<pugi::xml_node> paragraphs;
	std::function<void(const pugi::xml_node &)> collect = [&](const pugi::xml_node &parent) {
		for (auto node : parent.children()) {
			std::string tag = node.name();
			if (tag == "text:h" || tag == "text:p") {
				paragraphs.push_back(node);
			} else if (tag == "text:list" || tag == "text:list-item" || tag == "text:list-header") {
				collect(node);
			}
			// tables, sequence declarations and drawing frames are not read yet.
		}
	};
	collect(body);

	std::vector<OdtBlock> blocks;
	for (auto node : paragraphs) {
		std::string tag = node.name();
		bool is_heading = (tag == "text:h");

		std::vector<Run> runs;
		CollectRuns(node, RunFormat(), styles, runs);

		std::string all;
		bool any_format = false;
		for (auto &r : runs) {
			all += r.text;
			if (!r.fmt.Plain()) {
				any_format = true;
			}
		}
		auto begin = all.find_first_not_of(" \t\n");
		if (begin == std::string::npos) {
			continue; // whitespace-only
		}
		auto end = all.find_last_not_of(" \t\n");
		std::string trimmed = all.substr(begin, end - begin + 1);

		OdtBlock block;
		if (is_heading) {
			int level = node.attribute("text:outline-level").as_int(1);
			block.element_type = DuckBlockTypes::TYPE_HEADING;
			block.heading_level = (level >= 1 && level <= 6) ? level : 1;
		} else {
			block.element_type = DuckBlockTypes::TYPE_PARAGRAPH;
		}

		// Headings always flatten -- see the RTF and DOCX readers for why.
		if (!any_format || is_heading) {
			block.content = trimmed;
		} else {
			for (auto &r : runs) {
				block.inlines.push_back(OdtInline {r.fmt.ElementType(), r.text});
			}
		}
		blocks.push_back(std::move(block));
	}
	// AFTER the blocks -- spec 6.2 makes body-then-metadata a contract.
	CollectOdtMetadata(meta_xml, blocks);
	return blocks;
}

} // namespace odt

namespace {

struct OdtRow {
	std::string kind, element_type, content;
	bool has_level = false;
	int32_t level = 0;
	std::map<std::string, std::string> attributes;
	int32_t element_order = 0;
};

struct OdtBindData : public TableFunctionData {
	std::vector<OdtRow> rows;
};

struct OdtGlobalState : public GlobalTableFunctionState {
	idx_t offset = 0;
	static unique_ptr<GlobalTableFunctionState> Init(ClientContext &, TableFunctionInitInput &) {
		return make_uniq<OdtGlobalState>();
	}
};

unique_ptr<FunctionData> OdtBind(ClientContext &, TableFunctionBindInput &input, vector<LogicalType> &return_types,
                                 vector<string> &names) {
	names = {"kind", "element_type", "content", "level", "encoding", "attributes", "element_order"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::INTEGER,
	                LogicalType::VARCHAR, LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR),
	                LogicalType::INTEGER};

	auto result = make_uniq<OdtBindData>();
	int32_t order = 0;
	for (auto &block : odt::ParseOdtFile(input.inputs[0].GetValue<string>())) {
		OdtRow row;
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
			OdtRow child;
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

void OdtScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<OdtBindData>();
	auto &state = input.global_state->Cast<OdtGlobalState>();
	idx_t count = 0;
	while (state.offset < data.rows.size() && count < STANDARD_VECTOR_SIZE) {
		const auto &row = data.rows[state.offset];
		output.SetValue(0, count, Value(row.kind));
		output.SetValue(1, count, Value(row.element_type));
		output.SetValue(2, count, row.content.empty() ? Value(LogicalType::VARCHAR) : Value(row.content));
		output.SetValue(3, count, row.has_level ? Value::INTEGER(row.level) : Value(LogicalType::INTEGER));
		output.SetValue(4, count, Value("text"));
		output.SetValue(5, count, DuckBlockTypes::CreateAttributesMap(row.attributes));
		output.SetValue(6, count, Value::INTEGER(row.element_order));
		state.offset++;
		count++;
	}
	output.SetCardinality(count);
}

} // namespace

void RegisterOdtReaderFunction(ExtensionLoader &loader) {
	TableFunction fn("read_odt_blocks", {LogicalType::VARCHAR}, OdtScan, OdtBind, OdtGlobalState::Init);
	loader.RegisterFunction(fn);
}

} // namespace duckdb
