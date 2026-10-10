#pragma once

#include "panduck/byte_source.hpp"

#include "duckdb.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace duckdb {

//! Reader dispatch: which extension reads which file extension.
//!
//! LAYOUT INVARIANT: this file includes supported_extensions.hpp, never the
//! reverse. supported_extensions.cpp is a static table with no I/O and no
//! dependency on any other extension -- it must stay answerable when nothing
//! else is loaded, because it is what a dispatcher interrogates BEFORE deciding
//! anything. This file is its consumer.
//!
//! WHY C++ STATE RATHER THAN A SQL MACRO. Dispatch has to go through query(),
//! and query() rejects a subquery in its argument -- including one that arrives
//! by inlining a macro:
//!
//!     query('SELECT 1')                          -> ok
//!     query((SELECT 'SELECT 1'))                 -> Binder Error: cannot
//!     contain subqueries query(m())  where m()'s body is a subquery -> same
//!     error query(CASE WHEN lower(p) LIKE '%.md' ...)  -> ok
//!
//! That last line is how duck_block_utils' doc_to_blocks works: it dispatches
//! on string tests over its arguments and never looks anything up. A DERIVED
//! registry cannot do that, because the lookup is a subquery. So
//! panduck_format_for is a C++ scalar over an in-memory map -- no subquery, and
//! query() accepts it. The same state is what makes the registry
//! user-extensible at runtime.
//!
//! THE EXCLUSION RULE IS BY CONSTRUCTION, NOT BY SUBTRACTION. sitting_duck
//! claims md, html, json, toml and css as source code, and those claims are
//! real -- read_ast on a README yields thousands of tree-sitter nodes. But a
//! format with a document reader must never route to the code fallback. Rather
//! than enumerate sitting_duck's languages and subtract, the rule is simply: an
//! extension in this map routes to its reader, and anything else falls through
//! to code. .md is in the map, so it can never reach the fallback. Nothing here
//! needs to know what sitting_duck claims.
namespace readers {

//! What a registered reader produces.
static constexpr const char *KIND_DOC = "doc";     //!< duck_blocks
static constexpr const char *KIND_TABLE = "table"; //!< rows and columns

//! Where an entry came from -- so `user` overrides are visible as overrides.
static constexpr const char *SOURCE_BUILTIN = "builtin";
static constexpr const char *SOURCE_USER = "user";

//! One reader option, held as STRUCTURED DATA rather than as a SQL fragment.
//!
//! WHY NOT A FRAGMENT, which is what the first design stored. A registry entry
//! is process-wide and persists for the life of the process, so a fragment
//! stored by one caller would be interpolated into the SQL of EVERY later
//! read_panduck_doc -- including calls made by other sessions sharing that
//! process, which never registered anything and cannot see what was stored.
//! Registration would be an arbitrary-SQL injection point with session-crossing
//! reach.
//!
//! panduck renders the argument itself instead: `param` is validated as a bare
//! identifier and `arg` is validated against `arg_type`, both AT REGISTRATION,
//! and rendering re-checks before emitting. There is no path from an OPTION to
//! arbitrary SQL, which is a shorter security argument than any a stored
//! fragment could offer.
//!
//! THE SIBLING FIELD `ReaderEntry::function` IS COVERED SEPARATELY, and
//! differently. It names a table function to call, so it is interpolated BARE
//! -- there is no quoting to hide behind. It was validated nowhere until #5: a
//! registration could store `read_odt_blocks('x') UNION ALL SELECT ...` and
//! every later read of that extension ran it. It is now checked at registration
//! by IsQualifiedIdentifier, which permits a plain or one-dot identifier and
//! nothing else -- the shapes a survey of the builtin registry, every test and
//! every documented example actually found.
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
	//! panduck's intent vocabulary mapped to THIS reader's spelling; see
	//! ReaderOption. Deliberately NOT surfaced as a panduck_reader_registry()
	//! column: that function's shape is asserted by reader_registry.test, and
	//! options are dispatch machinery rather than the answer to "who reads this
	//! extension".
	std::vector<ReaderOption> options;
};

