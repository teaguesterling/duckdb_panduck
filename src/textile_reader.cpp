#include "textile_reader.hpp"
#include "panduck_duckdb_compat.hpp"

#include "block_json.hpp"
#include "duck_block_types.hpp"
#include "textile_scanner.hpp"

#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>

namespace duckdb {
namespace textile {

namespace {

std::string Trim(const std::string &s) {
	size_t b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos) {
		return {};
	}
	size_t e = s.find_last_not_of(" \t\r\n");
	return s.substr(b, e - b + 1);
}

//! Heading anchors, matching pandoc's: lowercase, spaces to hyphens, punctuation dropped.
//! Measured -- `h1. Top Heading` anchors as `top-heading`. NOTE THE SEPARATOR: MediaWiki
//! slugs use underscores and these use hyphens, which is a difference between the two
//! formats' conventions rather than a choice either reader made.
std::string Slugify(const std::string &text) {
	std::string out;
	bool prev_sep = false;
	for (char c : text) {
		if (std::isalnum(static_cast<unsigned char>(c))) {
			out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			prev_sep = false;
		} else if (c == ' ' || c == '-' || c == '_') {
			if (!out.empty() && !prev_sep) {
				out += '-';
				prev_sep = true;
			}
		}
	}
	while (!out.empty() && out.back() == '-') {
		out.pop_back();
	}
	return out;
}

//! Strip inline markup down to text, for table cells which are flattened into the native
//! {headers, rows} schema.
std::string PlainText(const std::string &s) {
	std::string out;
	for (size_t i = 0; i < s.size(); i++) {
		char c = s[i];
		if (c == '*' || c == '_' || c == '@' || c == '^' || c == '~' || c == '%' || c == '+') {
			continue;
		}
		out += c;
	}
	return out;
}

void PushText(std::vector<TxInline> &out, const std::string &text, int level) {
	if (text.empty()) {
		return;
	}
	if (!out.empty() && out.back().element_type == DuckBlockTypes::INLINE_TEXT && out.back().attributes.empty()) {
		out.back().content += text;
		return;
	}
	TxInline in;
	in.element_type = DuckBlockTypes::INLINE_TEXT;
	in.content = text;
	in.level = level;
	out.push_back(in);
}

void ParseInlines(const std::string &s, int level, std::vector<TxInline> &out);

void PushWrapped(std::vector<TxInline> &out, const char *type, const std::string &inner, int level) {
	TxInline node;
	node.element_type = type;
	node.level = level;
	std::vector<TxInline> children;
	ParseInlines(inner, level + 1, children);
	// Spec 6.0's content rule: a wrapper whose only child is plain text carries the text
	// itself rather than a lone `text` child.
	if (children.size() == 1 && children[0].element_type == DuckBlockTypes::INLINE_TEXT &&
	    children[0].attributes.empty()) {
		node.content = children[0].content;
		out.push_back(node);
		return;
	}
	out.push_back(node);
	for (auto &c : children) {
		out.push_back(c);
	}
}

//! One delimiter pair, e.g. `-del-` or `^sup^`. Returns false when unterminated, which
//! leaves the character to be emitted as ordinary text.
bool TryDelim(const std::string &s, size_t &i, const char *open, const char *type, int level,
              std::vector<TxInline> &out, std::string &pending) {
	size_t n = strlen(open);
	if (s.compare(i, n, open) != 0) {
		return false;
	}
	size_t close = s.find(open, i + n);
	if (close == std::string::npos) {
		return false;
	}
	PushText(out, pending, level);
	pending.clear();
	PushWrapped(out, type, s.substr(i + n, close - i - n), level);
	i = close + n - 1;
	return true;
}

void ParseInlines(const std::string &s, int level, std::vector<TxInline> &out) {
	std::string pending;
	for (size_t i = 0; i < s.size(); i++) {
		// `"text":url` -- the link form, checked first because a bare `"` is common in prose
		// and only the `":` sequence makes it a link.
		if (s[i] == '"') {
			size_t close = s.find("\":", i + 1);
			if (close != std::string::npos) {
				size_t url_end = s.find_first_of(" \t", close + 2);
				if (url_end == std::string::npos) {
					url_end = s.size();
				}
				// Trailing sentence punctuation is not part of the URL. Textile's own rule,
				// and without it every link at the end of a sentence keeps the full stop.
				while (url_end > close + 2 && strchr(".,;:!?", s[url_end - 1])) {
					url_end--;
				}
				TxInline node;
				node.element_type = DuckBlockTypes::INLINE_LINK;
				node.level = level;
				node.content = s.substr(i + 1, close - i - 1);
				node.attributes["href"] = s.substr(close + 2, url_end - close - 2);
				PushText(out, pending, level);
				pending.clear();
				out.push_back(node);
				i = url_end - 1;
				continue;
			}
		}

		// `!image.png!` -- and `!` is also ordinary punctuation, so an unterminated one
		// falls through to text.
		if (s[i] == '!') {
			size_t close = s.find('!', i + 1);
			if (close != std::string::npos && close > i + 1) {
				std::string src = s.substr(i + 1, close - i - 1);
				if (src.find(' ') == std::string::npos) {
					TxInline node;
					node.element_type = DuckBlockTypes::INLINE_IMAGE;
					node.level = level;
					node.attributes["src"] = src;
					PushText(out, pending, level);
					pending.clear();
					out.push_back(node);
					i = close;
					continue;
				}
			}
		}

		// DOUBLE MARKERS FIRST. `**bold**` and `*strong*` both become `bold`, `__italic__`
		// and `_em_` both become `italic` -- textile's distinction is <b> versus <strong>,
		// which duck_block does not carry. Testing the single form first would consume one
		// character of the double form and leave a stray marker in the text.
		if (TryDelim(s, i, "**", DuckBlockTypes::INLINE_BOLD, level, out, pending) ||
		    TryDelim(s, i, "__", DuckBlockTypes::INLINE_ITALIC, level, out, pending) ||
		    TryDelim(s, i, "??", DuckBlockTypes::INLINE_CITE, level, out, pending) ||
		    TryDelim(s, i, "*", DuckBlockTypes::INLINE_BOLD, level, out, pending) ||
		    TryDelim(s, i, "_", DuckBlockTypes::INLINE_ITALIC, level, out, pending) ||
		    TryDelim(s, i, "@", DuckBlockTypes::INLINE_CODE, level, out, pending) ||
		    TryDelim(s, i, "-", DuckBlockTypes::INLINE_STRIKETHROUGH, level, out, pending) ||
		    TryDelim(s, i, "+", DuckBlockTypes::INLINE_UNDERLINE, level, out, pending) ||
		    TryDelim(s, i, "^", DuckBlockTypes::INLINE_SUPERSCRIPT, level, out, pending) ||
		    TryDelim(s, i, "~", DuckBlockTypes::INLINE_SUBSCRIPT, level, out, pending) ||
		    TryDelim(s, i, "%", DuckBlockTypes::INLINE_SPAN, level, out, pending)) {
			continue;
		}

		pending += s[i];
	}
	PushText(out, pending, level);
}

void AttachInlines(TxBlock &block, const std::string &text) {
	std::vector<TxInline> inl;
	ParseInlines(text, block.level + 1, inl);
	if (inl.size() == 1 && inl[0].element_type == DuckBlockTypes::INLINE_TEXT && inl[0].attributes.empty()) {
		block.content = inl[0].content;
		return;
	}
	block.inlines = std::move(inl);
}

class Builder {
public:
	std::vector<TxBlock> Build(const std::string &src) {
		auto lines = ScanTextile(src);
		for (size_t i = 0; i < lines.size(); i++) {
			const auto &ln = lines[i];
			switch (ln.kind) {
			case LineKind::BLANK:
				Flush();
				CloseLists();
				break;
			case LineKind::COMMENT:
				Flush();
				break;
			case LineKind::HEADING: {
				Flush();
				CloseLists();
				TxBlock b;
				b.element_type = DuckBlockTypes::TYPE_HEADING;
				b.level = 1;
				b.attributes[DuckBlockTypes::ATTR_HEADING_LEVEL] = std::to_string(ln.level);
				// AN EXPLICIT ID WINS over the derived slug. `h1(#guide-title).` states its
				// anchor; the slugifier only guesses one from the heading text, and the two
				// agree by luck rather than by rule.
				auto id = ln.id.empty() ? Slugify(ln.text) : ln.id;
				if (!id.empty()) {
					b.attributes["id"] = id;
				}
				AddBlockAttrs(b, ln);
				AttachInlines(b, ln.text);
				blocks_.push_back(std::move(b));
				break;
			}
			case LineKind::BLOCKQUOTE: {
				Flush();
				CloseLists();
				TxBlock q;
				q.element_type = DuckBlockTypes::TYPE_BLOCKQUOTE;
				q.level = 1;
				AddBlockAttrs(q, ln);
				blocks_.push_back(std::move(q));
				TxBlock p;
				p.element_type = DuckBlockTypes::TYPE_PARAGRAPH;
				p.level = 2;
				AttachInlines(p, ln.text);
				blocks_.push_back(std::move(p));
				break;
			}
			case LineKind::CODE: {
				Flush();
				CloseLists();
				TxBlock c;
				c.element_type = DuckBlockTypes::TYPE_CODE;
				c.level = 1;
				c.content = ln.text;
				// A `bc.` BLOCK RUNS TO THE NEXT BLANK LINE. Taking only the marker's own
				// line truncated every multi-line listing and let the rest leak out as
				// PROSE -- `bc(python). def hello():` gave a code block holding the
				// signature and a paragraph holding "return 1".
				//
				// python-textile 4.0.2 settles it:
				//   bc(python). def hello():
				//       return 1
				// -> <pre class="python"><code class="python">def hello():
				//    return 1</code></pre>
				//
				// `raw` rather than `text`, because the continuation's INDENTATION is
				// content: "    return 1" read back as "return 1" is a different program.
				while (i + 1 < lines.size() && lines[i + 1].kind != LineKind::BLANK) {
					c.content += "\n" + lines[i + 1].raw;
					i++;
				}
				AddBlockAttrs(c, ln);
				// On a code block the class IS the language -- it is what the reference
				// emits as <code class="python"> and what pandoc reads as a CodeBlock's
				// first class. Recorded under the name a consumer looks for, rather than
				// left as a generic class it would have to know to interpret.
				auto cls = c.attributes.find("class");
				if (cls != c.attributes.end()) {
					c.attributes["language"] = cls->second;
					c.attributes.erase(cls);
				}
				blocks_.push_back(std::move(c));
				break;
			}
			case LineKind::NOTEXTILE: {
				Flush();
				CloseLists();
				// THE MARKER IS CONSUMED AND THE BODY HELD RAW.
				//
				// pandoc keeps `notextile.` as the paragraph's first word AND parses the body
				// as textile regardless -- so it both advertises the marker to the reader and
				// does the one thing the construct exists to prevent. Measured against
				// python-textile 4.0.2, which strips the marker and passes the body through.
				//
				// `html` because that is what a notextile body is for: markup the author wants
				// delivered verbatim.
				TxBlock r;
				r.element_type = DuckBlockTypes::TYPE_RAW;
				r.level = 1;
				r.content = ln.text;
				// The format never lives in `encoding`: duck_block_utils measured that a raw
				// block's encoding is `text` even for html and latex, which ARE declared
				// encodings, and made it a flat rule rather than a fallback.
				r.attributes["format"] = "html";
				r.attributes[DuckBlockTypes::ATTR_SOURCE_TYPE] = "notextile";
				blocks_.push_back(std::move(r));
				break;
			}
			case LineKind::HTML_BLOCK: {
				Flush();
				CloseLists();
				// Held RAW rather than allowed to reach a paragraph as literal markup.
				// pandoc's textile writer emits `<dl>` for a definition list because
				// `- term := def` is not in its writer, so this arrives in every
				// pandoc-generated document and in no hand-written one.
				TxBlock r;
				r.element_type = DuckBlockTypes::TYPE_RAW;
				r.level = 1;
				r.content = ln.text;
				// The format never lives in `encoding`: duck_block_utils measured that a raw
				// block's encoding is `text` even for html and latex, which ARE declared
				// encodings, and made it a flat rule rather than a fallback.
				r.attributes["format"] = "html";
				r.attributes[DuckBlockTypes::ATTR_SOURCE_TYPE] = ln.markers;
				blocks_.push_back(std::move(r));
				break;
			}
			case LineKind::LIST_ITEM:
				FlushParagraph();
				ListItem(ln);
				break;
			case LineKind::TABLE_ROW:
				FlushParagraph();
				CloseLists();
				i = Table(lines, i);
				break;
			case LineKind::PARA:
				Flush();
				CloseLists();
				pending_style_ = ln.style;
				pending_class_ = ln.css_class;
				para_ = ln.text;
				break;
			case LineKind::TEXT:
				CloseLists();
				if (!para_.empty()) {
					para_ += " ";
				}
				para_ += ln.text;
				break;
			}
		}
		Flush();
		CloseLists();
		return std::move(blocks_);
	}

private:
	void AddBlockAttrs(TxBlock &b, const Line &ln) {
		// pandoc wraps an attributed block in a Div and puts the style there. panduck keeps
		// the block's OWN type and carries the attribute on it -- a styled paragraph is still
		// a paragraph, and wrapping it changes the document's shape to record a colour.
		if (!ln.style.empty()) {
			b.attributes["style"] = ln.style;
		}
		if (!ln.css_class.empty()) {
			b.attributes["class"] = ln.css_class;
		}
	}

