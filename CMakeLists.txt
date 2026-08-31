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
# Both are resolved in Phase 1 even though no source calls them yet: that proves the
# dependency chain configures and links before any reader code exists to confound a
# failure.
find_package(pugixml CONFIG REQUIRED)
find_package(miniz CONFIG REQUIRED)

include_directories(src/include)

set(EXTENSION_SOURCES
    src/panduck_extension.cpp
    src/pandoc_ast_map.cpp
    src/reader_registry.cpp
    src/rtf_reader.cpp
    src/supported_extensions.cpp
)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

target_link_libraries(${EXTENSION_NAME} pugixml::pugixml miniz::miniz)
target_link_libraries(${LOADABLE_EXTENSION_NAME} pugixml::pugixml miniz::miniz)

set_property(TARGET ${EXTENSION_NAME} PROPERTY CXX_STANDARD 17)
set_property(TARGET ${LOADABLE_EXTENSION_NAME} PROPERTY CXX_STANDARD 17)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
