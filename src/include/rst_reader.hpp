#pragma once

#include "duckdb.hpp"

#include <string>
#include <vector>

namespace duckdb {

class ExtensionLoader;

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
	std::string list_start, number_style, number_delim;
	int heading_level = 0;
	int level = 1;
	std::vector<RstInline> inlines;
};

//! Parse reStructuredText into blocks. Never throws: malformed input degrades.
//! RST HAS NO DOCUMENT METADATA -- a field list is a definition list, measured against
//! pandoc -- so this reader emits no kind='value' rows and the struct carries no key.
std::vector<RstBlock> ParseRstString(const std::string &src);

void RegisterRstReader(ExtensionLoader &loader);

} // namespace rst
} // namespace duckdb
