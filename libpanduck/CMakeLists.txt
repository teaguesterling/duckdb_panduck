cmake_minimum_required(VERSION 3.16)
project(libpanduck CXX)

# THIS BUILD EXISTS TO PROVE THE SEAM, NOT TO SHIP A LIBRARY (issue #104, L2).
#
# panduck's own CMakeLists compiles these sources straight into the extension,
# where duckdb's include paths are applied directory-wide -- so a child target
# there would inherit them and "no DuckDB in libpanduck" could not be enforced
# by the compiler. Configuring THIS project on its own is the enforcement: it
# never adds duckdb to the include path, so a stray `#include "duckdb.hpp"`
# fails here while still compiling fine in the extension build.
# scripts/check_libpanduck_seam.py is the fast local signal.
#
# No install rules, no versioning, no release artifact. Those belong to L3, and
# only once the contract has stopped moving (split-after-stability).

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The duck_block vocabulary is a VENDORED BYTE-EXACT COPY owned by panduck
# (src/include/duck_block_vocabulary.hpp, see docs/architecture.md). It includes
# only <cstdint>, so the engine uses it unchanged. Pointed at rather than
# copied: a second copy would be a second thing to keep byte-exact.
set(PANDUCK_VOCABULARY_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/include"
    CACHE PATH "Directory holding duck_block_vocabulary.hpp")

# FORMAT MODULES ARE COMPILE-TIME OPTIONS. A composition may carry fewer modules
# than the engine supports: the DuckDB extension delegates markdown, HTML and
# PDF to sibling extensions rather than building them, while a standalone
# consumer would want them built in. The provider is a property of the
# composition, not of the format (docs/roadmap.md, Phase 4).

# MEASURED, so the next person does not retry it: DuckDB's vendored yyjson is
# NOT reusable outside duckdb. Its own header does
#   #include "duckdb/common/fast_mem.hpp"      (yyjson.hpp:45)
# so pointing this at duckdb/third_party/yyjson/include configures fine and then
# fails to compile. libpanduck therefore needs ITS OWN yyjson -- a system or
# vcpkg one -- and that is a real dependency decision, not a path setting.
#
# It is deliberately NOT taken here: vcpkg.json drives every platform build of
# the shipped extension including Wasm, and adding a dependency to it during the
# v2.0 migration is not a side effect to smuggle into a seam commit. Until it is
# decided, the ipynb module is off by default in the standalone build; the
# extension build is unaffected, since it compiles these sources itself with
# duckdb's yyjson already on the include path.
set(PANDUCK_YYJSON_INCLUDE_DIR
    ""
    CACHE PATH "Directory holding a STANDALONE yyjson (not duckdb's vendored copy)")

# yyjson is now a vcpkg dependency (vcpkg.json), which is what lets the ipynb
# module build standalone at all.
#
# TODO, and deliberately not done here: the EXTENSION build does not need it --
# it already compiles these sources with duckdb's vendored yyjson on the include
# path. So either
#   (a) drop yyjson from vcpkg.json once libpanduck links its own copy by some
#       other route, or
#   (b) make the dependency conditional, so an extension build links duckdb's
#       and only a standalone build pulls vcpkg's.
# Carrying both costs a dependency on every platform build (Wasm included) for a
# target that only the standalone configure uses. Left as one manifest entry for
# now because the alternative is build-system surgery during the v2.0 migration.
find_package(yyjson CONFIG QUIET)
if(yyjson_FOUND AND NOT PANDUCK_YYJSON_INCLUDE_DIR)
  message(STATUS "libpanduck: using vcpkg/system yyjson")
endif()

if(PANDUCK_YYJSON_INCLUDE_DIR OR yyjson_FOUND)
  set(_ipynb_default ON)
else()
  set(_ipynb_default OFF)
endif()
option(PANDUCK_WITH_IPYNB "Build the ipynb format module (requires a standalone yyjson)"
       ${_ipynb_default})

# latex is ONE MODULE IN THREE FILES -- parse core, tokenizer, macro table -- and
# all three sit behind this one option because the reader cannot work without
# either of the others. Like org, rst, textile and mediawiki it needs no
# third-party dependency, so unlike ipynb it defaults ON.
option(PANDUCK_WITH_LATEX "Build the latex format module (core, tokenizer and macro table)" ON)

