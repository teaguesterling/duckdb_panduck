#pragma once

#include "panduck/block.hpp"

#include <map>
#include <string>
#include <vector>

// Moved behind the libpanduck seam (issue #104, L2). Names no DuckDB type.
// The table function, its binds and its registration stay in
// src/mediawiki_reader.cpp.

namespace panduck {
namespace mediawiki {

//! An inline run. `level` is absolute: a run directly inside a block sits at
//! that block's level + 1, and a run nested inside another (bold wrapping
//! italic) one deeper again.
struct MwInline {
	std::string element_type; //!< text, bold, italic, code, link, note, raw
	std::string content;
	std::string encoding; //!< raw only: 'mediawiki' for an inline template
	std::map<std::string, std::string> attributes;
	int level = 2;
};

struct MwBlock {
	//! duck_block kind. Empty means `block`.
	std::string kind;
	std::string element_type;
	std::string content;
	std::string encoding; //!< table: 'json'. raw: 'mediawiki' or 'html'.
	std::map<std::string, std::string> attributes;
	int level = 1;
	std::vector<MwInline> inlines;
};

//! Parse MediaWiki source into format-shaped blocks. Never throws: malformed
//! input degrades, because wikitext has no error state -- MediaWiki renders
//! whatever it can and so does this. The intermediate; callers wanting
//! duck_block rows want ReadMediaWiki.
std::vector<MwBlock> ParseMediaWikiString(const std::string &src);

//! Read MediaWiki source as duck_block rows. THE MODULE'S PUBLIC SHAPE, shared
//! by every format module: source in, vocabulary rows out.
std::vector<Block> ReadMediaWiki(const std::string &src);

} // namespace mediawiki
} // namespace panduck
