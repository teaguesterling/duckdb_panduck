#pragma once

#include "panduck/textile_scanner.hpp"

namespace duckdb {
namespace textile {

// THE SCANNER LIVES IN libpanduck (issue #104, L2):
// libpanduck/include/panduck/textile_scanner.hpp and src/textile_scanner.cpp.
//
// ENUMERATE WITH A PATTERN THAT COVERS enum/using/constexpr, not just struct
// and function. The first cut of this shim missed `LineKind` because the grep
// used to list the header's surface matched only `^std::|^struct` -- the build
// failed with "'LineKind' has not been declared" 26 times.
//
// Leading `::` is mandatory: inside `namespace duckdb` a bare `panduck::` binds
// to `duckdb::panduck`, the compat helpers, not the library.
using ::panduck::textile::Line;
using ::panduck::textile::LineKind;
using ::panduck::textile::ScanTextile;
using ::panduck::textile::SplitRow;

} // namespace textile
} // namespace duckdb
