#include "panduck/rst.hpp"

#include "panduck/block_json.hpp"
#include "panduck/rst_scanner.hpp"
#include "panduck/slugify.hpp"
#include "panduck/vocabulary.hpp"

// INCLUDES NAMED RATHER THAN INHERITED. src/rst_reader.cpp named <algorithm>,
// <fstream> and <map>; of those the core uses only std::min, while <cctype>
// (tolower), <cstddef> (size_t, 50 sites) and <utility> (std::move) arrived
// transitively through duckdb.hpp. <fstream> and <map> belonged to the tail and
// stay there.
//
// MEASURED, not assumed: the standalone build still compiles with any one of
// these four deleted, because libstdc++ pulls them in behind <string>,
// <vector> and panduck/block.hpp. So unlike textile's <cstring> none of them
// was a break -- they were undeclared dependencies, named here so that what the
// file uses and what its includes promise are the same set.
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

// The parse core and the row flattening, moved out of src/rst_reader.cpp
// (issue #104, L2). Behaviour unchanged: the only edits are the namespace
// (duckdb::rst -> panduck::rst), the DuckBlockTypes:: -> DuckBlockVocabulary::
// rename (the same constants; DuckBlockTypes merely adds the LogicalType/Value
// helpers that cannot cross the seam), and RstRow -> panduck::Block.

