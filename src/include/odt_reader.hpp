#pragma once

#include "duckdb.hpp"

#include <map>
#include <string>
#include <vector>

namespace duckdb {

//! ODT (OpenDocument Text) reader.
//!
//! A ZIP whose body is content.xml, so it shares the container handling with DOCX via
//! ZipContainer and the XML parsing via pugixml. What differs is the vocabulary.
//!
//! ODT HAS NO HEADING AMBIGUITY, and the contrast is the interesting part. RTF and DOCX
//! both mark headings two mutually exclusive ways, with pandoc and LibreOffice on opposite
//! sides in the two formats. ODF does not, because it has a DEDICATED HEADING ELEMENT:
//!
//!     <text:h text:style-name="Heading_20_1" text:outline-level="1">Heading One</text:h>
//!
//! Both writers emit exactly that. There is nothing to disagree about, which is precisely
//! why the other two formats disagreed: they overload a paragraph with a property, and a
//! property can be expressed more than one way. A format with a real heading element
//! cannot have that problem.
//!
//! So the lesson from RTF and DOCX is not "always expect two mechanisms" -- it is "expect
//! them wherever headings are a paragraph wearing a hat."
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
	//! `value` only: the ORIGINAL field spelling, marking this as format-derived rather
	//! than pandoc-derived. See doc_metadata.hpp for why the marker is required.
	std::string source_type;
	std::string element_type; //!< "heading", "paragraph", "list", "list_item", "blockquote"
	std::string content;      //!< flattened text; empty when inlines are populated
	int heading_level = 0;    //!< 1-6 for headings, 0 otherwise
	//! STRUCTURAL depth, 1 for a top-level block. Lists nest, so this reader no longer
	//! emits everything at 1 the way it did while list content was flattened away.
	int level = 1;
	//! `list` only: DuckBlockTypes::LIST_TYPE_*
	std::string list_type;
	//! `table` only: 'json'.
	std::string encoding;
	//! Anything that is not one of the fields above -- currently a definition-list item's
	//! `role`. Merged into the emitted attributes map after the derived entries, so a
	//! reader-specific key cannot silently overwrite `list_type` or `heading_level`.
	std::map<std::string, std::string> attributes;
	std::vector<OdtInline> inlines;
};

//! Parse a .odt file into block elements. Throws IOException when the file is missing or
//! is not a readable ZIP, and InvalidInputException when content.xml is absent.
std::vector<OdtBlock> ParseOdtFile(const std::string &path);

} // namespace odt

void RegisterOdtReaderFunction(ExtensionLoader &loader);

} // namespace duckdb
