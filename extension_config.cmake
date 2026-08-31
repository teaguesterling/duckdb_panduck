# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo
duckdb_extension_load(panduck
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)

# NOTE (WASM): once panduck actually calls into pugixml/miniz, this load will need an
# explicit LINKED_LIBS list. The emcc -sSIDE_MODULE link only embeds libraries named
# here -- it ignores target_link_libraries() -- so a .wasm can ship green with
# unresolved imports that throw on first call. See duckdb_markdown issue #19.
# Deliberately omitted while Phase 1 has no call sites to leave unresolved.
