#pragma once

#include "panduck/block.hpp"

#include <string>
#include <vector>

// Moved behind the libpanduck seam (issue #104, L2). Names no DuckDB type.
// The table function, its bind and its registration stay in src/rtf_reader.cpp,
// including DuckDB's FileSystem -- read_rtf_blocks takes a path.
//
// RTF IS A TIER 2 READER THAT MOVES LIKE A TIER 1 ONE. The other three Tier 2
// readers -- docx, odt, epub -- additionally need pugixml and zip_container.hpp,
// which is a dependency decision for the standalone build. RTF needs neither:
// it is 7-bit ASCII with brace-delimited groups and backslash control words, so
// the whole core is plain C++ over the vocabulary. Its only non-portable include
// was duckdb/common/file_system.hpp, which belongs to the tail anyway.
//
// WHAT MADE IT HARDER THAN TIER 1 ANYWAY: every Tier 1 reader had a
// `BuildRows(src, rows)` free function sitting after its parse core -- a clean
// cut line. RTF had none. Its flatten was FUSED INTO RtfReaderBind, ~17 lines of
// row building wedged between the FileSystem read and the `return`. ReadRtf
// below is that code lifted out verbatim; see libpanduck/src/rtf.cpp.

namespace panduck {
namespace rtf {

//! An inline run within a block. element_type uses the duck_block inline
//! vocabulary
//! ("text", "bold", "italic", "underline", "strikethrough") fixed by
//! duck_block_utils.
//!
//! NO `encoding` AND NO `level` FIELD, unlike MwInline -- which is why ReadRtf
//! assigns both unconditionally rather than conditionally. See the flatten.
struct RtfInline {
	std::string element_type;
	std::string content;
};

//! One block-level element.
struct RtfBlock {
	//! duck_block kind. Empty means `block`. `value` is document METADATA.
	std::string kind;
	//! `value` only: field name for attributes['key'], in PANDOC's namespace.
	std::string key;
	std::string element_type;       //!< "heading", "paragraph", "list", "list_item"
	std::string content;            //!< flattened text; empty when inlines are populated
	int heading_level = 0;          //!< 1-6 for headings, 0 otherwise
	int level = 1;                  //!< STRUCTURAL depth; lists nest, so not always 1
	std::string list_type;          //!< `list` only: DuckBlockVocabulary::LIST_TYPE_*
	std::string encoding;           //!< `table` only: 'json'
	std::vector<RtfInline> inlines; //!< empty for a text-only run
};

//! Parse an RTF document into format-shaped blocks. The intermediate; callers
//! wanting duck_block rows want ReadRtf.
//!
//! Never throws on malformed input -- unbalanced groups and unknown control
//! words are tolerated, matching how readers must behave on documents in the
//! wild.
//!
//! RENAMED FROM ParseRtfDocument in the seam move, and it is the only rename in
//! it. Every other module spells this entry point ParseXString -- ParseOrgString,
//! ParseLatexString, ParseTextileString -- and rtf was the lone holdout. Safe
//! because the old name had exactly two references, both inside the reader's own
//! two files; nothing in test/ or in another reader named it. The shim
//! src/include/rtf_reader.hpp re-exports the new spelling, so `rtf::` call sites
//! in namespace duckdb are unaffected either way.
std::vector<RtfBlock> ParseRtfString(const std::string &data);

//! Read RTF source as duck_block rows. THE MODULE'S PUBLIC SHAPE, shared by
//! every format module: source in, vocabulary rows out, no DuckDB anywhere.
std::vector<Block> ReadRtf(const std::string &src);

} // namespace rtf
} // namespace panduck
