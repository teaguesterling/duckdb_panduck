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

std::vector<OdtBlock> ParseOdtFile(const std::string &path) {
	ZipContainer zip(path, "read_odt_blocks");
	auto content_xml = zip.ReadRequired("content.xml");

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
		row.kind = DuckBlockTypes::KIND_BLOCK;
		row.element_type = block.element_type;
		row.content = block.content;
		row.element_order = order++;
		if (block.heading_level > 0) {
			row.attributes["heading_level"] = std::to_string(block.heading_level);
		}
		result->rows.push_back(std::move(row));

		for (auto &inl : block.inlines) {
			OdtRow child;
			child.kind = DuckBlockTypes::KIND_INLINE;
			child.element_type = inl.element_type;
			child.content = inl.content;
			child.has_level = true;
			child.level = 1;
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
