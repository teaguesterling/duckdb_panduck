#pragma once

#include "panduck/textile.hpp"

#include "duckdb.hpp"

#include <map>
#include <string>
#include <vector>

namespace duckdb {

class ExtensionLoader;

namespace textile {

// THE PARSE CORE AND FLATTENING LIVE IN libpanduck (issue #104, L2):
// libpanduck/include/panduck/textile.hpp and libpanduck/src/textile.cpp, which
// name no DuckDB type. Only the table function, its binds and its registration
// stay on this side of the seam.
//
// Leading `::` is mandatory: inside `namespace duckdb` a bare `panduck::` binds
// to `duckdb::panduck`, the compat helpers, not the library. Enumerate the
// moved header's surface with a pattern covering enum/using/constexpr as well
// as struct and function -- the textile_scanner shim missed `LineKind` exactly
// that way.
using ::panduck::textile::ParseTextileString;
using ::panduck::textile::ReadTextile;
using ::panduck::textile::TxBlock;
using ::panduck::textile::TxInline;

void RegisterTextileReader(ExtensionLoader &loader);

} // namespace textile
} // namespace duckdb
