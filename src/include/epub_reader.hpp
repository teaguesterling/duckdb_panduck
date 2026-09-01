#pragma once

#include "duckdb.hpp"

#include <string>
#include <vector>

namespace duckdb {

//! EPUB reader.
//!
//! EPUB DOES NOT NEED AN HTML PARSER, and that is what makes this reader cheap. The open
//! question when EPUB was scheduled was how much of it to delegate to the webbed
//! extension, since EPUB content documents are "HTML". They are not: the EPUB
//! specification requires XHTML, which is well-formed XML BY DEFINITION. pugixml -- the
//! same parser DOCX and ODT already use -- reads it directly. Delegating would have
//! bought nothing and added a load-time dependency on webbed for a format that does not
//! need one. An arbitrary .html file still needs webbed, because arbitrary HTML is not
//! XML; an EPUB's content documents always are.
//!
//! A book is a ZIP with three levels of indirection before any text:
//!
//!     META-INF/container.xml  -- fixed path, the only fixed path in the format
//!       -> <rootfile full-path="..."/>          the OPF package document
//!         -> <manifest><item id href/>          id -> file
//!         -> <spine><itemref idref/>            READING ORDER
//!
//! The spine is why this cannot be "read every .xhtml member": ZIP member order is
//! arbitrary, and the spine is the only statement of what order a human reads the book in.
//!
//! WHAT THE TWO WRITERS DISAGREE ABOUT IS EVERYTHING. RTF and DOCX each offered two ways
//! to mark a heading and ODT offered one. EPUB is the first format where one writer emits
//! no semantics at all: pandoc writes <h1>, <ul>, <li>, <strong>; LibreOffice writes
//! <p class="para0"><span class="span0"> for every one of them and puts the meaning in a
//! CSS file. See css_style_sheet() below for where the line is drawn.
namespace epub {

struct EpubInline {
	std::string element_type;
	std::string content;
	std::string href; //!< set for links
	std::string src;  //!< set for images
};

struct EpubBlock {
	std::string element_type; //!< heading, paragraph, list_item, blockquote, div, code, hr
	std::string content;      //!< flattened text; empty when inlines are populated
	int heading_level = 0;    //!< 1-6 for headings, 0 otherwise
	bool container = false;   //!< true for blocks whose text lives in the blocks that follow
	//! Structural nesting depth, NOT the heading level. 0 means NULL -- a block at the
	//! top of the document, owned by no container. `level` IS duck_block's containment
	//! mechanism: a container's children follow it at level+1 and the container ends at
	//! the first element back at its own level, so a consumer has nothing else to read.
	int level = 0;
	//! 'bullet' or 'ordered' for a `list`, empty otherwise. Ordered lists additionally
	//! carry start/number_style/number_delim -- emitted always, even at their defaults,
	//! because that is what duck_block_utils' Pandoc reader does and matching the
	//! stricter producer keeps one shape rather than two.
	//! For a `section`: which kind of sectioning container the source marked, per the
	//! duck_block role vocabulary. Empty for anything else.
	std::string role;
	//! `table` only: 'json', because spec 5.0 makes table the one element_type whose
	//! content is a JSON document rather than text. Empty elsewhere, and an empty
	//! encoding is emitted as NULL rather than as the string.
	std::string encoding;
	std::string list_type;
	std::string list_start, number_style, number_delim;
	std::vector<EpubInline> inlines;
};

//! Parse a .epub file into block elements, in spine order. Throws IOException when the
//! file is missing or is not a readable ZIP, and InvalidInputException when
//! META-INF/container.xml or the package document it names is absent.
std::vector<EpubBlock> ParseEpubFile(const std::string &path);

} // namespace epub

void RegisterEpubReaderFunction(ExtensionLoader &loader);

} // namespace duckdb
