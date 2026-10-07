#ifdef PANDUCK_WITH_LATEX

#include "panduck/latex.hpp"
#include "panduck/vocabulary.hpp"
#include "panduck_test.hpp"
#include "panduck_test_modules.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace panduck_test {
namespace {

//! A `\section`, a paragraph with `\textbf` and an `itemize` environment.
//! latex is one module in three translation units -- core, tokenizer, macro
//! table -- so a fixture that reaches all three is worth more here than
//! elsewhere.
const char *const kSource = "\\section{Heading}\n"
                            "\n"
                            "Some \\textbf{bold} text.\n"
                            "\n"
                            "\\begin{itemize}\n"
                            "\\item one\n"
                            "\\item two\n"
                            "\\end{itemize}\n";

} // namespace

void RunLatexTests() {
	typedef ::panduck::DuckBlockVocabulary V;

	const std::vector<::panduck::Block> rows = ::panduck::latex::ReadLatex(kSource);

	CHECK(!rows.empty());
	CHECK_EQ(rows.size(), static_cast<size_t>(8));

	CheckVocabularyShape("latex", rows);

	const ::panduck::Block *heading = FindFirst(rows, V::KIND_BLOCK, V::TYPE_HEADING);
	CHECK(heading != NULL);
	if (heading) {
		CHECK_EQ(Content(*heading), std::string("Heading"));
		CHECK_EQ(Attr(*heading, V::ATTR_HEADING_LEVEL), std::string("1"));
		CHECK_EQ(heading->level, 1);
		CHECK_EQ(heading->encoding, Vocab(V::ENCODING_TEXT));
	}

	const ::panduck::Block *bold = FindFirst(rows, V::KIND_INLINE, V::INLINE_BOLD);
	CHECK(bold != NULL);
	if (bold) {
		CHECK_EQ(Content(*bold), std::string("bold"));
		CHECK_EQ(bold->encoding, Vocab(V::ENCODING_TEXT));
	}

	const ::panduck::Block *list = FindFirst(rows, V::KIND_BLOCK, V::TYPE_LIST);
	CHECK(list != NULL);
	if (list) {
		CHECK_EQ(Attr(*list, V::ATTR_LIST_TYPE), Vocab(V::LIST_TYPE_BULLET));
	}

	const ::panduck::Block *item = FindFirst(rows, V::KIND_BLOCK, V::TYPE_LIST_ITEM);
	CHECK(item != NULL);
	if (item) {
		CHECK_EQ(Content(*item), std::string("one"));
	}

	// The macro table is reachable on its own and HeadingLevelFor is a pure
	// function, so this pins the class-dependent heading mapping without going
	// through a parse at all: `article` puts \section at 1, while `book` gives
	// 1 to \chapter and shifts the rest down by one.
	CHECK_EQ(::panduck::latex::HeadingLevelFor("section", "article"), 1);
	CHECK_EQ(::panduck::latex::HeadingLevelFor("subsection", "article"), 2);
	CHECK_EQ(::panduck::latex::HeadingLevelFor("chapter", "book"), 1);
	CHECK_EQ(::panduck::latex::HeadingLevelFor("section", "book"), 2);

	// OBSERVED behaviour on empty input -- diverges from the ruling in
	// libpanduck/include/panduck/block.hpp; see test_org.cpp for the note.
	const std::vector<::panduck::Block> empty = ::panduck::latex::ReadLatex("");
	CHECK_EQ(empty.size(), static_cast<size_t>(0));

	const std::vector<::panduck::Block> blank = ::panduck::latex::ReadLatex("\n\n");
	CHECK_EQ(blank.size(), static_cast<size_t>(0));
}

} // namespace panduck_test

#endif // PANDUCK_WITH_LATEX
