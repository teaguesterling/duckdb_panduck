#pragma once

#include "panduck/epub.hpp"

#include "duckdb.hpp"

#include <string>
#include <vector>

namespace duckdb {

class ExtensionLoader;

//! EPUB reader.
//!
//! EPUB DOES NOT NEED AN HTML PARSER, and that is what makes this reader cheap.
//! The open question when EPUB was scheduled was how much of it to delegate to
//! the webbed extension, since EPUB content documents are "HTML". They are not:
//! the EPUB specification requires XHTML, which is well-formed XML BY
//! DEFINITION. pugixml -- the same parser DOCX and ODT already use -- reads it
//! directly. Delegating would have bought nothing and added a load-time
//! dependency on webbed for a format that does not need one. An arbitrary .html
//! file still needs webbed, because arbitrary HTML is not XML; an EPUB's
//! content documents always are.
//!
//! A book is a ZIP with three levels of indirection before any text:
//!
//!     META-INF/container.xml  -- fixed path, the only fixed path in the format
//!       -> <rootfile full-path="..."/>          the OPF package document
//!         -> <manifest><item id href/>          id -> file
//!         -> <spine><itemref idref/>            READING ORDER
//!
//! The spine is why this cannot be "read every .xhtml member": ZIP member order
//! is arbitrary, and the spine is the only statement of what order a human
//! reads the book in.
//!
//! WHAT THE TWO WRITERS DISAGREE ABOUT IS EVERYTHING. RTF and DOCX each offered
//! two ways to mark a heading and ODT offered one. EPUB is the first format
//! where one writer emits no semantics at all: pandoc writes <h1>, <ul>, <li>,
//! <strong>; LibreOffice writes <p class="para0"><span class="span0"> for every
//! one of them and puts the meaning in a CSS file. See css_style_sheet() below
//! for where the line is drawn.
namespace epub {

// THE PARSE CORE AND THE ROW FLATTENING LIVE IN libpanduck (issue #104, L2):
// libpanduck/include/panduck/epub.hpp and libpanduck/src/epub.cpp, which name
// no DuckDB type. Only the table function, its bind, its scan and its
// registration stay on this side of the seam.
//
// The flatten did NOT exist as a function to move: it was fused into EpubBind,
// appending into a TableFunctionData member through a private `struct EpubRow`.
// It is now ::panduck::epub::ReadEpub, the only place an epub row is built.
//
// BOTH ENTRY POINTS GAINED A ContainerStatus OUT-PARAMETER and lost the right to
// throw -- five throws' worth, the most of the three container readers, which is
// why ::panduck::ContainerStatus has five codes rather than three. Their TYPES
// are SQL-visible as the `IO Error:` / `Invalid Input Error:` prefix that
// test/sql/epub_reader.test matches, so the core reports and EpubBind raises
// through RaiseContainerStatus (src/container_status.hpp), which owns the format
// strings. They still take a PATH rather than source text, and epub is the
// clearest case for it: a book is a SPINE of members behind three levels of
// indirection, so there is no single source text to hand in.
//
// Leading `::` is mandatory: inside `namespace duckdb` a bare `panduck::` binds
// to `duckdb::panduck`, the compat helpers in panduck_duckdb_compat.hpp, not to
// the library. Enumerate the moved header's surface with a pattern covering
// enum/using/constexpr/class as well as struct and function -- the first textile
// scanner shim missed `LineKind` exactly that way. epub's surface is four names
// and no enum, no using, no constexpr, no class: `using CssRules = ...` lives
// inside the core's ANONYMOUS namespace and is not part of the module's surface.
// Alphabetised for clang-format.
using ::panduck::epub::EpubBlock;
using ::panduck::epub::EpubInline;
using ::panduck::epub::ParseEpubFile;
using ::panduck::epub::ReadEpub;

} // namespace epub

void RegisterEpubReaderFunction(ExtensionLoader &loader);

} // namespace duckdb
