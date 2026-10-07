#pragma once

#include "panduck/mediawiki_scanner.hpp"

namespace duckdb {
namespace mediawiki {

// THE SCANNER LIVES IN libpanduck (issue #104, L2):
// libpanduck/include/panduck/mediawiki_scanner.hpp and
// libpanduck/src/mediawiki_scanner.cpp. It never named a DuckDB type -- it sat
// in `namespace duckdb` by convention only -- so the move was a namespace swap
// and this shim keeps every existing `duckdb::mediawiki::Line` spelling
// resolving.
//
// ENUMERATE THE MOVED HEADER'S SURFACE WITH A PATTERN THAT COVERS
// enum/using/constexpr, not just struct and function. The first cut of the
// textile_scanner shim missed `enum class LineKind` because the grep matched
// only `^std::|^struct` -- 26 build errors reading "'LineKind' has not been
// declared". This scanner exports FOUR names, one more than org's and rst's:
// the enum, the struct, the entry point, and SplitCells.
//
// Leading `::` is mandatory: inside `namespace duckdb` a bare `panduck::` binds
// to `duckdb::panduck`, the compat helpers in panduck_duckdb_compat.hpp, not to
// the library. Alphabetised because clang-format requires it.
using ::panduck::mediawiki::Line;
using ::panduck::mediawiki::LineKind;
using ::panduck::mediawiki::ScanMediaWiki;
using ::panduck::mediawiki::SplitCells;

} // namespace mediawiki
} // namespace duckdb
