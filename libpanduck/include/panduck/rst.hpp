#pragma once

#include "panduck/block.hpp"

#include <map>
#include <string>
#include <vector>

// Moved behind the libpanduck seam (issue #104, L2). Names no DuckDB type.
// The table function, its binds and its registration stay in
// src/rst_reader.cpp.

namespace panduck {
namespace rst {

struct RstInline {
	std::string element_type;
	std::string content;
	std::string href;
	//! note and cite only: the label that joins a reference to its body (#85).
	//! An auto-numbered `#` or `*` label is numbered in document order.
	std::string id;
	int level = 2;
	//! A role's `class` and `role`, or a math run's `display` (#193). An image's `src`
	//! and `alt`, and the directive options it carries (#195).
	std::map<std::string, std::string> attributes;
};

struct RstBlock {
	std::string element_type;
	std::string content;
	std::string list_type;
	std::string role;
	std::string language;
	std::string source_type; //!< div only: the directive name it came from
	std::string encoding;
	//! A heading's pandoc-style anchor slug, or a footnote or citation body's
	//! label (#85).
	std::string id;
	std::string list_start, number_style, number_delim;
	int heading_level = 0;
	int level = 1;
	std::vector<RstInline> inlines;
	//! A directive's options (#195), by name. ReadRst copies them BEFORE the
	//! typed fields above, so an option named `source_type` or `language`
	//! cannot replace what the reader sets.
	std::map<std::string, std::string> attributes;
};

//! Parse reStructuredText into format-shaped blocks. Never throws: malformed
//! input degrades. The intermediate; callers wanting duck_block rows want
//! ReadRst.
//! RST HAS NO DOCUMENT METADATA -- a field list is a definition list, measured
//! against pandoc -- so this reader emits no kind='value' rows and the struct
//! carries no key.
std::vector<RstBlock> ParseRstString(const std::string &src);

//! Read reStructuredText as duck_block rows. THE MODULE'S PUBLIC SHAPE, shared
//! by every format module: source in, vocabulary rows out.
std::vector<Block> ReadRst(const std::string &src);

} // namespace rst
} // namespace panduck