//! The extension of a path, lowercased and dot-prefixed ("" when there is
//! none).
std::string ExtOfPath(const std::string &path);

//! A bare identifier: `^[A-Za-z_][A-Za-z0-9_]*$`. See ReaderOption.
bool IsIdentifier(const std::string &s);

//! Whether `arg` holds a value of `arg_type`. BOOLEAN and INTEGER render BARE,
//! so this is the check that keeps them literals rather than SQL. See
//! ReaderOption.
bool ArgMatchesType(const std::string &arg_type, const std::string &arg);

//! Process-wide registry. A user registration replaces any entry for the same
//! extension, so there is exactly one reader per extension by construction --
//! the drift the derived design exists to prevent cannot occur here, rather
//! than being tested for after the fact.
class ReaderRegistry {
public:
	static ReaderRegistry &Get();

	std::vector<ReaderEntry> Entries();
	//! Returns nullptr when the extension is unclaimed -- dispatch treats that as
	//! code.
	bool Lookup(const std::string &ext, ReaderEntry &out);
	void Register(const ReaderEntry &entry);

	//! What panduck NATIVELY calls this extension, frozen at construction and
	//! never mutated. Empty for an extension panduck does not ship a reader for.
	//!
	//! POLICY NEEDS A NAME A REGISTRATION CANNOT CHANGE. Register() REPLACES a
	//! row rather than shadowing it, and the policy name came from that row -- so
	//! re-registering an extension renamed it, and a denylist stopped applying to
	//! it:
	//!
	//!     SET panduck_disabled_readers = 'markdown';
	//!     CALL panduck_register_doc_reader('', 'read_markdown_blocks', ['.md']);
	//!     -- policy name became 'md'; the denylist no longer matched
	//!
	//! Measured. This map is consulted first, so '.md' answers 'markdown'
	//! whatever the live row says.
	std::string BuiltinFormat(const std::string &ext);

private:
	ReaderRegistry();
	std::mutex lock;
	std::vector<ReaderEntry> entries;
	//! ext -> format, populated from `entries` at the END of the constructor,
	//! when every row is still SOURCE_BUILTIN. Const after that by convention:
	//! nothing but the constructor writes it, which is what makes it trustworthy.
	std::map<std::string, std::string> builtin_format;
};

} // namespace readers

