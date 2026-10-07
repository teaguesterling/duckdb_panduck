#include "panduck/org.hpp"

#include "panduck/block_json.hpp"
#include "panduck/org_scanner.hpp"
#include "panduck/vocabulary.hpp"

// <cctype> (isspace) and <utility> (std::move, std::pair) are NAMED HERE even
// though src/org_reader.cpp never named them -- there they arrived through
// duckdb.hpp. MEASURED after the move: the standalone build still compiles with
// either one deleted, because <cctype> comes in via libstdc++'s <string> and
// <utility> via panduck/block.hpp. So unlike textile's <cstring> this was not a
// break, only an undeclared dependency. Declared anyway: what the file uses and
// what its includes promise should be the same set, and the next edit to
// block.hpp should not be able to break org.
#include <cctype>
#include <map>
#include <string>
#include <utility>
#include <vector>

// The parse core and the row flattening, moved out of src/org_reader.cpp
// (issue #104, L2). Behaviour unchanged: the only edits are the namespace
// (duckdb::org -> panduck::org), the DuckBlockTypes:: -> DuckBlockVocabulary::
// (the same constants; DuckBlockTypes merely adds the LogicalType/Value helpers
// that cannot cross the seam), and OrgRow -> panduck::Block.
//
// NOTE ON Builder::Block. The member function named `Block` shadows
// ::panduck::Block inside the class, which is legal and harmless -- the flatten
// below is outside Builder, so `Block` there is the vocabulary row. Renaming the
// member would be churn in a commit whose whole claim is that nothing changed.

