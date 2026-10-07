#include "panduck_test.hpp"
#include "panduck_test_modules.hpp"

#include <cstdio>

// THE RUNNER, and the gap it closes (issue #104, L2).
//
// Before this target existed the seam had two guards and a hole:
//
//   scripts/check_libpanduck_seam.py  -- a grep. Proves no DuckDB NAME appears.
//   cmake -S libpanduck               -- a compile. Proves it BUILDS with no
//                                        duckdb on the include path.
//   (nothing)                         -- proves the modules WORK without DuckDB.
//
// Every assertion about ReadOrg, ReadRst, ReadLatex, ReadMediaWiki, ReadTextile
// and ReadIpynb behaving correctly ran through SQL in test/sql/*.test -- i.e.
// through DuckDB. A consumer who links libpanduck and calls ReadOrg("* H") was
// exercising a path no test covered. "The engine compiles without DuckDB" is a
// much weaker claim than "the engine produces correct vocabulary rows without
// DuckDB", and only the second one is what L3 would ship.
//
// This is NOT a second conformance suite. test/sql/*.test remains the
// behavioural authority; this is a smoke test with teeth, ~10-14 checks per
// module, aimed at the vocabulary shape and at the encoding trap.

namespace {

struct ModuleTally {
	const char *name;
	int checks;
	int failures;
};

typedef void (*TestEntry)();

ModuleTally gTallies[8];
int gTallyCount = 0;

void RunModule(const char *name, TestEntry entry) {
	const int checks_before = ::panduck_test::checks;
	const int failures_before = ::panduck_test::failures;
	entry();
	ModuleTally tally;
	tally.name = name;
	tally.checks = ::panduck_test::checks - checks_before;
	tally.failures = ::panduck_test::failures - failures_before;
	gTallies[gTallyCount++] = tally;
}

} // namespace

int main() {
#ifdef PANDUCK_WITH_IPYNB
	RunModule("ipynb", &::panduck_test::RunIpynbTests);
#endif
#ifdef PANDUCK_WITH_LATEX
	RunModule("latex", &::panduck_test::RunLatexTests);
#endif
#ifdef PANDUCK_WITH_MEDIAWIKI
	RunModule("mediawiki", &::panduck_test::RunMediaWikiTests);
#endif
#ifdef PANDUCK_WITH_ORG
	RunModule("org", &::panduck_test::RunOrgTests);
#endif
#ifdef PANDUCK_WITH_RST
	RunModule("rst", &::panduck_test::RunRstTests);
#endif
#ifdef PANDUCK_WITH_TEXTILE
	RunModule("textile", &::panduck_test::RunTextileTests);
#endif

	printf("\nlibpanduck standalone tests\n");
	for (int i = 0; i < gTallyCount; ++i) {
		printf("  %-12s %4d checks, %d failures\n", gTallies[i].name, gTallies[i].checks, gTallies[i].failures);
	}
	printf("  %-12s %4d checks, %d failures (%d module(s))\n", "TOTAL", ::panduck_test::checks,
	       ::panduck_test::failures, gTallyCount);

	// A RUNNER THAT RAN NOTHING MUST NOT REPORT SUCCESS. Six times in this
	// project a gate printed a green verdict for work that had not happened --
	// a stale binary, a build that failed after the artifact was cited. Zero
	// checks is that same shape, so it is an error here, not a pass.
	if (::panduck_test::checks == 0) {
		fprintf(stderr, "ERROR: no checks ran -- every format module is off, or no module registered a test\n");
		return 2;
	}
	return ::panduck_test::failures == 0 ? 0 : 1;
}
