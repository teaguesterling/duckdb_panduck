#pragma once

#include <string>
#include <vector>

namespace duckdb {
namespace rst {

//! What a line of reStructuredText IS, as far as its own text can say.
//!
//! Two kinds are DELIBERATELY AMBIGUOUS here and resolved by the reader, because a line
//! cannot answer them alone:
//!
//!   ADORNMENT  `-----` is a heading underline when text precedes it and a TRANSITION when
//!              nothing does. Only the neighbour decides.
//!   TEXT       an indented run is a directive body, a definition, a literal block or a
//!              nested list depending on what opened above it. The scanner reports the
//!              INDENT and the reader groups by it, rather than guessing here.
enum class LineKind {
	BLANK,
	ADORNMENT,  //!< a run of one punctuation character: heading underline OR transition
	DIRECTIVE,  //!< `.. name:: argument`
	COMMENT,    //!< `.. anything else` -- an RST comment, which produces nothing
	FIELD,      //!< `:Name: value` -- a field list entry, which is NOT metadata
	BULLET,     //!< `- x`, `* x`, `+ x`
	ENUM,       //!< `1. x`, `1) x`, `#. x`
	GRID_SEP,   //!< `+-----+-----+` and `+=====+=====+`
	TABLE_ROW,  //!< `| a | b |`
	SIMPLE_SEP, //!< `=====  =====` -- a simple table's rule, whose runs give column spans
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
	bool header_sep = false; //!< GRID_SEP written with `=` -- promotes the rows above it
	std::vector<int> spans;  //!< SIMPLE_SEP: the rule runs, giving column boundaries
};

//! Classify every line. Never fails: an unrecognised line is TEXT.
std::vector<Line> ScanRst(const std::string &src);

} // namespace rst
} // namespace duckdb
