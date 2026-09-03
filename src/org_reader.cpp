#include "org_reader.hpp"

#include "block_json.hpp"
#include "duck_block_types.hpp"
#include "org_scanner.hpp"

#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <algorithm>
#include <fstream>
#include <map>

namespace duckdb {
namespace org {
namespace {

//! Org's inline markers, each mapping to a duck_block inline type.
//!
//! `=code=` and `~verbatim~` BOTH become `code`. Pandoc distinguishes them by a class --
//! `=code=` carries ["verbatim"] and `~verbatim~` carries none, the opposite way round
//! from the names -- and duck_block's `code` inline has no class field. Collapsing them is
//! deliberate and declared; the surprising direction is recorded because it is exactly
//! what a later reader would "correct" the wrong way.
struct InlineMarker {
	char open;
	const char *element_type;
};
const InlineMarker MARKERS[] = {
    {'*', DuckBlockTypes::INLINE_BOLD},      {'/', DuckBlockTypes::INLINE_ITALIC},
    {'_', DuckBlockTypes::INLINE_UNDERLINE}, {'=', DuckBlockTypes::INLINE_CODE},
    {'~', DuckBlockTypes::INLINE_CODE},      {'+', DuckBlockTypes::INLINE_STRIKETHROUGH},
};

const char *MarkerType(char c) {
	for (auto &m : MARKERS) {
		if (m.open == c) {
			return m.element_type;
		}
	}
	return nullptr;
}

//! A marker only opens when it is at a word boundary and its content is non-empty. Without
//! that, `a * b` and `2 + 2` become emphasis, and arithmetic in a paragraph turns into
//! markup -- the most common false positive in every lightweight markup reader.
bool OpensHere(const std::string &s, size_t i) {
	if (i > 0 && !isspace(static_cast<unsigned char>(s[i - 1])) && s[i - 1] != '(' && s[i - 1] != '[') {
		return false;
	}
	return i + 1 < s.size() && !isspace(static_cast<unsigned char>(s[i + 1]));
}

void PushText(std::vector<OrgInline> &out, const std::string &text, int level) {
	if (text.empty()) {
		return;
	}
	OrgInline run;
	run.element_type = DuckBlockTypes::INLINE_TEXT;
	run.content = text;
	run.level = level;
	out.push_back(std::move(run));
}

//! Split a line's text into inline runs. A FLAT scan: Org's emphasis markers do not nest
//! in practice and pandoc does not nest them either, so a stack would model something the
//! format does not have.
void ParseInlines(const std::string &s, int level, std::vector<OrgInline> &out) {
	std::string plain;
	size_t i = 0;
	while (i < s.size()) {
		// `[[url][label]]` and `[[url]]` -- checked first, because a link's target can
		// contain any of the emphasis characters and must not be scanned for them.
		if (s.compare(i, 2, "[[") == 0) {
			auto close = s.find("]]", i + 2);
			if (close != std::string::npos) {
				auto body = s.substr(i + 2, close - i - 2);
				auto sep = body.find("][");
				OrgInline link;
				link.element_type = DuckBlockTypes::INLINE_LINK;
				link.href = sep == std::string::npos ? body : body.substr(0, sep);
				link.content = sep == std::string::npos ? body : body.substr(sep + 2);
				link.level = level;
				PushText(out, plain, level);
				plain.clear();
				out.push_back(std::move(link));
				i = close + 2;
				continue;
			}
		}
		const char *type = MarkerType(s[i]);
		if (type && OpensHere(s, i)) {
			auto close = s.find(s[i], i + 1);
			// A closing marker must not be preceded by a space, or `* a *` in prose would
			// close a run the author never opened.
			while (close != std::string::npos && close > i + 1 && isspace(static_cast<unsigned char>(s[close - 1]))) {
				close = s.find(s[i], close + 1);
			}
			if (close != std::string::npos && close > i + 1) {
				PushText(out, plain, level);
				plain.clear();
				OrgInline run;
				run.element_type = type;
				run.content = s.substr(i + 1, close - i - 1);
				run.level = level;
				out.push_back(std::move(run));
				i = close + 1;
				continue;
			}
		}
		plain.push_back(s[i]);
		i++;
	}
	PushText(out, plain, level);
}

//! Split `| a | b |` into its cells.
std::vector<std::string> TableCells(const std::string &row) {
	std::vector<std::string> cells;
	size_t i = row.find('|');
	if (i == std::string::npos) {
		return cells;
	}
	i++;
	std::string cur;
	for (; i < row.size(); i++) {
		if (row[i] == '|') {
			size_t b = cur.find_first_not_of(" \t");
			size_t e = cur.find_last_not_of(" \t");
			cells.push_back(b == std::string::npos ? std::string() : cur.substr(b, e - b + 1));
			cur.clear();
			continue;
		}
		cur.push_back(row[i]);
	}
	return cells;
}

class Builder {
public:
	std::vector<OrgBlock> Build(const std::string &src) {
		auto lines = ScanOrg(src);
		for (size_t i = 0; i < lines.size(); i++) {
			auto &line = lines[i];
			switch (line.kind) {
			case LineKind::BLANK:
				FlushParagraph();
				CloseLists(-1);
				continue;
			case LineKind::COMMENT:
				// Pandoc emits nothing for a comment; it is not content.
				continue;
			case LineKind::KEYWORD:
				FlushParagraph();
				Keyword(line);
				continue;
			case LineKind::HEADING:
				FlushParagraph();
				CloseLists(-1);
				Heading(line);
				continue;
			case LineKind::HRULE:
				FlushParagraph();
				CloseLists(-1);
				Simple(DuckBlockTypes::TYPE_HR);
				continue;
			case LineKind::BLOCK_BEGIN:
				FlushParagraph();
				CloseLists(-1);
				Block(lines, i);
				continue;
			case LineKind::TABLE_ROW:
			case LineKind::TABLE_RULE:
				FlushParagraph();
				CloseLists(-1);
				Table(lines, i);
				continue;
			case LineKind::LIST_ITEM:
				FlushParagraph();
				Item(line);
				continue;
			case LineKind::TEXT:
				// A text line inside an open list CONTINUES its item rather than starting a
				// paragraph -- that is what an indented continuation line means in Org.
				para_.push_back(line.text);
				continue;
			case LineKind::DRAWER_BEGIN: {
				// Skip to :END:. An UNTERMINATED drawer stops at the next blank line rather
				// than eating the rest of the document -- the same runaway the LaTeX
				// reader's tabular walker had, avoided here by bounding the scan.
				size_t j = i + 1;
				while (j < lines.size() && lines[j].kind != LineKind::DRAWER_END && lines[j].kind != LineKind::BLANK) {
					j++;
				}
				i = j;
				continue;
			}
			case LineKind::DRAWER_END:
			case LineKind::BLOCK_END:
				continue; // consumed by Block() / DRAWER_BEGIN
			}
		}
		FlushParagraph();
		CloseLists(-1);
		EmitMetadata();
		return std::move(blocks_);
	}

private:
	std::vector<OrgBlock> blocks_;
	std::vector<std::string> para_;
	//! Open lists, innermost last: {source indent, structural level of the list block}.
	std::vector<std::pair<int, int>> lists_;
	std::map<std::string, std::string> meta_;