namespace readers {
//! Throw unless policy allows this FORMAT to read.
//!
//! For panduck's OWN readers, which are public table functions and therefore a
//! second door onto the same content that read_panduck_doc gates. Measured
//! before this existed: with `panduck_disabled_readers = 'odt'`,
//! read_panduck_doc refused and read_odt_blocks returned its 54 rows.
//! read_odt_blocks is not a wrapper -- it IS the reader dispatch routes to.
//!
//! Called from each reader's BIND rather than its scan, so a disabled reader
//! fails before any file is opened, and fails with panduck's named error rather
//! than the reader's own.
void RequireReaderEnabled(ClientContext &context, const char *format);

//! Read a whole file through DuckDB's FileSystem, so every reader honours whatever
//! filesystem the client has attached -- httpfs, s3, encrypted, in-memory, anything
//! registered now or later. `function_name` appears in the error message.
//!
//! WHY THIS EXISTS AT ALL. latex and rtf each carried a byte-identical ten-line
//! copy of this block and read any path DuckDB could; the other six readers built
//! a `std::ifstream` and could only ever see the local disk. Issue #120. Six more
//! copies of the block would have been absurd, so the block became this.
//!
//! IT IS DELIBERATELY MORE PRECISE THAN ifstream WAS. Only a path that does not
//! exist gets "file not found". A path that exists but will not open -- no read
//! permission, a remote that answers then dies -- lets DuckDB's own OpenFile error
//! propagate, naming the actual cause. ifstream collapsed both cases into one
//! "cannot open", which named neither.
std::string ReadFileThroughVFS(ClientContext &context, const std::string &path, const char *function_name);

//! Open a file through DuckDB's FileSystem and hand back the handle, for a reader
//! that cannot use the whole-file form above.
//!
//! WHY A SECOND ENTRY POINT. docx, odt and epub read a ZIP: miniz asks for bytes
//! at offsets, and EPUB walks dozens of members out of one open archive. Slurping
//! the whole file to satisfy that would copy an entire book into memory to read
//! its spine, so these three want the HANDLE and a ByteSource over it (see
//! FileHandleSource) rather than a string.
//!
//! ReadFileThroughVFS IS IMPLEMENTED OVER THIS, which is the point of extracting
//! it rather than writing the open twice: the "file not found" check, its wording
//! and its exception type are in ONE place, so a container reader's missing-file
//! error cannot drift from the other ten readers' -- and `function_name` appears
//! in that message for both.
unique_ptr<FileHandle> OpenFileThroughVFS(ClientContext &context, const std::string &path, const char *function_name);

//! A ::panduck::ByteSource over a duckdb::FileHandle -- THE VFS BRIDGE, and the
//! whole reason issue #120 built a ByteSource at all.
//!
//! With this, docx, odt and epub read whatever filesystem the client has attached
//! (httpfs, s3, encrypted, in-memory, anything registered now or later), which the
//! other ten readers have done since #122 and these three could not: they opened
//! their archive by path through miniz.
//!
//! IT CATCHES AT ITS OWN BOUNDARY, AND THAT IS NOT DEFENSIVE STYLE -- IT IS THE
//! CONTRACT. `ByteSource::Read` must not throw, because the call arrives through a
//! C callback frame inside miniz: an exception crossing it leaks miniz's internal
//! state and surfaces as whatever DuckDB wraps a foreign exception in, rather than
//! as the `IO Error:` / `Invalid Input Error:` prefix three tests and
//! test/fixtures/malformed/README.md match on. `Read` is deliberately NOT spelled
//! `noexcept` (see byte_source.hpp) precisely so that a leak here is a reportable
//! error instead of std::terminate -- which means the catch below, not the
//! compiler, is the enforcement. A `FileHandle::Read` on an httpfs or S3 path can
//! throw, so this is a live case and not a theoretical one.
//!
//! The division is unchanged by this class: the CORE reports failure through
//! ContainerStatus and the TAIL raises. A read that fails becomes 0 bytes, miniz
//! fails to open or fails to inflate, the core reports, and the reader raises the
//! same exception callers have always seen.
class FileHandleSource : public ::panduck::ByteSource {
public:
	//! `handle` must outlive this source, and the source must outlive anything
	//! built on it -- a ZipContainer holds its source by reference and reads
	//! members on demand (zip_container.hpp). Never throws.
	explicit FileHandleSource(FileHandle &handle);

	//! The size cached at construction, or 0 when it could not be determined.
	//! CACHED because this is const and `GetFileSize` is not -- the same bargain
	//! ::panduck::FileSource makes, and for the same reason.
	uint64_t Size() const override;

	//! Reads up to `nr_bytes` at `offset`, CLAMPED to the cached size. Returns the
	//! bytes copied: a short read at end of file, and 0 when the read failed or
	//! the offset is past the end. Never throws.
	size_t Read(void *buffer, size_t nr_bytes, uint64_t offset) override;

	//! The path as opened -- a path or a URL, whatever the client's filesystem was
	//! handed. Message material only.
	const std::string &Name() const override;

private:
	FileHandle &handle;
	uint64_t size;
	std::string name;
};
} // namespace readers

void RegisterReaderRegistry(ExtensionLoader &loader);

} // namespace duckdb
