#pragma once

#include "panduck/org_scanner.hpp"

namespace duckdb {
namespace org {

// THE SCANNER LIVES IN libpanduck (issue #104, L2):
// libpanduck/include/panduck/org_scanner.hpp and libpanduck/src/org_scanner.cpp.
// It never named a DuckDB type -- it sat in `namespace duckdb` by convention
// only -- so the move was a namespace swap and this shim keeps every existing
// `duckdb::org::Line` spelling resolving.
//
// ENUMERATE THE MOVED HEADER'S SURFACE WITH A PATTERN THAT COVERS
// enum/using/constexpr, not just struct and function. The first cut of the
// textile_scanner shim missed `enum class LineKind` because the grep matched
// only `^std::|^struct` -- 26 build errors reading "'LineKind' has not been
// declared". Org's scanner has the same shape, so the same three names:
// the enum, the struct, the entry point.
//
// Leading `::` is mandatory: inside `namespace duckdb` a bare `panduck::` binds
// to `duckdb::panduck`, the compat helpers in panduck_duckdb_compat.hpp, not to
// the library. Alphabetised because clang-format requires it.
using ::panduck::org::Line;
using ::panduck::org::LineKind;
using ::panduck::org::ScanOrg;

} // namespace org
} // namespace duckdb
