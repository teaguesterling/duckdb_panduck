#ifdef PANDUCK_WITH_RTF

#include "panduck/rtf.hpp"
#include "panduck/vocabulary.hpp"
#include "panduck_test.hpp"
#include "panduck_test_modules.hpp"

#include <cstddef>
#include <string>
#include <vector>

// rtf's FIRST STANDALONE ASSERTIONS (#129). Until this file, every assertion about ReadRtf
// ran through SQL (test/sql/rtf_reader.test), so through DuckDB.
//
// RTF IS THE ONE MODULE HERE THAT TAKES TEXT, NOT A PATH, so two inputs are used: a
// literal, for the control-word groups that make RTF what it is, and the fixture
// test/fixtures/rich.rtf, which test/sql also reads.

namespace panduck_test {
namespace {

//! A heading by outline level, then a paragraph whose middle is a {\b ...} GROUP. The
//! group is the RTF construct to assert: its formatting must end at the closing brace,
//! so the words after it are plain again.
const char *const kSource = "{\\rtf1\\ansi\\deff0{\\fonttbl{\\f0 Times;}}\n"
                            "{\\pard\\outlinelevel0 Title Here\\par}\n"
                            "{\\pard Plain {\\b bold words} and plain again.\\par}\n"
                            "}\n";

} // namespace

void RunRtfTests() {
	typedef ::panduck::DuckBlockVocabulary V;

	const std::vector<::panduck::Block> rows = ::panduck::rtf::ReadRtf(kSource);
	CHECK(!rows.empty());
	CheckVocabularyShape("rtf literal", rows);

	const ::panduck::Block *heading = FindFirst(rows, V::KIND_BLOCK, V::TYPE_HEADING);
	CHECK(heading != NULL);
	if (heading) {
		CHECK_EQ(Content(*heading), std::string("Title Here"));
		CHECK_EQ(Attr(*heading, V::ATTR_HEADING_LEVEL), std::string("1"));
	}

	// The control words never reach the text: no backslash, no brace, no font table.
	bool leaked = false;
	for (size_t i = 0; i < rows.size(); i++) {
		const std::string c = Content(rows[i]);
		if (c.find('\\') != std::string::npos || c.find('{') != std::string::npos ||
		    c.find("Times") != std::string::npos) {
			leaked = true;
		}
	}
	CHECK_MSG(!leaked, "an RTF control word or the font table reached block content");

	// The fixture: the same file test/sql/rtf_reader.test reads. Measured through
	// read_panduck_doc on 2026-11-10: heading, table, image, two paragraphs.
	const std::string rich = Slurp(FixturePath("rich.rtf"));
	CHECK_MSG(!rich.empty(), "rich.rtf could not be read; is PANDUCK_TEST_FIXTURE_DIR right?");
	const std::vector<::panduck::Block> fixture = ::panduck::rtf::ReadRtf(rich);
	CHECK_EQ(fixture.size(), static_cast<size_t>(5));
	CheckVocabularyShape("rtf rich.rtf", fixture);
	if (fixture.size() == 5) {
		CHECK_EQ(Content(fixture[0]), std::string("Rich Document"));
		CHECK_EQ(fixture[1].element_type, Vocab(V::TYPE_TABLE));
		CHECK_EQ(fixture[1].encoding, Vocab(V::ENCODING_JSON));
		CHECK_EQ(fixture[2].element_type, Vocab(V::TYPE_IMAGE));
		CHECK_EQ(Content(fixture[4]), std::string("Text with a footnote."));
	}
}

} // namespace panduck_test

#endif // PANDUCK_WITH_RTF
