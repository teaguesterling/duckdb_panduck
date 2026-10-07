#pragma once

#include "panduck/latex.hpp"

#include "duckdb.hpp"

#include <string>
#include <vector>

namespace duckdb {

class ExtensionLoader;

namespace latex {

// THE PARSE CORE AND THE ROW FLATTENING LIVE IN libpanduck (issue #104, L2):
// libpanduck/include/panduck/latex.hpp and libpanduck/src/latex.cpp, which name
// no DuckDB type. Only the two table functions, their binds, their scan and
// their registration stay on this side of the seam -- including DuckDB's
// FileSystem, which read_latex_blocks reads a path through.
//
// The tokenizer and the macro table moved too, and are reached through
// latex_tokenizer.hpp and panduck/latex_macros.hpp respectively. latex is ONE
// module in three files: the reader cannot work without either of them.
//
// HeadingLevelFor is re-exported because tests call it by name, not because the
// tail needs it.
//
// Leading `::` is mandatory: inside `namespace duckdb` a bare `panduck::` binds
// to `duckdb::panduck`, the compat helpers in panduck_duckdb_compat.hpp, not to
// the library. Enumerate the moved header's surface with a pattern covering
// enum/using/constexpr/class as well as struct and function -- the first textile
// scanner shim missed `LineKind` exactly that way. Alphabetised for
// clang-format.
using ::panduck::latex::HeadingLevelFor;
using ::panduck::latex::LatexBlock;
using ::panduck::latex::LatexInline;
using ::panduck::latex::ParseLatexString;
using ::panduck::latex::ReadLatex;

} // namespace latex

void RegisterLatexReaderFunction(ExtensionLoader &loader);

} // namespace duckdb