# mediawiki, like org, rst and textile, needs no third-party dependency -- its
# scanner and parser are plain C++ over the vocabulary -- so unlike ipynb it
# defaults ON.
option(PANDUCK_WITH_MEDIAWIKI "Build the mediawiki format module" ON)

# rst, like org and textile, needs no third-party dependency -- its scanner and
# parser are plain C++ over the vocabulary -- so unlike ipynb it defaults ON.
option(PANDUCK_WITH_RST "Build the rst format module" ON)

# rtf is the FIRST TIER 2 READER behind the seam, and it defaults ON for the same
# reason the Tier 1 modules do: no third-party dependency. RTF is 7-bit ASCII
# with brace-delimited groups and backslash control words, so it needs neither
# miniz nor pugixml. Its three Tier 2 siblings -- docx, odt, epub -- additionally
# need pugixml and a zip container, which is a real dependency decision for this
# build and is deliberately not taken here.
option(PANDUCK_WITH_RTF "Build the rtf format module" ON)

# org, like textile, needs no third-party dependency -- its scanner and parser
# are plain C++ over the vocabulary -- so unlike ipynb it defaults ON.
option(PANDUCK_WITH_ORG "Build the org format module" ON)

# textile needs no third-party dependency -- its scanner and parser are plain
# C++ over the vocabulary -- so unlike ipynb it defaults ON.
option(PANDUCK_WITH_TEXTILE "Build the textile format module" ON)

# Always present, so the target is never empty and the standalone build always
# has something to prove: panduck.cpp reaches the vendored vocabulary and
# nothing else, so compiling it here demonstrates that header really is
# portable -- one of the two concessions in panduck/vocabulary.hpp.
set(PANDUCK_SOURCES src/panduck.cpp)

if(PANDUCK_WITH_LATEX)
  list(APPEND PANDUCK_SOURCES src/latex.cpp src/latex_tokenizer.cpp src/latex_macros.cpp)
endif()

if(PANDUCK_WITH_MEDIAWIKI)
  list(APPEND PANDUCK_SOURCES src/mediawiki.cpp src/mediawiki_scanner.cpp)
endif()

if(PANDUCK_WITH_ORG)
  list(APPEND PANDUCK_SOURCES src/org.cpp src/org_scanner.cpp)
endif()

if(PANDUCK_WITH_RST)
  list(APPEND PANDUCK_SOURCES src/rst.cpp src/rst_scanner.cpp)
endif()

if(PANDUCK_WITH_RTF)
  list(APPEND PANDUCK_SOURCES src/rtf.cpp)
endif()

if(PANDUCK_WITH_TEXTILE)
  list(APPEND PANDUCK_SOURCES src/textile.cpp src/textile_scanner.cpp)
endif()

if(PANDUCK_WITH_IPYNB)
  list(APPEND PANDUCK_SOURCES src/ipynb.cpp)
endif()

add_library(panduck STATIC ${PANDUCK_SOURCES})

target_include_directories(panduck PUBLIC include)
target_include_directories(panduck PRIVATE "${PANDUCK_VOCABULARY_DIR}")

if(PANDUCK_WITH_LATEX)
  target_compile_definitions(panduck PUBLIC PANDUCK_WITH_LATEX)
endif()

if(PANDUCK_WITH_MEDIAWIKI)
  target_compile_definitions(panduck PUBLIC PANDUCK_WITH_MEDIAWIKI)
endif()

if(PANDUCK_WITH_ORG)
  target_compile_definitions(panduck PUBLIC PANDUCK_WITH_ORG)
endif()

if(PANDUCK_WITH_RST)
  target_compile_definitions(panduck PUBLIC PANDUCK_WITH_RST)
endif()

if(PANDUCK_WITH_RTF)
  target_compile_definitions(panduck PUBLIC PANDUCK_WITH_RTF)
endif()

if(PANDUCK_WITH_TEXTILE)
  target_compile_definitions(panduck PUBLIC PANDUCK_WITH_TEXTILE)
endif()

if(PANDUCK_WITH_IPYNB)
  target_compile_definitions(panduck PUBLIC PANDUCK_WITH_IPYNB)
  if(PANDUCK_YYJSON_INCLUDE_DIR)
    target_include_directories(panduck PRIVATE "${PANDUCK_YYJSON_INCLUDE_DIR}")
  elseif(yyjson_FOUND)
    target_link_libraries(panduck PRIVATE yyjson::yyjson)
  endif()
