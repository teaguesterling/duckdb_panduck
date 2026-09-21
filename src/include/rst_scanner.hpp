#pragma once

#include <string>
#include <vector>

namespace duckdb {
namespace rst {

//! What a line of reStructuredText IS, as far as its own text can say.
//!
//! Two kinds are DELIBERATELY AMBIGUOUS here and resolved by the reader,
//! because a line cannot answer them alone:
//!
//!   ADORNMENT  `-----` is a heading underline when text precedes it and a
//!   TRANSITION when
//!              nothing does. Only the neighbour decides.
//!   TEXT       an indented run is a directive body, a definition, a literal
//!   block or a
//!              nested list depending on what opened above it. The scanner
//!              reports the INDENT and the reader groups by it, rather than
//!              guessing here.
enum class LineKind {
	BLANK,
	ADORNMENT,  //!< a run of one punctuation character: heading underline OR
	            //!< transition
	DIRECTIVE,  //!< `.. name:: argument`
	COMMENT,    //!< `.. anything else` -- an RST comment, which produces nothing
	FIELD,      //!< `:Name: value` -- a field list entry, which is NOT metadata
	BULLET,     //!< `- x`, `* x`, `+ x`
	ENUM,       //!< `1. x`, `1) x`, `#. x`
	GRID_SEP,   //!< `+-----+-----+` and `+=====+=====+`
	TABLE_ROW,  //!< `| a | b |`
	SIMPLE_SEP, //!< `=====  =====` -- a simple table's rule, whose runs give
	            //!< column spans
	TEXT,
};

struct Line {
	LineKind kind = LineKind::TEXT;
	std::string text;   //!< content with the marker removed
	std::string name;   //!< DIRECTIVE: the directive name. FIELD: the field name.
	char adornment = 0; //!< ADORNMENT: which character
	int indent = 0;     //!< leading columns, which is how RST expresses containment
	bool ordered = false;
	int start = 1;
	//! BULLET/ENUM: the column where the item's TEXT begins -- the marker's
	//! width, not a fixed 2 (`10. x` and `-   x` both put it at 4). An item owns
	//! lines indented to this column or deeper; a line indented past the marker
	//! but short of it ends the list (pandoc, #64).
	int text_col = 0;
	//! BULLET/ENUM: the line as written, marker included. `text` has the marker
	//! stripped, and the marker cannot be rebuilt from `ordered`/`start`: `1.` and
	//! `1)` are both ENUM with start=1. A SECTION TITLE that happens to open with a
	//! marker -- `1. Table of Contents` under an underline -- is a title, not a list,
	//! and pandoc keeps the number in it (#84). Only the reader can tell the two
	//! apart, and only from the NEXT line, so the scanner carries the text it would
	//! otherwise discard.
	std::string raw_text;
	bool header_sep = false; //!< GRID_SEP written with `=` -- promotes the rows above it
	std::vector<int> spans;  //!< SIMPLE_SEP: the rule runs' WIDTHS
	//! SIMPLE_SEP: where each rule run STARTS. Widths alone are not enough to
	//! locate a column: RST separates them by one or more spaces, so a reader
	//! accumulating `width + gap` has to guess the gap and drifts further with
	//! every column.
	std::vector<int> span_starts;
};

//! Classify every line. Never fails: an unrecognised line is TEXT.
std::vector<Line> ScanRst(const std::string &src);

} // namespace rst
} // namespace duckdb
