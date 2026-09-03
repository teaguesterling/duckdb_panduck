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
    # Resolve the ARCHIVE behind an imported target.
    #
    # The dependencies are not shaped alike, which is why one property read is not enough:
    #   miniz::miniz      STATIC IMPORTED    -- archive on IMPORTED_LOCATION_<CONFIG>
    #   pugixml::pugixml  INTERFACE IMPORTED -- NO location; forwards
    #                                           INTERFACE_LINK_LIBRARIES to pugixml::static
    # A $<TARGET_FILE:> genexpr is not a way out either: it is a hard error on an
    # INTERFACE library.
    function(_panduck_resolve_archive OUT TGT)
        set(${OUT} "" PARENT_SCOPE)
        if(NOT TARGET ${TGT})
            return()
        endif()
        get_target_property(_type ${TGT} TYPE)
        if(_type STREQUAL "INTERFACE_LIBRARY")
            get_target_property(_deps ${TGT} INTERFACE_LINK_LIBRARIES)
            if(_deps)
                foreach(_d IN LISTS _deps)
                    if(TARGET ${_d})
                        _panduck_resolve_archive(_found ${_d})
                        if(_found)
                            set(${OUT} "${_found}" PARENT_SCOPE)
                            return()
                        endif()
                    endif()
                endforeach()
            endif()
            return()
        endif()
        get_target_property(_loc ${TGT} IMPORTED_LOCATION)
        if(NOT _loc)
            # Order matters. IMPORTED_CONFIGURATIONS commonly reads "DEBUG;RELEASE", so
            # taking the first entry that resolves links the DEBUG archive into a release
            # build -- which still links, still passes, and ships the wrong artifact.
            # Try the active config first, then optimised ones, and only then whatever
            # else the package happens to export.
            set(_prefer "")
            if(CMAKE_BUILD_TYPE)
                string(TOUPPER "${CMAKE_BUILD_TYPE}" _bt)
                list(APPEND _prefer ${_bt})
            endif()
            list(APPEND _prefer RELEASE RELWITHDEBINFO MINSIZEREL NOCONFIG)
            get_target_property(_cfgs ${TGT} IMPORTED_CONFIGURATIONS)
            if(_cfgs)
                list(APPEND _prefer ${_cfgs})
            endif()
            list(REMOVE_DUPLICATES _prefer)
            foreach(_c IN LISTS _prefer)
                get_target_property(_loc ${TGT} IMPORTED_LOCATION_${_c})
                if(_loc)
                    break()
                endif()
            endforeach()
        endif()
        if(_loc)
            set(${OUT} "${_loc}" PARENT_SCOPE)
        endif()
    endfunction()

    _panduck_resolve_archive(_panduck_pugixml_lib pugixml::pugixml)
    _panduck_resolve_archive(_panduck_miniz_lib miniz::miniz)
    if(_panduck_pugixml_lib AND _panduck_miniz_lib)
        set(DUCKDB_EXTENSION_PANDUCK_LINKED_LIBS
            "${_panduck_pugixml_lib};${_panduck_miniz_lib}")
        # Printed so the CI log shows WHICH archives were linked -- a debug archive
        # resolves every symbol just as well as a release one, so the symbol check
        # downstream cannot tell them apart. This line is the only place that can.
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