	void FlushParagraph() {
		if (para_.empty()) {
			return;
		}
		TxBlock b;
		b.element_type = DuckBlockTypes::TYPE_PARAGRAPH;
		b.level = 1;
		if (!pending_style_.empty()) {
			b.attributes["style"] = pending_style_;
		}
		if (!pending_class_.empty()) {
			b.attributes["class"] = pending_class_;
		}
		AttachInlines(b, para_);
		blocks_.push_back(std::move(b));
		para_.clear();
		pending_style_.clear();
		pending_class_.clear();
	}

	void Flush() {
		FlushParagraph();
	}

	//! `*` bullet, `#` ordered, `-` definition. Depth is the marker run's LENGTH.
	void ListItem(const Line &ln) {
		const std::string &m = ln.markers;
		size_t common = 0;
		while (common < m.size() && common < open_.size() && m[common] == open_[common]) {
			common++;
		}
		// A CHANGE OF LIST TYPE AT THE SAME DEPTH STARTS A NEW LIST, which is the divergence
		// this reader carries. pandoc turns `* bullet` followed by `# ordered` into a
		// PARAGRAPH containing a literal asterisk -- the list is lost and its marker becomes
		// prose. python-textile keeps a list. Sibling lists lose nothing and describe what
		// the author wrote; the reference nests them, but its own output there places an
		// <ol> directly inside a <ul>, which is invalid HTML and so not authoritative.
		while (open_.size() > common) {
			open_.pop_back();
		}
		for (size_t k = common; k < m.size(); k++) {
			TxBlock b;
			b.element_type = DuckBlockTypes::TYPE_LIST;
			b.level = static_cast<int>(2 * k + 1);
			const char *lt = m[k] == '#'   ? DuckBlockTypes::LIST_TYPE_ORDERED
			                 : m[k] == '-' ? DuckBlockTypes::LIST_TYPE_DEFINITION
			                               : DuckBlockTypes::LIST_TYPE_BULLET;
			b.attributes[DuckBlockTypes::ATTR_LIST_TYPE] = lt;
			b.attributes[DuckBlockTypes::ATTR_ORDERED_LEGACY] =
			    lt == std::string(DuckBlockTypes::LIST_TYPE_ORDERED) ? "true" : "false";
			if (lt == std::string(DuckBlockTypes::LIST_TYPE_ORDERED)) {
				b.attributes["start"] = "1";
				b.attributes["number_style"] = "Decimal";
				b.attributes["number_delim"] = "Period";
			}
			blocks_.push_back(std::move(b));
			open_.push_back(m[k]);
		}

		if (ln.definition) {
			TxBlock term;
			term.element_type = DuckBlockTypes::TYPE_LIST_ITEM;
			term.level = static_cast<int>(2 * m.size());
			term.attributes[DuckBlockTypes::ATTR_ROLE] = DuckBlockTypes::ROLE_TERM;
			AttachInlines(term, ln.term);
			blocks_.push_back(std::move(term));

			TxBlock def;
			def.element_type = DuckBlockTypes::TYPE_LIST_ITEM;
			def.level = static_cast<int>(2 * m.size());
			def.attributes[DuckBlockTypes::ATTR_ROLE] = DuckBlockTypes::ROLE_DEFINITION;
			AttachInlines(def, ln.text);
			blocks_.push_back(std::move(def));
			return;
		}

		TxBlock item;
		item.element_type = DuckBlockTypes::TYPE_LIST_ITEM;
		item.level = static_cast<int>(2 * m.size());
		AttachInlines(item, ln.text);
		blocks_.push_back(std::move(item));
	}

