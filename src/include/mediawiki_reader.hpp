#pragma once

#include "duckdb.hpp"

#include <map>
#include <string>
#include <vector>

namespace duckdb {

class ExtensionLoader;

namespace mediawiki {

//! An inline run. `level` is absolute: a run directly inside a block sits at that block's
//! level + 1, and a run nested inside another (bold wrapping italic) one deeper again.
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

//! Parse MediaWiki source into blocks. Never throws: malformed input degrades, because
//! wikitext has no error state -- MediaWiki renders whatever it can and so does this.
std::vector<MwBlock> ParseMediaWikiString(const std::string &src);

void RegisterMediaWikiReader(ExtensionLoader &loader);

} // namespace mediawiki
} // namespace duckdb
