#pragma once

#include "duckdb.hpp"

#include <map>
#include <string>
#include <vector>

namespace duckdb {

class ExtensionLoader;

namespace textile {

//! An inline run. `level` is absolute: a run directly inside a block sits at that block's
//! level + 1, and one nested inside another deeper again.
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
std::vector<TxBlock> ParseTextileString(const std::string &src);

void RegisterTextileReader(ExtensionLoader &loader);

} // namespace textile
} // namespace duckdb