	void CloseLists() {
		open_.clear();
	}

	//! Consume a run of `| ... |` rows into one native table. `_.` marks a header cell.
	size_t Table(const std::vector<Line> &lines, size_t start) {
		std::vector<std::string> headers;
		std::vector<std::vector<std::string>> rows;
		size_t i = start;
		for (; i < lines.size() && lines[i].kind == LineKind::TABLE_ROW; i++) {
			auto cells = SplitRow(lines[i].text);
			bool is_header = false;
			std::vector<std::string> values;
			for (auto &cell : cells) {
				std::string v = cell;
				if (v.rfind("_.", 0) == 0) {
					is_header = true;
					v = Trim(v.substr(2));
				}
				values.push_back(PlainText(v));
			}
			if (is_header && headers.empty()) {
				headers = values;
			} else {
				rows.push_back(values);
			}
		}
		TxBlock b;
		b.element_type = DuckBlockTypes::TYPE_TABLE;
		b.level = 1;
		b.encoding = DuckBlockTypes::ENCODING_JSON;
		b.content = BuildTableJson(headers, rows);
		blocks_.push_back(std::move(b));
		return i - 1;
	}

	std::vector<TxBlock> blocks_;
	std::string para_;
	std::string pending_style_, pending_class_;
	std::string open_; //!< marker chars of currently open lists, outermost first
};

} // namespace

std::vector<TxBlock> ParseTextileString(const std::string &src) {
	Builder builder;
	return builder.Build(src);
}

namespace {

struct TxRow {
	std::string kind, element_type, content;
	std::string encoding = DuckBlockTypes::ENCODING_TEXT;
	bool has_level = false;
	int32_t level = 0;
	std::map<std::string, std::string> attributes;
	int32_t element_order = 0;
};

struct TxBindData : public TableFunctionData {
	std::vector<TxRow> rows;
};

struct TxGlobalState : public GlobalTableFunctionState {
	idx_t offset = 0;
	static unique_ptr<GlobalTableFunctionState> Init(ClientContext &, TableFunctionInitInput &) {
		return make_uniq<TxGlobalState>();
	}
};

void TxColumns(vector<LogicalType> &types, panduck::BindNames &names) {
	types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	         LogicalType::INTEGER, LogicalType::VARCHAR, LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR),
	         LogicalType::INTEGER};
	names = {"kind", "element_type", "content", "level", "encoding", "attributes", "element_order"};
}

