#pragma once

#include <string>
#include <vector>

namespace duckdb {
namespace org {

//! What a single line of Org source IS, decided by its prefix alone.
//!
//! Org's block structure is entirely line-prefixed -- `*`, `-`, `|`, `#+` -- which is why
//! this reader scans lines where the LaTeX reader tokenizes characters. Only INLINE markup
//! is character-level, and that is a separate pass over a line's text.
enum class LineKind {
	BLANK,        //!< empty or whitespace-only: a paragraph and list separator
	HEADING,      //!< `* text` .. `****** text`, stars at column 0
	KEYWORD,      //!< `#+KEY: value` -- metadata and block options
	BLOCK_BEGIN,  //!< `#+BEGIN_SRC python`, `#+BEGIN_QUOTE`, ...
	BLOCK_END,    //!< `#+END_SRC`
	LIST_ITEM,    //!< `- x`, `+ x`, `1. x`, `1) x`, and `- term :: definition`
	TABLE_ROW,    //!< `| a | b |`
	TABLE_RULE,   //!< `|---+---|` -- the separator that promotes the row above it
	HRULE,        //!< five or more dashes alone on a line
	COMMENT,      //!< `# text` -- a comment, and NOT `#+KEY:`
	DRAWER_BEGIN, //!< `:PROPERTIES:` -- opens a drawer whose contents are not prose
	DRAWER_END,   //!< `:END:`
	TEXT,         //!< anything else: paragraph content
};

//! One classified line. `text` is the line's CONTENT with its marker removed, so a
//! consumer never re-parses the prefix.
struct Line {
	LineKind kind = LineKind::TEXT;
	std::string text;        //!< content after the marker
	std::string key;         //!< KEYWORD: the key, upper-cased. BLOCK_*: the block name.
	std::string raw;         //!< KEYWORD: the line VERBATIM. `raw` content must not be
	                         //!< reconstructed from an upper-cased key.
	std::string term;        //!< LIST_ITEM: the part before ` :: `, when there is one
	int level = 0;           //!< HEADING: star count. LIST_ITEM: indent columns.
	bool ordered = false;    //!< LIST_ITEM: `1.` / `1)` rather than `-` / `+`
	int start = 1;           //!< LIST_ITEM, ordered: the number the source wrote
	bool definition = false; //!< LIST_ITEM: had a ` :: `, so `term` is populated
};

//! Classify every line of an Org document. Never fails: an unrecognised line is TEXT.
std::vector<Line> ScanOrg(const std::string &src);

} // namespace org
} // namespace duckdb
