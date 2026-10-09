#pragma once

#include "panduck/zip_container.hpp"

#include "duckdb.hpp"

#include <string>

namespace duckdb {

// THE ARCHIVE CORE LIVES IN libpanduck (issue #104, L2):
// libpanduck/include/panduck/zip_container.hpp and src/zip_container.cpp, which
// is also where the miniz dependency now sits.
//
// THIS IS A WRAPPER, NOT A `using` SHIM, and the reason is worth stating because
// every other shim in src/include/ is one line per symbol. ZipContainer's
// DuckDB coupling was not the `unique_ptr` and the `namespace duckdb` it looked
// like from the header; it was the two THROWS in the .cpp. `IOException` and
// `InvalidInputException` are DuckDB API types, and their exception type is
// SQL-visible: it is the `IO Error:` / `Invalid Input Error:` prefix on the
// message. docx, odt and epub each have a `statement error` test on those
// messages, and test/fixtures/malformed/README.md names the two types.
//
// Throwing a panduck exception from behind the seam and letting DuckDB wrap it
// would have kept the message text and changed the type, which rule 4 of the
// seam contract ("behaviour must not change") does not allow. So the core
// REPORTS failure and this class raises it -- the same division as a reader,
// whose parse core moves and whose DuckDB tail stays.
//
// The call sites are untouched: same class name, same two-argument constructor,
// same Read/ReadRequired. docx, odt and epub did not change a line.
class ZipContainer {
public:
	//! Throws IOException when the file is missing or is not a readable ZIP.
	ZipContainer(const std::string &path, const char *reader_name);

	ZipContainer(const ZipContainer &) = delete;
	ZipContainer &operator=(const ZipContainer &) = delete;

	//! Reads one member into `out`. Returns false when the member is absent --
	//! normal for an optional part (a minimal DOCX may omit styles.xml) and fatal
	//! for the document body, so the caller decides which.
	bool Read(const char *member, std::string &out);

	//! Reads a member that must exist, throwing InvalidInputException naming both
	//! the file and the member when it does not. A ZIP without its body member is
	//! not the format it claims to be, and saying so beats returning zero rows.
	std::string ReadRequired(const char *member);

private:
	::panduck::ZipContainer zip;
	std::string reader_name;
};

} // namespace duckdb
