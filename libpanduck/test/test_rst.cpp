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

//! Footnote and citation REFERENCES and BODIES (#85 part 1, shape 2). The
//! reference becomes an inline anchor carrying attributes['id'], and the body a
//! `div` subtree with the same id, so a consumer joins the two on the id.
const char *const kNotes = "Ref [1]_ and cite [CIT2002]_, then [not a ref] and ``[9]_``.\n"
                           "\n"
                           ".. [1] One-line footnote body.\n"
                           "\n"
                           ".. [CIT2002] A citation body that wraps\n"
                           "   onto a second line.\n";

//! AUTO-NUMBERED labels. docutils pairs the N-th `[#]_` with the N-th `.. [#]`,
//! and pandoc 3.1.3 does the same (measured), so the ids must differ per pair.
const char *const kAutoNotes = "First [#]_ and second [#]_.\n"
                               "\n"
                               ".. [#] First auto body.\n"
                               ".. [#] Second auto body.\n";

//! Every row of `kind`/`type` whose attributes['id'] is `id`.
std::vector<const ::panduck::Block *> WithId(const std::vector<::panduck::Block> &rows, const char *kind,
                                             const char *type, const std::string &id) {
	std::vector<const ::panduck::Block *> out;
	for (size_t i = 0; i < rows.size(); i++) {
		if (rows[i].kind == kind && rows[i].element_type == type && Attr(rows[i], "id") == id) {
			out.push_back(&rows[i]);
		}
	}
	return out;
}

//! The text of every row, joined, so a test can say what must NOT survive.
std::string AllText(const std::vector<::panduck::Block> &rows) {
	std::string out;
	for (size_t i = 0; i < rows.size(); i++) {
		out += Content(rows[i]) + "\n";
	}
	return out;
}

void RunRstNoteTests() {
	typedef ::panduck::DuckBlockVocabulary V;
	const std::vector<::panduck::Block> rows = ::panduck::rst::ReadRst(kNotes);
	CheckVocabularyShape("rst notes", rows);

	// The reference: a `note` for a numeric label, a `cite` for any other.
	std::vector<const ::panduck::Block *> ref = WithId(rows, V::KIND_INLINE, V::INLINE_NOTE, "1");
	CHECK_EQ(ref.size(), static_cast<size_t>(1));
	// A footnote anchor carries its BODY TEXT, as docx, odt and mediawiki notes
	// do: it is the one choice that renders as pandoc(1) does (duckeye, measured).
	if (ref.size() == 1) {
		CHECK_EQ(Content(*ref[0]), std::string("One-line footnote body."));
	}
	// A citation anchor carries its LABEL; the body stays visible in its div.
	std::vector<const ::panduck::Block *> cref = WithId(rows, V::KIND_INLINE, V::INLINE_CITE, "CIT2002");
	CHECK_EQ(cref.size(), static_cast<size_t>(1));
	if (cref.size() == 1) {
		CHECK_EQ(Content(*cref[0]), std::string("CIT2002"));
	}

	// The body: one div per label, with the same id, and the body text under it.
	std::vector<const ::panduck::Block *> fn = WithId(rows, V::KIND_BLOCK, V::TYPE_DIV, "1");
	CHECK_EQ(fn.size(), static_cast<size_t>(1));
	if (fn.size() == 1) {
		CHECK_EQ(Attr(*fn[0], V::ATTR_SOURCE_TYPE), std::string("footnote"));
		const ::panduck::Block *body = fn[0] + 1;
		CHECK_EQ(body->element_type, std::string(V::TYPE_PARAGRAPH));
		CHECK_EQ(body->level, fn[0]->level + 1);
		CHECK_EQ(Content(*body), std::string("One-line footnote body."));
	}
	std::vector<const ::panduck::Block *> cit = WithId(rows, V::KIND_BLOCK, V::TYPE_DIV, "CIT2002");
	CHECK_EQ(cit.size(), static_cast<size_t>(1));
	if (cit.size() == 1) {
		CHECK_EQ(Attr(*cit[0], V::ATTR_SOURCE_TYPE), std::string("citation"));
		const ::panduck::Block *body = cit[0] + 1;
		CHECK_EQ(body->level, cit[0]->level + 1);
		// The label line and its continuation are ONE paragraph, as before (#67).
		CHECK_EQ(Content(*body), std::string("A citation body that wraps onto a second line."));
	}

	// The marker syntax is gone. A bracket with no trailing underscore is text,
	// and a marker inside inline code stays code.
	const std::string text = AllText(rows);
	CHECK(text.find("[1]_") == std::string::npos);
	CHECK(text.find("[CIT2002]_") == std::string::npos);
	CHECK(text.find("[not a ref]") != std::string::npos);
	const ::panduck::Block *code = FindFirst(rows, V::KIND_INLINE, V::INLINE_CODE);
	CHECK(code != NULL);
	if (code) {
		CHECK_EQ(Content(*code), std::string("[9]_"));
	}

	// Auto-numbered: two distinct pairs, joined in document order.
	const std::vector<::panduck::Block> autos = ::panduck::rst::ReadRst(kAutoNotes);
	CHECK_EQ(WithId(autos, V::KIND_INLINE, V::INLINE_NOTE, "#1").size(), static_cast<size_t>(1));
	std::vector<const ::panduck::Block *> ref2 = WithId(autos, V::KIND_INLINE, V::INLINE_NOTE, "#2");
	CHECK_EQ(ref2.size(), static_cast<size_t>(1));
	if (ref2.size() == 1) {
		CHECK_EQ(Content(*ref2[0]), std::string("Second auto body."));
	}
	std::vector<const ::panduck::Block *> second = WithId(autos, V::KIND_BLOCK, V::TYPE_DIV, "#2");
	CHECK_EQ(second.size(), static_cast<size_t>(1));
	if (second.size() == 1) {
		CHECK_EQ(Content(*(second[0] + 1)), std::string("Second auto body."));
	}
}

} // namespace

void RunRstTests() {
	RunRstNoteTests();
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
	const std::vector<::panduck::Block> empty = ::panduck::rst::ReadRst("");
	CHECK_EQ(empty.size(), static_cast<size_t>(0));

	const std::vector<::panduck::Block> blank = ::panduck::rst::ReadRst("\n\n");
	CHECK_EQ(blank.size(), static_cast<size_t>(0));
}

} // namespace panduck_test

#endif // PANDUCK_WITH_RST
