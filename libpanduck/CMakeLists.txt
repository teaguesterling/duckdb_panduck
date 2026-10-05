cmake_minimum_required(VERSION 3.16)
project(libpanduck CXX)

# THIS BUILD EXISTS TO PROVE THE SEAM, NOT TO SHIP A LIBRARY (issue #104, L2).
#
# panduck's own CMakeLists compiles these sources straight into the extension,
# where duckdb's include paths are applied directory-wide -- so a child target
# there would inherit them and "no DuckDB in libpanduck" could not be enforced
# by the compiler. Configuring THIS project on its own is the enforcement: it
# never adds duckdb to the include path, so a stray `#include "duckdb.hpp"`
# fails here while still compiling fine in the extension build. CI runs it as a
# compile check; scripts/check_libpanduck_seam.py is the fast local signal.
#
# No install rules, no versioning, no release artifact. Those belong to L3, and
# only once the contract has stopped moving (split-after-stability).

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The duck_block vocabulary is a VENDORED BYTE-EXACT COPY owned by panduck
# (src/include/duck_block_vocabulary.hpp, see docs/architecture.md). It includes
# only <cstdint>, so the engine can use it unchanged. Pointed at rather than
# copied: a second copy would be a second thing to keep byte-exact.
set(PANDUCK_VOCABULARY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../src/include"
    CACHE PATH "Directory holding duck_block_vocabulary.hpp")

# yyjson is the one third-party dependency the ipynb core has. In the extension
# build it resolves to duckdb's vendored copy; standalone it must be supplied.
# An OPEN CONTRACT QUESTION, recorded rather than decided: whether libpanduck
# should depend on a system/vcpkg yyjson or carry its own. Until that is
# settled, point this at a yyjson include dir to configure standalone.
set(PANDUCK_YYJSON_INCLUDE_DIR "" CACHE PATH "Directory holding yyjson.hpp")

add_library(panduck STATIC
    src/ipynb.cpp)

target_include_directories(panduck PUBLIC include)
target_include_directories(panduck PRIVATE "${PANDUCK_VOCABULARY_DIR}")
if(PANDUCK_YYJSON_INCLUDE_DIR)
  target_include_directories(panduck PRIVATE "${PANDUCK_YYJSON_INCLUDE_DIR}")
endif()
