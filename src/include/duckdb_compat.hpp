#pragma once

#include "duckdb.hpp"
#include <type_traits>
#include <utility>

// duckdb_compat.hpp — fleet-standard cross-version shim for DuckDB extensions.
//
// Cross-version coverage:
//   - duckdb v1.4.x / v1.5.x: old API
//   - duckdb main (the v2.0 line): new API
//
// DETECT THE MEMBER, NOT A PROXY HEADER.
//
// The original form of this header keyed on
// `__has_include("duckdb/common/vector/list_vector.hpp")` -- a header that moved in the
// same refactor as the API change (duckdb/duckdb#22377). That is a proxy: it asks "did
// that refactor's headers arrive" rather than "does this method exist here", and the two
// can separate. They demonstrably do. `duckdb/common/identifier.hpp` was BACKPORTED to
// the v1.5-variegata branch WITHOUT the signature change that made it interesting, so a
// sibling extension's __has_include probe on that header is now correct only by virtue of
// its submodule pin predating the backport -- and would flip to the wrong branch on the
// next bump. A header arrives before the behaviour does.
//
// Probing the member cannot go wrong that way: whichever DuckDB is in the tree, the
// question asked is the one that matters.
//
// `if constexpr` discards the untaken branch only inside a template, hence the template
// parameter -- SetChildCardinality does not exist on the pinned line and a non-template
// `if constexpr` would still have to parse it.
//
// The __has_include below is KEPT, but only for what it is actually good for: pulling in
// headers that exist on one line and not the other. It no longer selects a branch. The
// 16 translation units that include this file inherited these two declarations from it
// when they were added, and dropping them is a separate change from fixing the probe.
#if __has_include("duckdb/common/vector/list_vector.hpp")
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#endif

namespace duckdb {

// --- Output chunk finalization ---
//
// v1.5.x: DataChunk::SetCardinality(count) sets chunk.count and nothing else.
// main:   SetCardinality is [[deprecated]] but PRESERVED with that same behaviour; the
//         new SetChildCardinality(count) additionally sizes every child vector, which is
//         what an operator reading vec.Size() needs. Without it a caller that builds its
//         output through the chunk's own vectors can hit
//           "Mismatch in input vector sizes ... expected 0 rows but got N"
//
// The two are NOT interchangeable on main -- SetChildCardinality resizes the children,
// which upstream notes would overwrite data written by a caller that mutated them first.
// So this shim is for the cardinality-BEFORE-write order only, which is how its one call
// site is written.
template <class T, class = void>
struct CompatHasSetChildCardinality : std::false_type {};
template <class T>
struct CompatHasSetChildCardinality<T, decltype(void(std::declval<T &>().SetChildCardinality(idx_t(0))))>
    : std::true_type {};

template <class CHUNK = DataChunk>
inline void CompatSetOutputCardinality(CHUNK &chunk, idx_t count) {
	if constexpr (CompatHasSetChildCardinality<CHUNK>::value) {
		chunk.SetChildCardinality(count);
	} else {
		chunk.SetCardinality(count);
	}
}

} // namespace duckdb
