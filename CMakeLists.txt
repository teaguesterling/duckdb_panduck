cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME panduck)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

# DuckDB's top-level CMake pins CMAKE_CXX_STANDARD to 11 in the CACHE, so a CACHE set
# here is a silent no-op. A plain (non-CACHE) set shadows it for this directory, and
# the per-target set_property below makes C++17 explicit regardless.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Dependencies come from vcpkg (see vcpkg.json) rather than a vendored third_party/,
# matching duckdb_markdown / duckdb_webbed / duckdb_zim.
#   pugixml -- DOCX/ODT/EPUB XML parsing (word/document.xml, .opf, nav.xhtml)
#   miniz   -- ZIP container reading for the OOXML/ODF/EPUB archive formats
find_package(pugixml CONFIG REQUIRED)
find_package(miniz CONFIG REQUIRED)

# WASM SIDE-MODULE LINKING. The loadable .wasm is produced by a SEPARATE emcc step:
#
#   emcc <archive> -o <out>.wasm -O3 -sSIDE_MODULE=2 ... ${TO_BE_LINKED}
#
# and TO_BE_LINKED is set from DUCKDB_EXTENSION_PANDUCK_LINKED_LIBS alone
# (duckdb/extension/extension_build_tools.cmake, the EMSCRIPTEN block at ~L183). The
# target_link_libraries() calls further down are IGNORED by that link. Anything not
# named here is left as an unresolved import: the .wasm builds, CI goes green because
# it never instantiates the module, and the first call into any DOCX/ODT/EPUB reader
# throws in the user's browser.
#
# This is a known failure class in this family of extensions, shipped three times
# already -- duckdb_markdown#19, duckdb_webbed#96, duckdb_yaml#40.
#
# Resolved HERE rather than as a $<TARGET_FILE:> genexpr in extension_config.cmake:
# that file is evaluated in duckdb's scope, where these imported targets need not be
# defined, and an unresolvable genexpr expands to the EMPTY STRING rather than
# erroring -- which is precisely the silent success this block exists to prevent.
if(WASM_LOADABLE_EXTENSIONS)
    macro(_panduck_resolve_imported_lib OUT TGT)
        get_target_property(${OUT} ${TGT} IMPORTED_LOCATION)
        if(NOT ${OUT})
            # A CONFIG package commonly sets only the config-specific location.
            get_target_property(${OUT} ${TGT} IMPORTED_LOCATION_RELEASE)
        endif()
    endmacro()
    _panduck_resolve_imported_lib(_panduck_pugixml_lib pugixml::pugixml)
    _panduck_resolve_imported_lib(_panduck_miniz_lib miniz::miniz)
    if(_panduck_pugixml_lib AND _panduck_miniz_lib)
        set(DUCKDB_EXTENSION_PANDUCK_LINKED_LIBS
            "${_panduck_pugixml_lib};${_panduck_miniz_lib}")
        message(STATUS "panduck WASM link libs: ${DUCKDB_EXTENSION_PANDUCK_LINKED_LIBS}")
    else()
        # Fail the BUILD rather than emit a .wasm that loads and then throws.
        message(FATAL_ERROR
            "panduck: could not resolve pugixml/miniz archive paths for the WASM link "
            "(pugixml='${_panduck_pugixml_lib}' miniz='${_panduck_miniz_lib}')")
    endif()
endif()

# The duck_block vocabulary header is VENDORED into src/include rather than reached
# through a submodule include path -- see src/include/duck_block_types.hpp for why, and
# for how to re-sync it against upstream.
include_directories(src/include)
# DuckDB vendors yyjson as its own target, so .ipynb parses with NO new dependency.
include_directories(duckdb/third_party/yyjson/include)

set(EXTENSION_SOURCES
    src/docx_reader.cpp
    src/panduck_extension.cpp
    src/epub_reader.cpp
    src/latex_macros.cpp
    src/latex_reader.cpp
    src/latex_tokenizer.cpp
    src/odt_reader.cpp
    src/mediawiki_reader.cpp
    src/mediawiki_scanner.cpp
    src/org_reader.cpp
    src/org_scanner.cpp
    src/rst_reader.cpp
    src/textile_reader.cpp
    src/textile_scanner.cpp
    src/rst_scanner.cpp
    src/ipynb_reader.cpp
    src/pandoc_block_convert.cpp
    src/pandoc_inline_convert.cpp
    src/pandoc_reader.cpp
    src/pandoc_ast_map.cpp
    src/reader_registry.cpp
    src/rtf_reader.cpp
    src/supported_extensions.cpp
    src/zip_container.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

target_link_libraries(${EXTENSION_NAME} pugixml::pugixml miniz::miniz duckdb_yyjson)
target_link_libraries(${LOADABLE_EXTENSION_NAME} pugixml::pugixml miniz::miniz duckdb_yyjson)

set_property(TARGET ${EXTENSION_NAME} PROPERTY CXX_STANDARD 17)
set_property(TARGET ${LOADABLE_EXTENSION_NAME} PROPERTY CXX_STANDARD 17)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
