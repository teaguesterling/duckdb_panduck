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

//! One reader option, held as STRUCTURED DATA rather than as a SQL fragment.
//!
//! WHY NOT A FRAGMENT, which is what the first design stored. A registry entry is
//! process-wide and persists for the life of the process, so a fragment stored by one
//! caller would be interpolated into the SQL of EVERY later read_panduck_doc -- including
//! calls made by other sessions sharing that process, which never registered anything and
//! cannot see what was stored. Registration would be an arbitrary-SQL injection point with
//! session-crossing reach.
//!
//! panduck renders the argument itself instead: `param` is validated as a bare identifier
//! and `arg` is validated against `arg_type`, both AT REGISTRATION, and rendering re-checks
//! before emitting. There is no path from an OPTION to arbitrary SQL, which is a shorter
//! security argument than any a stored fragment could offer.
//!
//! THAT ARGUMENT COVERS `options` ONLY. The sibling field `ReaderEntry::function` is also
//! registration data, and it is interpolated bare into the generated SQL with no validation
//! at all -- a registration can put arbitrary SQL there. That is pre-existing (since
//! 27cd39d) and tracked as a separate follow-up; reaching it already requires the ability to
//! run `CALL panduck_register_doc_reader`. Registration as a whole is therefore NOT safe:
//! `options` is.
struct ReaderOption {
	std::string intent;   //!< panduck's vocabulary: "attributes"
	std::string value;    //!< the intent's value: "all"
	std::string param;    //!< the READER's parameter name: "capture_attributes"
	std::string arg;      //!< the value to pass, unrendered
	std::string arg_type; //!< VARCHAR | BOOLEAN | INTEGER
};

struct ReaderEntry {
	std::string ext;        //!< lowercase, dot-prefixed (".rtf")
	std::string format;     //!< format name; 'data' means "not a document"
	std::string reader_ext; //!< DuckDB extension that reads it
	std::string function;   //!< table function to call, or empty for a builtin branch
	std::string kind;       //!< KIND_DOC or KIND_TABLE
	std::string source;     //!< SOURCE_BUILTIN or SOURCE_USER
	//! panduck's intent vocabulary mapped to THIS reader's spelling; see ReaderOption.
	//! Deliberately NOT surfaced as a panduck_reader_registry() column: that function's
	//! shape is asserted by reader_registry.test, and options are dispatch machinery
	//! rather than the answer to "who reads this extension".
	std::vector<ReaderOption> options;
};

//! The extension of a path, lowercased and dot-prefixed ("" when there is none).
std::string ExtOfPath(const std::string &path);

//! A bare identifier: `^[A-Za-z_][A-Za-z0-9_]*$`. See ReaderOption.
bool IsIdentifier(const std::string &s);

//! Whether `arg` holds a value of `arg_type`. BOOLEAN and INTEGER render BARE, so this is
//! the check that keeps them literals rather than SQL. See ReaderOption.
bool ArgMatchesType(const std::string &arg_type, const std::string &arg);

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