	int Depth() const {
		return lists_.empty() ? 1 : lists_.back().second + 1;
	}

	void Simple(const char *type) {
		OrgBlock b;
		b.element_type = type;
		b.level = Depth();
		blocks_.push_back(std::move(b));
	}

	void FlushParagraph() {
		if (para_.empty()) {
			return;
		}
		std::string text;
		for (size_t i = 0; i < para_.size(); i++) {
			// A SOFT LINE BREAK IS A WORD BOUNDARY. Org wraps prose freely and the newline
			// carries no meaning, so joining with a space is what the document says.
			text += (i ? " " : "") + para_[i];
		}
		para_.clear();
		if (lists_.empty()) {
			Emit(DuckBlockTypes::TYPE_PARAGRAPH, text, Depth());
			return;
		}
		// Inside a list, a continuation line belongs to the item that is already open, so
		// it is appended there rather than becoming a sibling paragraph.
		auto &item = blocks_.back();
		if (item.element_type == DuckBlockTypes::TYPE_LIST_ITEM && item.inlines.empty() && item.content.empty()) {
			item.content = text;
			return;
		}
		Emit(DuckBlockTypes::TYPE_PARAGRAPH, text, Depth());
	}

	//! Emit a block whose text may carry inline markup. A run with no markup becomes the
	//! block's `content` -- duck_block's rule since v1 -- and anything richer becomes
	//! inline children.
	void Emit(const char *type, const std::string &text, int level, const char *role = nullptr) {
		OrgBlock b;
		b.element_type = type;
		b.level = level;
		if (role) {
			b.role = role;
		}
		std::vector<OrgInline> runs;
		ParseInlines(text, level + 1, runs);
		if (runs.size() == 1 && runs[0].element_type == DuckBlockTypes::INLINE_TEXT) {
			b.content = runs[0].content;
		} else {
			b.inlines = std::move(runs);
		}
		blocks_.push_back(std::move(b));
	}

