#pragma once

// This table IS the LaTeX reader's scope boundary: every macro and environment
// panduck claims to understand is listed here, and nothing the reader emits comes
// from a mapping invented ad hoc in latex_reader.cpp. Deliberately NOT exposed as a
// table function -- see the brief for Task 3 (2026-08-31-latex-reader) for why that
// argument does not survive contact: a static table in a source file is already
// enumerable by reading it, and every entry that matters is exercised by the
// reader's own tests.

#include <string>

namespace duckdb {
namespace latex {

//! What the reader does with a macro or environment it recognises.
//!  SEMANTIC    -- becomes a duck_block element_type; args feed its content/attributes
//!  TRANSPARENT -- the macro itself is dropped, but the reader DESCENDS into
//!                 content_arg and keeps reading its content as if the macro were
//!                 never there
//!  DROPPED     -- the macro AND all of its arguments vanish; presentational or
//!                 metadata with no duck_block counterpart
//!  TEXT        -- expands to a literal string (`expansion`), no descent
enum class Disposition { SEMANTIC, TRANSPARENT, DROPPED, TEXT };

struct MacroEntry {
	const char *name;
	Disposition disposition;
	const char *element_type; //!< duck_block element_type for SEMANTIC, else nullptr
	int args;                 //!< number of brace-delimited arguments the macro consumes
	int content_arg;          //!< 0-based index of the argument to descend into
	                           //!< (TRANSPARENT only), or -1
	const char *expansion;    //!< literal replacement text for TEXT; for environments
	                           //!< this field instead carries the list type
	                           //!< ("bullet"/"ordered") -- see the environment table
	                           //!< below for why that reuse is intentional
};

//! Look up a control word (without the leading backslash). Returns nullptr when the
//! macro is not one panduck claims to read, so the reader can fall back to a
//! disposition-free default (typically: keep the text, drop the macro).
//!
//! Linear scan, not a map: the table is ~40 entries, consulted once per token, and a
//! std::unordered_map buys nothing here but a static initialiser.
const MacroEntry *LookupMacro(const std::string &name);

//! Look up an environment name (the argument to \begin{...}). Same nullptr and
//! linear-scan reasoning as LookupMacro -- the table is a separate, much smaller
//! array because environments and macros are looked up from different call sites in
//! the reader and conflating them would just mean filtering one list by kind.
const MacroEntry *LookupEnvironment(const std::string &name);

} // namespace latex
} // namespace duckdb
