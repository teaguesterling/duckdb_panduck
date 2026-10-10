#include "panduck_test.hpp"

// WHAT IS ACTUALLY USED, rather than what might arrive transitively: size_t
// from <cstddef>, uint64_t from <cstdint>, memset from <cstring>, the fixture
// slurp's stream and iterator, std::string.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

// THE FIRST STANDALONE TEST WITH A DATA DEPENDENCY (issue #120, step 1).
//
// Every other file in this suite feeds a string literal to a reader. This one
// cannot: the claim under test is that an ARCHIVE can be opened from bytes
// rather than from a path, and a hand-built ZIP literal would prove the
// ByteSource plumbing against a toy rather than against the real containers
// docx and epub actually ship. So the fixture directory arrives from CMake as
// PANDUCK_TEST_FIXTURE_DIR and this file slurps the file itself.
//
// USING std::ifstream HERE IS NOT A LAYERING VIOLATION, and the distinction is
// worth stating because the whole point of ByteSource is to free the LIBRARY
// from the filesystem. A test is not the library: it is allowed to know where
// its data lives. What matters is that libpanduck/src/ no longer has to.
//
// THE IMPLICATION TO REMEMBER WHEN libpanduck IS EXTRACTED. The fixtures live
// at <repo>/test/fixtures/, OUTSIDE libpanduck/, because they are the
// extension's fixtures and duplicating a .docx to get a second copy byte-exact
// is exactly the trap the vendored vocabulary comment warns about. libpanduck
// is in-tree today so a relative path reaches them; the roadmap contemplates
// extracting it (docs/roadmap.md, L3), and on that day this compile definition
// points outside the project and has to be answered -- by vendoring a small
// fixture set, by a submodule, or by generating the archives at build time.
// Naming it now is cheaper than rediscovering it then.

// GUARDED INTERNALLY AS WELL AS IN CMake, which is this suite's convention: the
// file follows PANDUCK_HAVE_ZIP_CONTAINER in the source list, and the guard
// holds even if someone later adds it unconditionally. The container -- not
// docx or epub the READERS -- is what this exercises, so it is the container's
// availability that gates it; the fixtures are read as archives, never parsed
// as documents.
#ifdef PANDUCK_HAVE_ZIP_CONTAINER

#include "panduck/byte_source.hpp"
#include "panduck/zip_container.hpp"

#ifndef PANDUCK_TEST_FIXTURE_DIR
#error "PANDUCK_TEST_FIXTURE_DIR must be defined; libpanduck/CMakeLists.txt sets it"
#endif

