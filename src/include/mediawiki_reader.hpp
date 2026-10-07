#pragma once

#include "panduck/mediawiki.hpp"

#include "duckdb.hpp"

#include <map>
#include <string>
#include <vector>

namespace duckdb {

class ExtensionLoader;

namespace mediawiki {

// THE PARSE CORE AND FLATTENING LIVE IN libpanduck (issue #104, L2):
// libpanduck/include/panduck/mediawiki.hpp and libpanduck/src/mediawiki.cpp,
// which name no DuckDB type. Only the table function, its binds and its
// registration stay on this side of the seam.
//
// Leading `::` is mandatory: inside `namespace duckdb` a bare `panduck::` binds
// to `duckdb::panduck`, the compat helpers in panduck_duckdb_compat.hpp, not to
// the library. Enumerate the moved header's surface with a pattern covering
// enum/using/constexpr as well as struct and function -- the first textile
// scanner shim missed `LineKind` exactly that way. Alphabetised for
// clang-format.
using ::panduck::mediawiki::MwBlock;
using ::panduck::mediawiki::MwInline;
using ::panduck::mediawiki::ParseMediaWikiString;
using ::panduck::mediawiki::ReadMediaWiki;

void RegisterMediaWikiReader(ExtensionLoader &loader);

} // namespace mediawiki
} // namespace duckdb
