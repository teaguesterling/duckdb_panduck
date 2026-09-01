#pragma once

#include "duckdb.hpp"

#include <string>
#include <vector>

namespace duckdb {

class ExtensionLoader;

namespace ipynb {

struct IpynbInline {
	std::string element_type;
	std::string content;
	int level = 2;
};

struct IpynbBlock {
	std::string kind;        //!< empty = block; `value` for notebook metadata
	std::string key;         //!< `value` only: pandoc's key name
	std::string element_type;
	std::string content;
	std::string encoding;    //!< `raw`: the EMBEDDED format's name
	std::string language;    //!< `code` only
	std::string source_type; //!< div: the cell or output kind; value: the original field
	int level = 1;
	std::vector<IpynbInline> inlines;
};

//! Parse a Jupyter notebook into blocks. Never throws on malformed JSON: a notebook that
//! does not parse yields no blocks rather than failing the query.
std::vector<IpynbBlock> ParseIpynbString(const std::string &src);

void RegisterIpynbReader(ExtensionLoader &loader);

} // namespace ipynb
} // namespace duckdb
