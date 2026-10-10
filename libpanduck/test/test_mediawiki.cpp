#ifdef PANDUCK_WITH_MEDIAWIKI

#include "panduck/mediawiki.hpp"
#include "panduck/vocabulary.hpp"
#include "panduck_test.hpp"
#include "panduck_test_modules.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace panduck_test {
namespace {

//! A `= Heading =` section title, a paragraph with `'''bold'''` and a bullet
//! list -- the scanner, the parser and the flatten in one fixture.
const char *const kSource = "= Heading =\n"
                            "\n"
                            "Some '''bold''' text.\n"
                            "\n"
                            "* one\n"
                            "* two\n";

} // namespace

void RunMediaWikiTests() {
	typedef ::panduck::DuckBlockVocabulary V;

	const std::vector<::panduck::Block> rows = ::panduck::mediawiki::ReadMediaWiki(kSource);

	CHECK(!rows.empty());
	CHECK_EQ(rows.size(), static_cast<size_t>(8));

	CheckVocabularyShape("mediawiki", rows);

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

	const ::panduck::Block *item = FindFirst(rows, V::KIND_BLOCK, V::TYPE_LIST_ITEM);
	CHECK(item != NULL);
	if (item) {
		CHECK_EQ(Content(*item), std::string("one"));
		CHECK_EQ(item->level, 2);
	}

	// OBSERVED behaviour on empty input: zero rows. This note used to call that a
	// divergence from a "ruling" in libpanduck/include/panduck/block.hpp. There
	// was no ruling. And the `content` question is not open either: duck_block
	// ruled on 2026-09-01 that NULL and '' are the SAME absence and that
	// producers SHOULD emit NULL, so this reader's behaviour is the preferred
	// spelling rather than something awaiting a decision.
	//
	// What is still open is narrower and is not about `content` at all --
	// whether an empty document should have a spine, now that TYPE_DOCUMENT
	// makes a level-0 root expressible:
	// https://github.com/teaguesterling/duckdb_duck_block_utils/issues/63
	// See test_org.cpp for the note, block.hpp for the measured detail.
	const std::vector<::panduck::Block> empty = ::panduck::mediawiki::ReadMediaWiki("");
	CHECK_EQ(empty.size(), static_cast<size_t>(0));

	const std::vector<::panduck::Block> blank = ::panduck::mediawiki::ReadMediaWiki("\n\n");
	CHECK_EQ(blank.size(), static_cast<size_t>(0));
}

} // namespace panduck_test

#endif // PANDUCK_WITH_MEDIAWIKI
