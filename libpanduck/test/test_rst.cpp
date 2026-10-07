#ifdef PANDUCK_WITH_RST

#include "panduck/rst.hpp"
#include "panduck/vocabulary.hpp"
#include "panduck_test.hpp"
#include "panduck_test_modules.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace panduck_test {
namespace {

//! An underlined section title, a paragraph with inline emphasis, a bullet list
//! and a `.. code-block::` directive -- enough to show the scanner, the parser
//! and the flatten all ran.
const char *const kSource = "Heading\n"
                            "=======\n"
                            "\n"
                            "Some **bold** text.\n"
                            "\n"
                            "- one\n"
                            "- two\n"
                            "\n"
                            ".. code-block:: python\n"
                            "\n"
                            "   x=1\n";

} // namespace

void RunRstTests() {
	typedef ::panduck::DuckBlockVocabulary V;

	const std::vector<::panduck::Block> rows = ::panduck::rst::ReadRst(kSource);

	CHECK(!rows.empty());
	CHECK_EQ(rows.size(), static_cast<size_t>(9));

	CheckVocabularyShape("rst", rows);

	const ::panduck::Block *heading = FindFirst(rows, V::KIND_BLOCK, V::TYPE_HEADING);
	CHECK(heading != NULL);
	if (heading) {
		CHECK_EQ(Content(*heading), std::string("Heading"));
		CHECK_EQ(Attr(*heading, V::ATTR_HEADING_LEVEL), std::string("1"));
		CHECK_EQ(heading->level, 1);
		CHECK_EQ(heading->encoding, Vocab(V::ENCODING_TEXT));
		CHECK_EQ(Attr(*heading, "id"), std::string("heading"));
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

	const ::panduck::Block *code = FindFirst(rows, V::KIND_BLOCK, V::TYPE_CODE);
	CHECK(code != NULL);
	if (code) {
		CHECK_EQ(Content(*code), std::string("x=1"));
		CHECK_EQ(Attr(*code, "language"), std::string("python"));
	}

	// OBSERVED behaviour on empty input -- diverges from the ruling in
	// libpanduck/include/panduck/block.hpp; see test_org.cpp for the note.
	const std::vector<::panduck::Block> empty = ::panduck::rst::ReadRst("");
	CHECK_EQ(empty.size(), static_cast<size_t>(0));

	const std::vector<::panduck::Block> blank = ::panduck::rst::ReadRst("\n\n");
	CHECK_EQ(blank.size(), static_cast<size_t>(0));
}

} // namespace panduck_test

#endif // PANDUCK_WITH_RST
