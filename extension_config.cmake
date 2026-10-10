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
