#pragma once

#include "panduck/byte_source.hpp"

#include <memory>
#include <string>

namespace panduck {

//! Minimal RAII wrapper over a miniz ZIP reader.
//!
//! Every container format panduck reads is a ZIP with the document in a known
//! member: DOCX in word/document.xml, ODT in content.xml, EPUB in a spine of
//! XHTML files named by its .opf. The archive handling is identical; only the
//! member names and the XML inside them differ. This exists so that is written
//! once.
//!
//! Opening and closing per member would re-read the central directory each
//! time, which matters for EPUB where a book is dozens of members rather than
//! two.
//!
//! THIS CLASS DOES NOT THROW, and that is the one thing the move behind the
//! seam changed (issue #104, L2). The version in src/ threw DuckDB's
//! `IOException` and `InvalidInputException`, which are DuckDB API types --
//! exactly what may not live here. The alternative, throwing a panduck
//! exception instead, would have changed the SQL-visible error TYPE from
//! `IO Error`/`Invalid Input Error` to whatever DuckDB wraps a foreign
//! `std::exception` in, and three tests plus test/fixtures/malformed/README.md
//! name those types.
//!
//! So failure is REPORTED rather than thrown, and `duckdb::ZipContainer` in
//! src/include/zip_container.hpp turns each report back into the same DuckDB
//! exception the callers always saw. That split is the normal seam shape -- a
//! portable core plus a DuckDB-facing tail -- rather than a concession: a
//! standalone consumer has no DuckDB exception hierarchy to throw into, and
//! `ReadRequired`'s message was built out of a reader name the library has no
//! business knowing.
class ZipContainer {
public:
	//! Opens `path`. Never throws: ask `IsOpen()` whether it worked.
	//!
	//! Implemented over an OWNED `FileSource` since issue #120, so there is one
	//! archive code path rather than two. The behaviour is unchanged by
	//! construction -- a missing or unreadable file leaves `IsOpen()` false,
	//! exactly as `mz_zip_reader_init_file` did -- which is what lets every
	//! existing call site stay as it is.
	explicit ZipContainer(const std::string &path);

	//! Opens the archive in `source`, which may be bytes in memory, a handle
	//! from a host filesystem, or anything else positional (issue #120). Never
	//! throws: ask `IsOpen()`.
	//!
	//! LIFETIME: the source is held BY REFERENCE and must outlive this
	//! container. miniz reads the central directory at open and then reads each
	//! member on demand -- the container is deliberately kept open across reads
	//! (see above), so the source is still being read long after the
	//! constructor returns. For `MemorySource` that means the underlying buffer
	//! must outlive BOTH the source and this container.
	explicit ZipContainer(ByteSource &source);

	~ZipContainer();

	ZipContainer(const ZipContainer &) = delete;
	ZipContainer &operator=(const ZipContainer &) = delete;

	//! False when the file is missing or is not a readable ZIP. Every `Read`
	//! on a closed container returns false, so a caller that ignores this
	//! sees an empty archive rather than undefined behaviour.
	bool IsOpen() const;

	//! The path as opened, so a caller building an error message does not have
	//! to keep its own copy. For a container opened from a `ByteSource` this is
	//! that source's `Name()`, which may be a placeholder like `<memory>`
	//! rather than anything openable -- it has only ever been message material.
	const std::string &Path() const;

	//! Reads one member into `out`. Returns false when the member is absent --
	//! normal for an optional part (a minimal DOCX may omit styles.xml) and fatal
	//! for the document body, so the caller decides which.
	bool Read(const char *member, std::string &out);

private:
	//! Shared by both constructors: installs the miniz read callback over
	//! `source` and reads the central directory. Never throws; sets `IsOpen()`.
	void Open(ByteSource &source);

	struct Impl;
	std::unique_ptr<Impl> impl;
	std::string path;
};

} // namespace panduck
