#pragma once

#include "panduck/odt.hpp"

#include "duckdb.hpp"

#include <string>
#include <vector>

namespace duckdb {

class ExtensionLoader;

//! ODT (OpenDocument Text) reader.
//!
//! A ZIP whose body is content.xml, so it shares the container handling with
//! DOCX via ZipContainer and the XML parsing via pugixml. What differs is the
//! vocabulary.
//!
//! ODT HAS NO HEADING AMBIGUITY, and the contrast is the interesting part. RTF
//! and DOCX both mark headings two mutually exclusive ways, with pandoc and
//! LibreOffice on opposite sides in the two formats. ODF does not, because it
//! has a DEDICATED HEADING ELEMENT:
//!
//!     <text:h text:style-name="Heading_20_1" text:outline-level="1">Heading
//!     One</text:h>
//!
//! Both writers emit exactly that. There is nothing to disagree about, which is
//! precisely why the other two formats disagreed: they overload a paragraph
//! with a property, and a property can be expressed more than one way. A format
//! with a real heading element cannot have that problem.
//!
//! So the lesson from RTF and DOCX is not "always expect two mechanisms" -- it
//! is "expect them wherever headings are a paragraph wearing a hat."
namespace odt {

// THE PARSE CORE AND THE ROW FLATTENING LIVE IN libpanduck (issue #104, L2):
// libpanduck/include/panduck/odt.hpp and libpanduck/src/odt.cpp, which name no
// DuckDB type. Only the table function, its bind, its scan and its registration
// stay on this side of the seam.
//
// The flatten did NOT exist as a function to move: it was fused into OdtBind,
// appending into a TableFunctionData member through a private `struct OdtRow`.
// It is now ::panduck::odt::ReadOdt, the only place an odt row is built.
//
// BOTH ENTRY POINTS GAINED A ContainerStatus OUT-PARAMETER and lost the right to
// throw. ParseOdtFile threw IOException and InvalidInputException, whose TYPES
// are SQL-visible as the `IO Error:` / `Invalid Input Error:` prefix that
// test/sql/odt_reader.test matches -- so the core reports and OdtBind raises
// through RaiseContainerStatus (src/container_status.hpp), which owns the format
// strings. They still take a PATH rather than source text: a .odt is an archive,
// and ::panduck::ZipContainer is already behind the seam.
//
// Leading `::` is mandatory: inside `namespace duckdb` a bare `panduck::` binds
// to `duckdb::panduck`, the compat helpers in panduck_duckdb_compat.hpp, not to
// the library. Enumerate the moved header's surface with a pattern covering
// enum/using/constexpr/class as well as struct and function -- the first textile
// scanner shim missed `LineKind` exactly that way. odt's surface is four names
// and no enum, no using, no constexpr, no class. Alphabetised for clang-format.
using ::panduck::odt::OdtBlock;
using ::panduck::odt::OdtInline;
using ::panduck::odt::ParseOdtFile;
using ::panduck::odt::ReadOdt;

} // namespace odt

void RegisterOdtReaderFunction(ExtensionLoader &loader);

} // namespace duckdb