namespace panduck {
namespace rst {

namespace {

//! The CAP on how long an adornment run must be to make a SECTION (#87, #88).
//!
//! The threshold is `min(RST_MIN_SECTION_ADORNMENT, title length)`, not this
//! number alone. #87 recorded a flat four-character floor and said outright that
//! the title's own length was not part of it. That was WRONG, and wrong in the
//! direction that keeps a bug alive: it is correct for every enumerated title,
//! because `1. a` is already four characters, so the min collapses to 4 and the
//! flat rule can never be caught out there. A plain TEXT title can be shorter.
//! Measured on docutils 0.20.1:
//!
//!   `Doc`   / `===`  (3,3)  -> TITLE      3 >= min(4,3)
//!   `Ab`    / `--`   (2,2)  -> TITLE      2 >= min(4,2)
//!   `Abc`   / `--`   (3,2)  -> paragraph  2 <  min(4,3)
//!   `Abcde` / `----` (5,4)  -> TITLE      4 >= min(4,5)
//!   `Abcde` / `---`  (5,3)  -> paragraph  3 <  min(4,5)
//!
//! So a run reaching the title's length always underlines it, and four characters
//! suffice however long the title is. Only pandoc requires the run to reach the
//! title -- it reads `Abcde` / `----` as prose where docutils reads a section --
//! and panduck follows docutils, as #87 ruled.
//!
//! DOCUTILS HAS A SECOND, LOWER MINIMUM, and the two are easy to conflate:
//!
//!   2  a run is an adornment AT ALL -- `rst_scanner.cpp`'s IsAdornment, which is
//!      why a lone `::` reaches the reader as an ADORNMENT line. A one-character
//!      run is a section to docutils but is invisible to this reader, which is a
//!      scanner-level divergence and not this constant's business.
//!
//! `::` is NOT specially excluded here, and must not be: under a title of two
//! characters or fewer it genuinely IS a section underline, and all three
//! implementations agree it is. Under anything longer it falls below the
//! threshold and folds into the paragraph, where flush()'s end-of-paragraph rule
//! opens the literal block.
constexpr size_t RST_MIN_SECTION_ADORNMENT = 4;

void PushText(std::vector<RstInline> &out, const std::string &text, int level) {
	if (text.empty()) {
		return;
	}
	RstInline run;
	run.element_type = DuckBlockVocabulary::INLINE_TEXT;
	run.content = text;
	run.level = level;
	out.push_back(std::move(run));
}

//! Split text into inline runs. `**strong**` is tested BEFORE `*emph*`: the
//! shorter marker is a prefix of the longer one, so checking emphasis first
//! turns every bold run into an empty italic followed by stray asterisks.
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
				emit(DuckBlockVocabulary::INLINE_CODE, s.substr(i + 2, close - i - 2), std::string());
				i = close + 2;
				continue;
			}
		}
		if (s.compare(i, 2, "**") == 0) {
			auto close = s.find("**", i + 2);
			if (close != std::string::npos && close > i + 2) {
				emit(DuckBlockVocabulary::INLINE_BOLD, s.substr(i + 2, close - i - 2), std::string());
				i = close + 2;
				continue;
			}
		}
		if (s[i] == '*') {
			auto close = s.find('*', i + 1);
			if (close != std::string::npos && close > i + 1) {
				emit(DuckBlockVocabulary::INLINE_ITALIC, s.substr(i + 1, close - i - 1), std::string());
				i = close + 1;
				continue;
			}
		}
		if (s[i] == '`') {
			// `text <url>`_ -- the trailing underscore is what makes it a link rather
			// than interpreted text, so it is required here.
			auto close = s.find("`_", i + 1);
			if (close != std::string::npos) {
				auto body = s.substr(i + 1, close - i - 1);
				auto lt = body.rfind(" <");
				std::string text = body, href = body;
				if (lt != std::string::npos && !body.empty() && body.back() == '>') {
					text = body.substr(0, lt);
					href = body.substr(lt + 2, body.size() - lt - 3);
				}
				emit(DuckBlockVocabulary::INLINE_LINK, text, href);
				i = close + 2;
				// A named reference may be spelled with two underscores; consume the
				// second.
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

//! A table cell's text with its inline markup RESOLVED, not carried literally.
//!
//! `**a**` is a cell containing `a`, not a cell containing four asterisks.
//! panduck's own sibling already answers this: webbed reads `<td><b>a</b></td>`
//! as `"a"`. So this is not a vocabulary question needing a ruling -- there is
//! a worked answer in the fleet, and .rst was the reader diverging from it.
//!
//! REUSES ParseInlines RATHER THAN STRIPPING CHARACTERS. The resolver already
//! exists and is used for paragraph text; it simply was never reached on the
//! cell path. Deleting `*` by hand would also eat a literal asterisk in prose,
//! and would need re-deciding every time rst grows a marker.
//!
//! FLATTENED TO TEXT because a cell in the native {headers, rows} schema is a
//! STRING. The emphasis is lost either way; the choice is only whether the
//! markers are lost with it. Reported by duckeye (#38), who found it against
//! pandoc, which resolves the same cell.
std::string CellText(const std::string &s) {
	std::vector<RstInline> runs;
	ParseInlines(s, 2, runs);
	std::string out;
	for (auto &r : runs) {
		out += r.content;
	}
	return out;
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

//! A simple table's cells come from COLUMN POSITIONS, not delimiters -- the
//! rule row's runs locate them. This is the only positional extraction in the
//! reader, and it is why the rule line's offsets are carried on the Line.
//!
//! Each cell spans [start of its run, start of the NEXT run), and the last runs
//! to the end of the line. Earlier this walked forward as `at += width + 2`,
//! assuming two spaces between columns. RST separates them by ONE OR MORE, so
//! on a single-space table the offset drifted by a character per column and the
//! error accumulated:
//!
//!     ==== ===== ====        headers  ["Name", "alue", "te"]
//!     Name Value Note        rows     [["a", "", ""], ["b", "", ""]]
//!
//! -- the first column right, the second short a character, the third short
//! two, and every data row past column one empty. Reading the boundaries
//! instead of predicting them means there is no gap to guess.
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
		Run(0, lines_.size(), 1, 0);
		return std::move(blocks_);
	}

private:
	std::vector<Line> lines_;
	std::vector<RstBlock> blocks_;
	//! ADORNMENT CHARACTERS IN ORDER OF FIRST APPEARANCE. RST sets a heading's
	//! level by WHERE its adornment first appeared in the document, not by which
	//! character it is -- measured, and the opposite of the usual assumption that
	//! `=` is level 1. A reader that hardcodes the conventional order is right on
	//! conventional documents and wrong on valid ones.
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
		if (runs.size() == 1 && runs[0].element_type == DuckBlockVocabulary::INLINE_TEXT) {
			b.content = runs[0].content;
		} else if (!runs.empty()) {
			b.inlines = std::move(runs);
		}
		blocks_.push_back(std::move(b));
	}

	//! The indented run starting at `i`, as [i, end) -- every following line
	//! indented deeper than `base`, blank lines included. RST expresses
	//! containment by indent and four constructs share the shape, so finding the
	//! run is one function.
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

	//! The shallowest indent among the non-blank lines of [from, to): a nested
	//! run's own left margin. `fallback` when the run is all blank.
	int MinIndent(size_t from, size_t to, int fallback) const {
		int m = -1;
		for (size_t j = from; j < to && j < lines_.size(); j++) {
			if (lines_[j].kind != LineKind::BLANK && (m < 0 || lines_[j].indent < m)) {
				m = lines_[j].indent;
			}
		}
		return m < 0 ? fallback : m;
	}

	//! Parse [from, to) at `depth`. `base` is the run's OWN LEFT MARGIN -- 0 for
	//! the document, a directive body's indent, a quote's, a list item's text
	//! column. RST expresses containment by indentation alone, so the margin is
	//! what makes a line "indented" at all (#64).
	void Run(size_t from, size_t to, int depth, int base) {
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
			// `::` AT THE END OF A PARAGRAPH opens a literal block AND stays as a
			// colon in the prose -- docutils drops one colon and keeps the other. A
			// bare `::` on its own paragraph disappears entirely.
			if (text.size() >= 2 && text.compare(text.size() - 2, 2, "::") == 0) {
				literal_pending = true;
				text = text.size() == 2 ? std::string() : text.substr(0, text.size() - 1);
			}
			if (!text.empty()) {
				Emit(DuckBlockVocabulary::TYPE_PARAGRAPH, text, depth);
			}
		};

		for (size_t i = from; i < to; i++) {
			auto &line = lines_[i];
			// A LITERAL BLOCK claims the indented run after a `::` paragraph,
			// whatever its first line looks like -- code can open with `- ` or
			// `+--+`. Checked before the quote rule, which would otherwise take it
			// (#64).
			if (literal_pending && line.kind != LineKind::BLANK) {
				literal_pending = false;
				if (line.indent > base) {
					size_t end = std::min(IndentedRun(i, base), to);
					RstBlock b;
					b.element_type = DuckBlockVocabulary::TYPE_CODE;
					b.content = RawBody(i, end, line.indent);
					b.level = depth;
					blocks_.push_back(std::move(b));
					i = end - 1;
					continue;
				}
			}
			// A BLOCK QUOTE is an indented run that nothing above it claims (#64).
			// Every claim is made by the line that OPENS a run -- a definition term,
			// a field, a list item, a directive, a comment, a `::` -- and each
			// consumes its run before control returns here. So an unclaimed line
			// indented past this run's margin, at the START of a block, is a quote.
			// Before #64 it read as a level-1 paragraph, and duck_blocks_to_md lost
			// the `>`.
			if (para.empty() && line.kind != LineKind::BLANK && line.indent > base) {
				size_t end = std::min(IndentedRun(i, base), to);
				RstBlock q;
				q.element_type = DuckBlockVocabulary::TYPE_BLOCKQUOTE;
				q.level = depth;
				blocks_.push_back(std::move(q));
				Run(i, end, depth + 1, MinIndent(i, end, line.indent));
				i = end - 1;
				continue;
			}
			switch (line.kind) {
			case LineKind::BLANK:
				flush();
				continue;
			case LineKind::COMMENT: {
				// A ONE-LINE FOOTNOTE OR CITATION carries its body on the LABEL LINE:
				// `.. [1] text`. The scanner files that whole line under COMMENT with
				// `[1] text` as its text, and the body rule below only ever looked at
				// the NEXT line -- so this text went nowhere (#67). Measured on main
				// before this: `.. [1] The footnote body.` emitted NOTHING, and the
				// wrapped form emitted only its continuation line, losing the label
				// line's words.
				//
				// SEEDING `para` RATHER THAN EMITTING is what joins the label line with
				// an indented continuation into ONE paragraph, as pandoc does. The
				// quote rule and the definition-list branch both require an empty
				// `para`, so the following indented line falls through to TEXT and
				// appends here. The run is deliberately NOT consumed.
				if (!line.text.empty() && line.text[0] == '[') {
					size_t close = line.text.find(']');
					if (close != std::string::npos) {
						size_t b = line.text.find_first_not_of(" \t", close + 1);
						if (b != std::string::npos) {
							// The scanner already trimmed this line, so the remainder needs
							// no trim.
							para.push_back(line.text.substr(b));
							continue;
						}
					}
				}
				// A COMMENT'S BODY is the indented run that starts on the VERY NEXT
				// line. A blank line straight after the comment means it has none, and
				// the run after the blank is a quote
				// -- the `..` + blank idiom. Measured against pandoc; before #64 the
				// body leaked into the document as prose.
				if (i + 1 < to && lines_[i + 1].kind != LineKind::BLANK && lines_[i + 1].indent > line.indent) {
					size_t end = std::min(IndentedRun(i + 1, line.indent), to);
					// A FOOTNOTE OR CITATION BODY IS DOCUMENT TEXT, not commentary. The
					// scanner files
					// `.. [1]` and `.. [CIT]` under COMMENT with every other
					// non-directive `..`, so the label is what tells them apart. pandoc
					// carries the body as a Note; panduck keeps it as prose at this
					// depth, which is what it emitted before #64. Dropping it was the
					// first cut of #64, and only the word-loss guard noticed.
					if (!line.text.empty() && line.text[0] == '[') {
						flush();
						Run(i + 1, end, depth, MinIndent(i + 1, end, line.indent + 1));
					}
					i = end - 1;
				}
				continue; // produces nothing, and must not fall through as prose
			}
			case LineKind::ADORNMENT: {
				// A LONE `::` IS A LITERAL-BLOCK MARKER, not a transition. `::` is a
				// legal two-character adornment, so the scanner cannot tell; nothing
				// preceding it can. flush() already turns a paragraph ENDING in `::`
				// into a pending literal -- this is the paragraph that is ONLY
				// `::`, which never reached flush() and became an hr (#64).
				if (para.empty() && line.text == "::") {
					literal_pending = true;
					continue;
				}
				// A TRANSITION when nothing precedes it, a heading UNDERLINE when text
				// does. The scanner cannot tell them apart; this is the only place that
				// can.
				if (!para.empty()) {
					// HOW LONG THE RUN MUST BE: min(4, title length) (#88), measured on
					// docutils 0.20.1. #87 recorded a flat four-character floor, which was
					// right for every case it measured and wrong in general -- see the
					// constant's own comment.
					//
					//   `Doc`   / `===`  (3,3)  -> TITLE      3 >= min(4,3)
					//   `Ab`    / `--`   (2,2)  -> TITLE      2 >= min(4,2)
					//   `Abc`   / `--`   (3,2)  -> paragraph  2 <  min(4,3)
					//   `Abcde` / `----` (5,4)  -> TITLE      4 >= min(4,5)
					//
					// A flat floor would have turned test/fixtures/table_markup.rst's
					// opening `Doc` over `===` into a paragraph.
					const size_t need = std::min<size_t>(RST_MIN_SECTION_ADORNMENT, para.back().size());
					if (line.text.size() < need) {
						// BELOW THE THRESHOLD THE RUN IS PROSE. Falling through to the
						// transition arm below would emit an `hr` -- trading an invented
						// heading for an invented rule. Both references fold the run into
						// the paragraph, so it joins `para` as a continuation line and
						// flush() joins it with a space like any wrapped paragraph.
						//
						// This is also what fixes `::` under a long title, with no arm of
						// its own: `Plain item` over `::` folds to `Plain item ::`, and
						// flush()'s EXISTING end-of-paragraph rule opens the literal block
						// and leaves one colon in the prose. Before this it became a
						// heading with a blockquote under it, which neither reference has.
						para.push_back(line.text);
						continue;
					}
					std::string title = para.back();
					para.pop_back();
					flush();
					RstBlock b;
					b.element_type = DuckBlockVocabulary::TYPE_HEADING;
					b.heading_level = LevelFor(line.adornment);
					b.level = depth;
					// THE TITLE WAS NOT PARSED AT ALL, so `**Bold** title` reached
					// `content` as literal source -- markup leaking as text, which is
					// worse than the flattening the other readers did. Now it is parsed
					// like any other run.
					std::vector<RstInline> runs;
					ParseInlines(title, depth + 1, runs);
					if (runs.size() == 1 && runs[0].element_type == DuckBlockVocabulary::INLINE_TEXT) {
						b.content = runs[0].content;
					} else {
						// A HEADING CARRIES BOTH: a flattened title in `content` AND the
						// rich inline children beside it (duck_block ruling d003d32).
						//
						// Flattening alone loses formatting irreversibly -- `**Bold**
						// title` and `Bold title` become byte-identical, so a round trip
						// rewrites the first as the second. Children alone break every
						// consumer that reads a title from `content`, which doc_toc does.
						//
						// The structure marks itself and needs no new vocabulary: a lone
						// text child lives in `content` and produces NO children, so
						// children alongside non-empty content can only mean the content is
						// a DERIVED flattening. CHILDREN ARE AUTHORITATIVE when both are
						// present.
						std::string all;
						for (auto &r : runs) {
							all += r.content;
						}
						b.content = all;
						b.inlines = std::move(runs);
					}
					// THE ANCHOR (#85). RST emitted none, so doc_render produced `<h1>Title</h1>`
					// where pandoc produces `<h1 id="title">`, and doc_section -- which matches
					// `content = section OR attributes['id'] = section` -- could not find a
					// heading by anchor. webbed's renderer needed no change; it emits the id
					// whenever the block carries one, which textile already demonstrated.
					//
					// Derived from `content`, the FLATTENED title in both branches above, so
					// `**Bold** Title` slugs as `bold-title` and the delimiters contribute
					// nothing. slugify.hpp holds the rule and why the separator is its only
					// parameter.
					b.id = HeadingSlug(b.content, '-');
					blocks_.push_back(std::move(b));
					continue;
				}
				flush();
				RstBlock b;
				b.element_type = DuckBlockVocabulary::TYPE_HR;
				b.level = depth;
				blocks_.push_back(std::move(b));
				continue;
			}
			case LineKind::DIRECTIVE: {
				flush();
				size_t end = std::min(IndentedRun(i + 1, line.indent), to);
				Directive(line, i + 1, end, depth);
				i = end - 1;
				continue;
			}
			case LineKind::FIELD: {
				flush();
				size_t j = i;
				RstBlock list;
				list.element_type = DuckBlockVocabulary::TYPE_LIST;
				list.list_type = DuckBlockVocabulary::LIST_TYPE_DEFINITION;
				list.level = depth;
				blocks_.push_back(std::move(list));
				// A FIELD LIST IS A DEFINITION LIST, NOT METADATA. Measured: pandoc
				// emits a DefinitionList and an EMPTY meta. RST is the only panduck
				// format with no document metadata at all, and the opposite reading is
				// the obvious one -- which is why it is asserted rather than assumed.
				while (j < to && lines_[j].kind == LineKind::FIELD) {
					const int field_indent = lines_[j].indent;
					std::string value = lines_[j].text;
					Emit(DuckBlockVocabulary::TYPE_LIST_ITEM, lines_[j].name, depth + 1,
					     DuckBlockVocabulary::ROLE_TERM);
					j++;
					// A FIELD VALUE WRAPS onto indented lines that follow it with no
					// blank between -- the same value, as pandoc reads it. Before #64
					// each wrap leaked out as a level-1 paragraph and split the field
					// list in two.
					while (j < to && lines_[j].kind == LineKind::TEXT && lines_[j].indent > field_indent) {
						value += (value.empty() ? "" : " ") + lines_[j].text;
						j++;
					}
					Emit(DuckBlockVocabulary::TYPE_LIST_ITEM, value, depth + 1, DuckBlockVocabulary::ROLE_DEFINITION);
				}
				i = j - 1;
				continue;
			}
			case LineKind::BULLET:
			case LineKind::ENUM: {
				// AN ENUMERATED SECTION TITLE (#84). `1. Table of Contents` under an underline
				// is a title, and the scanner cannot know: it classifies a line from its own
				// text, so `1. ` is ENUM before anything looks ahead. The list then ate the
				// title, and the underline -- left over with an empty paragraph -- became an
				// `hr`. Measured on duckeye's fixture: 2 headings where pandoc found 4.
				//
				// WHAT THIS CASE DECIDES, AND WHAT IT NO LONGER DOES (#84, #87, #88). An
				// enumerated line followed by an adornment run is handed to the ADORNMENT case
				// by seeding `para`. That case owns the whole rule; this one only recognises
				// the shape, which keeps the level rule and inline parsing in one place.
				//
				// THE THRESHOLD USED TO BE DUPLICATED HERE as a flat four, and #87 wrote it up
				// as "the run's own length -- four characters -- not the title's". That was
				// wrong in general (#88), and this path could never have revealed it: the
				// shortest possible enum title is `1. a`, already four characters, so
				// min(4, title length) always collapses to 4 here. A flat rule cannot be caught
				// out by an enumerated document -- only a plain TEXT title is ever shorter.
				//
				// Measured on docutils 0.20.1:
				//   `1. Title` / `----------`  -> section title, marker included
				//   `1. item`  / `---`         -> ONE PARAGRAPH `1. item ---`; no list, no `hr`
				//   `1. item`  / `::`          -> paragraph plus a literal block
				//   `- Title`  / `----------`  -> bullet_list + TRANSITION, untouched here
				//
				// The middle two are #88. Below the threshold the marker line and the run fold
				// into prose, which RECOVERS THE `1.` the list reading used to discard -- the
				// same breach of README's "discard nothing" that #87 fixed above the threshold,
				// surviving below it until now. pandoc differs in the band `4 <= run < title
				// length`, needing the run to reach the title; panduck follows docutils.
				//
				// `::` needs no arm of its own. Once the line folds, flush()'s end-of-paragraph
				// rule opens the literal block and leaves one colon in the prose.
				//
				// A BULLET never reaches the seeding path: docutils keeps the bullet_list, so
				// that shape falls through to List() untouched.
				//
				// A transition separated by a blank line is out of reach for a different
				// reason: `lines_[i + 1]` is then BLANK, not ADORNMENT.
				if (line.kind == LineKind::ENUM && i + 1 < to && lines_[i + 1].kind == LineKind::ADORNMENT) {
					// raw_text keeps the marker the scanner stripped; the heading text is
					// `1. Table of Contents`, number included.
					const std::string &title = line.raw_text.empty() ? line.text : line.raw_text;
					// NO THRESHOLD TEST HERE (#88). Seeding `para` unconditionally lets the
					// ADORNMENT case apply min(4, title length) ONCE, for this path and the
					// plain-text one alike. Below the threshold it folds the run into the
					// paragraph, which is what recovers the `1.` that the list reading used
					// to discard -- the same breach #87 fixed above the threshold, surviving
					// below it until now. `1. item` over `---` is one paragraph in docutils,
					// not a list and not an `hr`; `1. item` over `::` is a paragraph plus a
					// literal block.
					para.push_back(title);
					continue;
				}
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
				// A DEFINITION: a term line whose next line is indented and not a list.
				if (para.empty() && i + 1 < to && lines_[i + 1].indent > line.indent &&
				    lines_[i + 1].kind == LineKind::TEXT) {
					size_t end = std::min(IndentedRun(i + 1, line.indent), to);
					RstBlock list;
					list.element_type = DuckBlockVocabulary::TYPE_LIST;
					list.list_type = DuckBlockVocabulary::LIST_TYPE_DEFINITION;
					list.level = depth;
					blocks_.push_back(std::move(list));
					Emit(DuckBlockVocabulary::TYPE_LIST_ITEM, line.text, depth + 1, DuckBlockVocabulary::ROLE_TERM);
					std::string def;
					for (size_t k = i + 1; k < end; k++) {
						if (lines_[k].kind == LineKind::BLANK) {
							continue;
						}
						def += (def.empty() ? "" : " ") + lines_[k].text;
					}
					Emit(DuckBlockVocabulary::TYPE_LIST_ITEM, def, depth + 1, DuckBlockVocabulary::ROLE_DEFINITION);
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
			b.element_type = DuckBlockVocabulary::TYPE_CODE;
			b.content = RawBody(from, to, line.indent);
			b.language = line.text;
			b.level = depth;
			blocks_.push_back(std::move(b));
			return;
		}
		// EVERY OTHER DIRECTIVE IS A DIV, with the directive NAME in
		// attributes['source_type'] rather than a minted role. The directive set is
		// OPEN -- docutils ships dozens and Sphinx hundreds -- so a reader cannot
		// enumerate it, and the spec's instruction for an unrecognised name is to
		// keep the original in source_type so it is visible as a gap rather than
		// silently private.
		RstBlock b;
		b.element_type = DuckBlockVocabulary::TYPE_DIV;
		b.source_type = line.name;
		b.level = depth;
		blocks_.push_back(std::move(b));
		// THE BODY IS DESCENDED INTO, never dropped. A directive body is prose, and
		// the LaTeX reader's rule applies: an unknown environment usually wraps
		// paragraphs, so dropping it loses them.
		if (to > from) {
			Run(from, to, depth + 1, MinIndent(from, to, line.indent + 1));
		}
	}

	size_t List(size_t i, size_t to, int depth) {
		const int indent = lines_[i].indent;
		const bool ordered = lines_[i].kind == LineKind::ENUM;
		RstBlock list;
		list.element_type = DuckBlockVocabulary::TYPE_LIST;
		list.list_type = ordered ? DuckBlockVocabulary::LIST_TYPE_ORDERED : DuckBlockVocabulary::LIST_TYPE_BULLET;
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
				// ORDEREDNESS ENDS A LIST as surely as a dedent does. Without this a
				// bullet list followed by an enumerated one at the same indent became a
				// single list carrying both, with the second list's numbering lost
				// entirely.
				break;
			}
			const int text_col = line.text_col > 0 ? line.text_col : indent + 2;
			std::string text = line.text;
			j++;
			// THE ITEM'S TEXT WRAPS onto following lines indented to its text column
			// with no blank between -- one run, as pandoc reads it. Absorbed BEFORE
			// the body, or a wrapped item's second line would become a child
			// paragraph. Before #64 the wrap split the list.
			while (j < to && lines_[j].kind == LineKind::TEXT && lines_[j].indent >= text_col) {
				text += (text.empty() ? "" : " ") + lines_[j].text;
				j++;
			}
			Emit(DuckBlockVocabulary::TYPE_LIST_ITEM, text, depth + 1);
			// THE ITEM'S BODY is everything indented to its TEXT COLUMN or deeper:
			// more paragraphs, a nested list, a quote. Measured against pandoc, the
			// column is the marker's width, and a line indented past the marker but
			// SHORT of it is not the item's: the list ends there and the run is
			// quoted beside it (#64). Before #64 only a nested list was recognised,
			// and only with no blank line before it.
			size_t end = std::min(IndentedRun(j, text_col - 1), to);
			if (end > j) {
				Run(j, end, depth + 2, text_col);
				j = end;
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
		// RESOLVED HERE, at the last point before the cells become an opaque JSON
		// string.
		for (auto &h : headers) {
			h = CellText(h);
		}
		for (auto &r : body) {
			for (auto &c : r) {
				c = CellText(c);
			}
		}
		RstBlock b;
		b.element_type = DuckBlockVocabulary::TYPE_TABLE;
		b.content = BuildTableJson(headers, body);
		b.encoding = DuckBlockVocabulary::ENCODING_JSON;
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

std::vector<Block> ReadRst(const std::string &src) {
	std::vector<Block> rows;
	int32_t order = 0;
	for (auto &block : ParseRstString(src)) {
		Block row;
		// UNCONDITIONAL, unlike org and textile: RstBlock carries no `kind` field at
		// all, because RST has no document metadata and so this reader never emits a
		// kind='value' row. Preserved exactly as BuildRows had it.
		row.kind = DuckBlockVocabulary::KIND_BLOCK;
		row.element_type = block.element_type;
		row.content = block.content;
		// ENCODING SET EXPLICITLY, not defaulted. RstRow defaulted this field to
		// ENCODING_TEXT and the loop only overrode it when the block carried one
		// (`if (!block.encoding.empty())`); panduck::Block defaults it to empty,
		// because a shared vocabulary type should not carry one format's default.
		// Without this line every rst row would ship a blank encoding -- a silent
		// data change no compiler catches.
		row.encoding = block.encoding.empty() ? DuckBlockVocabulary::ENCODING_TEXT : block.encoding;
		row.element_order = order++;
		if (block.heading_level > 0) {
			row.attributes[DuckBlockVocabulary::ATTR_HEADING_LEVEL] = std::to_string(block.heading_level);
		}
		if (!block.source_type.empty()) {
			// The directive name a `div` came from. RST has NO document metadata -- a
			// field list is a definition list -- so there is no ATTR_KEY here, unlike
			// every other reader.
			row.attributes[DuckBlockVocabulary::ATTR_SOURCE_TYPE] = block.source_type;
		}
		if (!block.role.empty()) {
			row.attributes[DuckBlockVocabulary::ATTR_ROLE] = block.role;
		}
		if (!block.language.empty()) {
			row.attributes["language"] = block.language;
		}
		if (!block.id.empty()) {
			row.attributes["id"] = block.id;
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
			// Same reason as above: RstRow's default supplied this, and the inline
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

} // namespace rst
} // namespace panduck