	void Heading(const Line &line) {
		OrgBlock b;
		b.element_type = DuckBlockTypes::TYPE_HEADING;
		b.heading_level = line.level;
		b.level = 1;
		std::vector<OrgInline> runs;
		ParseInlines(line.text, 2, runs);
		if (runs.size() == 1 && runs[0].element_type == DuckBlockTypes::INLINE_TEXT) {
			b.content = runs[0].content;
		} else {
			// A HEADING CARRIES BOTH: a flattened title in `content` AND the rich
			// inline children beside it (duck_block ruling d003d32).
			//
			// Flattening alone loses formatting irreversibly -- `**Bold** title` and
			// `Bold title` become byte-identical, so a round trip rewrites the first as
			// the second. Children alone break every consumer that reads a title from
			// `content`, which doc_toc does.
			//
			// The structure marks itself and needs no new vocabulary: a lone text child
			// lives in `content` and produces NO children, so children alongside
			// non-empty content can only mean the content is a DERIVED flattening.
			// CHILDREN ARE AUTHORITATIVE when both are present.
			std::string all;
			for (auto &r : runs) {
				all += r.content;
			}
			b.content = all;
			b.inlines = std::move(runs);
		}
		blocks_.push_back(std::move(b));
	}

	void CloseLists(int indent) {
		while (!lists_.empty() && lists_.back().first > indent) {
			lists_.pop_back();
		}
	}

	void Item(const Line &line) {
		CloseLists(line.level);
		const char *want = line.definition ? DuckBlockTypes::LIST_TYPE_DEFINITION
		                   : line.ordered  ? DuckBlockTypes::LIST_TYPE_ORDERED
		                                   : DuckBlockTypes::LIST_TYPE_BULLET;
		if (lists_.empty() || lists_.back().first < line.level) {
			OrgBlock list;
			list.element_type = DuckBlockTypes::TYPE_LIST;
			list.list_type = want;
			list.level = Depth();
			if (line.ordered) {
				// ALWAYS emitted, including at their defaults, matching the stricter of the
				// two upstream producers so there is one shape rather than two.
				list.list_start = std::to_string(line.start);
				list.number_style = "Decimal";
				list.number_delim = "Period";
			}
			int level = list.level;
			blocks_.push_back(std::move(list));
			lists_.push_back({line.level, level});
		}
		const int item_level = lists_.back().second + 1;
		if (line.definition) {
			// ONE ITEM PRODUCES TWO ROWS, as in the EPUB and LaTeX readers: `- term :: def`
			// is a term and a definition, and the term is the half carrying the meaning.
			Emit(DuckBlockTypes::TYPE_LIST_ITEM, line.term, item_level, DuckBlockTypes::ROLE_TERM);
			Emit(DuckBlockTypes::TYPE_LIST_ITEM, line.text, item_level, DuckBlockTypes::ROLE_DEFINITION);
			return;
		}
		Emit(DuckBlockTypes::TYPE_LIST_ITEM, line.text, item_level);
	}

	void Block(std::vector<Line> &lines, size_t &i) {
		const std::string name = lines[i].key;
		const std::string arg = lines[i].text;
		std::string body;
		size_t j = i + 1;
		for (; j < lines.size(); j++) {
			if (lines[j].kind == LineKind::BLOCK_END && lines[j].key == name) {
				break;
			}
			// The body is taken VERBATIM -- a source block's indentation and blank lines are
			// its content, so the scanner's classification of those lines is ignored here.
			body += (body.empty() ? "" : "\n") + lines[j].text;
		}
		i = j; // the END line, or the last line for an unterminated block
		if (name == "QUOTE") {
			OrgBlock b;
			b.element_type = DuckBlockTypes::TYPE_BLOCKQUOTE;
			b.level = Depth();
			blocks_.push_back(std::move(b));
			Emit(DuckBlockTypes::TYPE_PARAGRAPH, body, Depth() + 1);
			return;
		}
		OrgBlock b;
		b.element_type = DuckBlockTypes::TYPE_CODE;
		b.content = body;
		b.level = Depth();
		if (name == "SRC" && !arg.empty()) {
			// `#+BEGIN_EXAMPLE` gets NO language: pandoc gives it the class "example", which
			// is not a language, and recording it as one would make a consumer highlight
			// text as a dialect that does not exist.
			auto sp = arg.find_first_of(" \t");
			b.language = sp == std::string::npos ? arg : arg.substr(0, sp);
		}
		blocks_.push_back(std::move(b));
	}