namespace panduck {
namespace org {

namespace {

//! Org's inline markers, each mapping to a duck_block inline type.
//!
//! `=code=` and `~verbatim~` BOTH become `code`. Pandoc distinguishes them by a
//! class --
//! `=code=` carries ["verbatim"] and `~verbatim~` carries none, the opposite
//! way round from the names -- and duck_block's `code` inline has no class
//! field. Collapsing them is deliberate and declared; the surprising direction
//! is recorded because it is exactly what a later reader would "correct" the
//! wrong way.
struct InlineMarker {
	char open;
	const char *element_type;
};
const InlineMarker MARKERS[] = {
    {'*', DuckBlockVocabulary::INLINE_BOLD},      {'/', DuckBlockVocabulary::INLINE_ITALIC},
    {'_', DuckBlockVocabulary::INLINE_UNDERLINE}, {'=', DuckBlockVocabulary::INLINE_CODE},
    {'~', DuckBlockVocabulary::INLINE_CODE},      {'+', DuckBlockVocabulary::INLINE_STRIKETHROUGH},
};

const char *MarkerType(char c) {
	for (auto &m : MARKERS) {
		if (m.open == c) {
			return m.element_type;
		}
	}
	return nullptr;
}

//! A marker only opens when it is at a word boundary and its content is
//! non-empty. Without that, `a * b` and `2 + 2` become emphasis, and arithmetic
//! in a paragraph turns into markup -- the most common false positive in every
//! lightweight markup reader.
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
	run.element_type = DuckBlockVocabulary::INLINE_TEXT;
	run.content = text;
	run.level = level;
	out.push_back(std::move(run));
}

//! Split a line's text into inline runs. A FLAT scan: Org's emphasis markers do
//! not nest in practice and pandoc does not nest them either, so a stack would
//! model something the format does not have.
void ParseInlines(const std::string &s, int level, std::vector<OrgInline> &out) {
	std::string plain;
	size_t i = 0;
	while (i < s.size()) {
		// `[[url][label]]` and `[[url]]` -- checked first, because a link's target
		// can contain any of the emphasis characters and must not be scanned for
		// them.
		if (s.compare(i, 2, "[[") == 0) {
			auto close = s.find("]]", i + 2);
			if (close != std::string::npos) {
				auto body = s.substr(i + 2, close - i - 2);
				auto sep = body.find("][");
				OrgInline link;
				link.element_type = DuckBlockVocabulary::INLINE_LINK;
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
			// A closing marker must not be preceded by a space, or `* a *` in prose
			// would close a run the author never opened.
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
				Simple(DuckBlockVocabulary::TYPE_HR);
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
				// A text line inside an open list CONTINUES its item rather than
				// starting a paragraph -- that is what an indented continuation line
				// means in Org.
				para_.push_back(line.text);
				continue;
			case LineKind::DRAWER_BEGIN: {
				// Skip to :END:. An UNTERMINATED drawer stops at the next blank line
				// rather than eating the rest of the document -- the same runaway the
				// LaTeX reader's tabular walker had, avoided here by bounding the scan.
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
	//! Open lists, innermost last: {source indent, structural level of the list
	//! block}.
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
			// A SOFT LINE BREAK IS A WORD BOUNDARY. Org wraps prose freely and the
			// newline carries no meaning, so joining with a space is what the
			// document says.
			text += (i ? " " : "") + para_[i];
		}
		para_.clear();
		if (lists_.empty()) {
			Emit(DuckBlockVocabulary::TYPE_PARAGRAPH, text, Depth());
			return;
		}
		// Inside a list, a continuation line belongs to the item that is already
		// open, so it is appended there rather than becoming a sibling paragraph.
		auto &item = blocks_.back();
		if (item.element_type == DuckBlockVocabulary::TYPE_LIST_ITEM && item.inlines.empty() && item.content.empty()) {
			item.content = text;
			return;
		}
		Emit(DuckBlockVocabulary::TYPE_PARAGRAPH, text, Depth());
	}

	//! Emit a block whose text may carry inline markup. A run with no markup
	//! becomes the block's `content` -- duck_block's rule since v1 -- and
	//! anything richer becomes inline children.
	void Emit(const char *type, const std::string &text, int level, const char *role = nullptr) {
		OrgBlock b;
		b.element_type = type;
		b.level = level;
		if (role) {
			b.role = role;
		}
		std::vector<OrgInline> runs;
		ParseInlines(text, level + 1, runs);
		if (runs.size() == 1 && runs[0].element_type == DuckBlockVocabulary::INLINE_TEXT) {
			b.content = runs[0].content;
		} else {
			b.inlines = std::move(runs);
		}
		blocks_.push_back(std::move(b));
	}

	void Heading(const Line &line) {
		OrgBlock b;
		b.element_type = DuckBlockVocabulary::TYPE_HEADING;
		b.heading_level = line.level;
		b.level = 1;
		std::vector<OrgInline> runs;
		ParseInlines(line.text, 2, runs);
		if (runs.size() == 1 && runs[0].element_type == DuckBlockVocabulary::INLINE_TEXT) {
			b.content = runs[0].content;
		} else {
			// A HEADING CARRIES BOTH: a flattened title in `content` AND the rich
			// inline children beside it (duck_block ruling d003d32).
			//
			// Flattening alone loses formatting irreversibly -- `**Bold** title` and
			// `Bold title` become byte-identical, so a round trip rewrites the first
			// as the second. Children alone break every consumer that reads a title
			// from `content`, which doc_toc does.
			//
			// The structure marks itself and needs no new vocabulary: a lone text
			// child lives in `content` and produces NO children, so children
			// alongside non-empty content can only mean the content is a DERIVED
			// flattening. CHILDREN ARE AUTHORITATIVE when both are present.
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
		const char *want = line.definition ? DuckBlockVocabulary::LIST_TYPE_DEFINITION
		                   : line.ordered  ? DuckBlockVocabulary::LIST_TYPE_ORDERED
		                                   : DuckBlockVocabulary::LIST_TYPE_BULLET;
		if (lists_.empty() || lists_.back().first < line.level) {
			OrgBlock list;
			list.element_type = DuckBlockVocabulary::TYPE_LIST;
			list.list_type = want;
			list.level = Depth();
			if (line.ordered) {
				// ALWAYS emitted, including at their defaults, matching the stricter of
				// the two upstream producers so there is one shape rather than two.
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
			// ONE ITEM PRODUCES TWO ROWS, as in the EPUB and LaTeX readers: `- term
			// :: def` is a term and a definition, and the term is the half carrying
			// the meaning.
			Emit(DuckBlockVocabulary::TYPE_LIST_ITEM, line.term, item_level, DuckBlockVocabulary::ROLE_TERM);
			Emit(DuckBlockVocabulary::TYPE_LIST_ITEM, line.text, item_level, DuckBlockVocabulary::ROLE_DEFINITION);
			return;
		}
		Emit(DuckBlockVocabulary::TYPE_LIST_ITEM, line.text, item_level);
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
			// The body is taken VERBATIM -- a source block's indentation and blank
			// lines are its content, so the scanner's classification of those lines
			// is ignored here.
			body += (body.empty() ? "" : "\n") + lines[j].text;
		}
		i = j; // the END line, or the last line for an unterminated block
		if (name == "QUOTE") {
			OrgBlock b;
			b.element_type = DuckBlockVocabulary::TYPE_BLOCKQUOTE;
			b.level = Depth();
			blocks_.push_back(std::move(b));
			Emit(DuckBlockVocabulary::TYPE_PARAGRAPH, body, Depth() + 1);
			return;
		}
		OrgBlock b;
		b.element_type = DuckBlockVocabulary::TYPE_CODE;
		b.content = body;
		b.level = Depth();
		if (name == "SRC" && !arg.empty()) {
			// `#+BEGIN_EXAMPLE` gets NO language: pandoc gives it the class
			// "example", which is not a language, and recording it as one would make
			// a consumer highlight text as a dialect that does not exist.
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
		// A RULE AFTER THE FIRST ROW PROMOTES IT. Measured against pandoc for Org
		// rather than carried over from the LaTeX reader -- the formats are
		// unrelated and pandoc's readers share no logic. They agree, but as a
		// measurement.
		if (rows.size() > 1 && ruled[0]) {
			headers = rows[0];
			first = 1;
		}
		std::vector<std::vector<std::string>> body(rows.begin() + (long)first, rows.end());
		OrgBlock b;
		b.element_type = DuckBlockVocabulary::TYPE_TABLE;
		b.content = BuildTableJson(headers, body);
		b.encoding = DuckBlockVocabulary::ENCODING_JSON;
		b.level = Depth();
		blocks_.push_back(std::move(b));
	}

	void Keyword(const Line &line) {
		if (line.key != "TITLE" && line.key != "AUTHOR" && line.key != "DATE") {
			// EVERY OTHER #+KEY: IS HELD RAW, not dropped.
			//
			// This returned early -- "every other #+KEY: is an option, not document
			// metadata"
			// -- which is true and was the wrong conclusion. Not being metadata does
			// not make it not content. MEASURED: pandoc emits RawBlock ["org",
			// "#+notarealkeyword: X"] for an unrecognised keyword, so it occupies a
			// block position, and panduck was silently discarding #+CAPTION:,
			// #+ATTR_HTML: and every custom keyword a document carries.
			//
			// Found by duck_block_utils, who tested a claim I made about org keywords
			// never being in the block flow. The claim was wrong and this was hiding
			// behind it.
			OrgBlock b;
			b.element_type = DuckBlockVocabulary::TYPE_RAW;
			b.level = 1;
			// VERBATIM, not reconstructed: the scanner upper-cases `key`, and a `raw`
			// block rebuilt from it would report #+NOTAREALKEYWORD for a source that
			// wrote
			// #+notarealkeyword.
			b.content = line.raw.empty() ? "#+" + line.key + ": " + line.text : line.raw;
			// THE FORMAT NAME IS AN ATTRIBUTE, NOT AN `encoding`. duck_block
			// validates encoding against a closed set -- text, json, yaml, html, xml,
			// latex, markdown, toml -- with no `org` in it, so encoding='org' is not
			// conformant. The converter already reads attributes['format'] to build
			// RawBlock [<format>, ...], which is the same lesson the mediawiki reader
			// learned.
			b.attributes["format"] = "org";
			blocks_.push_back(std::move(b));
			return;
		}
		std::string key = line.key == "TITLE" ? "title" : line.key == "AUTHOR" ? "author" : "date";
		auto it = meta_.find(key);
		if (it == meta_.end()) {
			meta_[key] = line.text;
			return;
		}
		// REPEATED #+AUTHOR: CONCATENATES into ONE value -- measured. LaTeX's
		// \author yields a MetaList for the same logical field, so this reader
		// cannot generalise from that one. Joined with a space here; pandoc uses a
		// SoftBreak node, which duck_block has as an inline type but which a single
		// flattened value cannot carry.
		it->second += " " + line.text;
	}

	void EmitMetadata() {
		// AFTER the blocks -- spec 6.2 makes body-then-metadata a contract.
		// std::map iterates sorted, which is also pandoc's Meta serialisation
		// order.
		for (auto &kv : meta_) {
			OrgBlock b;
			b.kind = DuckBlockVocabulary::KIND_VALUE;
			b.element_type = DuckBlockVocabulary::VALUE_INLINES;
			b.key = kv.first;
			b.level = 1;
			if (!kv.second.empty()) {
				// An EMPTY #+AUTHOR: emits the key with NO child, matching pandoc's
				// MetaInlines []. Present-and-empty is not absent.
				OrgInline run;
				run.element_type = DuckBlockVocabulary::INLINE_TEXT;
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

std::vector<Block> ReadOrg(const std::string &src) {
	std::vector<Block> rows;
	int32_t order = 0;
	for (auto &block : ParseOrgString(src)) {
		Block row;
		row.kind = block.kind.empty() ? DuckBlockVocabulary::KIND_BLOCK : block.kind;
		row.element_type = block.element_type;
		row.content = block.content;
		// ENCODING SET EXPLICITLY, not defaulted. OrgRow defaulted this field to
		// ENCODING_TEXT and the loop only overrode it when the block carried one
		// (`if (!block.encoding.empty())`); panduck::Block defaults it to empty,
		// because a shared vocabulary type should not carry one format's default.
		// Without this line every org row would ship a blank encoding -- a silent
		// data change no compiler catches. Third reader in a row to hit it, after
		// ipynb and textile.
		row.encoding = block.encoding.empty() ? DuckBlockVocabulary::ENCODING_TEXT : block.encoding;
		row.element_order = order++;
		for (auto &kv : block.attributes) {
			row.attributes[kv.first] = kv.second;
		}
		if (block.heading_level > 0) {
			row.attributes[DuckBlockVocabulary::ATTR_HEADING_LEVEL] = std::to_string(block.heading_level);
		}
		if (!block.key.empty()) {
			row.attributes[DuckBlockVocabulary::ATTR_KEY] = block.key;
		}
		if (!block.role.empty()) {
			row.attributes[DuckBlockVocabulary::ATTR_ROLE] = block.role;
		}
		if (!block.language.empty()) {
			row.attributes["language"] = block.language;
		}
		if (!block.list_type.empty()) {
			// BOTH SPELLINGS, as every other panduck reader emits: `ordered` is the
			// v1 name and `list_type` the later alias, and a consumer written against
			// either reads this output.
			row.attributes[DuckBlockVocabulary::ATTR_ORDERED_LEGACY] =
			    block.list_type == DuckBlockVocabulary::LIST_TYPE_ORDERED ? "true" : "false";
			row.attributes[DuckBlockVocabulary::ATTR_LIST_TYPE] = block.list_type;
			if (!block.list_start.empty()) {
				row.attributes["start"] = block.list_start;
				row.attributes["number_style"] = block.number_style;
				row.attributes["number_delim"] = block.number_delim;
			}
		}
		const int32_t block_level = block.level > 0 ? block.level : 1;
		row.level = block_level;
		rows.push_back(std::move(row));

		for (auto &inl : block.inlines) {
			Block child;
			// Same reason as above: OrgRow's default supplied this, and the inline
			// loop never assigned encoding at all.
			child.encoding = DuckBlockVocabulary::ENCODING_TEXT;
			child.kind = DuckBlockVocabulary::KIND_INLINE;
			child.element_type = inl.element_type;
			child.content = inl.content;
			child.level = inl.level > 0 ? inl.level : block_level + 1;
			child.element_order = order++;
			if (!inl.href.empty()) {
				child.attributes["href"] = inl.href;
			}
			rows.push_back(std::move(child));
		}
	}
	return rows;
}

} // namespace org
} // namespace panduck
