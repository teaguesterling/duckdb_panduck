#ifdef PANDUCK_WITH_DOCX

#include "panduck/container_status.hpp"
#include "panduck/docx.hpp"
#include "panduck/vocabulary.hpp"
#include "panduck_test.hpp"
#include "panduck_test_modules.hpp"

#include <cstddef>
#include <string>
#include <vector>

// docx's FIRST STANDALONE ASSERTIONS (#129). Until this file, every assertion about
// ReadDocx ran through SQL (test/sql/docx_reader.test), so through DuckDB. This one runs
// it with no DuckDB in the process, which is the claim the seam makes.
//
// The fixture is test/fixtures/constructs.docx, the same document test/sql reads, so a
// difference between the two suites points at the DuckDB tail and not at the fixture.

namespace panduck_test {

void RunDocxTests() {
	typedef ::panduck::DuckBlockVocabulary V;

	::panduck::ContainerStatus status;
	const std::vector<::panduck::Block> rows = ::panduck::docx::ReadDocx(FixturePath("constructs.docx"), status);

	CHECK(status.Ok());
	// Measured through read_panduck_doc on 2026-11-10: 46 body rows and 6 metadata rows.
	CHECK_EQ(rows.size(), static_cast<size_t>(52));

	CheckVocabularyShape("docx", rows);

	const ::panduck::Block *heading = FindFirst(rows, V::KIND_BLOCK, V::TYPE_HEADING);
	CHECK(heading != NULL);
	if (heading) {
		CHECK_EQ(Content(*heading), std::string("Heading One"));
		CHECK_EQ(Attr(*heading, V::ATTR_HEADING_LEVEL), std::string("1"));
		CHECK_EQ(heading->element_order, 0);
	}

	// THE STYLED RUN. A paragraph's runs become inline rows one level under it: the
	// paragraph itself carries no content, and `bold` is its own row.
	const ::panduck::Block *bold = FindFirst(rows, V::KIND_INLINE, V::INLINE_BOLD);
	CHECK(bold != NULL);
	if (bold) {
		CHECK_EQ(Content(*bold), std::string("bold"));
		CHECK_EQ(bold->level, 2);
		CHECK_EQ(bold->encoding, Vocab(V::ENCODING_TEXT));
	}
	// NO CONTENT, in either spelling. The engine emits "" here, and the DuckDB tail
	// writes that as NULL (panduck/block.hpp:166), so SQL shows NULL and this seam shows
	// "". The portable test is the one block.hpp:84 names, coalesce(content, '') <> ''.
	// Pinning one spelling would fail on #107's change, which is not a regression.
	CHECK(rows[1].content.is_null || rows[1].content.value.empty());
	CHECK_EQ(rows[1].element_type, Vocab(V::TYPE_PARAGRAPH));

	const ::panduck::Block *link = FindFirst(rows, V::KIND_INLINE, V::INLINE_LINK);
	CHECK(link != NULL);
	if (link) {
		CHECK_EQ(Attr(*link, "href"), std::string("https://example.com"));
	}

	const ::panduck::Block *list = FindFirst(rows, V::KIND_BLOCK, V::TYPE_LIST);
	CHECK(list != NULL);
	if (list) {
		CHECK_EQ(Attr(*list, V::ATTR_LIST_TYPE), Vocab(V::LIST_TYPE_ORDERED));
	}

	// METADATA: the author is a value row whose text is its inline child.
	const ::panduck::Block *value = FindFirst(rows, V::KIND_VALUE, NULL);
	CHECK(value != NULL);
	if (value) {
		CHECK_EQ(Attr(*value, V::ATTR_KEY), std::string("author"));
		CHECK_EQ(Attr(*value, V::ATTR_SOURCE_TYPE), std::string("dc:creator"));
	}

	// BOTH SIDES. A missing file reports NOT_A_ZIP and returns no rows, rather than
	// throwing or returning a partial document.
	::panduck::ContainerStatus missing;
	const std::vector<::panduck::Block> none = ::panduck::docx::ReadDocx(FixturePath("no_such_file.docx"), missing);
	CHECK(!missing.Ok());
	CHECK_EQ(static_cast<int>(missing.code), static_cast<int>(::panduck::ContainerStatus::NOT_A_ZIP));
	CHECK(none.empty());
}

} // namespace panduck_test

#endif // PANDUCK_WITH_DOCX
