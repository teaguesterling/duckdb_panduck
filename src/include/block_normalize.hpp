#pragma once

#include "duck_block_normalize.hpp"
#include "duck_block_types.hpp"

namespace duckdb {

//! Spec 6.0's content rule over a finished block vector, bound to panduck's block type.
//!
//! The implementation is duck_block_utils' VENDORED header -- the vendorable unit Teague
//! asked them to prepare -- rather than a copy of the logic. That is the same arrangement
//! as duck_block_vocabulary.hpp: a C++ header has to be compiled in, so it is vendored and
//! covered by the drift check, whereas the conformance SQL is read from upstream at
//! runtime and never copied. Compiled things are vendored and checked; runtime things are
//! read.
//!
//! WHAT DID NOT COME WITH IT is the registration of `duck_blocks_normalize`. A name is
//! owned by exactly one extension in this family -- two registrations both survive as
//! ambiguous overloads and every call then fails at bind time -- so the function travels
//! and the SQL name stays upstream.
//!
//! The converter needs this because the rule is SIBLING-DEPENDENT: whether a text run
//! becomes its container's content or stays a `plain` depends on what FOLLOWS it, which a
//! converter walking Pandoc's tree does not know when it reaches the run. panduck's own
//! readers enforce the rule at emission instead, where they do.
inline void CollapseLonePlainIntoParent(vector<Value> &blocks) {
	duck_block::CollapseLonePlainIntoParent(blocks, DuckBlockTypes::DuckBlockType());
}

} // namespace duckdb
