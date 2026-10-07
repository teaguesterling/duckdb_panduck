#include "panduck/textile.hpp"

#include "panduck/block_json.hpp"
#include "panduck/slugify.hpp"
#include "panduck/textile_scanner.hpp"
#include "panduck/vocabulary.hpp"

#include <algorithm>
#include <cctype>
// <cstring> for strlen/strchr. The original file never named it: it arrived
// transitively through duckdb.hpp. Compiling without duckdb on the include
// path is what surfaced the real dependency -- the seam earning its keep.
#include <cstring>
#include <map>
#include <string>
#include <vector>

// The parse core and the row flattening, moved out of src/textile_reader.cpp
// (issue #104, L2). Behaviour unchanged: the only edits are the namespace
// (duckdb::textile -> panduck::textile), DuckBlockTypes:: -> DuckBlockVocabulary::
// (the same constants; DuckBlockTypes merely adds the LogicalType/Value helpers
// that cannot cross the seam), and TxRow -> panduck::Block.

namespace panduck {
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

// The heading anchor moved to slugify.hpp, shared with rst and mediawiki (#85). The copy
// that lived here differed from mediawiki's only in its separator -- and the two were
// wrong in the SAME three ways, which is what one function in two places hides: it reads
// as two independent decisions agreeing, rather than one mistake written twice.

//! Strip a leading textile CELL MODIFIER, and report whether it marked a header
//! cell.
//!
//! Textile writes alignment and cell role as a prefix ending in `.`:
//!     <. left   >. right   =. centre   <>. justify   ^. top   ~. bottom   _.
//!     header
//!
//! Only `_.` was handled, so the others SURVIVED INTO THE CELL VALUE -- `|<.
//! a|` read as the four characters `<. a` where the cell contains `a`. pandoc
//! reads the same table as `a`, and panduck's own webbed reads
//! `<td><b>a</b></td>` as `"a"`, so the fleet already had the answer. Reported
//! for .rst by duckeye (#38); this reader had the same defect, unreported, plus
//! a second one below.
//!
//! THE `.` IS REQUIRED before anything is stripped. Without it a cell
//! legitimately beginning with `<` or `~` would lose its first characters, and
//! a modifier without its terminator is not a modifier.
bool StripCellModifier(std::string &v) {
	size_t i = 0;
	bool header = false;
	while (i < v.size() &&
	       (v[i] == '<' || v[i] == '>' || v[i] == '=' || v[i] == '^' || v[i] == '~' || v[i] == '-' || v[i] == '_')) {
		if (v[i] == '_') {
			header = true;
		}
		i++;
	}
	if (i > 0 && i < v.size() && v[i] == '.') {
		v = Trim(v.substr(i + 1));
		return header;
	}
	return false;
}

//! Strip inline markup down to text, for table cells which are flattened into
//! the native {headers, rows} schema.
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
	if (!out.empty() && out.back().element_type == DuckBlockVocabulary::INLINE_TEXT && out.back().attributes.empty()) {
		out.back().content += text;
		return;
	}
	TxInline in;
	in.element_type = DuckBlockVocabulary::INLINE_TEXT;
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
	// Spec 6.0's content rule: a wrapper whose only child is plain text carries
	// the text itself rather than a lone `text` child.
	if (children.size() == 1 && children[0].element_type == DuckBlockVocabulary::INLINE_TEXT &&
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

//! One delimiter pair, e.g. `-del-` or `^sup^`. Returns false when unterminated
//! or when either end sits INSIDE A WORD, which leaves the character to be
//! emitted as ordinary text.
//!
//! THE WORD-BOUNDARY RULE (#90). Without it, ordinary hyphenated English is
//! corrupted and characters are LOST. `a well-known co-author wrote this` paired
//! the two hyphens and produced `a well` + strikethrough `known co` + `author
//! wrote this` -- the sentence split across three nodes and both hyphens eaten.
//! check-wordloss cannot catch that: every WORD survives, only the punctuation
//! dies. Two `snake_case` identifiers in one sentence do the same thing.
//!
//! Measured against RedCloth 4.3.3 -- textile's reference implementation -- and
//! python-textile 4.0.4, which agree with each other and with pandoc. ALL ELEVEN
//! delimiters were affected, not only the `_` and `-` this was reported for.
//!
//! THE TEST IS ON ALPHANUMERICS, deliberately, and not on spaces:
//!
//!   `see (*bold*) here`     -> markup:  `(` and `)` are not alphanumeric
//!   `a -strike-.`           -> markup:  the close is followed by `.`
//!   `a well-known co-b`     -> literal: the open is preceded by `n`
//!   `a *bold*text here`     -> literal: the close is followed by `t`
//!
//! A "must be surrounded by spaces" rule would wrongly reject the first two, both
//! of which the references read as markup.
//!
//! REJECTION IS PER CANDIDATE, not per line. Returning false leaves ParseInlines
//! to emit one character and keep scanning, so `a -del- well-known b` still gets
//! its `del` AND still keeps `well-known` literal -- measured on both references.
//!
//! A failed close GIVES UP rather than searching for a later one: both references
//! read `a *bold*text here` as literal rather than pairing the open with some
//! delimiter further along.
//!
//! `??` is the single delimiter the two references disagree on -- RedCloth makes
//! it a cite intra-word, python-textile leaves it literal. The rule is applied
//! uniformly, so panduck follows python-textile there. A declared divergence on a
//! shape nobody writes beats a special case the next reader has to justify.
bool TryDelim(const std::string &s, size_t &i, const char *open, const char *type, int level,
              std::vector<TxInline> &out, std::string &pending) {
	size_t n = strlen(open);
	if (s.compare(i, n, open) != 0) {
		return false;
	}
	// The OPENING delimiter may not sit inside a word.
	if (i > 0 && std::isalnum(static_cast<unsigned char>(s[i - 1]))) {
		return false;
	}
	size_t close = s.find(open, i + n);
	if (close == std::string::npos) {
		return false;
	}
	// ...and neither may the CLOSING one.
	if (close + n < s.size() && std::isalnum(static_cast<unsigned char>(s[close + n]))) {
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
		// `"text":url` -- the link form, checked first because a bare `"` is common
		// in prose and only the `":` sequence makes it a link.
		if (s[i] == '"') {
			size_t close = s.find("\":", i + 1);
			if (close != std::string::npos) {
				size_t url_end = s.find_first_of(" \t", close + 2);
				if (url_end == std::string::npos) {
					url_end = s.size();
				}
				// Trailing sentence punctuation is not part of the URL. Textile's own
				// rule, and without it every link at the end of a sentence keeps the
				// full stop.
				while (url_end > close + 2 && strchr(".,;:!?", s[url_end - 1])) {
					url_end--;
				}
				TxInline node;
				node.element_type = DuckBlockVocabulary::INLINE_LINK;
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

		// `!image.png!` -- and `!` is also ordinary punctuation, so an unterminated
		// one falls through to text.
		if (s[i] == '!') {
			size_t close = s.find('!', i + 1);
			if (close != std::string::npos && close > i + 1) {
				std::string src = s.substr(i + 1, close - i - 1);
				if (src.find(' ') == std::string::npos) {
					TxInline node;
					node.element_type = DuckBlockVocabulary::INLINE_IMAGE;
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

		// DOUBLE MARKERS FIRST. `**bold**` and `*strong*` both become `bold`,
		// `__italic__` and `_em_` both become `italic` -- textile's distinction is
		// <b> versus <strong>, which duck_block does not carry. Testing the single
		// form first would consume one character of the double form and leave a
		// stray marker in the text.
		if (TryDelim(s, i, "**", DuckBlockVocabulary::INLINE_BOLD, level, out, pending) ||
		    TryDelim(s, i, "__", DuckBlockVocabulary::INLINE_ITALIC, level, out, pending) ||
		    TryDelim(s, i, "??", DuckBlockVocabulary::INLINE_CITE, level, out, pending) ||
		    TryDelim(s, i, "*", DuckBlockVocabulary::INLINE_BOLD, level, out, pending) ||
		    TryDelim(s, i, "_", DuckBlockVocabulary::INLINE_ITALIC, level, out, pending) ||
		    TryDelim(s, i, "@", DuckBlockVocabulary::INLINE_CODE, level, out, pending) ||
		    TryDelim(s, i, "-", DuckBlockVocabulary::INLINE_STRIKETHROUGH, level, out, pending) ||
		    TryDelim(s, i, "+", DuckBlockVocabulary::INLINE_UNDERLINE, level, out, pending) ||
		    TryDelim(s, i, "^", DuckBlockVocabulary::INLINE_SUPERSCRIPT, level, out, pending) ||
		    TryDelim(s, i, "~", DuckBlockVocabulary::INLINE_SUBSCRIPT, level, out, pending) ||
		    TryDelim(s, i, "%", DuckBlockVocabulary::INLINE_SPAN, level, out, pending)) {
			continue;
		}

		pending += s[i];
	}
	PushText(out, pending, level);
}

void AttachInlines(TxBlock &block, const std::string &text) {
	std::vector<TxInline> inl;
	ParseInlines(text, block.level + 1, inl);
	if (inl.size() == 1 && inl[0].element_type == DuckBlockVocabulary::INLINE_TEXT && inl[0].attributes.empty()) {
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
				b.element_type = DuckBlockVocabulary::TYPE_HEADING;
				b.level = 1;
				b.attributes[DuckBlockVocabulary::ATTR_HEADING_LEVEL] = std::to_string(ln.level);
				// AN EXPLICIT ID WINS over the derived slug. `h1(#guide-title).` states
				// its anchor; the slugifier only guesses one from the heading text, and
				// the two agree by luck rather than by rule.
				auto id = ln.id.empty() ? HeadingSlug(ln.text, '-') : ln.id;
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
				q.element_type = DuckBlockVocabulary::TYPE_BLOCKQUOTE;
				q.level = 1;
				AddBlockAttrs(q, ln);
				blocks_.push_back(std::move(q));
				TxBlock p;
				p.element_type = DuckBlockVocabulary::TYPE_PARAGRAPH;
				p.level = 2;
				AttachInlines(p, ln.text);
				blocks_.push_back(std::move(p));
				break;
			}
			case LineKind::CODE: {
				Flush();
				CloseLists();
				TxBlock c;
				c.element_type = DuckBlockVocabulary::TYPE_CODE;
				c.level = 1;
				c.content = ln.text;
				// A `bc.` BLOCK RUNS TO THE NEXT BLANK LINE. Taking only the marker's
				// own line truncated every multi-line listing and let the rest leak out
				// as PROSE -- `bc(python). def hello():` gave a code block holding the
				// signature and a paragraph holding "return 1".
				//
				// python-textile 4.0.2 settles it:
				//   bc(python). def hello():
				//       return 1
				// -> <pre class="python"><code class="python">def hello():
				//    return 1</code></pre>
				//
				// `raw` rather than `text`, because the continuation's INDENTATION is
				// content: "    return 1" read back as "return 1" is a different
				// program.
				while (i + 1 < lines.size() && lines[i + 1].kind != LineKind::BLANK) {
					c.content += "\n" + lines[i + 1].raw;
					i++;
				}
				AddBlockAttrs(c, ln);
				// On a code block the class IS the language -- it is what the reference
				// emits as <code class="python"> and what pandoc reads as a CodeBlock's
				// first class. Recorded under the name a consumer looks for, rather
				// than left as a generic class it would have to know to interpret.
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
				// pandoc keeps `notextile.` as the paragraph's first word AND parses
				// the body as textile regardless -- so it both advertises the marker to
				// the reader and does the one thing the construct exists to prevent.
				// Measured against python-textile 4.0.2, which strips the marker and
				// passes the body through.
				//
				// `html` because that is what a notextile body is for: markup the
				// author wants delivered verbatim.
				TxBlock r;
				r.element_type = DuckBlockVocabulary::TYPE_RAW;
				r.level = 1;
				r.content = ln.text;
				// The format never lives in `encoding`: duck_block_utils measured that
				// a raw block's encoding is `text` even for html and latex, which ARE
				// declared encodings, and made it a flat rule rather than a fallback.
				r.attributes["format"] = "html";
				r.attributes[DuckBlockVocabulary::ATTR_SOURCE_TYPE] = "notextile";
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
				r.element_type = DuckBlockVocabulary::TYPE_RAW;
				r.level = 1;
				r.content = ln.text;
				// The format never lives in `encoding`: duck_block_utils measured that
				// a raw block's encoding is `text` even for html and latex, which ARE
				// declared encodings, and made it a flat rule rather than a fallback.
				r.attributes["format"] = "html";
				r.attributes[DuckBlockVocabulary::ATTR_SOURCE_TYPE] = ln.markers;
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
		// pandoc wraps an attributed block in a Div and puts the style there.
		// panduck keeps the block's OWN type and carries the attribute on it -- a
		// styled paragraph is still a paragraph, and wrapping it changes the
		// document's shape to record a colour.
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
		b.element_type = DuckBlockVocabulary::TYPE_PARAGRAPH;
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
		// A CHANGE OF LIST TYPE AT THE SAME DEPTH STARTS A NEW LIST, which is the
		// divergence this reader carries. pandoc turns `* bullet` followed by `#
		// ordered` into a PARAGRAPH containing a literal asterisk -- the list is
		// lost and its marker becomes prose. python-textile keeps a list. Sibling
		// lists lose nothing and describe what the author wrote; the reference
		// nests them, but its own output there places an <ol> directly inside a
		// <ul>, which is invalid HTML and so not authoritative.
		while (open_.size() > common) {
			open_.pop_back();
		}
		for (size_t k = common; k < m.size(); k++) {
			TxBlock b;
			b.element_type = DuckBlockVocabulary::TYPE_LIST;
			b.level = static_cast<int>(2 * k + 1);
			const char *lt = m[k] == '#'   ? DuckBlockVocabulary::LIST_TYPE_ORDERED
			                 : m[k] == '-' ? DuckBlockVocabulary::LIST_TYPE_DEFINITION
			                               : DuckBlockVocabulary::LIST_TYPE_BULLET;
			b.attributes[DuckBlockVocabulary::ATTR_LIST_TYPE] = lt;
			b.attributes[DuckBlockVocabulary::ATTR_ORDERED_LEGACY] =
			    lt == std::string(DuckBlockVocabulary::LIST_TYPE_ORDERED) ? "true" : "false";
			if (lt == std::string(DuckBlockVocabulary::LIST_TYPE_ORDERED)) {
				b.attributes["start"] = "1";
				b.attributes["number_style"] = "Decimal";
				b.attributes["number_delim"] = "Period";
			}
			blocks_.push_back(std::move(b));
			open_.push_back(m[k]);
		}

		if (ln.definition) {
			TxBlock term;
			term.element_type = DuckBlockVocabulary::TYPE_LIST_ITEM;
			term.level = static_cast<int>(2 * m.size());
			term.attributes[DuckBlockVocabulary::ATTR_ROLE] = DuckBlockVocabulary::ROLE_TERM;
			AttachInlines(term, ln.term);
			blocks_.push_back(std::move(term));

			TxBlock def;
			def.element_type = DuckBlockVocabulary::TYPE_LIST_ITEM;
			def.level = static_cast<int>(2 * m.size());
			def.attributes[DuckBlockVocabulary::ATTR_ROLE] = DuckBlockVocabulary::ROLE_DEFINITION;
			AttachInlines(def, ln.text);
			blocks_.push_back(std::move(def));
			return;
		}

		TxBlock item;
		item.element_type = DuckBlockVocabulary::TYPE_LIST_ITEM;
		item.level = static_cast<int>(2 * m.size());
		AttachInlines(item, ln.text);
		blocks_.push_back(std::move(item));
	}

	void CloseLists() {
		open_.clear();
	}

	//! Consume a run of `| ... |` rows into one native table. `_.` marks a header
	//! cell.
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
				if (StripCellModifier(v)) {
					is_header = true;
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
		b.element_type = DuckBlockVocabulary::TYPE_TABLE;
		b.level = 1;
		b.encoding = DuckBlockVocabulary::ENCODING_JSON;
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


std::vector<Block> ReadTextile(const std::string &src) {
	std::vector<Block> rows;
	int32_t order = 0;
	for (auto &block : ParseTextileString(src)) {
		Block row;
		row.kind = block.kind.empty() ? DuckBlockVocabulary::KIND_BLOCK : block.kind;
		row.element_type = block.element_type;
		row.content = block.content;
		// ENCODING SET EXPLICITLY, not defaulted. TxRow defaulted this field to
		// ENCODING_TEXT and the loop only overrode it when the block carried one;
		// panduck::Block defaults it to empty, because a shared vocabulary type
		// should not carry one format's default. Without this line every textile
		// row would ship a blank encoding -- a silent data change no compiler
		// catches. Same trap as ipynb.
		row.encoding = block.encoding.empty() ? DuckBlockVocabulary::ENCODING_TEXT : block.encoding;
		row.attributes = block.attributes;
		row.level = block.level > 0 ? block.level : 1;
		row.element_order = order++;
		const int32_t block_level = row.level;
		rows.push_back(std::move(row));

		for (auto &inl : block.inlines) {
			Block child;
			// Same reason as above: the old struct's default supplied this.
			child.encoding = DuckBlockVocabulary::ENCODING_TEXT;
			child.kind = DuckBlockVocabulary::KIND_INLINE;
			child.element_type = inl.element_type;
			child.content = inl.content;
			child.attributes = inl.attributes;
			child.level = inl.level > 0 ? inl.level : block_level + 1;
			child.element_order = order++;
			rows.push_back(std::move(child));
		}
	}
	return rows;
}

} // namespace textile
} // namespace panduck
