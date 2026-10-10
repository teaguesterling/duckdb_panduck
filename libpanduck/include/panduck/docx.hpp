#pragma once

#include "panduck/block.hpp"
#include "panduck/byte_source.hpp"
#include "panduck/container_status.hpp"

#include <map>
#include <string>
#include <vector>

// Moved behind the libpanduck seam (issue #104, L2). Names no DuckDB type.
// The table function, its bind, its scan and its registration stay in
// src/docx_reader.cpp.
//
// THE ENTRY POINTS TAKE AN ARCHIVE, not source text, for the reason recorded at
// the top of panduck/odt.hpp: a .docx is an archive, libpanduck exists so a
// consumer can read a document without DuckDB, and a core that only accepted
// pre-extracted word/document.xml would make every standalone consumer
// reimplement OOXML part lookup. ::panduck::ZipContainer already opens files
// from disk, so nothing new is depended on here. Since #120 the archive arrives
// as a `ByteSource` -- bytes, a stdio file, or a host file handle -- with the
// path form kept as a delegate over `FileSource`; see the note above the entry
// points.
//
// FAILURE IS REPORTED, NOT THROWN -- a ContainerStatus out-parameter and an
// empty result. ParseDocxFile threw IOException and InvalidInputException, whose
// TYPES are the SQL-visible `IO Error:` / `Invalid Input Error:` prefix that
// test/sql/docx_reader.test matches four times. src/docx_reader.cpp raises. See
// panduck/container_status.hpp.

namespace panduck {
namespace docx {

struct DocxInline {
	std::string element_type; //!< duck_block inline vocabulary: text, bold,
	                          //!< italic, image, note
	std::string content;
	//! `image`: src. `note`: nothing today, the body is the content.
	std::map<std::string, std::string> attributes;
};

struct DocxBlock {
	//! duck_block kind. Empty means `block`. `value` is document METADATA.
	std::string kind;
	//! `value` only: the field name for attributes['key'], in PANDOC's namespace.
	std::string key;
	//! `value` only: the ORIGINAL field spelling, marking this as format-derived
	//! rather than pandoc-derived. See doc_metadata.hpp for why the marker is
	//! required.
	std::string source_type;
	std::string element_type; //!< "heading", "paragraph", "list", "list_item",
	                          //!< "blockquote"
	std::string content;      //!< flattened text; empty when inlines are populated
	int heading_level = 0;    //!< 1-6 for headings, 0 otherwise
	//! STRUCTURAL depth, 1 for a top-level block. This reader emits containers
	//! now.
	int level = 1;
	//! `list` only: DuckBlockVocabulary::LIST_TYPE_*
	std::string list_type;
	//! `table` only: 'json', for the spec 5.0 native schema.
	std::string encoding;
	//! Anything not covered by the fields above -- currently a definition-list
	//! item's `role`. Merged after the derived entries so it cannot displace one.
	std::map<std::string, std::string> attributes;
	std::vector<DocxInline> inlines;
};

// THE `ByteSource &` FORM IS THE IMPLEMENTATION; THE PATH FORM DELEGATES TO IT
// (issue #120 step 2), and that direction is the point rather than an
// implementation detail worth hiding. A .docx is a ZIP, so until #120 the only
// way into this module was a path that `ZipContainer` handed to miniz -- which
// means docx, odt and epub were the three readers that could not see an `s3://`
// or `https://` file, while the other ten read whatever filesystem the host had
// attached. The extension's tail now opens through DuckDB's FileSystem and
// passes a `FileHandleSource`; a standalone consumer with no host filesystem
// passes a path and gets the `FileSource` the path form builds for it.
//
// Written THIS way round -- source primitive, path delegate -- so there is one
// archive code path. The reverse (a path primitive plus a parallel source
// implementation) is how the two drift: a fix applied to one spelling and not
// the other is invisible until a user hits the branch that did not get it.

//! Parse a .docx archive into format-shaped blocks. The intermediate; callers
//! wanting duck_block rows want ReadDocx.
//!
//! Reads the archive out of `source`, which must outlive the call. On failure
//! `status` says which failure and, for a missing or unparseable member, which
//! member, and the result is empty. A source that cannot be read at all reports
//! the same `NotAZip` an unopenable path does -- see byte_source.hpp on why a
//! short read is a report rather than an error.
std::vector<DocxBlock> ParseDocxFile(ByteSource &source, ContainerStatus &status);

//! Parse a .docx file into format-shaped blocks. Opens `path` as a `FileSource`
//! and calls the overload above; a missing or unreadable path reports `NotAZip`,
//! exactly as it did when this function opened the archive itself.
std::vector<DocxBlock> ParseDocxFile(const std::string &path, ContainerStatus &status);

//! Read a .docx archive as duck_block rows. THE MODULE'S PUBLIC SHAPE, shared by
//! every format module -- document in, vocabulary rows out, no DuckDB anywhere.
//! Container formats take a byte source or a path where the text formats take
//! source text.
std::vector<Block> ReadDocx(ByteSource &source, ContainerStatus &status);

//! Read a .docx file as duck_block rows, by path. The thin delegate; see above.
std::vector<Block> ReadDocx(const std::string &path, ContainerStatus &status);

} // namespace docx
} // namespace panduck
