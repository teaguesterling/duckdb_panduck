#pragma once

#include "duckdb.hpp"

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
};

struct OdtBlock {
	std::string element_type; //!< "heading" or "paragraph"
	std::string content;      //!< flattened text; empty when inlines are populated
	int heading_level = 0;    //!< 1-6 for headings, 0 otherwise
	std::vector<OdtInline> inlines;
};

//! Parse a .odt file into block elements. Throws IOException when the file is missing or
//! is not a readable ZIP, and InvalidInputException when content.xml is absent.
std::vector<OdtBlock> ParseOdtFile(const std::string &path);

} // namespace odt

void RegisterOdtReaderFunction(ExtensionLoader &loader);

} // namespace duckdb
