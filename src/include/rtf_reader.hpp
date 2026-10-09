#pragma once

#include "panduck/rtf.hpp"

#include "duckdb.hpp"

#include <string>
#include <vector>

namespace duckdb {

class ExtensionLoader;

//! RTF (Rich Text Format) reader.
//!
//! RTF is 7-bit ASCII with brace-delimited groups and backslash control words,
//! so unlike DOCX/ODT/EPUB it needs neither miniz nor pugixml -- this reader is
//! self-contained. THAT is what let it move behind the seam alongside the Tier 1
//! readers while its three Tier 2 siblings wait on a dependency decision: its
//! only non-portable include was duckdb/common/file_system.hpp, and that belongs
//! to the tail.
//!
//! Heading detection deliberately supports TWO mechanisms, because real writers
//! disagree and handling only one silently loses every heading from the other:
//!
//!   * `\outlinelevelN` on the paragraph -- emitted by pandoc and newer
//!   writers.
//!   * `\sN` referencing a `{\stylesheet}` entry whose name matches "Heading N"
//!   --
//!     emitted by LibreOffice and Word, which write no `\outlinelevel` at all.
//!
//! Both fixtures in test/fixtures/ were produced by real writers and exercise
//! exactly one mechanism each, so a regression in either path fails a test.
namespace rtf {

// THE PARSE CORE AND THE ROW FLATTENING LIVE IN libpanduck (issue #104, L2):
// libpanduck/include/panduck/rtf.hpp and libpanduck/src/rtf.cpp, which name no
// DuckDB type. Only the table function, its bind, its scan and its registration
// stay on this side of the seam -- including DuckDB's FileSystem, which
// read_rtf_blocks reads a path through.
//
// The flatten did NOT exist as a function to move: it was fused into
// RtfReaderBind. It is now ::panduck::rtf::ReadRtf, the only place an rtf row is
// built.
//
// ParseRtfDocument was RENAMED to ParseRtfString here, the one rename in the
// move, so rtf spells its intermediate entry point like every other module. The
// old name had two references, both inside this reader's own two files.
//
// Leading `::` is mandatory: inside `namespace duckdb` a bare `panduck::` binds
// to `duckdb::panduck`, the compat helpers in panduck_duckdb_compat.hpp, not to
// the library. Enumerate the moved header's surface with a pattern covering
// enum/using/constexpr/class as well as struct and function -- the first textile
// scanner shim missed `LineKind` exactly that way. rtf's surface is four names
// and no enum. Alphabetised for clang-format.
using ::panduck::rtf::ParseRtfString;
using ::panduck::rtf::ReadRtf;
using ::panduck::rtf::RtfBlock;
using ::panduck::rtf::RtfInline;

} // namespace rtf

//! Registers read_rtf_blocks(VARCHAR). Columns mirror the duck_block struct
//! field order, so `SELECT list(b::duck_block) FROM read_rtf_blocks(path) b`
//! works the way duck_block_utils' doc_to_blocks dispatcher expects.
void RegisterRtfReaderFunction(ExtensionLoader &loader);

} // namespace duckdb
