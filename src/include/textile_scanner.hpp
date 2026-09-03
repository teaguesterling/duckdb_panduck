#pragma once

#include <string>
#include <vector>

namespace duckdb {
namespace textile {

//! What a single line of Textile source IS.
//!
//! SIMPLER THAN THE MEDIAWIKI SCANNER, deliberately and by the format's nature. MediaWiki
//! cannot classify a line by its prefix alone -- a `|` means one thing inside `{{...}}` and
//! another at line start -- so that scanner balances braces as it walks. Textile has no
//! construct whose interior changes how a later line is read, so this is a pure line
//! classifier with no carried state.
//!
//! The one wrinkle is that a block marker is not a fixed prefix. `p.`, `h2.`, `bq.` may
//! carry attributes between the name and the dot -- `p{color:red}.`, `h2(cls).`, `bq[fr].`
//! -- so the marker head is PARSED rather than matched.
enum class LineKind {
	BLANK,      //!< empty or whitespace-only: separates blocks
	HEADING,    //!< `h1.` .. `h6.`
	PARA,       //!< an explicit `p.` marker
	BLOCKQUOTE, //!< `bq.`
	CODE,       //!< `pre.` or `bc.` -- both yield CodeBlock in pandoc, measured
	NOTEXTILE,  //!< `notextile.` -- body passes through unprocessed
	COMMENT,    //!< `###.` -- not content
	LIST_ITEM,  //!< `*`/`#` runs, and `-` for a definition
	TABLE_ROW,  //!< `| a | b |`, with `_.` marking a header cell
	HTML_BLOCK, //!< a block-level HTML element, which pandoc's textile WRITER emits
	TEXT,       //!< anything else: paragraph content
};

//! One classified line. `text` is the line's CONTENT with its marker removed, so a consumer
//! never re-parses the prefix.
struct Line {
	LineKind kind = LineKind::TEXT;
	std::string text;        //!< content after the marker
	std::string markers;     //!< LIST_ITEM: the raw marker run (`##`), so depth knows its type
	std::string style;       //!< block attributes from `{...}`
	std::string css_class;   //!< block attributes from `(...)`, the class part
	std::string id;          //!< block attributes from `(...)`, the `#id` part
	std::string term;        //!< LIST_ITEM: the part before ` := `, when there is one
	bool definition = false; //!< LIST_ITEM: had a ` := `, so `term` is populated
	int level = 0;           //!< HEADING: 1-6. LIST_ITEM: marker run length.
	//! The source line before any trimming. A code block's continuation lines carry their
	//! INDENTATION as content, and `text` has already had it stripped -- reading "    return
	//! 1" back as "return 1" changes the program.
	std::string raw;
};

//! Classify every line of a Textile document. Never fails: an unrecognised line is TEXT.
std::vector<Line> ScanTextile(const std::string &src);

//! Split a table row on `|`, returning each cell's raw text. Exposed because the reader
//! needs it and it is pure string work with no state.
std::vector<std::string> SplitRow(const std::string &s);

} // namespace textile
} // namespace duckdb
