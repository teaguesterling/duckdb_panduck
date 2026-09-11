#pragma once

//! Fragment repair for the EXPORT path -- a vendored behaviour, not a panduck invention.
//!
//! duck_block spec 1.2 made fragments LEGAL INPUT and declared, in the vocabulary header,
//! what each one's implicit parent is: list_item -> list, caption -> figure, inline -> plain.
//! Before that, panduck's exporter dropped what it could not place at the top level:
//!
//!     6 list_items in  ->  "blocks": []      silently, no error
//!     a bare caption   ->  Para              degraded rather than wrapped
//!
//! duckeye reported the first as #36 and lost real debugging time to it: duck_blocks_to_md
//! rendered the same input correctly, so two writers disagreed about one document and only
//! one of them said so. An empty result is indistinguishable from "this input has no blocks".
//!
//! PORTED FROM duck_block_utils v3.1.0 (6c1c2e5) src/repair.cpp, NOT re-derived. panduck's
//! converter is a vendored copy of theirs and check-divergence exists to keep the two
//! aligned; solving this differently here would create exactly the drift that gate guards.
//!
//! THE PUBLIC FUNCTION IS DELIBERATELY NOT PORTED. Upstream registers `duck_blocks_repair`;
//! panduck needs the BEHAVIOUR on its own export path, not a second copy of a function
//! another extension in the same fleet already publishes. Registering it here would collide
//! the moment both are loaded, and panduck has verified zero function-name collisions across
//! the fleet as a property worth keeping.

#include "duckdb.hpp"
#include "duck_block_types.hpp"

namespace duckdb {
namespace panduck {

//! Wrap orphaned fragments in their implicit parents, rebase levels, collapse level jumps,
//! and renumber element_order densely from 0. A NO-OP on a whole document -- which is why it
//! is safe to apply unconditionally on export.
void RepairBlocks(vector<Value> &blocks);

} // namespace panduck
} // namespace duckdb
