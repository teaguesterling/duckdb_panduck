#pragma once

#include "duckdb.hpp"
#include <cstddef>

namespace duckdb {

//! Panduck's self-description as a reader.
//!
//! A dispatcher that routes a path to a reader extension has two ways to know which
//! extension reads `.docx`: a central table it maintains by hand, or asking each reader
//! what it reads. The first drifts -- duck_block_utils' `doc_to_blocks` and
//! `doc_supported_extensions` disagreed within four days of being split -- and the
//! second cannot, because there is only one copy of the answer and it lives next to the
//! code that implements it.
//!
//! `sitting_duck` already does the second thing with `ast_supported_languages()`. This
//! is panduck's equivalent, deliberately shaped so a dispatcher can UNION the two
//! without special-casing either:
//!
//!   ast_supported_languages()      -> (language, extensions, parser_type, node_type_count)
//!   panduck_supported_extensions() -> (format,   extensions, reader,      status, notes)
//!
//! `extensions` follows sitting_duck's convention exactly: lowercase, **no leading
//! dot** ("docx", not ".docx"). A consumer that has to normalise one registry
//! differently from another has reintroduced the per-reader knowledge this is meant to
//! remove.
namespace readers {

//! State of a format's reader in THIS build.
//!  "implemented" -- panduck reads this today; a dispatcher may route to `reader`
//!  "planned"     -- panduck intends to read this; a dispatcher must NOT route here
//!
//! The distinction is the whole point of the status column. Panduck currently ships no
//! readers, so every row is "planned" and a dispatcher filtering on "implemented"
//! correctly routes nothing to panduck. `reader` is NULL for exactly those rows, so a
//! consumer that forgets to filter builds a CASE branch naming NULL rather than a
//! plausible-looking function that does not exist.
static constexpr const char *STATUS_IMPLEMENTED = "implemented";
static constexpr const char *STATUS_PLANNED = "planned";

struct FormatReader {
	const char *format;            //!< pandoc's name for the format, i.e. its `--from` value
	const char *const *extensions; //!< nullptr-terminated, lowercase, no leading dot
	const char *reader;            //!< panduck table function that reads it, or nullptr when none exists yet
	const char *status;            //!< one of STATUS_*
	const char *notes;
};

extern const FormatReader FORMATS[];
extern const size_t FORMAT_COUNT;

} // namespace readers

//! Registers panduck_supported_extensions() -- exposes FORMATS as a queryable table so
//! reader dispatch can be derived from it instead of maintained against it.
void RegisterSupportedExtensionsFunction(ExtensionLoader &loader);

} // namespace duckdb
