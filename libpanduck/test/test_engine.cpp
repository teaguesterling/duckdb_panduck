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
//! Each module gets an #ifdef/#else pair, so every configure asserts something. That
//! matters most for ipynb, which is OFF in the default configure the CI job runs (it
//! needs a standalone yyjson) -- so CI exercises the FALSE arm for real, while a local
//! `-DPANDUCK_WITH_IPYNB=ON` exercises the TRUE arm. Neither arm is hypothetical.
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

#ifdef PANDUCK_WITH_TEXTILE
	CheckModule("textile", true);
#else
	CheckModule("textile", false);
#endif
}

} // namespace panduck_test
