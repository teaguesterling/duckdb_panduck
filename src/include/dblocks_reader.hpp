#pragma once

namespace duckdb {

class ExtensionLoader;

//! The compact duck_blocks line format (.dblocks), read and written natively from the
//! codec vendored in duck_block_compact.hpp: read_dblocks_blocks, its _string form,
//! panduck_dblocks_to_blocks, panduck_blocks_to_dblocks and panduck_write_dblocks.
//!
//! THE FORMAT IS duck_block_utils'. Its SQL macros (duck_blocks_to_compact,
//! duck_blocks_from_compact, read_compact_blocks) and this codec are built from the
//! same code tables, and the golden corpus in test/fixtures/dblocks holds the two in
//! step. These names are panduck's own: a name has exactly one owner in this family.
void RegisterDblocksFunctions(ExtensionLoader &loader);

} // namespace duckdb
