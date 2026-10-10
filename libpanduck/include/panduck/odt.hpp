#pragma once

#include "panduck/block.hpp"
#include "panduck/container_status.hpp"

#include <map>
#include <string>
#include <vector>

// Moved behind the libpanduck seam (issue #104, L2). Names no DuckDB type.
// The table function, its bind, its scan and its registration stay in
// src/odt_reader.cpp.
//
// THE ENTRY POINTS TAKE A PATH, not source text, and that is deliberate rather
// than a shortcut. The seven Tier 1 modules are `ReadX(const std::string &src)`
// because their formats ARE text. A .odt is a ZIP archive. libpanduck exists so
// a consumer can read a document without DuckDB, so if the core only accepted
// pre-extracted content.xml then every standalone consumer would have to
// reimplement ODF part lookup -- and libpanduck would own a zip reader its own
// modules refused to use. ::panduck::ZipContainer is already behind the seam and
// already opens files from disk (mz_zip_reader_init_file), so there is no new
// dependency in taking the path here.
//
// FAILURE IS REPORTED, NOT THROWN. ParseOdtFile used to throw IOException and
// InvalidInputException; those are DuckDB API types AND their type is the
// SQL-visible `IO Error:` / `Invalid Input Error:` prefix that
// test/sql/odt_reader.test matches. Both entry points therefore take a
// ContainerStatus out-parameter and return an EMPTY vector on failure;
// src/odt_reader.cpp raises. See panduck/container_status.hpp for why neither
// the throw nor the message could come along.

namespace panduck {
namespace odt {

struct OdtInline {
	std::string element_type;
	std::string content;
	//! `image`: src.
	std::map<std::string, std::string> attributes;
};

struct OdtBlock {
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
	//! STRUCTURAL depth, 1 for a top-level block. Lists nest, so this reader no
	//! longer emits everything at 1 the way it did while list content was
	//! flattened away.
	int level = 1;
	//! `list` only: DuckBlockVocabulary::LIST_TYPE_*
	std::string list_type;
	//! `table` only: 'json'.
	std::string encoding;
	//! Anything that is not one of the fields above -- currently a
	//! definition-list item's `role`. Merged into the emitted attributes map
	//! after the derived entries, so a reader-specific key cannot silently
	//! overwrite `list_type` or `heading_level`.
	std::map<std::string, std::string> attributes;
	std::vector<OdtInline> inlines;
};

//! Parse a .odt file into format-shaped blocks. The intermediate; callers
//! wanting duck_block rows want ReadOdt.
//!
//! Opens the archive itself. On failure `status` says which failure and, for a
//! missing or unparseable member, which member, and the result is empty.
std::vector<OdtBlock> ParseOdtFile(const std::string &path, ContainerStatus &status);

//! Read a .odt file as duck_block rows. THE MODULE'S PUBLIC SHAPE, shared by
//! every format module -- document in, vocabulary rows out, no DuckDB anywhere.
//! Container formats take a path where the text formats take source text; see
//! the note at the top of this file.
std::vector<Block> ReadOdt(const std::string &path, ContainerStatus &status);

} // namespace odt
} // namespace panduck
