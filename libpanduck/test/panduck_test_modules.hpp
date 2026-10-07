#pragma once

// One declaration per format module, guarded by the module's own option.
//
// The modules are COMPILE-TIME OPTIONS (libpanduck/CMakeLists.txt: a
// composition may carry fewer modules than the engine supports), so both the
// declaration here and the call in test_main.cpp are #ifdef-guarded. A
// configure with -DPANDUCK_WITH_ORG=OFF must still build this target, and the
// CI job that runs it takes the defaults -- which means ipynb is OFF there,
// since it needs a standalone yyjson.

namespace panduck_test {

#ifdef PANDUCK_WITH_IPYNB
void RunIpynbTests();
#endif

#ifdef PANDUCK_WITH_LATEX
void RunLatexTests();
#endif

#ifdef PANDUCK_WITH_MEDIAWIKI
void RunMediaWikiTests();
#endif

#ifdef PANDUCK_WITH_ORG
void RunOrgTests();
#endif

#ifdef PANDUCK_WITH_RST
void RunRstTests();
#endif

#ifdef PANDUCK_WITH_TEXTILE
void RunTextileTests();
#endif

} // namespace panduck_test
