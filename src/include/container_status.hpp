#pragma once

#include "panduck/container_status.hpp"

#include "duckdb.hpp"

#include <string>

namespace duckdb {

// THE DUCKDB TAIL OF CONTAINER FAILURE, AND ALL OF IT (issue #104, L2).
//
// docx, odt and epub each read a ZIP, and each has `statement error` tests
// matching an exact message whose `IO Error:` / `Invalid Input Error:` prefix is
// the DuckDB exception TYPE. Neither the type nor the message could cross the
// seam -- see panduck/container_status.hpp for the three separate reasons -- so
// the cores report a `::panduck::ContainerStatus` and this function turns it
// back into the exception the SQL layer has always shown.
//
// FIVE CODES, FIVE FORMAT STRINGS, ONE PLACE. Before the move those five
// strings lived in four files: two in src/zip_container.cpp (which now
// delegates here, so its own two throws and the readers' share one copy) and
// one each in the docx, odt and epub readers. Collecting them here is what makes
// the byte-identity claim checkable by reading one function instead of grepping
// three readers -- and it is the only reason to prefer a single raiser over a
// `switch` in each tail.
//
// TWO OF THE CODES ARE EPUB'S TODAY, and their wording says so: it names
// container.xml and the package document, because EPUB is the one format panduck
// reads whose manifest is two levels deep. They are answered here anyway rather
// than in epub's tail, so that the property above -- every container error
// message in this build has exactly one source -- holds without an exception.

//! Raise the exception a container-reading failure has always raised. Never
//! returns: passing an OK status is a programming error and says so.
[[noreturn]] void RaiseContainerStatus(const ::panduck::ContainerStatus &status, const char *reader_name,
                                       const std::string &path);

} // namespace duckdb
