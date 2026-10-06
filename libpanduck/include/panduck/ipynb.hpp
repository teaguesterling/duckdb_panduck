#pragma once

#include "panduck/block.hpp"

#include <string>
#include <vector>

// libpanduck -- the format engine, with no DuckDB in it.
//
// This header must not include duckdb.hpp, nor any duckdb/ header. The whole
// point of the seam is that everything behind it is unaffected by DuckDB API
// churn: every break this project has absorbed (named_parameters, SetValue,
// ExecuteWithNulls in a sibling) lived in the ~2% of reader code that names a
// DuckDB type. scripts/check_libpanduck_seam.py enforces this.
//
// The DuckDB-facing half of this reader -- the table function, its bind, and
// its registration -- stays in src/ipynb_reader.cpp.

namespace panduck {
namespace ipynb {

struct IpynbInline {
	std::string element_type;
	std::string content;
	int level = 2;
};

struct IpynbBlock {
	std::string kind; //!< empty = block; `value` for notebook metadata
	std::string key;  //!< `value` only: pandoc's key name
	std::string element_type;
	std::string content;
	std::string encoding;    //!< left at its default; a raw block's format is NOT here
	std::string raw_format;  //!< `raw` only: attributes['format'] -- the embedded format
	std::string language;    //!< `code` only
	std::string source_type; //!< div: the cell or output kind; value: the original field
	int level = 1;
	std::vector<IpynbInline> inlines;
};

//! Parse a Jupyter notebook into blocks. Never throws on malformed JSON: a
//! notebook that does not parse yields no blocks rather than failing the query.
//!
//! The format-shaped intermediate. Callers that just want duck_block rows want
//! ReadIpynb instead.
std::vector<IpynbBlock> ParseIpynbString(const std::string &src);

//! Read a Jupyter notebook as duck_block rows. THE MODULE'S PUBLIC SHAPE, and
//! the one every format module should share: source in, vocabulary rows out.
//!
//! The flattening used to live in the DuckDB half of this reader, which is
//! where it accidentally ended up rather than where it belongs -- it names no
//! DuckDB type, only vocabulary constants. Moving it here is what lets a
//! non-DuckDB consumer get rows at all.
std::vector<Block> ReadIpynb(const std::string &src);

} // namespace ipynb
} // namespace panduck
