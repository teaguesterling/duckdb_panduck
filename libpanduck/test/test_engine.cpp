#include "panduck_test.hpp"

#include "panduck/panduck.hpp"
#include "panduck/vocabulary.hpp"

#include <cstring>
#include <string>

// THE ENGINE'S OWN IDENTITY -- SpecVersion() and HasModule() (issue #104).
//
// WHY THIS FILE EXISTS, given that both functions are two lines each: they were the
// only part of libpanduck with NO test at all, and HasModule in particular had been
// wrong twice without anything noticing, because it has no callers.
//
//   - It was missing a `textile` branch, so it answered false for a module that was
//     compiled in. Found by reading, not by failing.
//   - In the EXTENSION build it could not answer at all: libpanduck/src/panduck.cpp
//     was never in EXTENSION_SOURCES, so `nm -C` on the artifact found zero matches
//     for `panduck::HasModule`. A caller would have hit a link error.
//
// A function nobody calls is a function nobody is checking. These tests give it a
// caller, which is the only thing that makes the next drift loud instead of silent.
//
// NOTE ON SCOPE: this covers the STANDALONE build only. The extension-side defect was
// a build-wiring one (fixed in the root CMakeLists), and the extension has no C++ test
// harness to assert through -- its gate is that the defines now appear on panduck.cpp's
// compile line and both objects define the symbols. Verified with nm, not asserted here.

namespace panduck_test {

namespace {

//! BOTH ANSWERS PINNED, which is this project's standing rule for a detector: a check
//! that only ever exercises the side we expect to pass is half-checked.
//!
//! Each module gets an #ifdef/#else pair, so every configure asserts something -- and
//! since #124 both arms are RUN, by two CI jobs rather than one.
//!
//!   - libpanduck-seam configures with no -D flags, so docx, epub, ipynb, odt and zip
//!     are OFF there (each default is conditional on find_package() locating yyjson,
//!     miniz or pugixml, and none of the three is on the runner). That job exercises
//!     their FALSE arms for real.
//!   - libpanduck-seam-all-modules passes all eleven explicitly ON and fails if the
//!     configure disagrees, so it exercises their TRUE arms for real.
//!
//! latex, mediawiki, org, rst, rtf and textile default ON unconditionally, so their
//! TRUE arm runs in both jobs and their FALSE arm runs only under an explicit
//! `-DPANDUCK_WITH_<MODULE>=OFF`. Nothing below is hypothetical, which was not quite
//! true while the all-modules job did not exist: before it, the TRUE arms of the five
//! dependency-gated modules ran on whichever machine last passed the flags by hand.
void CheckModule(const char *name, bool expected) {
	CHECK_MSG(::panduck::HasModule(name) == expected, std::string("HasModule(\"") + name + "\") should be " +
	                                                      (expected ? "true" : "false") + " for this configure");
}

} // namespace

void RunEngineTests() {
	// SpecVersion reports the VENDORED vocabulary's number, so this pins the two
	// together: a vocabulary re-sync that changes SPEC_VERSION without rebuilding, or
	// a SpecVersion() that starts reporting a libpanduck release number instead, both
	// fail here. Compared through Vocab() because binding the constant to a reference
	// odr-uses it and will not link at C++11 -- see panduck_test.hpp.
	const char *spec = ::panduck::SpecVersion();
	CHECK(spec != NULL);
	CHECK(std::strlen(spec) > 0);
	CHECK_EQ(std::string(spec), Vocab(::panduck::DuckBlockVocabulary::SPEC_VERSION));

	// A null name must be answered, not dereferenced.
	CHECK(::panduck::HasModule(NULL) == false);

	// An unknown name is false rather than a crash or a default-true.
	CheckModule("no_such_format", false);

	// The empty string is a name no module has.
	CheckModule("", false);

	// Case matters -- the names are the lowercase module names, and a caller passing
	// a display name should get false rather than an accidental match.
	CheckModule("ORG", false);

	// ORDERED AS HasModule's OWN BRANCHES ARE, so the two lists can be read side by
	// side. The list has to be extended by hand on every module move, and it has
	// already drifted twice -- textile had no branch at all, and docx/epub/odt (#121)
	// and rtf (#118) landed branches with nothing asserting them either way.
#ifdef PANDUCK_WITH_DOCX
	CheckModule("docx", true);
#else
	CheckModule("docx", false);
#endif

#ifdef PANDUCK_WITH_EPUB
	CheckModule("epub", true);
#else
	CheckModule("epub", false);
#endif

#ifdef PANDUCK_WITH_IPYNB
	CheckModule("ipynb", true);
#else
	CheckModule("ipynb", false);
#endif

#ifdef PANDUCK_WITH_LATEX
	CheckModule("latex", true);
#else
	CheckModule("latex", false);
#endif

#ifdef PANDUCK_WITH_MEDIAWIKI
	CheckModule("mediawiki", true);
#else
	CheckModule("mediawiki", false);
#endif

#ifdef PANDUCK_WITH_ODT
	CheckModule("odt", true);
#else
	CheckModule("odt", false);
#endif

#ifdef PANDUCK_WITH_ORG
	CheckModule("org", true);
#else
	CheckModule("org", false);
#endif

#ifdef PANDUCK_WITH_RST
	CheckModule("rst", true);
#else
	CheckModule("rst", false);
#endif

#ifdef PANDUCK_WITH_RTF
	CheckModule("rtf", true);
#else
	CheckModule("rtf", false);
#endif

#ifdef PANDUCK_WITH_TEXTILE
	CheckModule("textile", true);
#else
	CheckModule("textile", false);
#endif

	// "zip" IS NOT A FORMAT and HasModule answers it anyway -- see the comment on its
	// branch in libpanduck/src/panduck.cpp: ZipContainer is the shared capability
	// docx, odt and epub each need, and a caller has no other way to ask whether this
	// build can open a container. Pinned here on the same terms as the formats,
	// because an answer that is only ever wrong and never broken is exactly the kind
	// that drifts.
#ifdef PANDUCK_WITH_ZIP
	CheckModule("zip", true);
#else
	CheckModule("zip", false);
#endif
}

} // namespace panduck_test