void BuildRows(const std::string &src, std::vector<TxRow> &rows) {
	int32_t order = 0;
	for (auto &block : ParseTextileString(src)) {
		TxRow row;
		row.kind = block.kind.empty() ? DuckBlockTypes::KIND_BLOCK : block.kind;
		row.element_type = block.element_type;
		row.content = block.content;
		if (!block.encoding.empty()) {
			row.encoding = block.encoding;
		}
		row.attributes = block.attributes;
		row.has_level = true;
		row.level = block.level > 0 ? block.level : 1;
		row.element_order = order++;
		const int32_t block_level = row.level;
		rows.push_back(std::move(row));

		for (auto &inl : block.inlines) {
			TxRow child;
			child.kind = DuckBlockTypes::KIND_INLINE;
			child.element_type = inl.element_type;
			child.content = inl.content;
			child.attributes = inl.attributes;
			child.has_level = true;
			child.level = inl.level > 0 ? inl.level : block_level + 1;
			child.element_order = order++;
			rows.push_back(std::move(child));
		}
	}
}

unique_ptr<FunctionData> TxFileBind(ClientContext &, TableFunctionBindInput &input, vector<LogicalType> &return_types,
                                    panduck::BindNames &names) {
	TxColumns(return_types, names);
	auto path = input.inputs[0].GetValue<string>();
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		throw IOException("read_textile_blocks: cannot open %s", path);
	}
	std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	auto result = make_uniq<TxBindData>();
	BuildRows(src, result->rows);
	return std::move(result);
}

unique_ptr<FunctionData> TxStringBind(ClientContext &, TableFunctionBindInput &input, vector<LogicalType> &return_types,
                                      panduck::BindNames &names) {
	TxColumns(return_types, names);
	auto result = make_uniq<TxBindData>();
	BuildRows(input.inputs[0].GetValue<string>(), result->rows);
	return std::move(result);
}

void TxScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<TxBindData>();
	auto &state = input.global_state->Cast<TxGlobalState>();
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

void RegisterTextileReader(ExtensionLoader &loader) {
	TableFunction file_fn("read_textile_blocks", {LogicalType::VARCHAR}, TxScan, TxFileBind, TxGlobalState::Init);
	loader.RegisterFunction(file_fn);

	TableFunction string_fn("read_textile_blocks_string", {LogicalType::VARCHAR}, TxScan, TxStringBind,
	                        TxGlobalState::Init);
	loader.RegisterFunction(string_fn);
}

} // namespace textile
} // namespace duckdb
