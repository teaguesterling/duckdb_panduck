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

# miniz IS THE DEPENDENCY CASE yyjson WAS NOT, and the contrast is the whole
# reason this comment exists next to the one above.
#
# yyjson could not be reused because DuckDB's copy is VENDORED AND
# DuckDB-flavoured: its header does `#include "duckdb/common/fast_mem.hpp"` and
# it lives in `namespace duckdb_yyjson`, so pointing at
# duckdb/third_party/yyjson/include configures and then fails to compile.
#
# MEASURED, rather than assumed by analogy: miniz reaches the extension build
# through `find_package(miniz CONFIG REQUIRED)` in the ROOT CMakeLists (line
# ~22), from vcpkg -- `vcpkg.json` lists `miniz` -- and its package files are
# ordinary vcpkg ones at
#   <vcpkg_installed>/<triplet>/share/miniz/minizConfig.cmake
#   <vcpkg_installed>/<triplet>/include/miniz/miniz.h
# No duckdb/ include, no DuckDB namespace, nothing vendored. So the standalone
# build can use the same `find_package` the extension does, and unlike yyjson
# it actually works.
#
# NO include-dir-only escape hatch, deliberately, which is the one place this
# does NOT mirror PANDUCK_YYJSON_INCLUDE_DIR above: miniz is a compiled library,
# and an include path alone would configure, compile (panduck is STATIC, so it
# never links) and then fail at the first executable that linked it. A path
# setting that defers the error to someone else's link line is worse than
# requiring the package.
#
# This is OFF in CI, and that is correct rather than a gap: the libpanduck-seam
# job configures with no toolchain file, so find_package finds nothing and the
# zip module is simply not built -- exactly what happens to ipynb there. The
# extension build is unaffected either way; it compiles these sources itself
# with miniz already resolved.
find_package(miniz CONFIG QUIET)
if(miniz_FOUND)
  message(STATUS "libpanduck: using vcpkg/system miniz")
  set(_zip_default ON)
else()
  set(_zip_default OFF)
endif()
option(PANDUCK_WITH_ZIP "Build the shared ZIP container reader (requires miniz)" ${_zip_default})

# pugixml IS THE SECOND DEPENDENCY OF THE SAME SHAPE AS miniz, and like miniz it
# is nothing like yyjson. It reaches the extension build through
# `find_package(pugixml CONFIG REQUIRED)` in the ROOT CMakeLists (line ~21), from
# vcpkg -- `vcpkg.json` lists `pugixml` -- and its package files are ordinary
# vcpkg ones: no duckdb/ include, no DuckDB namespace, nothing vendored. So the
# standalone build can use the same find_package the extension does, and unlike
# yyjson it actually links. VERIFIED BY FORCE-LINKING, not by `panduck_tests`
# building: an unused object is never pulled from a static archive, so a test
# binary that happens to link proves nothing about whether pugixml resolves.
#
# NO include-dir-only escape hatch, for miniz's reason: pugixml is a compiled
# library, so a bare include path would configure, compile (panduck is STATIC and
# never links) and then fail at someone else's link line.
#
# OFF in CI, and correct rather than a gap: the libpanduck-seam job passes no
# toolchain file, so find_package finds nothing and docx/odt/epub are simply not
# built -- exactly what happens to ipynb and zip there.
find_package(pugixml CONFIG QUIET)
if(pugixml_FOUND)
  message(STATUS "libpanduck: using vcpkg/system pugixml")
endif()

# THE THREE CONTAINER READERS NEED BOTH, so their default is the conjunction. A
# format module whose default depended on one dependency could be switched on
# with the other missing and fail at compile; `AND` is the whole of the check.
if(pugixml_FOUND AND miniz_FOUND)
  set(_container_default ON)
else()
  set(_container_default OFF)
endif()

# docx, odt and epub are the TIER 2 READERS PROPER -- the three that needed a
# dependency decision rtf did not (see PANDUCK_WITH_RTF below). Each is a ZIP
# with its document in a known member, so each needs ::panduck::ZipContainer
# (hence miniz) and pugixml, and each gets its option here as it moves.
#
# PANDUCK_WITH_ZIP is a SEPARATE option and these do not force it on. Switching
# zip off while leaving a container reader on is a configure the source list has
# to handle rather than reject, and it handles it by adding zip_container.cpp
# once for whichever of the four asked -- see the list below. The zip option then
# means only what HasModule("zip") reports.
option(PANDUCK_WITH_DOCX "Build the docx format module (requires pugixml and miniz)"
       ${_container_default})
