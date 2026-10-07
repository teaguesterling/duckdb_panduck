#pragma once

#include "panduck/block.hpp"

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
	int level = 2;
};

struct RstBlock {
	std::string element_type;
	std::string content;
	std::string list_type;
	std::string role;
	std::string language;
	std::string source_type; //!< div only: the directive name it came from
	std::string encoding;
	std::string id; //!< heading only: the pandoc-style anchor slug (#85)
	std::string list_start, number_style, number_delim;
	int heading_level = 0;
	int level = 1;
	std::vector<RstInline> inlines;
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
