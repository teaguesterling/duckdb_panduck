#pragma once

#include "duckdb.hpp"

namespace duckdb {

class ExtensionLoader;

//! panduck's surface over the Pandoc AST converter.
//!
//! TABLE FUNCTIONS, not scalars, and that is the point rather than a detail. Every other
//! panduck reader is a table function so a filter pushes down into it; the scalar
//! LIST(duck_block) form plants a blocking aggregate no predicate can pass, which is fine
//! for a README and not for a 400-page document. A scalar-only reader would be the one
//! panduck format that cannot be filtered.
//!
//! THE NAMES ARE PANDUCK'S OWN and deliberately do not match duck_block_utils'
//! pandoc_ast_to_blocks / read_pandoc_ast. A name is owned by exactly one extension in
//! this family: two registrations of one name both survive as ambiguous overloads and
//! every call then fails at bind time. Upstream keeps its names until a released panduck
//! can be installed and verified, so for that window the two surfaces must not overlap.
void RegisterPandocReader(ExtensionLoader &loader);

} // namespace duckdb
