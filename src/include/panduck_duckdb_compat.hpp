#pragma once

#include "duckdb.hpp"
#include "duckdb/catalog/default/default_functions.hpp"
#include "duckdb/catalog/default/default_table_functions.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"

#include <string>
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

//! Set a function's NULL handling across the v1.5.5 / v2.0 boundary.
//!
//! v1.5.5 exposes `null_handling` as a public member; v2.0 moved it behind
//! `SetNullHandling()` as part of encapsulating the function properties. Neither spelling
//! compiles against the other version, and there is no version macro worth branching on:
//! the interesting question is not "which DuckDB is this" but "does this type have the
//! setter", which the compiler can answer directly.
//!
//! Overload resolution does the version detection. The `int` overload is preferred when it
//! is viable at all, and it is viable only when `SetNullHandling` exists; otherwise the
//! `long` overload takes the member. Passing a literal 0 makes the int form the better
//! match, so no version check is written down anywhere.
template <typename FUNC>
auto SetNullHandlingCompat(FUNC &fn, FunctionNullHandling value, int) -> decltype(fn.SetNullHandling(value), void()) {
	fn.SetNullHandling(value);
}

template <typename FUNC>
void SetNullHandlingCompat(FUNC &fn, FunctionNullHandling value, long) {
	fn.null_handling = value;
}

//! Call this rather than either spelling: `SetNullHandling(fn, SPECIAL_HANDLING)`.
template <typename FUNC>
void SetNullHandling(FUNC &fn, FunctionNullHandling value) {
	SetNullHandlingCompat(fn, value, 0);
}

//! A scalar macro in a form that does not depend on DuckDB's struct layout.
//!
//! v1.5.5's DefaultMacro is {schema, name, parameters[8], named_parameters[8], macro};
//! v2.0 collapsed it to {schema, name, macro_definition}, folding the parameter list into
//! the definition string as "(a, b) AS body". The SHAPE differs, so a table written for one
//! cannot compile against the other -- and the table is the part worth keeping single, since
//! it holds the actual SQL.
struct PanduckMacro {
	const char *schema;
	const char *name;
	//! nullptr-terminated, at most 7 plus the terminator to fit DuckDB's fixed array.
	const char *parameters[8];
	//! Also nullptr-terminated. `doc_render` really uses one (`format := 'auto'`), so this
	//! is not a field that could be dropped for being always empty.
	DefaultNamedParameter named_parameters[8];
	const char *macro;
};

template <class T, class = void>
struct HasMacroDefinition : std::false_type {};

template <class T>
struct HasMacroDefinition<T, decltype(void(std::declval<T &>().macro_definition))> : std::true_type {};

//! Fill in whichever body representation this DuckDB's DefaultMacro has.
//!
//! These are SFINAE overloads on a template parameter rather than `if constexpr`: the
//! struct is a concrete type, so BOTH branches of an `if constexpr` would still have to
//! parse, and each names a member the other version does not declare. Overload resolution
//! discards the non-viable one before it is ever instantiated.
template <class M>
auto AssignMacroBody(M &out, const PanduckMacro &src, const std::string &storage, int)
    -> decltype(void(out.macro_definition)) {
	// v2.0: parameters live inside the definition string, built by the caller.
	out.macro_definition = storage.c_str();
}

template <class M>
void AssignMacroBody(M &out, const PanduckMacro &src, const std::string &storage, long) {
	// v1.5.5: a fixed parameter array beside the body. `out` was value-initialised, so
	// named_parameters is already the {nullptr, nullptr} terminator these macros want.
	out.macro = src.macro;
	for (size_t i = 0; i < 8; i++) {
		out.parameters[i] = src.parameters[i];
		out.named_parameters[i] = src.named_parameters[i];
	}
}

//! Build DuckDB's DefaultMacro from the neutral form.
//!
//! `storage` must outlive the returned value on v2.0, which holds a pointer INTO it; the
//! caller keeps it alive across the CreateInternalMacroInfo call. On v1.5.5 the parameter
//! strings are copied into the fixed array and `storage` is untouched.
inline DefaultMacro MakeDefaultMacro(const PanduckMacro &src, std::string &storage) {
	DefaultMacro out {};
	if (HasMacroDefinition<DefaultMacro>::value) {
		storage = "(";
		for (idx_t i = 0; src.parameters[i]; i++) {
			if (i) {
				storage += ", ";
			}
			storage += src.parameters[i];
		}
		for (idx_t i = 0; src.named_parameters[i].name; i++) {
			if (i || src.parameters[0]) {
				storage += ", ";
			}
			storage += src.named_parameters[i].name;
			storage += " := ";
			storage += src.named_parameters[i].default_value;
		}
		storage += ") AS ";
		storage += src.macro;
	}
	AssignMacroBody(out, src, storage, 0);
	out.schema = src.schema;
	out.name = src.name;
	return out;
}

} // namespace panduck
} // namespace duckdb
