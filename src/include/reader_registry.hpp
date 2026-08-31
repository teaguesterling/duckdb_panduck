#pragma once

#include "duckdb.hpp"

#include <mutex>
#include <string>
#include <vector>

namespace duckdb {

//! Reader dispatch: which extension reads which file extension.
//!
//! LAYOUT INVARIANT: this file includes supported_extensions.hpp, never the reverse.
//! supported_extensions.cpp is a static table with no I/O and no dependency on any other
//! extension -- it must stay answerable when nothing else is loaded, because it is what a
//! dispatcher interrogates BEFORE deciding anything. This file is its consumer.
//!
//! WHY C++ STATE RATHER THAN A SQL MACRO. Dispatch has to go through query(), and query()
//! rejects a subquery in its argument -- including one that arrives by inlining a macro:
//!
//!     query('SELECT 1')                          -> ok
//!     query((SELECT 'SELECT 1'))                 -> Binder Error: cannot contain subqueries
//!     query(m())  where m()'s body is a subquery -> same error
//!     query(CASE WHEN lower(p) LIKE '%.md' ...)  -> ok
//!
//! That last line is how duck_block_utils' doc_to_blocks works: it dispatches on string
//! tests over its arguments and never looks anything up. A DERIVED registry cannot do
//! that, because the lookup is a subquery. So panduck_format_for is a C++ scalar over an
//! in-memory map -- no subquery, and query() accepts it. The same state is what makes the
//! registry user-extensible at runtime.
//!
//! THE EXCLUSION RULE IS BY CONSTRUCTION, NOT BY SUBTRACTION. sitting_duck claims md,
//! html, json, toml and css as source code, and those claims are real -- read_ast on a
//! README yields thousands of tree-sitter nodes. But a format with a document reader must
//! never route to the code fallback. Rather than enumerate sitting_duck's languages and
//! subtract, the rule is simply: an extension in this map routes to its reader, and
//! anything else falls through to code. .md is in the map, so it can never reach the
//! fallback. Nothing here needs to know what sitting_duck claims.
namespace readers {

//! What a registered reader produces.
static constexpr const char *KIND_DOC = "doc";     //!< duck_blocks
static constexpr const char *KIND_TABLE = "table"; //!< rows and columns

//! Where an entry came from -- so `user` overrides are visible as overrides.
static constexpr const char *SOURCE_BUILTIN = "builtin";
static constexpr const char *SOURCE_USER = "user";

struct ReaderEntry {
	std::string ext;        //!< lowercase, dot-prefixed (".rtf")
	std::string format;     //!< format name; 'data' means "not a document"
	std::string reader_ext; //!< DuckDB extension that reads it
	std::string function;   //!< table function to call, or empty for a builtin branch
	std::string kind;       //!< KIND_DOC or KIND_TABLE
	std::string source;     //!< SOURCE_BUILTIN or SOURCE_USER
};

//! The extension of a path, lowercased and dot-prefixed ("" when there is none).
std::string ExtOfPath(const std::string &path);

//! Process-wide registry. A user registration replaces any entry for the same extension,
//! so there is exactly one reader per extension by construction -- the drift the derived
//! design exists to prevent cannot occur here, rather than being tested for after the fact.
class ReaderRegistry {
public:
	static ReaderRegistry &Get();

	std::vector<ReaderEntry> Entries();
	//! Returns nullptr when the extension is unclaimed -- dispatch treats that as code.
	bool Lookup(const std::string &ext, ReaderEntry &out);
	void Register(const ReaderEntry &entry);

private:
	ReaderRegistry();
	std::mutex lock;
	std::vector<ReaderEntry> entries;
};

} // namespace readers

void RegisterReaderRegistry(ExtensionLoader &loader);

} // namespace duckdb
