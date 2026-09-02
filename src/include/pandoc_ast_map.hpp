#pragma once

#include "duckdb.hpp"
#include <cstddef>

namespace duckdb {

//! Panduck's contract with the Pandoc AST.
//!
//! Panduck deliberately does NOT depend on pandoc. Pandoc is a Haskell program; the
//! only C bindings that ever existed (ShabbyX/libpandoc) went unmaintained in 2017 and
//! target pandoc 1.x, and upstream has declined to ship a Cabal foreign-library
//! (jgm/pandoc#6611, open since 2020). Linking the GHC runtime into a dlopen'd DuckDB
//! extension would be a bad neighbour even if it existed.
//!
//! So panduck is compatible with pandoc's DATA MODEL, not its ABI: its readers emit
//! duck_block elements, and duck_block_utils already round-trips those to and from
//! Pandoc JSON. This table is the single source of truth for that correspondence, and
//! test/pandoc/check_pandoc_alignment.py verifies it against a real pandoc binary.
namespace pandoc_ast {

//! Pandoc AST API version this mapping targets ("pandoc-api-version" in the JSON).
//! Bumping pandoc-types is a breaking AST change; the alignment test fails loudly
//! rather than letting the mapping drift silently.
static constexpr int API_VERSION_MAJOR = 1;
static constexpr int API_VERSION_MINOR = 23;
//! The third component, which only the EXPORT path needs: pandoc validates the full
//! "pandoc-api-version" triple it is handed and refuses a mismatch outright --
//!
//!     Incompatible API versions: encoded with [1,20] but attempted to decode with [1,23,1]
//!
//! -- which is how duck_block_utils v1.4.3 came to produce exports no pandoc could read.
//! It lives here beside the other two because the writer previously hardcoded the whole
//! triple twice, in two different literal forms (a vector of ints and a string), neither
//! of which was reachable from this header. Three copies of one fact is how the drift that
//! caused [1,20] becomes possible; there is now one.
static constexpr int API_VERSION_PATCH = 1;

//! State of a constructor's correspondence with duck_block.
//!  "mapped"  -- round-trip implemented in duck_block_utils today
//!  "planned" -- named in the spec, no implementation on either side yet
//!  "dropped" -- intentionally yields no element
static constexpr const char *STATUS_MAPPED = "mapped";
static constexpr const char *STATUS_PLANNED = "planned";
static constexpr const char *STATUS_DROPPED = "dropped";

struct Mapping {
	const char *pandoc_type;  //!< Pandoc constructor name, e.g. "Header"
	const char *kind;         //!< "block" or "inline"
	const char *element_type; //!< duck_block element_type, or nullptr when dropped
	const char *status;       //!< one of STATUS_*
	const char *notes;
};

extern const Mapping MAPPINGS[];
extern const size_t MAPPING_COUNT;

} // namespace pandoc_ast

//! Registers panduck_pandoc_ast_map() -- exposes MAPPINGS as a queryable table so the
//! contract can be asserted from SQL as well as from the python conformance harness.
void RegisterPandocAstMapFunction(ExtensionLoader &loader);

} // namespace duckdb
