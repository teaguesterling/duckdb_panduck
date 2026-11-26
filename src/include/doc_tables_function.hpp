#pragma once

#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

//! Registers doc_tables(src) and doc_table(src, n | id := ...) (#215): a document's tables
//! from ATTACH's own table scan, with no ATTACH first.
void RegisterDocTablesFunctions(ExtensionLoader &loader);

} // namespace duckdb
