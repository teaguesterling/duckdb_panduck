#pragma once

#include "panduck/ipynb.hpp"

#include "duckdb.hpp"

#include <string>
#include <vector>

namespace duckdb {

class ExtensionLoader;

namespace ipynb {

// THE PARSE CORE LIVES IN libpanduck (issue #104, milestone L2):
// libpanduck/include/panduck/ipynb.hpp and libpanduck/src/ipynb.cpp, which name
// no DuckDB type. Only the table function, its bind and its registration stay on
// this side of the seam.
//
// These using-declarations keep `duckdb::ipynb::ParseIpynbString` and friends
// resolving, so moving the core cost no call-site churn anywhere else in the
// extension. They are the seam's compatibility shim, not its interface -- new
// code should prefer panduck::ipynb directly.
using panduck::ipynb::IpynbBlock;
using panduck::ipynb::IpynbInline;
using panduck::ipynb::ParseIpynbString;

void RegisterIpynbReader(ExtensionLoader &loader);

} // namespace ipynb
} // namespace duckdb
