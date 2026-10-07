#pragma once

#include "panduck/block.hpp"

#include <map>
#include <string>
#include <vector>

// Moved behind the libpanduck seam (issue #104, L2). Names no DuckDB type.
// The table function, its binds and its registration stay in
// src/org_reader.cpp.

namespace panduck {
namespace org {

//! An inline run. `level` is absolute: a run directly inside a block sits at
//! that block's level + 1.
struct OrgInline {
	std::string element_type; //!< text, bold, italic, underline, code,
	                          //!< strikethrough, link
	std::string content;
	std::string href; //!< link only
	int level = 2;
};

struct OrgBlock {
	//! duck_block kind. Empty means `block`; `value` is document metadata.
	std::string kind;
	std::string key; //!< `value` only: the field name, in pandoc's namespace
	std::string element_type;
	std::string content;
	std::string list_type; //!< list only: DuckBlockVocabulary::LIST_TYPE_*
	std::string role;      //!< list_item only: term / definition
	std::string language;  //!< code only
	std::string encoding;  //!< table only: 'json'
	std::string list_start, number_style, number_delim;
	//! Free-form attributes -- `format` for a raw block, and whatever a later
	//! construct needs. The named fields above predate this and are left alone.
	std::map<std::string, std::string> attributes;
	int heading_level = 0;
	int level = 1;
	std::vector<OrgInline> inlines;
};

//! Parse Org source into format-shaped blocks. Never throws: malformed input
//! degrades. The intermediate; callers wanting duck_block rows want ReadOrg.
std::vector<OrgBlock> ParseOrgString(const std::string &src);

//! Read Org source as duck_block rows. THE MODULE'S PUBLIC SHAPE, shared by
//! every format module: source in, vocabulary rows out.
std::vector<Block> ReadOrg(const std::string &src);

} // namespace org
} // namespace panduck
