#pragma once

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {
namespace panduck {

// panduck_table_read(content VARCHAR): a table block's native content
// ({"headers": [...], "rows": [[...]]}) back to a relation with typed columns.
// The inverse of duck_block_utils' duck_block_json_to_table.
//
// Moved from duck_block_utils (#78, where it was duck_block_table_read):
// duck_block_utils keeps only macros, and this needs C++. It is a table
// function because the bind step must read the content to decide the columns,
// which no macro can do. The argument must be a constant (a literal,
// getvariable(), or a prepared parameter).
void RegisterTableReadFunctions(ExtensionLoader &loader);

} // namespace panduck
} // namespace duckdb
