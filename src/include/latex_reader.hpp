#pragma once

// The LaTeX reader turns a token stream into duck_blocks. It knows nothing about bytes
// (latex_tokenizer.cpp owns those) and nothing about which macros mean what
// (latex_macros.cpp owns that), so what is left here is exactly the structural argument:
// where a paragraph starts, what a heading's rank is, and which of a macro's arguments is
// content rather than plumbing.
//
// WHY THE MACRO TABLE IS THE WHOLE POINT. LaTeX has no document model -- it is a macro
// language, and two writers producing "the same document" agree on almost none of the
// bytes. pandoc buries every heading two brace-levels deep inside
// \hypertarget{id}{%\n\section{...}\label{id}}; a person writes \section at the top level.
// A reader with a pandoc branch would read both and learn nothing general. A reader with a
// TRANSPARENT disposition on \hypertarget reads both because descending into a content
// argument is what \hypertarget MEANS, not what pandoc happens to emit -- and the same
// rule then reads \texorpdfstring, \mbox and \textnormal for free.
//
// WHAT THE PREAMBLE IS FOR. Everything before \begin{document} is discarded except
// \documentclass, whose first required argument decides whether \section is a level-1
// heading (article) or a level-2 one (book/report, where \chapter is 1). A source with no
// \begin{document} at all is parsed WHOLE as a body: handwritten fragments are common,
// they are the reason read_latex_blocks_string exists, and erroring on them helps nobody.

#include "duckdb.hpp"

#include <string>
#include <vector>

namespace duckdb {
namespace latex {

//! An inline run. `level` is absolute, not relative: a run directly inside a block sits
//! at that block's level + 1, and a run nested inside another run is one deeper again.
struct LatexInline {
	std::string element_type; //!< bold, italic, text, link, image, math, ...
	std::string content;      //!< empty when this run has structured children
	std::string href;         //!< link only
	std::string src;          //!< image only
	std::string display;      //!< math only: "inline" or "block"
	int level = 2;
};

struct LatexBlock {
	//! duck_block kind. Empty means `block`. `value` is document METADATA.
	std::string kind;
	//! `value` only: the field name for attributes['key'], in PANDOC's namespace.
	std::string key;
	std::string element_type;
	std::string content;      //!< empty when a container, or when inlines are populated
	std::string list_type;    //!< list only: DuckBlockTypes::LIST_TYPE_* value
	std::string role;         //!< duck_block role vocabulary: term / definition on an item
	//! `table` only: 'json'. Spec 5.0 makes table the one element_type whose content is a
	//! JSON document rather than text. Empty means the default, `text`.
	std::string encoding;
	std::string list_start, number_style, number_delim; //!< ordered lists, always set
	int heading_level = 0;    //!< 1-6 for headings, 0 otherwise
	int level = 1;            //!< structural depth; never 0, never NULL on emission
	std::vector<LatexInline> inlines;
};

//! Parse LaTeX source into blocks. Never throws: malformed input degrades.
std::vector<LatexBlock> ParseLatexString(const std::string &src);

//! heading_level for a sectioning macro under a given document class. `article` (and the
//! default, and a fragment with no preamble) puts \section at 1; `book` and `report` put
//! \chapter at 1 and shift the rest down by one. Returns 1..6, capped.
int HeadingLevelFor(const std::string &macro_name, const std::string &document_class);

} // namespace latex

class ExtensionLoader;
void RegisterLatexReaderFunction(ExtensionLoader &loader);

} // namespace duckdb
