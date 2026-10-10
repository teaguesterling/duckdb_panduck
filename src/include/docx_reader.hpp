#pragma once

#include "panduck/docx.hpp"

#include "duckdb.hpp"

#include <string>
#include <vector>

namespace duckdb {

class ExtensionLoader;

//! DOCX (Office Open XML) reader.
//!
//! A ZIP container whose body lives in word/document.xml, so this is the first
//! reader using both vendored dependencies: miniz to open the archive, pugixml
//! to parse the XML.
//!
//! HEADINGS COME FROM TWO MUTUALLY EXCLUSIVE MECHANISMS, measured on real
//! writers:
//!
//!                        w:pStyle "Heading*"      w:outlineLvl
//!     pandoc             Heading1, Heading2       none
//!     LibreOffice        none (only "Normal")     0, 1
//!
//! Handling only one loses every heading from the other. This is the same split
//! RTF has
//! (\outlinelevel versus a {\stylesheet} \sN reference) with the writers on
//! opposite sides -- pandoc is style-based in DOCX and outline-based in RTF,
//! LibreOffice the reverse. Two formats, two writers, four combinations, and no
//! writer agrees with itself across formats. Treat "one heading mechanism" as
//! an unsafe assumption by default rather than something to discover per
//! format.
//!
//! This is also where panduck earns its design. DOCX carries REAL heading
//! semantics, unlike the PDF bridge (which recovers none: 0 headings from a
//! 100-page manual) or a markdown round-trip. A native reader keeps structure
//! the bridges throw away.
namespace docx {

// THE PARSE CORE AND THE ROW FLATTENING LIVE IN libpanduck (issue #104, L2):
// libpanduck/include/panduck/docx.hpp and libpanduck/src/docx.cpp, which name
// no DuckDB type. Only the table function, its bind, its scan and its
// registration stay on this side of the seam.
//
// The flatten did NOT exist as a function to move: it was fused into DocxBind,
// appending into a TableFunctionData member through a private `struct DocxRow`.
// It is now ::panduck::docx::ReadDocx, the only place a docx row is built.
//
// BOTH ENTRY POINTS GAINED A ContainerStatus OUT-PARAMETER and lost the right to
// throw. ParseDocxFile threw IOException and InvalidInputException, whose TYPES
// are SQL-visible as the `IO Error:` / `Invalid Input Error:` prefix that
// test/sql/docx_reader.test matches four times -- so the core reports and
// DocxBind raises through RaiseContainerStatus (src/container_status.hpp), which
// owns the format strings. They still take a PATH rather than source text: a
// .docx is an archive, and ::panduck::ZipContainer is already behind the seam.
//
// Leading `::` is mandatory: inside `namespace duckdb` a bare `panduck::` binds
// to `duckdb::panduck`, the compat helpers in panduck_duckdb_compat.hpp, not to
// the library. Enumerate the moved header's surface with a pattern covering
// enum/using/constexpr/class as well as struct and function -- the first textile
// scanner shim missed `LineKind` exactly that way. docx's surface is four names
// and no enum, no using, no constexpr, no class. Alphabetised for clang-format.
using ::panduck::docx::DocxBlock;
using ::panduck::docx::DocxInline;
using ::panduck::docx::ParseDocxFile;
using ::panduck::docx::ReadDocx;

} // namespace docx

void RegisterDocxReaderFunction(ExtensionLoader &loader);

} // namespace duckdb
