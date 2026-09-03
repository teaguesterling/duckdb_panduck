#pragma once

#include "duckdb.hpp"

#include <string>
#include <vector>

namespace duckdb {

//! RTF (Rich Text Format) reader.
//!
//! RTF is 7-bit ASCII with brace-delimited groups and backslash control words, so unlike
//! DOCX/ODT/EPUB it needs neither miniz nor pugixml -- this reader is self-contained.
//!
//! Heading detection deliberately supports TWO mechanisms, because real writers disagree
//! and handling only one silently loses every heading from the other:
//!
//!   * `\outlinelevelN` on the paragraph -- emitted by pandoc and newer writers.
//!   * `\sN` referencing a `{\stylesheet}` entry whose name matches "Heading N" --
//!     emitted by LibreOffice and Word, which write no `\outlinelevel` at all.
//!
//! Both fixtures in test/fixtures/ were produced by real writers and exercise exactly one
//! mechanism each, so a regression in either path fails a test.
namespace rtf {

//! An inline run within a block. element_type uses the duck_block inline vocabulary
//! ("text", "bold", "italic", "underline", "strikethrough") fixed by duck_block_utils.
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
	std::string list_type;          //!< `list` only: DuckBlockTypes::LIST_TYPE_*
	std::vector<RtfInline> inlines; //!< empty for a text-only run
};

//! Parse an RTF document into block elements.
//! Never throws on malformed input -- unbalanced groups and unknown control words are
//! tolerated, matching how readers must behave on documents in the wild.
std::vector<RtfBlock> ParseRtfDocument(const std::string &data);

} // namespace rtf

//! Registers read_rtf_blocks(VARCHAR). Columns mirror the duck_block struct field order,
//! so `SELECT list(b::duck_block) FROM read_rtf_blocks(path) b` works the way
//! duck_block_utils' doc_to_blocks dispatcher expects.
void RegisterRtfReaderFunction(ExtensionLoader &loader);

} // namespace duckdb