	void Table(std::vector<Line> &lines, size_t &i) {
		std::vector<std::vector<std::string>> rows;
		std::vector<bool> ruled;
		size_t j = i;
		for (; j < lines.size(); j++) {
			if (lines[j].kind == LineKind::TABLE_RULE) {
				if (!ruled.empty()) {
					ruled.back() = true;
				}
				continue;
			}
			if (lines[j].kind != LineKind::TABLE_ROW) {
				break;
			}
			rows.push_back(TableCells(lines[j].text));
			ruled.push_back(false);
		}
		i = j - 1;
		if (rows.empty()) {
			return;
		}
		std::vector<std::string> headers;
		size_t first = 0;
		// A RULE AFTER THE FIRST ROW PROMOTES IT. Measured against pandoc for Org rather
		// than carried over from the LaTeX reader -- the formats are unrelated and pandoc's
		// readers share no logic. They agree, but as a measurement.
		if (rows.size() > 1 && ruled[0]) {
			headers = rows[0];
			first = 1;
		}
		std::vector<std::vector<std::string>> body(rows.begin() + (long)first, rows.end());
		OrgBlock b;
		b.element_type = DuckBlockTypes::TYPE_TABLE;
		b.content = BuildTableJson(headers, body);
		b.encoding = DuckBlockTypes::ENCODING_JSON;
		b.level = Depth();
		blocks_.push_back(std::move(b));
	}

	void Keyword(const Line &line) {
		if (line.key != "TITLE" && line.key != "AUTHOR" && line.key != "DATE") {
			return; // every other #+KEY: is an option, not document metadata
		}
		std::string key = line.key == "TITLE" ? "title" : line.key == "AUTHOR" ? "author" : "date";
		auto it = meta_.find(key);
		if (it == meta_.end()) {
			meta_[key] = line.text;
			return;
		}
		// REPEATED #+AUTHOR: CONCATENATES into ONE value -- measured. LaTeX's \author yields
		// a MetaList for the same logical field, so this reader cannot generalise from that
		// one. Joined with a space here; pandoc uses a SoftBreak node, which duck_block has
		// as an inline type but which a single flattened value cannot carry.
		it->second += " " + line.text;
	}

