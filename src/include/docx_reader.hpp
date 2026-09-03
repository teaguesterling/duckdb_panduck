#pragma once

#include "duckdb.hpp"

#include <string>
#include <vector>

namespace duckdb {

//! DOCX (Office Open XML) reader.
//!
//! A ZIP container whose body lives in word/document.xml, so this is the first reader
//! using both vendored dependencies: miniz to open the archive, pugixml to parse the XML.
//!
//! HEADINGS COME FROM TWO MUTUALLY EXCLUSIVE MECHANISMS, measured on real writers:
//!
//!                        w:pStyle "Heading*"      w:outlineLvl
//!     pandoc             Heading1, Heading2       none
//!     LibreOffice        none (only "Normal")     0, 1
//!
//! Handling only one loses every heading from the other. This is the same split RTF has
//! (\outlinelevel versus a {\stylesheet} \sN reference) with the writers on opposite
//! sides -- pandoc is style-based in DOCX and outline-based in RTF, LibreOffice the
//! reverse. Two formats, two writers, four combinations, and no writer agrees with
//! itself across formats. Treat "one heading mechanism" as an unsafe assumption by
//! default rather than something to discover per format.
//!
//! This is also where panduck earns its design. DOCX carries REAL heading semantics,
//! unlike the PDF bridge (which recovers none: 0 headings from a 100-page manual) or a
//! markdown round-trip. A native reader keeps structure the bridges throw away.
namespace docx {

struct DocxInline {
	std::string element_type; //!< duck_block inline vocabulary: text, bold, italic, ...
	std::string content;
};

struct DocxBlock {
	//! duck_block kind. Empty means `block`. `value` is document METADATA.
	std::string kind;
	//! `value` only: the field name for attributes['key'], in PANDOC's namespace.
	std::string key;
	//! `value` only: the ORIGINAL field spelling, marking this as format-derived rather
	//! than pandoc-derived. See doc_metadata.hpp for why the marker is required.
	std::string source_type;
	std::string element_type; //!< "heading", "paragraph", "list", "list_item", "blockquote"
	std::string content;      //!< flattened text; empty when inlines are populated
	int heading_level = 0;    //!< 1-6 for headings, 0 otherwise
	//! STRUCTURAL depth, 1 for a top-level block. This reader emits containers now.
	int level = 1;
	//! `list` only: DuckBlockTypes::LIST_TYPE_*
	std::string list_type;
	//! `table` only: 'json', for the spec 5.0 native schema.
	std::string encoding;
	std::vector<DocxInline> inlines;
};

//! Parse a .docx file into block elements. Throws IOException when the file is missing or
//! is not a readable ZIP, and InvalidInputException when word/document.xml is absent --
//! a ZIP without it is not a DOCX, and saying so beats returning zero rows.
std::vector<DocxBlock> ParseDocxFile(const std::string &path);

} // namespace docx

void RegisterDocxReaderFunction(ExtensionLoader &loader);

} // namespace duckdb
