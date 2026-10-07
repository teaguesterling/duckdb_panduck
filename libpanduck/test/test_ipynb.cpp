#ifdef PANDUCK_WITH_IPYNB

#include "panduck/ipynb.hpp"
#include "panduck/vocabulary.hpp"
#include "panduck_test.hpp"
#include "panduck_test_modules.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace panduck_test {
namespace {

//! A markdown cell, a code cell and notebook metadata. ipynb is the module the
//! encoding trap was FIRST caught in, and the one whose rows are least
//! prose-shaped -- a div per cell with the payload nested under it at level 2.
const char *const kSource = "{\"nbformat\":4,\"nbformat_minor\":5,"
                            "\"metadata\":{\"language_info\":{\"name\":\"python\"}},"
                            "\"cells\":["
                            "{\"cell_type\":\"markdown\",\"source\":[\"# Heading\\n\"]},"
                            "{\"cell_type\":\"code\",\"execution_count\":1,"
                            "\"source\":[\"x=1\\n\"],\"outputs\":[]}"
                            "]}";

} // namespace

void RunIpynbTests() {
	typedef ::panduck::DuckBlockVocabulary V;

	const std::vector<::panduck::Block> rows = ::panduck::ipynb::ReadIpynb(kSource);

	CHECK(!rows.empty());
	CHECK_EQ(rows.size(), static_cast<size_t>(6));

	CheckVocabularyShape("ipynb", rows);

	// A cell becomes a div carrying the cell kind, with its payload nested.
	const ::panduck::Block *div = FindFirst(rows, V::KIND_BLOCK, V::TYPE_DIV);
	CHECK(div != NULL);
	if (div) {
		CHECK_EQ(Attr(*div, V::ATTR_SOURCE_TYPE), std::string("markdown"));
		CHECK_EQ(div->level, 1);
		CHECK_EQ(div->encoding, Vocab(V::ENCODING_TEXT));
	}

	// A markdown cell's body is held RAW -- panduck does not re-parse an
	// embedded format inline, it records the format in attributes['format'] and
	// leaves post-parsing to the caller.
	const ::panduck::Block *raw = FindFirst(rows, V::KIND_BLOCK, V::TYPE_RAW);
	CHECK(raw != NULL);
	if (raw) {
		CHECK_EQ(Content(*raw), std::string("# Heading"));
		CHECK_EQ(Attr(*raw, "format"), std::string("markdown"));
		CHECK_EQ(raw->level, 2);
	}

	const ::panduck::Block *code = FindFirst(rows, V::KIND_BLOCK, V::TYPE_CODE);
	CHECK(code != NULL);
	if (code) {
		CHECK_EQ(Content(*code), std::string("x=1"));
		CHECK_EQ(code->level, 2);
	}

	// Notebook metadata arrives as kind='value' rows with inline children.
	const ::panduck::Block *meta = FindFirst(rows, V::KIND_VALUE, V::VALUE_INLINES);
	CHECK(meta != NULL);
	if (meta) {
		CHECK_EQ(Attr(*meta, V::ATTR_KEY), std::string("kernel"));
	}

	// Malformed JSON yields no blocks rather than failing -- the documented
	// contract in libpanduck/include/panduck/ipynb.hpp. The DuckDB-free path
	// must honour it too, since there is no query to fail here.
	const std::vector<::panduck::Block> broken = ::panduck::ipynb::ReadIpynb("{not json");
	CHECK_EQ(broken.size(), static_cast<size_t>(0));

	// OBSERVED behaviour on empty input -- diverges from the ruling in
	// libpanduck/include/panduck/block.hpp; see test_org.cpp for the note.
	const std::vector<::panduck::Block> empty = ::panduck::ipynb::ReadIpynb("");
	CHECK_EQ(empty.size(), static_cast<size_t>(0));

	const std::vector<::panduck::Block> no_cells = ::panduck::ipynb::ReadIpynb("{\"cells\":[],\"metadata\":{}}");
	CHECK_EQ(no_cells.size(), static_cast<size_t>(0));
}

} // namespace panduck_test

#endif // PANDUCK_WITH_IPYNB