option(PANDUCK_WITH_ODT "Build the odt format module (requires pugixml and miniz)"
       ${_container_default})

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

# zip IS NOT A FORMAT MODULE, and is listed with them because the option
# mechanism is the same one. ZipContainer is shared INFRASTRUCTURE: docx, odt
# and epub are each a ZIP with their document in a known member, and none of the
# three can move behind the seam until the archive handling has. It carries no
# reader and emits no rows. Its option is declared above with find_package(miniz)
# because, unlike every format module, its default depends on a dependency being
# found.

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

if(PANDUCK_WITH_DOCX)
  list(APPEND PANDUCK_SOURCES src/docx.cpp)
endif()

if(PANDUCK_WITH_ODT)
  list(APPEND PANDUCK_SOURCES src/odt.cpp)
endif()

# THE SHARED CONTAINER READER, ADDED ONCE FOR WHOEVER ASKED. It used to follow
# PANDUCK_WITH_ZIP alone, which was right while nothing behind the seam used it.
# odt does now, so `-DPANDUCK_WITH_ZIP=OFF -DPANDUCK_WITH_ODT=ON` would have
# configured, compiled (panduck is STATIC, so it never links) and then failed at
# the first executable that linked it -- the same deferred-error shape the
# find_package comments above refuse for an include-dir escape hatch.
set(PANDUCK_NEEDS_ZIP_CONTAINER OFF)
if(PANDUCK_WITH_ZIP
   OR PANDUCK_WITH_DOCX
   OR PANDUCK_WITH_ODT)
  set(PANDUCK_NEEDS_ZIP_CONTAINER ON)
  list(APPEND PANDUCK_SOURCES src/zip_container.cpp)
endif()

# Likewise for pugixml, which only the container readers name.
set(PANDUCK_NEEDS_PUGIXML OFF)
if(PANDUCK_WITH_DOCX OR PANDUCK_WITH_ODT)
  set(PANDUCK_NEEDS_PUGIXML ON)
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

if(PANDUCK_WITH_DOCX)
  target_compile_definitions(panduck PUBLIC PANDUCK_WITH_DOCX)
endif()

if(PANDUCK_WITH_ODT)
  target_compile_definitions(panduck PUBLIC PANDUCK_WITH_ODT)
endif()

if(PANDUCK_WITH_ZIP)
  target_compile_definitions(panduck PUBLIC PANDUCK_WITH_ZIP)
endif()

# LINKED FROM THE "NEEDS" FLAGS, not from the module options, so a container
# reader built with PANDUCK_WITH_ZIP=OFF still gets miniz.
if(PANDUCK_NEEDS_ZIP_CONTAINER)
  target_link_libraries(panduck PRIVATE miniz::miniz)
endif()

if(PANDUCK_NEEDS_PUGIXML)
  target_link_libraries(panduck PRIVATE pugixml::pugixml)
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
  # NO test/test_docx.cpp and NO test/test_odt.cpp, for rtf's reason below -- and odt has a second
  # one on top of it: a standalone odt assertion needs a .odt FIXTURE on disk,
  # which means this target would grow a data dependency on test/fixtures/. That
  # is a decision about what the standalone suite is allowed to read, not a side
  # effect of moving a reader.
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
  STATUS "libpanduck modules: docx=${PANDUCK_WITH_DOCX}"
         " ipynb=${PANDUCK_WITH_IPYNB}"
         " latex=${PANDUCK_WITH_LATEX}"
         " mediawiki=${PANDUCK_WITH_MEDIAWIKI} org=${PANDUCK_WITH_ORG}"
         " odt=${PANDUCK_WITH_ODT}"
         " rst=${PANDUCK_WITH_RST} rtf=${PANDUCK_WITH_RTF}"
         " textile=${PANDUCK_WITH_TEXTILE}"
         " zip=${PANDUCK_WITH_ZIP}"
         " (tests=${PANDUCK_BUILD_TESTS})")
