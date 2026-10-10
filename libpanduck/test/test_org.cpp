#ifdef PANDUCK_WITH_ORG

#include "panduck/org.hpp"
#include "panduck/vocabulary.hpp"
#include "panduck_test.hpp"
#include "panduck_test_modules.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace panduck_test {
namespace {

//! Deliberately small and deliberately mixed: a metadata keyword, a heading, a
//! paragraph with inline emphasis, a bullet list and a source block. The inline
//! emphasis is there on purpose -- the encoding trap was predicted to bite
//! inline CHILDREN hardest, because the flatten assigns them in a nested loop
//! that is easy to write without touching encoding at all.
const char *const kSource = "#+TITLE: T\n"
                            "\n"
                            "* Heading\n"
                            "\n"
                            "Some *bold* text.\n"
                            "\n"
                            "- one\n"
                            "- two\n"
                            "\n"
                            "#+BEGIN_SRC python\n"
                            "x=1\n"
                            "#+END_SRC\n";

} // namespace

void RunOrgTests() {
	typedef ::panduck::DuckBlockVocabulary V;

	const std::vector<::panduck::Block> rows = ::panduck::org::ReadOrg(kSource);

	// 1. It returns rows at all, through the DuckDB-free entry point.
	CHECK(!rows.empty());
	CHECK_EQ(rows.size(), static_cast<size_t>(11));

	// 2. The vocabulary shape, including the encoding check that is the whole
	//    point of this suite. Four checks.
	CheckVocabularyShape("org", rows);

	// 3. Parsing actually happened: the heading, with its level attribute.
	const ::panduck::Block *heading = FindFirst(rows, V::KIND_BLOCK, V::TYPE_HEADING);
	CHECK(heading != NULL);
	if (heading) {
		CHECK_EQ(Content(*heading), std::string("Heading"));
		CHECK_EQ(Attr(*heading, V::ATTR_HEADING_LEVEL), std::string("1"));
		CHECK_EQ(heading->level, 1);
		CHECK_EQ(heading->encoding, Vocab(V::ENCODING_TEXT));
	}

	// 4. An inline child, which is the row class the encoding trap favours.
	const ::panduck::Block *bold = FindFirst(rows, V::KIND_INLINE, V::INLINE_BOLD);
	CHECK(bold != NULL);
	if (bold) {
		CHECK_EQ(Content(*bold), std::string("bold"));
		CHECK_EQ(bold->encoding, Vocab(V::ENCODING_TEXT));
	}

	// 5. Format-specific structure: a bullet list and a source block with its
	//    language. Asserted through the vocabulary constants, never literals,
	//    so the test tracks the vocabulary rather than shadowing it.
	const ::panduck::Block *list = FindFirst(rows, V::KIND_BLOCK, V::TYPE_LIST);
	CHECK(list != NULL);
	if (list) {
		CHECK_EQ(Attr(*list, V::ATTR_LIST_TYPE), Vocab(V::LIST_TYPE_BULLET));
	}

	const ::panduck::Block *code = FindFirst(rows, V::KIND_BLOCK, V::TYPE_CODE);
	CHECK(code != NULL);
	if (code) {
		CHECK_EQ(Content(*code), std::string("x=1"));
		// "language" is spelled as a literal because the vocabulary has no
		// constant for it -- every reader writes attributes["language"]
		// directly (libpanduck/src/{org,rst,ipynb,textile,mediawiki}.cpp). Worth
		// an ATTR_LANGUAGE upstream; not this commit's call to make.
		CHECK_EQ(Attr(*code, "language"), std::string("python"));
	}

	// 6. org carries document metadata as kind='value' rows, so this module is
	//    the one that proves CheckVocabularyShape covers that kind too.
	const ::panduck::Block *meta = FindFirst(rows, V::KIND_VALUE, V::VALUE_INLINES);
	CHECK(meta != NULL);
	if (meta) {
		CHECK_EQ(Attr(*meta, V::ATTR_KEY), std::string("title"));
	}

	// 7. OBSERVED behaviour on empty input: ReadOrg returns NO rows, and that is
	//    all that is asserted.
	//
	//    THIS NOTE HAS BEEN WRONG TWICE, which is why it is long.
	//
	//    It first said the observation "diverges from Teague's ruling" that an
	//    empty document is a Doc row plus a Text row holding the empty string.
	//    There is no such ruling: that was a QUESTION Teague asked on
	//    2026-10-05, recorded in block.hpp as though it were his answer.
	//
	//    The correction then claimed the vocabulary "has no document or text
	//    `element_type` to express it with anyway". That is false. It was
	//    measured against panduck's VENDORED copy, which was two releases
	//    stale; upstream added TYPE_DOCUMENT in duck_block_utils v3.4.0 on
	//    2026-09-16, and this repo now vendors v3.5.0, so the constant is
	//    present in the very header beside this test.
	//
	//    WHAT IS ACTUALLY SETTLED. The `content` half is not open and never
	//    was: duck_block ruled on 2026-09-01, five weeks before any of this,
	//    that consumers MUST treat NULL and '' as the same absence and
	//    producers SHOULD emit NULL -- `coalesce(content, '') <> ''` being the
	//    portable test. So nothing about empty content awaits a decision, and
	//    the readers are already at the preferred spelling.
	//
	//    WHAT IS STILL OPEN is narrower and is not about `content`: whether an
	//    empty document should have a spine at all, which only became
	//    answerable once TYPE_DOCUMENT made a level-0 root expressible. All six
	//    readers return zero rows uniformly, which reads as one shared
	//    convention rather than six oversights:
	//    https://github.com/teaguesterling/duckdb_duck_block_utils/issues/63
	//
	//    So the assertion below stays as OBSERVED behaviour. Asserting the
	//    unanswered half would be a failing test with no owner; asserting the
	//    answered half would be asserting something this reader already does.
	//    block.hpp carries the measured detail so the other five notes stay
	//    short.
	const std::vector<::panduck::Block> empty = ::panduck::org::ReadOrg("");
	CHECK_EQ(empty.size(), static_cast<size_t>(0));

	const std::vector<::panduck::Block> blank = ::panduck::org::ReadOrg("\n\n");
	CHECK_EQ(blank.size(), static_cast<size_t>(0));
}

} // namespace panduck_test

#endif // PANDUCK_WITH_ORG
