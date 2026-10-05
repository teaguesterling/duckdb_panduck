#pragma once

#include "duck_block_vocabulary.hpp"

// THE SEAM'S ONE CONCESSION, and the reason it is confined to this file.
//
// The duck_block vocabulary is a vendored byte-exact copy of duck_block_utils'
// header (docs/architecture.md), and that header declares everything inside
// `namespace duckdb` -- see its lines 242..806. So the engine cannot name the
// constants without naming that namespace, even though nothing DuckDB-ish is
// involved: these are `static constexpr const char *` strings and the header
// includes only <cstdint>.
//
// Rather than let `duckdb::` appear throughout libpanduck, the dependency is
// aliased once, here. Because the alias lands in `namespace panduck`, every
// call site inside panduck::* keeps writing `DuckBlockVocabulary::KIND_VALUE`
// unchanged and resolves it through the enclosing namespace.
//
// scripts/check_libpanduck_seam.py masks THIS SITE specifically -- the vendored
// vocabulary's namespace -- and still flags every other `duckdb::` anywhere in
// libpanduck, including elsewhere in this file. Masking per site rather than
// exempting per file is deliberate: a file-wide exemption would take the real
// leaks with it.
//
// OPEN QUESTION FOR duck_block_utils, recorded rather than worked around: a
// vocabulary intended for consumers outside DuckDB would be more useful in a
// neutral namespace, or offered in both. That is upstream's call, not panduck's
// -- the copy here must stay byte-exact, so this alias is the only local move
// available.

namespace panduck {

using DuckBlockVocabulary = ::duckdb::DuckBlockVocabulary;

} // namespace panduck
