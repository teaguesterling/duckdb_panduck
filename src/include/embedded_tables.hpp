#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace panduck {

//! panduck_embedded_to_table(text, format) and panduck_blocks_parse_embedded(blocks[,
//! formats]) (#188): csv, tsv, json and jsonl code blocks become native `table` blocks.
//! The parsing is libpanduck's (panduck/embedded_table.hpp); this is the DuckDB tail.
void RegisterEmbeddedTableFunctions(ExtensionLoader &loader);

} // namespace panduck
} // namespace duckdb
