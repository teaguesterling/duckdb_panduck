#include "rst_reader.hpp"
#include "panduck_bind_names.hpp"

#include "block_json.hpp"
#include "duck_block_types.hpp"
#include "rst_scanner.hpp"

#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <fstream>
#include <map>

namespace duckdb {
namespace rst {
namespace {

void PushText(std::vector<RstInline> &out, const std::string &text, int level) {
	if (text.empty()) {
		return;
	}
	RstInline run;
	run.element_type = DuckBlockTypes::INLINE_TEXT;
	run.content = text;
	run.level = level;
	out.push_back(std::move(run));
}

//! Split text into inline runs. `**strong**` is tested BEFORE `*emph*`: the shorter marker
//! is a prefix of the longer one, so checking emphasis first turns every bold run into an
//! empty italic followed by stray asterisks.
void ParseInlines(const std::string &s, int level, std::vector<RstInline> &out) {
	std::string plain;
	size_t i = 0;
	auto flush = [&]() {
		PushText(out, plain, level);
		plain.clear();
	};
	auto emit = [&](const char *type, const std::string &content, const std::string &href) {
		flush();
		RstInline run;
		run.element_type = type;
		run.content = content;
		run.href = href;
		run.level = level;
		out.push_back(std::move(run));
	};
	while (i < s.size()) {
		if (s.compare(i, 2, "``") == 0) {
			auto close = s.find("``", i + 2);
			if (close != std::string::npos) {
				emit(DuckBlockTypes::INLINE_CODE, s.substr(i + 2, close - i - 2), std::string());
				i = close + 2;
				continue;
			}
		}
		if (s.compare(i, 2, "**") == 0) {
			auto close = s.find("**", i + 2);
			if (close != std::string::npos && close > i + 2) {
				emit(DuckBlockTypes::INLINE_BOLD, s.substr(i + 2, close - i - 2), std::string());
				i = close + 2;
				continue;
			}
		}
		if (s[i] == '*') {
			auto close = s.find('*', i + 1);
			if (close != std::string::npos && close > i + 1) {
				emit(DuckBlockTypes::INLINE_ITALIC, s.substr(i + 1, close - i - 1), std::string());
				i = close + 1;
				continue;
			}
		}
		if (s[i] == '`') {
			// `text <url>`_ -- the trailing underscore is what makes it a link rather than
			// interpreted text, so it is required here.
			auto close = s.find("`_", i + 1);
			if (close != std::string::npos) {
				auto body = s.substr(i + 1, close - i - 1);
				auto lt = body.rfind(" <");
				std::string text = body, href = body;
				if (lt != std::string::npos && !body.empty() && body.back() == '>') {
					text = body.substr(0, lt);
					href = body.substr(lt + 2, body.size() - lt - 3);
				}
				emit(DuckBlockTypes::INLINE_LINK, text, href);
				i = close + 2;
				// A named reference may be spelled with two underscores; consume the second.
				if (i < s.size() && s[i] == '_') {
					i++;
				}
				continue;
			}
		}
		plain.push_back(s[i]);
		i++;
	}
	flush();
}

std::vector<std::string> GridCells(const std::string &row) {
	std::vector<std::string> cells;
	size_t i = row.find('|');
	if (i == std::string::npos) {
		return cells;
	}
	std::string cur;
	for (i++; i < row.size(); i++) {
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

//! A simple table's cells come from COLUMN POSITIONS, not delimiters -- the rule row's runs
//! locate them. This is the only positional extraction in the reader, and it is why the
//! rule line's offsets are carried on the Line.
//!
//! Each cell spans [start of its run, start of the NEXT run), and the last runs to the end
//! of the line. Earlier this walked forward as `at += width + 2`, assuming two spaces
//! between columns. RST separates them by ONE OR MORE, so on a single-space table the
//! offset drifted by a character per column and the error accumulated:
//!
//!     ==== ===== ====        headers  ["Name", "alue", "te"]
//!     Name Value Note        rows     [["a", "", ""], ["b", "", ""]]
//!
//! -- the first column right, the second short a character, the third short two, and every
//! data row past column one empty. Reading the boundaries instead of predicting them means
//! there is no gap to guess.
std::vector<std::string> SimpleCells(const std::string &row, const std::vector<int> &starts) {
	std::vector<std::string> cells;
	for (size_t c = 0; c < starts.size(); c++) {
		size_t at = static_cast<size_t>(starts[c]);
		size_t len = std::string::npos;
		if (c + 1 < starts.size()) {
			len = static_cast<size_t>(starts[c + 1]) - at;
		}
		std::string cell = at < row.size() ? row.substr(at, len) : "";
		size_t b = cell.find_first_not_of(" \t");
		size_t e = cell.find_last_not_of(" \t");
		cells.push_back(b == std::string::npos ? std::string() : cell.substr(b, e - b + 1));
	}
	return cells;
}

class Builder {
public:
	std::vector<RstBlock> Build(const std::string &src) {
		lines_ = ScanRst(src);
		Run(0, lines_.size(), 1);
		return std::move(blocks_);
	}

private:
	std::vector<Line> lines_;
	std::vector<RstBlock> blocks_;
	//! ADORNMENT CHARACTERS IN ORDER OF FIRST APPEARANCE. RST sets a heading's level by
	//! WHERE its adornment first appeared in the document, not by which character it is --
	//! measured, and the opposite of the usual assumption that `=` is level 1. A reader
	//! that hardcodes the conventional order is right on conventional documents and wrong
	//! on valid ones.
	std::vector<char> adornments_;

	int LevelFor(char c) {
		for (size_t i = 0; i < adornments_.size(); i++) {
			if (adornments_[i] == c) {
				return static_cast<int>(i) + 1;
			}
		}
		adornments_.push_back(c);
		auto n = static_cast<int>(adornments_.size());
		return n > 6 ? 6 : n;
	}

	void Emit(const char *type, const std::string &text, int level, const char *role = nullptr) {
		RstBlock b;
		b.element_type = type;
		b.level = level;
		if (role) {
			b.role = role;
		}
		std::vector<RstInline> runs;
		ParseInlines(text, level + 1, runs);
		if (runs.size() == 1 && runs[0].element_type == DuckBlockTypes::INLINE_TEXT) {
			b.content = runs[0].content;
		} else if (!runs.empty()) {
			b.inlines = std::move(runs);
		}
		blocks_.push_back(std::move(b));
	}

	//! The indented run starting at `i`, as [i, end) -- every following line indented
	//! deeper than `base`, blank lines included. RST expresses containment by indent and
	//! four constructs share the shape, so finding the run is one function.
	size_t IndentedRun(size_t i, int base) const {
		size_t j = i;
		while (j < lines_.size()) {
			if (lines_[j].kind == LineKind::BLANK) {
				size_t k = j;
				while (k < lines_.size() && lines_[k].kind == LineKind::BLANK) {
					k++;
				}
				if (k >= lines_.size() || lines_[k].indent <= base) {
					break;
				}
				j = k;
				continue;
			}
			if (lines_[j].indent <= base) {
				break;
			}
			j++;
		}
		return j;
	}

	std::string RawBody(size_t from, size_t to, int base) const {
		std::string body;
		for (size_t j = from; j < to; j++) {
			std::string t = lines_[j].kind == LineKind::BLANK ? std::string() : lines_[j].text;
			if (!body.empty()) {
				body += "\n";
			}
			body += t;
		}
		(void)base;
		return body;
	}

	void Run(size_t from, size_t to, int depth) {
		std::vector<std::string> para;
		bool literal_pending = false;
		auto flush = [&]() {
			if (para.empty()) {
				return;
			}
			std::string text;
			for (size_t k = 0; k < para.size(); k++) {
				text += (k ? " " : "") + para[k];
			}
			para.clear();
			// `::` AT THE END OF A PARAGRAPH opens a literal block AND stays as a colon in
			// the prose -- docutils drops one colon and keeps the other. A bare `::` on its
			// own paragraph disappears entirely.
			if (text.size() >= 2 && text.compare(text.size() - 2, 2, "::") == 0) {
				literal_pending = true;
				text = text.size() == 2 ? std::string() : text.substr(0, text.size() - 1);
			}
			if (!text.empty()) {
				Emit(DuckBlockTypes::TYPE_PARAGRAPH, text, depth);
			}
		};

		for (size_t i = from; i < to; i++) {
			auto &line = lines_[i];
			switch (line.kind) {
			case LineKind::BLANK:
				flush();
				continue;
			case LineKind::COMMENT:
				continue; // produces nothing, and must not fall through as prose
			case LineKind::ADORNMENT: {
				// A TRANSITION when nothing precedes it, a heading UNDERLINE when text does.
				// The scanner cannot tell them apart; this is the only place that can.
				if (!para.empty()) {
					std::string title = para.back();
					para.pop_back();
					flush();
					RstBlock b;
					b.element_type = DuckBlockTypes::TYPE_HEADING;
					b.heading_level = LevelFor(line.adornment);
					b.level = depth;
					// THE TITLE WAS NOT PARSED AT ALL, so `**Bold** title` reached `content`
					// as literal source -- markup leaking as text, which is worse than the
					// flattening the other readers did. Now it is parsed like any other run.
					std::vector<RstInline> runs;
					ParseInlines(title, depth + 1, runs);
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
					continue;
				}
				flush();
				RstBlock b;
				b.element_type = DuckBlockTypes::TYPE_HR;
				b.level = depth;
				blocks_.push_back(std::move(b));
				continue;
			}
			case LineKind::DIRECTIVE: {
				flush();
				size_t end = IndentedRun(i + 1, line.indent);
				Directive(line, i + 1, end, depth);
				i = end - 1;
				continue;
			}
			case LineKind::FIELD: {
				flush();
				size_t j = i;
				RstBlock list;
				list.element_type = DuckBlockTypes::TYPE_LIST;
				list.list_type = DuckBlockTypes::LIST_TYPE_DEFINITION;
				list.level = depth;
				blocks_.push_back(std::move(list));
				// A FIELD LIST IS A DEFINITION LIST, NOT METADATA. Measured: pandoc emits a
				// DefinitionList and an EMPTY meta. RST is the only panduck format with no
				// document metadata at all, and the opposite reading is the obvious one --
				// which is why it is asserted rather than assumed.
				while (j < to && lines_[j].kind == LineKind::FIELD) {
					Emit(DuckBlockTypes::TYPE_LIST_ITEM, lines_[j].name, depth + 1, DuckBlockTypes::ROLE_TERM);
					Emit(DuckBlockTypes::TYPE_LIST_ITEM, lines_[j].text, depth + 1, DuckBlockTypes::ROLE_DEFINITION);
					j++;
				}
				i = j - 1;
				continue;
			}
			case LineKind::BULLET:
			case LineKind::ENUM: {
				flush();
				i = List(i, to, depth) - 1;
				continue;
			}
			case LineKind::GRID_SEP:
			case LineKind::TABLE_ROW:
			case LineKind::SIMPLE_SEP: {
				flush();
				i = Table(i, to, depth) - 1;
				continue;
			}
			case LineKind::TEXT: {
				if (literal_pending) {
					size_t end = IndentedRun(i, line.indent - 1);
					if (end > i) {
						RstBlock b;
						b.element_type = DuckBlockTypes::TYPE_CODE;
						b.content = RawBody(i, end, line.indent);
						b.level = depth;
						blocks_.push_back(std::move(b));
						literal_pending = false;
						i = end - 1;
						continue;
					}
					literal_pending = false;
				}
				// A DEFINITION: a term line whose next line is indented and not a list.
				if (para.empty() && i + 1 < to && lines_[i + 1].indent > line.indent &&
				    lines_[i + 1].kind == LineKind::TEXT) {
					size_t end = IndentedRun(i + 1, line.indent);
					RstBlock list;
					list.element_type = DuckBlockTypes::TYPE_LIST;
					list.list_type = DuckBlockTypes::LIST_TYPE_DEFINITION;
					list.level = depth;
					blocks_.push_back(std::move(list));
					Emit(DuckBlockTypes::TYPE_LIST_ITEM, line.text, depth + 1, DuckBlockTypes::ROLE_TERM);
					std::string def;
					for (size_t k = i + 1; k < end; k++) {
						if (lines_[k].kind == LineKind::BLANK) {
							continue;
						}
						def += (def.empty() ? "" : " ") + lines_[k].text;
					}
					Emit(DuckBlockTypes::TYPE_LIST_ITEM, def, depth + 1, DuckBlockTypes::ROLE_DEFINITION);
					i = end - 1;
					continue;
				}
				para.push_back(line.text);
				continue;
			}
			}
		}
		flush();
	}

	void Directive(const Line &line, size_t from, size_t to, int depth) {
		auto name = line.name;
		for (auto &c : name) {
			c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
		}
		if (name == "code-block" || name == "code" || name == "sourcecode") {
			RstBlock b;
			b.element_type = DuckBlockTypes::TYPE_CODE;
			b.content = RawBody(from, to, line.indent);
			b.language = line.text;
			b.level = depth;
			blocks_.push_back(std::move(b));
			return;
		}
		// EVERY OTHER DIRECTIVE IS A DIV, with the directive NAME in
		// attributes['source_type'] rather than a minted role. The directive set is OPEN --
		// docutils ships dozens and Sphinx hundreds -- so a reader cannot enumerate it, and
		// the spec's instruction for an unrecognised name is to keep the original in
		// source_type so it is visible as a gap rather than silently private.
		RstBlock b;
		b.element_type = DuckBlockTypes::TYPE_DIV;
		b.source_type = line.name;
		b.level = depth;
		blocks_.push_back(std::move(b));
		// THE BODY IS DESCENDED INTO, never dropped. A directive body is prose, and the
		// LaTeX reader's rule applies: an unknown environment usually wraps paragraphs, so
		// dropping it loses them.
		if (to > from) {
			Run(from, to, depth + 1);
		}
	}

	size_t List(size_t i, size_t to, int depth) {
		const int indent = lines_[i].indent;
		const bool ordered = lines_[i].kind == LineKind::ENUM;
		RstBlock list;
		list.element_type = DuckBlockTypes::TYPE_LIST;
		list.list_type = ordered ? DuckBlockTypes::LIST_TYPE_ORDERED : DuckBlockTypes::LIST_TYPE_BULLET;
		list.level = depth;
		if (ordered) {
			list.list_start = std::to_string(lines_[i].start);
			list.number_style = "Decimal";
			list.number_delim = "Period";
		}
		blocks_.push_back(std::move(list));
		size_t j = i;
		while (j < to) {
			auto &line = lines_[j];
			if (line.kind == LineKind::BLANK) {
				if (j + 1 < to && (lines_[j + 1].kind == LineKind::BULLET || lines_[j + 1].kind == LineKind::ENUM) &&
				    lines_[j + 1].indent == indent) {
					j++;
					continue;
				}
				break;
			}
			const bool line_ordered = line.kind == LineKind::ENUM;
			if ((line.kind != LineKind::BULLET && line.kind != LineKind::ENUM) || line.indent != indent ||
			    line_ordered != ordered) {
				// ORDEREDNESS ENDS A LIST as surely as a dedent does. Without this a bullet
				// list followed by an enumerated one at the same indent became a single
				// list carrying both, with the second list's numbering lost entirely.
				break;
			}
			Emit(DuckBlockTypes::TYPE_LIST_ITEM, line.text, depth + 1);
			j++;
			// A NESTED list is the indented run after an item.
			size_t end = IndentedRun(j, indent);
			if (end > j && (lines_[j].kind == LineKind::BULLET || lines_[j].kind == LineKind::ENUM)) {
				j = List(j, end, depth + 2);
			}
		}
		return j;
	}

	size_t Table(size_t i, size_t to, int depth) {
		std::vector<std::vector<std::string>> rows;
		std::vector<bool> ruled;
		std::vector<int> spans;
		bool simple = lines_[i].kind == LineKind::SIMPLE_SEP;
		size_t j = i;
		for (; j < to; j++) {
			auto &line = lines_[j];
			if (line.kind == LineKind::GRID_SEP) {
				if (line.header_sep && !ruled.empty()) {
					ruled.back() = true;
				}
				continue;
			}
			if (line.kind == LineKind::SIMPLE_SEP) {
				if (spans.empty()) {
					spans = line.span_starts;
				} else if (!ruled.empty()) {
					ruled.back() = true;
				}
				continue;
			}
			if (line.kind == LineKind::TABLE_ROW) {
				rows.push_back(GridCells(line.text));
				ruled.push_back(false);
				continue;
			}
			if (simple && line.kind == LineKind::TEXT && !spans.empty()) {
				rows.push_back(SimpleCells(line.text, spans));
				ruled.push_back(false);
				continue;
			}
			break;
		}
		if (rows.empty()) {
			return j;
		}
		std::vector<std::string> headers;
		size_t first = 0;
		if (rows.size() > 1 && ruled[0]) {
			headers = rows[0];
			first = 1;
		}
		std::vector<std::vector<std::string>> body(rows.begin() + (long)first, rows.end());
		RstBlock b;
		b.element_type = DuckBlockTypes::TYPE_TABLE;
		b.content = BuildTableJson(headers, body);
		b.encoding = DuckBlockTypes::ENCODING_JSON;
		b.level = depth;
		blocks_.push_back(std::move(b));
		return j;
	}
};

} // namespace

std::vector<RstBlock> ParseRstString(const std::string &src) {
	Builder builder;
	return builder.Build(src);
}

namespace {

struct RstRow {
	std::string kind, element_type, content;
	std::string encoding = DuckBlockTypes::ENCODING_TEXT;
	bool has_level = false;
	int32_t level = 0;
	std::map<std::string, std::string> attributes;
	int32_t element_order = 0;
};

struct RstBindData : public TableFunctionData {
	std::vector<RstRow> rows;
};

struct RstGlobalState : public GlobalTableFunctionState {
	idx_t offset = 0;
	static unique_ptr<GlobalTableFunctionState> Init(ClientContext &, TableFunctionInitInput &) {
		return make_uniq<RstGlobalState>();
	}
};

void RstColumns(vector<LogicalType> &types, panduck::BindNames &names) {
	types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	         LogicalType::INTEGER, LogicalType::VARCHAR, LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR),
	         LogicalType::INTEGER};
	names = {"kind", "element_type", "content", "level", "encoding", "attributes", "element_order"};
}

void BuildRows(const std::string &src, std::vector<RstRow> &rows) {
	int32_t order = 0;
	for (auto &block : ParseRstString(src)) {
		RstRow row;
		row.kind = DuckBlockTypes::KIND_BLOCK;
		row.element_type = block.element_type;
		row.content = block.content;
		if (!block.encoding.empty()) {
			row.encoding = block.encoding;
		}
		row.element_order = order++;
		if (block.heading_level > 0) {
			row.attributes[DuckBlockTypes::ATTR_HEADING_LEVEL] = std::to_string(block.heading_level);
		}
		if (!block.source_type.empty()) {
			// The directive name a `div` came from. RST has NO document metadata -- a field
			// list is a definition list -- so there is no ATTR_KEY here, unlike every other
			// reader.
			row.attributes[DuckBlockTypes::ATTR_SOURCE_TYPE] = block.source_type;
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
			RstRow child;
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

unique_ptr<FunctionData> RstFileBind(ClientContext &, TableFunctionBindInput &input, vector<LogicalType> &return_types,
                                     panduck::BindNames &names) {
	RstColumns(return_types, names);
	auto path = input.inputs[0].GetValue<string>();
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		throw IOException("read_rst_blocks: cannot open %s", path);
	}
	std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	auto result = make_uniq<RstBindData>();
	BuildRows(src, result->rows);
	return std::move(result);
}

unique_ptr<FunctionData> RstStringBind(ClientContext &, TableFunctionBindInput &input,
                                       vector<LogicalType> &return_types, panduck::BindNames &names) {
	RstColumns(return_types, names);
	auto result = make_uniq<RstBindData>();
	BuildRows(input.inputs[0].GetValue<string>(), result->rows);
	return std::move(result);
}

void RstScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<RstBindData>();
	auto &state = input.global_state->Cast<RstGlobalState>();
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

void RegisterRstReader(ExtensionLoader &loader) {
	TableFunction file_fn("read_rst_blocks", {LogicalType::VARCHAR}, RstScan, RstFileBind, RstGlobalState::Init);
	loader.RegisterFunction(file_fn);

	// The string form, as the LaTeX reader has: asserting a two-line snippet is how the
	// nesting and inline rules stay readable in the tests.
	TableFunction string_fn("read_rst_blocks_string", {LogicalType::VARCHAR}, RstScan, RstStringBind,
	                        RstGlobalState::Init);
	loader.RegisterFunction(string_fn);
}

} // namespace rst
} // namespace duckdb