namespace panduck_test {

namespace {

std::string FixturePath(const char *name) {
	return std::string(PANDUCK_TEST_FIXTURE_DIR) + "/" + name;
}

//! The whole file as bytes, or "" when it could not be read. Binary mode is
//! MANDATORY, not hygiene: a text-mode read on a platform that translates
//! newlines would silently rewrite a compressed member and every byte-exact
//! comparison below would be comparing something other than the file.
std::string Slurp(const std::string &path) {
	std::ifstream in(path.c_str(), std::ios::in | std::ios::binary);
	if (!in) {
		return std::string();
	}
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

//! Reads one member through BOTH constructors and compares the bytes.
//!
//! BOTH SIDES ASSERTED, which is this project's standing rule: a check that
//! exercises only the side we expect to pass is half a check. If the new
//! ByteSource path returned nothing, a test that only asserted "no exception"
//! would pass; if the compatibility path regressed, a test that only exercised
//! the new one would not notice. So the expected LENGTH is pinned too -- a
//! measured constant, so that two readers agreeing on zero bytes cannot read as
//! success.
//!
//! `expected_size` was measured with
//!   python3 -c "import zipfile; print(len(zipfile.ZipFile(F).read(M)))"
void CheckMemberMatchesPath(const char *fixture, const char *member, size_t expected_size) {
	const std::string path = FixturePath(fixture);
	const std::string label = std::string(fixture) + ":" + member;

	// --- via the path constructor (the compatibility path) ---
	std::string from_path;
	{
		::panduck::ZipContainer zip(path);
		CHECK_MSG(zip.IsOpen(), label + " -- path constructor could not open the archive");
		CHECK_MSG(zip.Read(member, from_path), label + " -- path constructor could not read the member");
		CHECK_EQ(zip.Path(), path);
	}

	// --- via a MemorySource over the same file's bytes (the new path) ---
	const std::string bytes = Slurp(path);
	CHECK_MSG(!bytes.empty(), label + " -- fixture slurp came back empty; is PANDUCK_TEST_FIXTURE_DIR right?");
	std::string from_memory;
	{
		// `bytes` IS A NAMED LOCAL, not a temporary, because MemorySource refers
		// to the caller's buffer and never copies it (byte_source.hpp). It also
		// outlives `zip`, which keeps reading through the source on every
		// Read().
		::panduck::MemorySource source(bytes, label);
		::panduck::ZipContainer zip(source);
		CHECK_MSG(zip.IsOpen(), label + " -- MemorySource constructor could not open the archive");
		CHECK_MSG(zip.Read(member, from_memory), label + " -- MemorySource constructor could not read the member");
		// Path() falls back to the source's Name(), which is message material
		// and need not be openable.
		CHECK_EQ(zip.Path(), label);
	}

	CHECK_EQ(from_path.size(), expected_size);
	CHECK_EQ(from_memory.size(), expected_size);
	CHECK_MSG(from_memory == from_path, label + " -- the two constructors returned different bytes");
}

//! `Size()` AND `Name()` THROUGH A const REFERENCE, which is a COMPILE-TIME pin
//! masquerading as a runtime one: if either loses its `const`, this function
//! stops compiling. That is the property the const exists for -- a
//! `const ByteSource &` nobody can ask how big it is would be a wart every
//! implementation downstream inherits -- and it is cheaper to pin here than to
//! change the signature once steps 2 and 3 have implemented against it.
//!
//! `Read` is deliberately NOT exercised here: it is non-const on purpose,
//! because a file-backed source moves an OS file position to serve a read. See
//! the note on ByteSource::Size().
void CheckConstObservers(const ::panduck::ByteSource &source, uint64_t expected_size, const char *expected_name) {
	CHECK_EQ(source.Size(), expected_size);
	CHECK_EQ(source.Name(), std::string(expected_name));
}

void CheckMemorySourceMechanics() {
	const std::string bytes = "0123456789";
	::panduck::MemorySource source(bytes, "ten-digits");

	CHECK_EQ(source.Size(), static_cast<uint64_t>(10));
	CHECK_EQ(source.Name(), std::string("ten-digits"));
	CheckConstObservers(source, 10, "ten-digits");

	char buffer[16];
	memset(buffer, 0, sizeof(buffer));
	CHECK_EQ(source.Read(buffer, 4, 0), static_cast<size_t>(4));
	CHECK_EQ(std::string(buffer, 4), std::string("0123"));

	// POSITIONAL, with no cursor: the same source read twice at different
	// offsets must not depend on the order of the calls.
	memset(buffer, 0, sizeof(buffer));
	CHECK_EQ(source.Read(buffer, 3, 7), static_cast<size_t>(3));
	CHECK_EQ(std::string(buffer, 3), std::string("789"));
	memset(buffer, 0, sizeof(buffer));
	CHECK_EQ(source.Read(buffer, 3, 2), static_cast<size_t>(3));
	CHECK_EQ(std::string(buffer, 3), std::string("234"));

	// A SHORT READ IS A REPORT, NOT AN ERROR -- the rule the whole interface
	// hangs on. miniz probes past the end of a truncated archive looking for a
	// central directory, and that probe has to come back empty rather than loud.
	CHECK_EQ(source.Read(buffer, 8, 6), static_cast<size_t>(4));
	CHECK_EQ(source.Read(buffer, 4, 10), static_cast<size_t>(0));
	CHECK_EQ(source.Read(buffer, 4, 9999), static_cast<size_t>(0));
}

void CheckFileSourceMechanics() {
	const std::string path = FixturePath("pandoc.epub");
	::panduck::FileSource source(path);
	CHECK(source.IsOpen());
	CHECK_EQ(source.Name(), path);
	// Measured: `wc -c test/fixtures/pandoc.epub`.
	CHECK_EQ(source.Size(), static_cast<uint64_t>(5066));
	// Both implementations through the const reference, not just MemorySource:
	// FileSource is the one whose Size() could plausibly have wanted a lazy
	// stat, so it is the one worth proving does not.
	CheckConstObservers(source, 5066, path.c_str());

	// THE FIRST FOUR BYTES OF ANY ZIP are the local file header signature
	// "PK\x03\x04". Asserting them rather than just a non-zero count is what
	// makes this a read and not a syscall that returned something.
	char head[4];
	memset(head, 0, sizeof(head));
	CHECK_EQ(source.Read(head, 4, 0), static_cast<size_t>(4));
	CHECK_EQ(std::string(head, 4), std::string("PK\x03\x04", 4));

	// Reading at an offset must not need a preceding read, and the last two
	// bytes of this archive are inside its end-of-central-directory record.
	char tail[2];
	memset(tail, 0, sizeof(tail));
	CHECK_EQ(source.Read(tail, 2, 5064), static_cast<size_t>(2));
	CHECK_EQ(source.Read(tail, 2, 5066), static_cast<size_t>(0));

	// A MISSING FILE REPORTS, it does not throw -- the same bargain
	// ZipContainer::IsOpen() makes, and the reason FileSource has an IsOpen()
	// at all.
	::panduck::FileSource missing(FixturePath("there-is-no-such-fixture.zip"));
	CHECK(!missing.IsOpen());
	CHECK_EQ(missing.Size(), static_cast<uint64_t>(0));
	char ignored[4];
	CHECK_EQ(missing.Read(ignored, 4, 0), static_cast<size_t>(0));
}

void CheckNonArchiveReports() {
	// A ByteSource THAT IS NOT A ZIP must leave IsOpen() false rather than
	// throw, exactly as a non-ZIP path always has. Both arms pinned, so the new
	// constructor's failure behaviour is checked and not assumed.
	const std::string not_a_zip = "this is not an archive, it is a sentence";
	::panduck::MemorySource source(not_a_zip, "<not-a-zip>");
	::panduck::ZipContainer zip(source);
	CHECK(!zip.IsOpen());
	std::string out;
	CHECK(!zip.Read("word/document.xml", out));
	CHECK_EQ(zip.Path(), std::string("<not-a-zip>"));

	// An EMPTY source is the degenerate case the path constructor reaches for a
	// missing file, so it must report the same way.
	::panduck::MemorySource empty_source(std::string(), "<empty>");
	::panduck::ZipContainer empty_zip(empty_source);
	CHECK(!empty_zip.IsOpen());
}

} // namespace

void RunByteSourceTests() {
	CheckMemorySourceMechanics();
	CheckFileSourceMechanics();
	CheckNonArchiveReports();

	// DOCX: the single-member case. word/document.xml is the part every docx
	// fixes by name, and the one src/docx_reader.cpp treats as required.
	CheckMemberMatchesPath("constructs.docx", "word/document.xml", 8503);

	// EPUB: the MULTI-MEMBER case, and the reason the container is deliberately
	// kept open across reads rather than reopened per member. An EPUB fixes only
	// META-INF/container.xml; the .opf and then each spine document are found by
	// following it, so three reads here go through the same still-live
	// MemorySource -- which is the property a per-read reopen would hide.
	CheckMemberMatchesPath("pandoc.epub", "META-INF/container.xml", 252);
	CheckMemberMatchesPath("pandoc.epub", "EPUB/content.opf", 1268);
	CheckMemberMatchesPath("pandoc.epub", "EPUB/text/ch001.xhtml", 817);

	// THREE MEMBERS FROM ONE OPEN CONTAINER, which the per-member helper above
	// does not cover: it opens a fresh container each time. This is the shape
	// epub.cpp actually uses.
	{
		const std::string bytes = Slurp(FixturePath("pandoc.epub"));
		::panduck::MemorySource source(bytes, "pandoc.epub");
		::panduck::ZipContainer zip(source);
		CHECK(zip.IsOpen());
		std::string container_xml, opf, chapter;
		CHECK(zip.Read("META-INF/container.xml", container_xml));
		CHECK(zip.Read("EPUB/content.opf", opf));
		CHECK(zip.Read("EPUB/text/ch001.xhtml", chapter));
		CHECK_EQ(container_xml.size(), static_cast<size_t>(252));
		CHECK_EQ(opf.size(), static_cast<size_t>(1268));
		CHECK_EQ(chapter.size(), static_cast<size_t>(817));
		// A member that is not there is absent, not fatal: the same report an
		// optional part gets.
		std::string absent;
		CHECK(!zip.Read("EPUB/text/there-is-no-ch999.xhtml", absent));
	}
}

} // namespace panduck_test

#endif // PANDUCK_HAVE_ZIP_CONTAINER
