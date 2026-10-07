#pragma once

#include "panduck/rst.hpp"

#include "duckdb.hpp"

#include <string>
#include <vector>

namespace duckdb {

class ExtensionLoader;

namespace rst {

// THE PARSE CORE AND FLATTENING LIVE IN libpanduck (issue #104, L2):
// libpanduck/include/panduck/rst.hpp and libpanduck/src/rst.cpp, which name no
// DuckDB type. Only the table function, its binds and its registration stay on
// this side of the seam.
//
// Leading `::` is mandatory: inside `namespace duckdb` a bare `panduck::` binds
// to `duckdb::panduck`, the compat helpers in panduck_duckdb_compat.hpp, not to
// the library. Enumerate the moved header's surface with a pattern covering
// enum/using/constexpr as well as struct and function -- the first textile
// scanner shim missed `LineKind` exactly that way. Alphabetised for
// clang-format.
using ::panduck::rst::ParseRstString;
using ::panduck::rst::ReadRst;
using ::panduck::rst::RstBlock;
using ::panduck::rst::RstInline;

void RegisterRstReader(ExtensionLoader &loader);

} // namespace rst
} // namespace duckdb
