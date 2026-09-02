#pragma once

#include <string>
#include <vector>

namespace duckdb {
namespace mediawiki {

//! What a single line of wikitext IS.
//!
//! Like Org and RST, MediaWiki's block structure is line-prefixed -- `=`, `*`, `#`, `;`,
//! `:`, `!`, `|`, and a leading space. So this scans lines rather than tokenizing
//! characters, and inline markup is a separate pass over a line's text.
//!
//! WITH ONE DEPARTURE THAT THE FORMAT FORCES. A `|` at the start of a line is a table cell;
//! a `|` inside `{{...}}` is an argument separator. The two are indistinguishable by prefix,
//! so this scanner cannot be purely line-local: it tracks `{{`/`}}` and `{|`/`|}` nesting as
//! it walks. Templates NEST -- `{{a|{{b|x}}|y}}` is one call, measured against pandoc -- so
//! that tracking is brace BALANCING, not matching.
//!
//! Because balancing is character-level work, it lives here rather than in the reader, and a
//! multi-line template arrives as ONE line of kind TEMPLATE carrying the whole raw call.
enum class LineKind {
	BLANK,        //!< empty or whitespace-only: separates paragraphs and lists
	HEADING,      //!< `== text ==`, level from the `=` run length
	LIST_ITEM,    //!< `*`, `#`, `;`, `:` runs -- nesting is the run's LENGTH, not indentation
	PREFORMATTED, //!< a leading space. MediaWiki renders this <pre>; see the reader.
	TABLE_START,  //!< `{|`
	TABLE_ROW,    //!< `|-` -- a row separator, not a row's content
	TABLE_CELL,   //!< `| a` or `| a || b`
	TABLE_HEADER, //!< `! a` or `! a !! b`
	TABLE_END,    //!< `|}`
	HRULE,        //!< four or more dashes alone on a line
	TEMPLATE,     //!< a `{{...}}` call starting a line, possibly spanning many source lines
	BEHAVIOR,     //!< `__TOC__`, `__NOTOC__` -- consumed by MediaWiki, never shown to a reader
	COMMENT,      //!< `<!-- ... -->` alone on a line
	HTML_BLOCK,   //!< `<blockquote>`, `<syntaxhighlight>`, `<pre>`, `<references/>`, ...
	TEXT,         //!< anything else: paragraph content
};

//! One classified line. `text` is the line's CONTENT with its marker removed, so a consumer
//! never re-parses the prefix.
struct Line {
	LineKind kind = LineKind::TEXT;
	std::string text;    //!< content after the marker; for TEMPLATE, the whole raw call
	std::string name;    //!< TEMPLATE: the template's name. BEHAVIOR: the switch, e.g. __TOC__.
	                     //!< HTML_BLOCK: the tag name, lowercased.
	std::string attrs;   //!< HTML_BLOCK: the raw attribute text inside the opening tag
	std::string markers; //!< LIST_ITEM: the raw marker run (`#*`), so each depth knows its type
	bool verbatim = false; //!< HTML_BLOCK: `text` is the COMPLETE raw markup, emit it as-is
	int level = 0;       //!< HEADING: `=` count, capped at 6. LIST_ITEM: marker run length.
};

//! Classify every line of a MediaWiki document. Never fails: an unrecognised line is TEXT,
//! and an unterminated template or table runs to end of input rather than throwing.
std::vector<Line> ScanMediaWiki(const std::string &src);

//! Split a table cell line on `||` / `!!`, the inline cell separators. Exposed because the
//! reader needs it and it is pure string work with no state.
std::vector<std::string> SplitCells(const std::string &s, char sep);

} // namespace mediawiki
} // namespace duckdb