	void EmitMetadata() {
		// AFTER the blocks -- spec 6.2 makes body-then-metadata a contract. std::map
		// iterates sorted, which is also pandoc's Meta serialisation order.
		for (auto &kv : meta_) {
			OrgBlock b;
			b.kind = DuckBlockTypes::KIND_VALUE;
			b.element_type = DuckBlockTypes::VALUE_INLINES;
			b.key = kv.first;
			b.level = 1;
			if (!kv.second.empty()) {
				// An EMPTY #+AUTHOR: emits the key with NO child, matching pandoc's
				// MetaInlines []. Present-and-empty is not absent.
				OrgInline run;
				run.element_type = DuckBlockTypes::INLINE_TEXT;
				run.content = kv.second;
				run.level = 2;
				b.inlines.push_back(std::move(run));
			}
			blocks_.push_back(std::move(b));
		}
	}
};

} // namespace

std::vector<OrgBlock> ParseOrgString(const std::string &src) {
	Builder builder;
	return builder.Build(src);
}

namespace {

struct OrgRow {
	std::string kind, element_type, content;
	std::string encoding = DuckBlockTypes::ENCODING_TEXT;
	bool has_level = false;
	int32_t level = 0;
	std::map<std::string, std::string> attributes;
	int32_t element_order = 0;
};

struct OrgBindData : public TableFunctionData {
	std::vector<OrgRow> rows;
};

struct OrgGlobalState : public GlobalTableFunctionState {
	idx_t offset = 0;
	static unique_ptr<GlobalTableFunctionState> Init(ClientContext &, TableFunctionInitInput &) {
		return make_uniq<OrgGlobalState>();
	}
};

void OrgColumns(vector<LogicalType> &types, vector<string> &names) {
	types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	         LogicalType::INTEGER, LogicalType::VARCHAR, LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR),
	         LogicalType::INTEGER};
	names = {"kind", "element_type", "content", "level", "encoding", "attributes", "element_order"};
}

void BuildRows(const std::string &src, std::vector<OrgRow> &rows) {
	int32_t order = 0;
	for (auto &block : ParseOrgString(src)) {
		OrgRow row;
		row.kind = block.kind.empty() ? DuckBlockTypes::KIND_BLOCK : block.kind;
		row.element_type = block.element_type;
		row.content = block.content;
		if (!block.encoding.empty()) {
			row.encoding = block.encoding;
		}
		row.element_order = order++;
		if (block.heading_level > 0) {
			row.attributes[DuckBlockTypes::ATTR_HEADING_LEVEL] = std::to_string(block.heading_level);
		}
		if (!block.key.empty()) {
			row.attributes[DuckBlockTypes::ATTR_KEY] = block.key;
		}
		if (!block.role.empty()) {
			row.attributes[DuckBlockTypes::ATTR_ROLE] = block.role;
		}
		if (!block.language.empty()) {
			row.attributes["language"] = block.language;
		}
		if (!block.list_type.empty()) {
			// BOTH SPELLINGS, as every other panduck reader emits: `ordered` is the v1 name
			// and `list_type` the later alias, and a consumer written against either reads
			// this output.
			row.attributes[DuckBlockTypes::ATTR_ORDERED_LEGACY] =
			    block.list_type == DuckBlockTypes::LIST_TYPE_ORDERED ? "true" : "false";
			row.attributes[DuckBlockTypes::ATTR_LIST_TYPE] = block.list_type;
			if (!block.list_start.empty()) {
				row.attributes["start"] = block.list_start;
				row.attributes["number_style"] = block.number_style;
				row.attributes["number_delim"] = block.number_delim;
			}
		}
		const int32_t block_level = block.level > 0 ? block.level : 1;
		row.has_level = true;
		row.level = block_level;
		rows.push_back(std::move(row));

		for (auto &inl : block.inlines) {
			OrgRow child;
			child.kind = DuckBlockTypes::KIND_INLINE;
			child.element_type = inl.element_type;
			child.content = inl.content;
			child.has_level = true;
			child.level = inl.level > 0 ? inl.level : block_level + 1;
			child.element_order = order++;
			if (!inl.href.empty()) {
				child.attributes["href"] = inl.href;
			}
			rows.push_back(std::move(child));
		}
	}
}

unique_ptr<FunctionData> OrgFileBind(ClientContext &, TableFunctionBindInput &input, vector<LogicalType> &return_types,
                                     vector<string> &names) {
	OrgColumns(return_types, names);
	auto path = input.inputs[0].GetValue<string>();
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		throw IOException("read_org_blocks: cannot open %s", path);
	}
	std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	auto result = make_uniq<OrgBindData>();
	BuildRows(src, result->rows);
	return std::move(result);
}

unique_ptr<FunctionData> OrgStringBind(ClientContext &, TableFunctionBindInput &input,
                                       vector<LogicalType> &return_types, vector<string> &names) {
	OrgColumns(return_types, names);
	auto result = make_uniq<OrgBindData>();
	BuildRows(input.inputs[0].GetValue<string>(), result->rows);
	return std::move(result);
}

void OrgScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<OrgBindData>();
	auto &state = input.global_state->Cast<OrgGlobalState>();
	idx_t count = 0;
	while (state.offset < data.rows.size() && count < STANDARD_VECTOR_SIZE) {
		const auto &row = data.rows[state.offset];
		output.SetValue(0, count, Value(row.kind));
		output.SetValue(1, count, Value(row.element_type));
		output.SetValue(2, count, row.content.empty() ? Value(LogicalType::VARCHAR) : Value(row.content));
		output.SetValue(3, count, row.has_level ? Value::INTEGER(row.level) : Value(LogicalType::INTEGER));
		output.SetValue(4, count, Value(row.encoding));
		output.SetValue(5, count, DuckBlockTypes::CreateAttributesMap(row.attributes));
		output.SetValue(6, count, Value::INTEGER(row.element_order));
		state.offset++;
		count++;
	}
	output.SetCardinality(count);
}

} // namespace

void RegisterOrgReader(ExtensionLoader &loader) {
	TableFunction file_fn("read_org_blocks", {LogicalType::VARCHAR}, OrgScan, OrgFileBind, OrgGlobalState::Init);
	loader.RegisterFunction(file_fn);

	// The string form, as the LaTeX reader has: asserting a two-line snippet is how the
	// nesting and inline rules stay readable in the tests.
	TableFunction string_fn("read_org_blocks_string", {LogicalType::VARCHAR}, OrgScan, OrgStringBind,
	                        OrgGlobalState::Init);
	loader.RegisterFunction(string_fn);
}

} // namespace org
} // namespace duckdb
