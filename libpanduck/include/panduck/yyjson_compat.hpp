#pragma once

// THE SEAM'S SECOND CONCESSION, RESOLVED (issue #123, part of #104).
//
// yyjson is a C library, but the copy the DUCKDB EXTENSION build uses is
// DuckDB's vendored one, which wraps the whole API in `namespace duckdb_yyjson`.
// A STANDALONE libpanduck has no such copy: it links a system or vcpkg yyjson,
// whose plain C header `<yyjson.h>` declares the same API at GLOBAL scope.
//
// That difference is the only thing that kept `libpanduck/src/ipynb.cpp` from
// compiling in the standalone build -- the seam's ONLY compile-level
// enforcement, which a grep script cannot replace (it sees named DuckDB types,
// not transitive includes, C++11 aggregate regressions or ODR-use failures).
// This header is the one place that difference is spelled, so that every
// `yyjson_*` name in ipynb.cpp -- 36 function calls and 10 uses of the
// `yyjson_val` type, measured -- stays UNQUALIFIED and IDENTICAL in both
// builds. If you are reaching for a `#if` around a call site, this header is
// what you wanted instead.
//
// WHICH COPY IS CHOSEN IS THE HOST'S DECISION, not the format module's:
//
//   PANDUCK_YYJSON_VENDORED_DUCKDB defined   DuckDB's namespaced vendored copy,
//                                            pulled into scope by the directive
//                                            below. Defined by the top-level
//                                            CMakeLists.txt, where
//                                            libpanduck/src/*.cpp join
//                                            EXTENSION_SOURCES.
//   not defined                              a standalone yyjson, already at
//                                            global scope. This is the
//                                            standalone project's arm; it links
//                                            yyjson::yyjson (see
//                                            libpanduck/CMakeLists.txt).
//
// The macro is deliberately defined by the EXTENSION build rather than defaulted
// on here, so the shipped binary keeps using the copy DuckDB already vendors and
// links. Nothing changes for it, and no build ever risks linking two yyjson
// copies into one artifact.
//
// INCLUDE THIS FROM .cpp FILES ONLY. The `using namespace` below is why: it is
// unscoped, so a header that included this one would leak every `yyjson_*` name
// into every translation unit downstream of it. No libpanduck public header
// needs a yyjson type in its interface, and none should -- yyjson is an
// implementation detail of the ipynb parse core.

#if defined(PANDUCK_YYJSON_VENDORED_DUCKDB)
#include "yyjson.hpp"
using namespace duckdb_yyjson; // NOLINT -- the spelling the json extension uses
#else
#include <yyjson.h>
#endif
