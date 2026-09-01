#include "latex_macros.hpp"

// element_type comes from the VENDORED VOCABULARY, never from a literal here. A rename
// upstream is then a compile error at every row; a literal would compile clean and
// silently stop matching what consumers look for -- the exact trade
// src/include/duck_block_types.hpp:19-20 spells out. The constants are constexpr, so the
// table below is still statically initialised.
#include "duck_block_types.hpp"

#include <cstddef>

namespace duckdb {
namespace latex {

namespace {

const MacroEntry MACROS[] = {
    // SEMANTIC -- inline
    // \bf and \it are DELIBERATELY ABSENT. They are font SWITCHES -- `{\bf hello world}`
    // makes the whole group bold and takes no argument at all -- so declaring them as
    // one-argument macros made the reader eat a single character and emit `bold "h"`
    // followed by `text "ello world"`. Their modern spellings, \textbf and \textit, DO
    // take an argument and are right below. Unclaimed, a switch drops its name and keeps
    // every character around it: the formatting is lost, the prose is not, which is the
    // same trade every other unclaimed presentational macro already makes.
    {"textbf", Disposition::SEMANTIC, DuckBlockTypes::INLINE_BOLD, 1, 0, nullptr},
    {"emph", Disposition::SEMANTIC, DuckBlockTypes::INLINE_ITALIC, 1, 0, nullptr},
    {"textit", Disposition::SEMANTIC, DuckBlockTypes::INLINE_ITALIC, 1, 0, nullptr},
    {"underline", Disposition::SEMANTIC, DuckBlockTypes::INLINE_UNDERLINE, 1, 0, nullptr},
    {"uline", Disposition::SEMANTIC, DuckBlockTypes::INLINE_UNDERLINE, 1, 0, nullptr},
    {"sout", Disposition::SEMANTIC, DuckBlockTypes::INLINE_STRIKETHROUGH, 1, 0, nullptr},
    {"st", Disposition::SEMANTIC, DuckBlockTypes::INLINE_STRIKETHROUGH, 1, 0, nullptr},
    {"texttt", Disposition::SEMANTIC, DuckBlockTypes::INLINE_CODE, 1, 0, nullptr},
    {"textsc", Disposition::SEMANTIC, DuckBlockTypes::INLINE_SMALLCAPS, 1, 0, nullptr},
    {"textsuperscript", Disposition::SEMANTIC, DuckBlockTypes::INLINE_SUPERSCRIPT, 1, 0, nullptr},
    {"textsubscript", Disposition::SEMANTIC, DuckBlockTypes::INLINE_SUBSCRIPT, 1, 0, nullptr},
    {"href", Disposition::SEMANTIC, DuckBlockTypes::INLINE_LINK, 2, 1, nullptr},
    {"url", Disposition::SEMANTIC, DuckBlockTypes::INLINE_LINK, 1, 0, nullptr},
    {"includegraphics", Disposition::SEMANTIC, DuckBlockTypes::INLINE_IMAGE, 1, 0, nullptr},
    {"footnote", Disposition::SEMANTIC, DuckBlockTypes::INLINE_NOTE, 1, 0, nullptr},
    {"cite", Disposition::SEMANTIC, DuckBlockTypes::INLINE_CITE, 1, 0, nullptr},

    // SEMANTIC -- sectioning. heading_level is resolved at parse time from the
    // documentclass, so element_type alone is not enough; the reader consults
    // HeadingLevelFor() rather than a level column here.
    {"part", Disposition::SEMANTIC, DuckBlockTypes::TYPE_HEADING, 1, 0, nullptr},
    {"chapter", Disposition::SEMANTIC, DuckBlockTypes::TYPE_HEADING, 1, 0, nullptr},
    {"section", Disposition::SEMANTIC, DuckBlockTypes::TYPE_HEADING, 1, 0, nullptr},
    {"subsection", Disposition::SEMANTIC, DuckBlockTypes::TYPE_HEADING, 1, 0, nullptr},
    {"subsubsection", Disposition::SEMANTIC, DuckBlockTypes::TYPE_HEADING, 1, 0, nullptr},
    {"paragraph", Disposition::SEMANTIC, DuckBlockTypes::TYPE_HEADING, 1, 0, nullptr},
    {"subparagraph", Disposition::SEMANTIC, DuckBlockTypes::TYPE_HEADING, 1, 0, nullptr},

    // TRANSPARENT -- drop the macro, DESCEND into the content argument.
    {"hypertarget", Disposition::TRANSPARENT, nullptr, 2, 1, nullptr},
    {"texorpdfstring", Disposition::TRANSPARENT, nullptr, 2, 0, nullptr},
    {"textnormal", Disposition::TRANSPARENT, nullptr, 1, 0, nullptr},
    {"mbox", Disposition::TRANSPARENT, nullptr, 1, 0, nullptr},
    {"text", Disposition::TRANSPARENT, nullptr, 1, 0, nullptr},
    {"protect", Disposition::TRANSPARENT, nullptr, 0, -1, nullptr},

    // DROPPED -- macro AND arguments. Presentational or metadata.
    // documentclass, usepackage and PassOptionsToPackage are PREAMBLE macros: Parse()
    // already special-cases \documentclass to read the class name and find where the
    // preamble ends, but a FRAGMENT with no \begin{document} never reaches that logic for
    // anything past it, and an unclaimed macro's brace group is TRANSPARENT, not dropped --
    // so \usepackage{ulem} in a bare preamble leaked "ulem" into the output as a paragraph.
    // Claiming all three here closes that for the fragment path the same way the preamble
    // scan already closes it for a full document.
    {"documentclass", Disposition::DROPPED, nullptr, 1, -1, nullptr},
    {"usepackage", Disposition::DROPPED, nullptr, 1, -1, nullptr},
    {"PassOptionsToPackage", Disposition::DROPPED, nullptr, 2, -1, nullptr},
    {"label", Disposition::DROPPED, nullptr, 1, -1, nullptr},
    {"tightlist", Disposition::DROPPED, nullptr, 0, -1, nullptr},
    {"maketitle", Disposition::DROPPED, nullptr, 0, -1, nullptr},
    {"title", Disposition::DROPPED, nullptr, 1, -1, nullptr},
    {"author", Disposition::DROPPED, nullptr, 1, -1, nullptr},
    {"date", Disposition::DROPPED, nullptr, 1, -1, nullptr},
    {"vspace", Disposition::DROPPED, nullptr, 1, -1, nullptr},
    {"hspace", Disposition::DROPPED, nullptr, 1, -1, nullptr},
    {"newpage", Disposition::DROPPED, nullptr, 0, -1, nullptr},
    {"clearpage", Disposition::DROPPED, nullptr, 0, -1, nullptr},
    {"noindent", Disposition::DROPPED, nullptr, 0, -1, nullptr},
    {"centering", Disposition::DROPPED, nullptr, 0, -1, nullptr},
    {"pagestyle", Disposition::DROPPED, nullptr, 1, -1, nullptr},
    {"setlength", Disposition::DROPPED, nullptr, 2, -1, nullptr},
    {"index", Disposition::DROPPED, nullptr, 1, -1, nullptr},
    {"tableofcontents", Disposition::DROPPED, nullptr, 0, -1, nullptr},
    {"newcommand", Disposition::DROPPED, nullptr, 2, -1, nullptr},
    {"renewcommand", Disposition::DROPPED, nullptr, 2, -1, nullptr},
    {"bibliography", Disposition::DROPPED, nullptr, 1, -1, nullptr},
    {"bibliographystyle", Disposition::DROPPED, nullptr, 1, -1, nullptr},

    // TEXT -- expands to literal characters.
    {"ldots", Disposition::TEXT, nullptr, 0, -1, "…"},
    {"dots", Disposition::TEXT, nullptr, 0, -1, "…"},
    {"textbackslash", Disposition::TEXT, nullptr, 0, -1, "\\"},
    {"LaTeX", Disposition::TEXT, nullptr, 0, -1, "LaTeX"},
    {"TeX", Disposition::TEXT, nullptr, 0, -1, "TeX"},
};

const size_t MACRO_COUNT = sizeof(MACROS) / sizeof(MACROS[0]);

const MacroEntry ENVIRONMENTS[] = {
    {"itemize", Disposition::SEMANTIC, DuckBlockTypes::TYPE_LIST, 0, -1, "bullet"},
    {"enumerate", Disposition::SEMANTIC, DuckBlockTypes::TYPE_LIST, 0, -1, "ordered"},
    {"quote", Disposition::SEMANTIC, DuckBlockTypes::TYPE_BLOCKQUOTE, 0, -1, nullptr},
    {"quotation", Disposition::SEMANTIC, DuckBlockTypes::TYPE_BLOCKQUOTE, 0, -1, nullptr},
    {"verbatim", Disposition::SEMANTIC, DuckBlockTypes::TYPE_CODE, 0, -1, nullptr},
    {"lstlisting", Disposition::SEMANTIC, DuckBlockTypes::TYPE_CODE, 0, -1, nullptr},
    {"center", Disposition::TRANSPARENT, nullptr, 0, -1, nullptr},
    {"abstract", Disposition::TRANSPARENT, nullptr, 0, -1, nullptr},
    {"document", Disposition::TRANSPARENT, nullptr, 0, -1, nullptr},
    // `description` WAS HELD on the transparent path: duck_block had no settled list_type
    // for a definition list, and inventing one would have produced a value no consumer
    // could read. Spec 5.0 settled it -- a definition list is a LIST KIND, `deflist` is
    // deprecated -- so the deferral is discharged on its own stated condition.
    {"description", Disposition::SEMANTIC, DuckBlockTypes::TYPE_LIST, 0, -1, "definition"},
    // Dropped whole: descending yields mangled cell and coordinate text as prose.
    {"tabular", Disposition::DROPPED, nullptr, 0, -1, nullptr},
    {"tikzpicture", Disposition::DROPPED, nullptr, 0, -1, nullptr},
    // DISPLAY MATH ENVIRONMENTS ARE DROPPED, AND `\[..\]` IS NOT. The asymmetry is
    // intended, not pending: `\[..\]` has one formula with a body the tokenizer can cut
    // out whole, so it becomes an inline `math` run. These environments hold a numbered,
    // aligned, multi-row LAYOUT -- `&` columns, `\\` rows, \intertext between them -- and
    // there is no duck_block shape for that, so emitting their source as one formula would
    // claim something the document does not say. The starred spellings -- align*, gather*
    // -- reach these through LookupEnvironment, which strips the star.
    {"equation", Disposition::DROPPED, nullptr, 0, -1, nullptr},
    {"align", Disposition::DROPPED, nullptr, 0, -1, nullptr},
    {"gather", Disposition::DROPPED, nullptr, 0, -1, nullptr},
    {"multline", Disposition::DROPPED, nullptr, 0, -1, nullptr},
    {"displaymath", Disposition::DROPPED, nullptr, 0, -1, nullptr},
    {"figure", Disposition::DROPPED, nullptr, 0, -1, nullptr},
    {"table", Disposition::DROPPED, nullptr, 0, -1, nullptr},
};

const size_t ENVIRONMENT_COUNT = sizeof(ENVIRONMENTS) / sizeof(ENVIRONMENTS[0]);

} // namespace

const MacroEntry *LookupMacro(const std::string &name) {
	for (size_t i = 0; i < MACRO_COUNT; i++) {
		if (name == MACROS[i].name) {
			return &MACROS[i];
		}
	}
	return nullptr;
}

const MacroEntry *LookupEnvironment(const std::string &name) {
	// A STARRED ENVIRONMENT IS THE UNNUMBERED VARIANT OF THE SAME CONSTRUCT, never a
	// different one, so `align*` gets `align`'s disposition and figure* gets figure's. The
	// tokenizer strips the star from a control WORD, but an environment name arrives from a
	// brace group and keeps it -- so without this, \begin{align*} (the most common display
	// math there is) missed the DROPPED list entirely and its source came out as prose,
	// which is the exact outcome that list exists to prevent.
	const std::string base = !name.empty() && name.back() == '*' ? name.substr(0, name.size() - 1) : name;
	for (size_t i = 0; i < ENVIRONMENT_COUNT; i++) {
		if (base == ENVIRONMENTS[i].name) {
			return &ENVIRONMENTS[i];
		}
	}
	return nullptr;
}

} // namespace latex
} // namespace duckdb
