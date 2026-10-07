#pragma once

#include "panduck/block_json.hpp"

namespace duckdb {

// THE IMPLEMENTATION LIVES IN libpanduck (issue #104, milestone L2):
// libpanduck/include/panduck/block_json.hpp, which names no DuckDB type.
//
// FULLY QUALIFIED WITH A LEADING `::`, and it matters. Inside `namespace
// duckdb`, a bare `panduck::` resolves to `duckdb::panduck` -- the compat
// helpers in panduck_duckdb_compat.hpp -- and NOT to the library's global
// ::panduck.
//
// These using-declarations keep `duckdb::JsonEscapeString` and
// `duckdb::BuildTableJson` resolving, so moving the implementation cost no
// call-site churn anywhere else in the extension. They are the seam's
// compatibility shim, not its interface -- new code should prefer
// panduck::JsonEscapeString / panduck::BuildTableJson directly.
using ::panduck::BuildTableJson;
using ::panduck::JsonEscapeString;

} // namespace duckdb
