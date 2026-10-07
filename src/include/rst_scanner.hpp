#pragma once

#include "panduck/rst_scanner.hpp"

namespace duckdb {
namespace rst {

// THE SCANNER LIVES IN libpanduck (issue #104, L2):
// libpanduck/include/panduck/rst_scanner.hpp and libpanduck/src/rst_scanner.cpp.
// It never named a DuckDB type -- it sat in `namespace duckdb` by convention
// only -- so the move was a namespace swap and this shim keeps every existing
// `duckdb::rst::Line` spelling resolving.
//
// ENUMERATE THE MOVED HEADER'S SURFACE WITH A PATTERN THAT COVERS
// enum/using/constexpr, not just struct and function. The first cut of the
// textile_scanner shim missed `enum class LineKind` because the grep matched
// only `^std::|^struct` -- 26 build errors reading "'LineKind' has not been
// declared". RST's scanner has the same three names: the enum, the struct, the
// entry point.
//
// Leading `::` is mandatory: inside `namespace duckdb` a bare `panduck::` binds
// to `duckdb::panduck`, the compat helpers in panduck_duckdb_compat.hpp, not to
// the library. Alphabetised because clang-format requires it.
using ::panduck::rst::Line;
using ::panduck::rst::LineKind;
using ::panduck::rst::ScanRst;

} // namespace rst
} // namespace duckdb
