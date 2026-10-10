#pragma once

#include "panduck/block.hpp"
#include "panduck/byte_source.hpp"
#include "panduck/container_status.hpp"

#include <string>
#include <vector>

// Moved behind the libpanduck seam (issue #104, L2). Names no DuckDB type.
// The table function, its bind, its scan and its registration stay in
// src/epub_reader.cpp.
//
// THE ENTRY POINTS TAKE AN ARCHIVE, not source text, and epub is the clearest
// case of why (see panduck/odt.hpp for the general argument). A book is not one
// member but a SPINE of them, reached through three levels of indirection --
// META-INF/container.xml -> the OPF package document -> manifest + spine -- and
// the stylesheets are more members again. There is no "source text" to hand in.
// A core that did not open the archive could not read an EPUB at all. Since #120
// the archive arrives as a `ByteSource`, with the path form kept as a delegate
// over `FileSource`; see the note above the entry points.
//
// EPUB IS ALSO WHERE THE SOURCE'S LIFETIME MATTERS MOST. The container is opened
// once and read dozens of times -- once per spine member -- so a `ByteSource`
// handed in here is still being read long after the call begins and must outlive
// it. That is the rule byte_source.hpp states for every source; epub is just the
// module where a dangling one would be hardest to miss.
//
// FAILURE IS REPORTED, NOT THROWN -- a ContainerStatus out-parameter and an
// empty result. epub had the most throws of the three: IOException on a bad ZIP,
// InvalidInputException on a missing META-INF/container.xml, on any of four
// members not being well-formed XML, on a container.xml naming no rootfile, and
// on the named package document being absent. Those TYPES are the SQL-visible
// `IO Error:` / `Invalid Input Error:` prefix test/sql/epub_reader.test matches.
// src/epub_reader.cpp raises. See panduck/container_status.hpp -- EPUB is why
// ContainerStatus has five codes rather than three.
//
// EPUB DOES NOT NEED AN HTML PARSER, which is what makes this module cheap:
// EPUB requires XHTML, which is well-formed XML by definition, so pugixml reads
// it directly. The reader doc comment in src/include/epub_reader.hpp has the
// full argument.

namespace panduck {
namespace epub {

struct EpubInline {
	std::string element_type;
	std::string content;
	std::string href; //!< set for links
	std::string src;  //!< set for images
};

struct EpubBlock {
	//! duck_block kind. Empty means `block`, which is everything a document body
	//! produces. `value` is document METADATA -- a discrete field, not body
	//! content.
	std::string kind;
	//! `value` only: the field name, in attributes['key']. PANDOC'S namespace,
	//! not the source's -- dc:creator is `author`.
	std::string key;
	//! `page_break` only: the PRINT edition's page label, from
	//! epub:type="pagebreak". EPUB 3's way of recording where the print edition's
	//! page N began, which citation and library workflows need and which this
	//! reader discarded until 2026-09-01.
	std::string page_number;
	std::string element_type; //!< heading, paragraph, list_item, blockquote, div,
	                          //!< code, hr
	std::string content;      //!< flattened text; empty when inlines are populated
	int heading_level = 0;    //!< 1-6 for headings, 0 otherwise
	bool container = false;   //!< true for blocks whose text lives in the blocks that follow
	//! Structural nesting depth, NOT the heading level. 0 means NULL -- a block
	//! at the top of the document, owned by no container. `level` IS duck_block's
	//! containment mechanism: a container's children follow it at level+1 and the
	//! container ends at the first element back at its own level, so a consumer
	//! has nothing else to read.
	int level = 0;
	//! 'bullet' or 'ordered' for a `list`, empty otherwise. Ordered lists
	//! additionally carry start/number_style/number_delim -- emitted always, even
	//! at their defaults, because that is what duck_block_utils' Pandoc reader
	//! does and matching the stricter producer keeps one shape rather than two.
	//! For a `section`: which kind of sectioning container the source marked, per
	//! the duck_block role vocabulary. Empty for anything else.
	std::string role;
	//! `table` only: 'json', because spec 5.0 makes table the one element_type
	//! whose content is a JSON document rather than text. Empty elsewhere, and an
	//! empty encoding is emitted as NULL rather than as the string.
	std::string encoding;
	std::string list_type;
	std::string list_start, number_style, number_delim;
	std::vector<EpubInline> inlines;
};

// THE `ByteSource &` FORM IS THE IMPLEMENTATION; THE PATH FORM DELEGATES TO IT
// (issue #120 step 2). The full argument is recorded once, above the entry
// points in panduck/docx.hpp: source primitive plus path delegate is one
// archive code path, where a path primitive plus a parallel source
// implementation is two that drift. Until #120 this module's only way in was a
// path handed to miniz, which is why epub was one of the three readers that
// could not read an `s3://` or `https://` file while the other ten could.

//! Parse a .epub archive into format-shaped blocks, in spine order. The
//! intermediate; callers wanting duck_block rows want ReadEpub.
//!
//! Reads the archive out of `source`, which must outlive the call -- and this is
//! the module where that is read from most often; see above. On failure `status`
//! says which failure and, for a missing or unparseable member, which member,
//! and the result is empty.
std::vector<EpubBlock> ParseEpubFile(ByteSource &source, ContainerStatus &status);

//! Parse a .epub file into format-shaped blocks. Opens `path` as a `FileSource`
//! and calls the overload above; a missing or unreadable path reports `NotAZip`,
//! exactly as it did when this function opened the archive itself.
std::vector<EpubBlock> ParseEpubFile(const std::string &path, ContainerStatus &status);

//! Read a .epub archive as duck_block rows. THE MODULE'S PUBLIC SHAPE, shared by
//! every format module -- document in, vocabulary rows out, no DuckDB anywhere.
//! Container formats take a byte source or a path where the text formats take
//! source text.
std::vector<Block> ReadEpub(ByteSource &source, ContainerStatus &status);

//! Read a .epub file as duck_block rows, by path. The thin delegate; see above.
std::vector<Block> ReadEpub(const std::string &path, ContainerStatus &status);

} // namespace epub
} // namespace panduck
