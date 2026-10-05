#pragma once

// libpanduck -- the format engine's identity.
//
// Small on purpose, and always compiled: this is the one translation unit the
// standalone build has when every format module is switched off, which makes it
// the thing that proves the seam's foundations hold. It reaches the duck_block
// vocabulary and nothing else, so building it standalone demonstrates that the
// vendored vocabulary header really is portable (it includes only <cstdint>) --
// one of the two concessions recorded in panduck/vocabulary.hpp.

namespace panduck {

//! The duck_block SPEC_VERSION this engine was built against, as the vendored
//! vocabulary reports it. Consumers pin behaviour to the vocabulary, not to a
//! libpanduck release number -- the vocabulary is the results format, and it is
//! versioned upstream by duck_block_utils.
const char *SpecVersion();

//! Which format modules this build actually contains. Modules are compile-time
//! options (PANDUCK_WITH_*), and a composition may legitimately carry fewer
//! than the engine supports -- the DuckDB extension delegates markdown, HTML
//! and PDF to sibling extensions rather than building them here. A caller that
//! needs to know what it got asks, instead of assuming.
bool HasModule(const char *name);

} // namespace panduck
