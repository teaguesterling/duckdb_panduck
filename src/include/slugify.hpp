#pragma once

#include "panduck/slugify.hpp"

namespace duckdb {

// THE IMPLEMENTATION LIVES IN libpanduck (issue #104, milestone L2):
// libpanduck/include/panduck/slugify.hpp, which names no DuckDB type.
//
// FULLY QUALIFIED WITH A LEADING `::`, and it matters. Inside `namespace
// duckdb`, a bare `panduck::` resolves to `duckdb::panduck` -- the compat
// helpers in panduck_duckdb_compat.hpp -- and NOT to the library's global
// ::panduck.
//
// This using-declaration keeps `duckdb::HeadingSlug` resolving, so moving the
// implementation cost no call-site churn anywhere else in the extension. It
// is the seam's compatibility shim, not its interface -- new code should
// prefer panduck::HeadingSlug directly.
using ::panduck::HeadingSlug;

} // namespace duckdb
