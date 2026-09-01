#include "latex_macros.hpp"

#include <cstddef>

namespace duckdb {
namespace latex {

namespace {

const MacroEntry MACROS[] = {
    // SEMANTIC -- inline
    {"textbf", Disposition::SEMANTIC, "bold", 1, 0, nullptr},
    {"bf", Disposition::SEMANTIC, "bold", 1, 0, nullptr},
    {"emph", Disposition::SEMANTIC, "italic", 1, 0, nullptr},
    {"textit", Disposition::SEMANTIC, "italic", 1, 0, nullptr},
    {"it", Disposition::SEMANTIC, "italic", 1, 0, nullptr},
    {"underline", Disposition::SEMANTIC, "underline", 1, 0, nullptr},
    {"uline", Disposition::SEMANTIC, "underline", 1, 0, nullptr},
    {"sout", Disposition::SEMANTIC, "strikethrough", 1, 0, nullptr},
    {"st", Disposition::SEMANTIC, "strikethrough", 1, 0, nullptr},
    {"texttt", Disposition::SEMANTIC, "code", 1, 0, nullptr},
    {"textsc", Disposition::SEMANTIC, "smallcaps", 1, 0, nullptr},
    {"textsuperscript", Disposition::SEMANTIC, "superscript", 1, 0, nullptr},
    {"textsubscript", Disposition::SEMANTIC, "subscript", 1, 0, nullptr},
    {"href", Disposition::SEMANTIC, "link", 2, 1, nullptr},
    {"url", Disposition::SEMANTIC, "link", 1, 0, nullptr},
    {"includegraphics", Disposition::SEMANTIC, "image", 1, 0, nullptr},
    {"footnote", Disposition::SEMANTIC, "note", 1, 0, nullptr},
    {"cite", Disposition::SEMANTIC, "cite", 1, 0, nullptr},

    // SEMANTIC -- sectioning. heading_level is resolved at parse time from the
    // documentclass, so element_type alone is not enough; the reader consults
    // HeadingLevelFor() rather than a level column here.
    {"part", Disposition::SEMANTIC, "heading", 1, 0, nullptr},
    {"chapter", Disposition::SEMANTIC, "heading", 1, 0, nullptr},
    {"section", Disposition::SEMANTIC, "heading", 1, 0, nullptr},
    {"subsection", Disposition::SEMANTIC, "heading", 1, 0, nullptr},
    {"subsubsection", Disposition::SEMANTIC, "heading", 1, 0, nullptr},
    {"paragraph", Disposition::SEMANTIC, "heading", 1, 0, nullptr},
    {"subparagraph", Disposition::SEMANTIC, "heading", 1, 0, nullptr},

    // TRANSPARENT -- drop the macro, DESCEND into the content argument.
    {"hypertarget", Disposition::TRANSPARENT, nullptr, 2, 1, nullptr},
    {"texorpdfstring", Disposition::TRANSPARENT, nullptr, 2, 0, nullptr},
    {"textnormal", Disposition::TRANSPARENT, nullptr, 1, 0, nullptr},
    {"mbox", Disposition::TRANSPARENT, nullptr, 1, 0, nullptr},
    {"text", Disposition::TRANSPARENT, nullptr, 1, 0, nullptr},
    {"protect", Disposition::TRANSPARENT, nullptr, 0, -1, nullptr},

    // DROPPED -- macro AND arguments. Presentational or metadata.
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
    {"itemize", Disposition::SEMANTIC, "list", 0, -1, "bullet"},
    {"enumerate", Disposition::SEMANTIC, "list", 0, -1, "ordered"},
    {"quote", Disposition::SEMANTIC, "blockquote", 0, -1, nullptr},
    {"quotation", Disposition::SEMANTIC, "blockquote", 0, -1, nullptr},
    {"verbatim", Disposition::SEMANTIC, "code", 0, -1, nullptr},
    {"lstlisting", Disposition::SEMANTIC, "code", 0, -1, nullptr},
    {"center", Disposition::TRANSPARENT, nullptr, 0, -1, nullptr},
    {"abstract", Disposition::TRANSPARENT, nullptr, 0, -1, nullptr},
    {"document", Disposition::TRANSPARENT, nullptr, 0, -1, nullptr},
    // `description` is HELD: duck_block has no settled list_type for a definition list and
    // emitting one would invent a value no consumer can read. TRANSPARENT keeps its text.
    {"description", Disposition::TRANSPARENT, nullptr, 0, -1, nullptr},
    // Dropped whole: descending yields mangled cell and coordinate text as prose.
    {"tabular", Disposition::DROPPED, nullptr, 0, -1, nullptr},
    {"tikzpicture", Disposition::DROPPED, nullptr, 0, -1, nullptr},
    {"equation", Disposition::DROPPED, nullptr, 0, -1, nullptr},
    {"align", Disposition::DROPPED, nullptr, 0, -1, nullptr},
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
	for (size_t i = 0; i < ENVIRONMENT_COUNT; i++) {
		if (name == ENVIRONMENTS[i].name) {
			return &ENVIRONMENTS[i];
		}
	}
	return nullptr;
}

} // namespace latex
} // namespace duckdb
