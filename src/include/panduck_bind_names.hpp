#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"

#include <type_traits>

namespace duckdb {
namespace panduck {

//! The type of a table function bind callback's `names` out-parameter, DERIVED FROM
//! DuckDB'S OWN TYPEDEF rather than written out.
//!
//! DuckDB v1.5.5 declares it `vector<string> &`; v2.0 changed it to `vector<Identifier> &`,
//! a case-insensitive wrapper. Naming either one directly pins this extension to a single
//! DuckDB major version -- and `Identifier` does not exist in the older tree, so a
//! `#if`-on-version would need the symbol to be spelled in a branch the preprocessor keeps
//! but the compiler still parses.
//!
//! Reading the type off `table_function_bind_t` sidesteps both problems: whatever DuckDB
//! declares is what these functions accept, and a future change to the parameter follows
//! automatically instead of breaking 44 signatures again.
//!
//! Column names are still written as STRING LITERALS at every call site, which is what
//! makes this safe in both directions: `Identifier(const char *)` is implicit, while
//! construction from a runtime `string` is explicit. A name built at runtime would not
//! compile against v2.0 and would need an explicit conversion -- deliberately, because an
//! Identifier carries case-insensitive semantics a bare string does not.
template <typename T>
struct BindNamesTypeOf;

template <typename R, typename A, typename B, typename C, typename D>
struct BindNamesTypeOf<R (*)(A, B, C, D)> {
	using type = typename std::remove_reference<D>::type;
};

using BindNames = typename BindNamesTypeOf<table_function_bind_t>::type;

} // namespace panduck
} // namespace duckdb
