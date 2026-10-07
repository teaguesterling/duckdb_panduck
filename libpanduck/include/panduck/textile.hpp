#pragma once

#include "panduck/block.hpp"

#include <map>
#include <string>
#include <vector>

// Moved behind the libpanduck seam (issue #104, L2). Names no DuckDB type.
// The table function, its binds and its registration stay in
// src/textile_reader.cpp.

namespace panduck {
namespace textile {

struct TxInline {
	std::string element_type;
	std::string content;
	std::map<std::string, std::string> attributes;
	int level = 2;
};

struct TxBlock {
	std::string kind; //!< empty means `block`
	std::string element_type;
	std::string content;
	std::string encoding; //!< table: 'json'. raw: 'html'.
	std::map<std::string, std::string> attributes;
	int level = 1;
	std::vector<TxInline> inlines;
};

//! Parse Textile source into blocks. Never throws: malformed input degrades.
//! Parse Textile source into format-shaped blocks. The intermediate; callers
//! wanting duck_block rows want ReadTextile.
std::vector<TxBlock> ParseTextileString(const std::string &src);

//! Read Textile source as duck_block rows. THE MODULE'S PUBLIC SHAPE, shared by
//! every format module: source in, vocabulary rows out.
std::vector<Block> ReadTextile(const std::string &src);

} // namespace textile
} // namespace panduck
