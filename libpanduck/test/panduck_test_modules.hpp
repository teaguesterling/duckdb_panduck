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

// NOT guarded: the engine's identity (SpecVersion, HasModule) is always compiled,
// so this is the one entry point that exists in every configure -- including one
// with every format module switched off, which is exactly the configure where the
// runner's "zero checks is an error" rule would otherwise be the only thing running.
void RunEngineTests();

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
