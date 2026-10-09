#ifdef PANDUCK_WITH_TEXTILE

#include "panduck/textile.hpp"
#include "panduck/vocabulary.hpp"
#include "panduck_test.hpp"
#include "panduck_test_modules.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace panduck_test {
namespace {

//! A heading, a paragraph with inline emphasis, a bullet list and a `bc.` code
//! block. textile is one of the two readers the encoding trap was actually
//! caught in, so the inline rows here are not decoration.
const char *const kSource = "h1. Heading\n"
                            "\n"
                            "Some *bold* text.\n"
                            "\n"
                            "* one\n"
                            "* two\n"
                            "\n"
                            "bc. x=1\n";

} // namespace

void RunTextileTests() {
	typedef ::panduck::DuckBlockVocabulary V;

	const std::vector<::panduck::Block> rows = ::panduck::textile::ReadTextile(kSource);

	CHECK(!rows.empty());
	CHECK_EQ(rows.size(), static_cast<size_t>(9));

	CheckVocabularyShape("textile", rows);

	const ::panduck::Block *heading = FindFirst(rows, V::KIND_BLOCK, V::TYPE_HEADING);
	CHECK(heading != NULL);
	if (heading) {
		CHECK_EQ(Content(*heading), std::string("Heading"));
		CHECK_EQ(Attr(*heading, V::ATTR_HEADING_LEVEL), std::string("1"));
		CHECK_EQ(heading->level, 1);
		CHECK_EQ(heading->encoding, Vocab(V::ENCODING_TEXT));
		// textile slugifies heading ids (libpanduck/include/panduck/slugify.hpp).
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
	}

	// OBSERVED behaviour on empty input: zero rows. This note used to call that a
	// divergence from a "ruling" in libpanduck/include/panduck/block.hpp. There
	// was no ruling, and the question is open upstream where the vocabulary is
	// owned: https://github.com/teaguesterling/duckdb_duck_block_utils/issues/60
	// See test_org.cpp for the note, block.hpp for the measured detail.
	const std::vector<::panduck::Block> empty = ::panduck::textile::ReadTextile("");
	CHECK_EQ(empty.size(), static_cast<size_t>(0));

	const std::vector<::panduck::Block> blank = ::panduck::textile::ReadTextile("\n\n");
	CHECK_EQ(blank.size(), static_cast<size_t>(0));
}

} // namespace panduck_test

#endif // PANDUCK_WITH_TEXTILE
