# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo
# WASM: the emcc -sSIDE_MODULE link embeds ONLY the libraries named in LINKED_LIBS --
# it ignores target_link_libraries() -- so a .wasm can ship green with unresolved
# imports that throw on first call (duckdb_markdown#19).
#
# The note that stood here said this list was "deliberately omitted while Phase 1 has
# no call sites to leave unresolved". That was true when written and stopped being true
# without anything noticing: pugixml is now called by the DOCX, ODT and EPUB readers and
# miniz by the ZIP container underneath all three. The condition the deferral rested on
# had expired, and nothing measured it -- a build-time green does not instantiate the
# module, so nothing ever would have.
#
# The values come from DUCKDB_EXTENSION_PANDUCK_LINKED_LIBS, set by CMakeLists.txt where
# the imported targets actually exist; see the long comment there for why not a genexpr
# in this file. test/wasm/ checks the built artifact for unresolved symbols.
duckdb_extension_load(panduck
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)

# Any extra extensions that should be built.
#
# httpfs: test/sql/vfs_readers.test reads fixtures over HTTPS behind a file-level
# `require httpfs`, to prove every reader goes through DuckDB's FileSystem rather
# than a private std::ifstream. Without this line that whole file is SILENTLY
# SKIPPED -- duckdb_markdown hit exactly that with `require json` (its issue #36),
# and a skipped file reports the same green as a passing one.
#
# AND NOTHING CURRENTLY GUARDS THAT. An earlier version of this comment claimed
# scripts/check_test_skips.py would make such a skip visible. It does not: that
# script scans for exactly two causes -- a bare `INSTALL x;` of a community
# extension, and `require-env` -- and does not predict a skip from
# `require <extension>`. Measured: with httpfs unbuilt, the new test file skipped
# and `check_test_skips.py --static` still reported "4 predicted skip(s), 4
# declared. OK". A silent green, from the guard whose whole purpose is catching
# silent greens.
#
# So this line IS the guard, and extending check_test_skips.py to predict
# `require <ext>` for anything not declared here is a real follow-up.
#
# WHY THIS TEST NEEDS THE NETWORK, since nothing else in the suite does: the defect
# it covers is precisely that a reader bypasses DuckDB's filesystem, and the only
# way to tell the two paths apart is a path DuckDB can resolve and the C++ standard
# library cannot. The URLs are pinned to an immutable commit so neither the remote
# nor a local fixture edit can drift the expected counts.
# httpfs is NOT an in-tree extension in duckdb v1.5.6 -- `duckdb/extension/` holds
# json, parquet, icu, tpch and friends, and no httpfs -- so the bare
# `duckdb_extension_load(httpfs)` that works for duckdb_markdown's `json` fails at
# configure with "duckdb/extension/httpfs which is not [a directory]". The GIT_TAG
# below is the exact sha duckdb v1.5.6 pins in
# duckdb/.github/config/extensions/httpfs.cmake, so the httpfs built here is the one
# version-matched to the submodule rather than whatever its main branch holds.
#
# APPLY_PATCHES is deliberately omitted: duckdb's own declaration uses it to apply
# patches from duckdb/.github/patches/extensions/httpfs, and that path is resolved
# relative to DuckDB's config dir, not this file's.
duckdb_extension_load(httpfs
    GIT_URL https://github.com/duckdb/duckdb-httpfs
    GIT_TAG 4bc690dba4496c765777a0269d48fdbaff7cdc11
)