endif()

# THE STANDALONE TEST SUITE, and the hole it fills (issue #104, L2).
#
# The seam had two guards and a gap: check_libpanduck_seam.py proves no DuckDB
# NAME appears, and configuring this project proves the engine BUILDS with no
# duckdb on the include path. Neither proves a module WORKS without DuckDB --
# every assertion about ReadOrg, ReadRst, ReadLatex, ReadMediaWiki, ReadTextile
# and ReadIpynb ran through SQL in test/sql/*.test, i.e. through DuckDB. This
# target is the first thing that calls them with no DuckDB in the process.
#
# NO TEST FRAMEWORK, deliberately: see the note at the top of
# test/panduck_test.hpp. Linking gtest or Catch2 to prove libpanduck needs
# nothing would undo the thing being proved, so the harness is two counters and
# three macros.
#
# Default ON, and the runner EXITS NON-ZERO IF IT RAN ZERO CHECKS, so a
# configure that happens to switch every module off cannot report a green test.
option(PANDUCK_BUILD_TESTS "Build libpanduck's standalone tests" ON)

if(PANDUCK_BUILD_TESTS)
  enable_testing()

  # test_engine.cpp is unconditional, like the engine's own translation unit: it
  # covers SpecVersion() and HasModule(), which exist in every configure. It is also
  # what keeps this target meaningful with every format module switched off.
  set(PANDUCK_TEST_SOURCES test/panduck_test.cpp test/test_main.cpp test/test_engine.cpp)

  # Per-module test files follow their module's option, exactly as the sources
  # do -- a test referencing a module that was not built must not break the
  # configure. Each file is ALSO #ifdef-guarded internally, so the guard holds
  # even if someone adds it to the list unconditionally.
  if(PANDUCK_WITH_IPYNB)
    list(APPEND PANDUCK_TEST_SOURCES test/test_ipynb.cpp)
  endif()
  if(PANDUCK_WITH_LATEX)
    list(APPEND PANDUCK_TEST_SOURCES test/test_latex.cpp)
  endif()
  if(PANDUCK_WITH_MEDIAWIKI)
    list(APPEND PANDUCK_TEST_SOURCES test/test_mediawiki.cpp)
  endif()
  if(PANDUCK_WITH_ORG)
    list(APPEND PANDUCK_TEST_SOURCES test/test_org.cpp)
  endif()
  if(PANDUCK_WITH_RST)
    list(APPEND PANDUCK_TEST_SOURCES test/test_rst.cpp)
  endif()
  # NO test/test_rtf.cpp, and the gap is deliberate rather than an oversight.
  # Adding a module its first standalone assertions in the same commit that moves
  # it would be implementation and tests changing together, which this project
  # does not do. rtf's behaviour is pinned through SQL in test/sql/rtf_reader.test
  # meanwhile; a standalone test module is its own change.
  if(PANDUCK_WITH_TEXTILE)
    list(APPEND PANDUCK_TEST_SOURCES test/test_textile.cpp)
  endif()

  add_executable(panduck_tests ${PANDUCK_TEST_SOURCES})
  target_link_libraries(panduck_tests PRIVATE panduck)
  target_include_directories(panduck_tests PRIVATE test)

  # panduck/vocabulary.hpp is a PUBLIC header whose own include of
  # duck_block_vocabulary.hpp resolves through a PRIVATE include directory on
  # the library target, so a consumer that names the vocabulary needs the path
  # itself. Repeated here rather than widened on `panduck`: making it PUBLIC
  # changes the library's interface, which is an L3 decision about what
  # libpanduck installs, not a side effect of adding tests.
  target_include_directories(panduck_tests PRIVATE "${PANDUCK_VOCABULARY_DIR}")

  add_test(NAME panduck_tests COMMAND panduck_tests)
endif()

message(
  STATUS "libpanduck modules: ipynb=${PANDUCK_WITH_IPYNB}"
         " latex=${PANDUCK_WITH_LATEX}"
         " mediawiki=${PANDUCK_WITH_MEDIAWIKI} org=${PANDUCK_WITH_ORG}"
         " rst=${PANDUCK_WITH_RST} rtf=${PANDUCK_WITH_RTF}"
         " textile=${PANDUCK_WITH_TEXTILE}"
         " (tests=${PANDUCK_BUILD_TESTS})")
