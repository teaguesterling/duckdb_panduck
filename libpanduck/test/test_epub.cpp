#ifdef PANDUCK_WITH_EPUB

#include "panduck/container_status.hpp"
#include "panduck/epub.hpp"
#include "panduck/vocabulary.hpp"
#include "panduck_test.hpp"
#include "panduck_test_modules.hpp"

#include <cstddef>
#include <string>
#include <vector>

// epub's FIRST STANDALONE ASSERTIONS (#129). Until this file, every assertion about
// ReadEpub ran through SQL (test/sql/epub_reader.test), so through DuckDB.
//
// What is characteristic of EPUB is the SPINE: a book is several XHTML documents, and
// the reader walks them in spine order. test/fixtures/pandoc.epub is pandoc's output for
// a two-heading document, which pandoc splits into one section per chapter. So the
// assertions below are on the SECTIONS and their order, not only on a heading.

namespace panduck_test {

void RunEpubTests() {
	typedef ::panduck::DuckBlockVocabulary V;

	::panduck::ContainerStatus status;
	const std::vector<::panduck::Block> rows = ::panduck::epub::ReadEpub(FixturePath("pandoc.epub"), status);

	CHECK(status.Ok());
	// Measured through read_panduck_doc on 2026-11-10: 18 body rows and 6 metadata rows.
	CHECK_EQ(rows.size(), static_cast<size_t>(24));

	CheckVocabularyShape("epub", rows);

	// THE SPINE, in order: section, its heading, ..., a second section, its heading.
	size_t sections = 0;
	size_t first_section = rows.size();
	size_t second_section = rows.size();
	for (size_t i = 0; i < rows.size(); i++) {
		if (rows[i].element_type == V::TYPE_SECTION) {
			if (sections == 0) {
				first_section = i;
			} else if (sections == 1) {
				second_section = i;
			}
			sections++;
		}
	}
	CHECK_EQ(sections, static_cast<size_t>(2));
	CHECK_EQ(first_section, static_cast<size_t>(0));
	CHECK_EQ(second_section, static_cast<size_t>(10));
	if (second_section + 1 < rows.size()) {
		CHECK_EQ(Attr(rows[0], V::ATTR_ROLE), std::string("section"));
		CHECK_EQ(rows[1].element_type, Vocab(V::TYPE_HEADING));
		CHECK_EQ(Content(rows[1]), std::string("Heading One"));
		// A heading sits one level inside its section.
		CHECK_EQ(rows[1].level, rows[0].level + 1);
		CHECK_EQ(rows[second_section + 1].element_type, Vocab(V::TYPE_HEADING));
		CHECK_EQ(Content(rows[second_section + 1]), std::string("Heading Two"));
		CHECK_EQ(Attr(rows[second_section + 1], V::ATTR_HEADING_LEVEL), std::string("2"));
	}

	// Non-ASCII survives the container and the XML parser byte for byte.
	bool unicode = false;
	for (size_t i = 0; i < rows.size(); i++) {
		if (Content(rows[i]).find("caf\xC3\xA9 \xE2\x80\x94 em-dash") != std::string::npos) {
			unicode = true;
		}
	}
	CHECK_MSG(unicode, "the paragraph 'Unicode: cafe -- em-dash' lost its UTF-8");

	const ::panduck::Block *value = FindFirst(rows, V::KIND_VALUE, NULL);
	CHECK(value != NULL);
	if (value) {
		CHECK_EQ(Attr(*value, V::ATTR_KEY), std::string("date"));
	}

	// BOTH SIDES: a missing file is NOT_A_ZIP with no rows.
	::panduck::ContainerStatus missing;
	const std::vector<::panduck::Block> none = ::panduck::epub::ReadEpub(FixturePath("no_such_file.epub"), missing);
	CHECK(!missing.Ok());
	CHECK_EQ(static_cast<int>(missing.code), static_cast<int>(::panduck::ContainerStatus::NOT_A_ZIP));
	CHECK(none.empty());
}

} // namespace panduck_test

#endif // PANDUCK_WITH_EPUB
