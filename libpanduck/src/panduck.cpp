#include "panduck/panduck.hpp"

// HEADER-ONLY PIECES OF THE ENGINE ARE INCLUDED HERE ON PURPOSE, even though
// this file does not need all of them.
//
// A header nothing compiles is a header nobody has proved portable. Before
// this, slugify.hpp and block_json.hpp lived under libpanduck/ and were reached
// only through the shims in src/include/, i.e. compiled solely by the EXTENSION
// build, with duckdb on the include path. The only thing asserting they were
// DuckDB-free was the seam script's regex scan -- which cannot catch a type that
// arrives transitively.
//
// Including them in the one translation unit the standalone build always
// compiles turns that claim into a compile. If any of these ever reaches for a
// DuckDB type, `cmake -S libpanduck` fails, which is the whole point of the
// standalone project existing.
//
// Flagged by the subagent that performed the move; it was right.
#include "panduck/block.hpp"
#include "panduck/block_json.hpp"
#include "panduck/doc_metadata.hpp"
#include "panduck/slugify.hpp"
#include "panduck/vocabulary.hpp"

#include <cstring>

namespace panduck {

const char *SpecVersion() {
	return DuckBlockVocabulary::SPEC_VERSION;
}

bool HasModule(const char *name) {
	if (name == nullptr) {
		return false;
	}
	// ONE GUARDED BRANCH PER MODULE, and the list has to be extended by hand with
	// every move -- textile went behind the seam without one, so HasModule
	// answered false for a module that was compiled in. Nothing fails when this
	// drifts, which is exactly why it drifts: the answer is only wrong, never
	// broken. Adding textile's missing branch here alongside org's.
#ifdef PANDUCK_WITH_IPYNB
	if (std::strcmp(name, "ipynb") == 0) {
		return true;
	}
#endif
#ifdef PANDUCK_WITH_LATEX
	if (std::strcmp(name, "latex") == 0) {
		return true;
	}
#endif
#ifdef PANDUCK_WITH_MEDIAWIKI
	if (std::strcmp(name, "mediawiki") == 0) {
		return true;
	}
#endif
#ifdef PANDUCK_WITH_ORG
	if (std::strcmp(name, "org") == 0) {
		return true;
	}
#endif
#ifdef PANDUCK_WITH_RST
	if (std::strcmp(name, "rst") == 0) {
		return true;
	}
#endif
#ifdef PANDUCK_WITH_TEXTILE
	if (std::strcmp(name, "textile") == 0) {
		return true;
	}
#endif
	// "zip" IS NOT A FORMAT, and is answered here anyway. ZipContainer is shared
	// infrastructure that docx, odt and epub each need, and a caller asking
	// whether this build can open a container has no other way to find out --
	// the three readers themselves are still in src/, so there is no "docx"
	// module to ask about yet. Answering about the capability is more useful
	// than refusing because it has no reader.
#ifdef PANDUCK_WITH_ZIP
	if (std::strcmp(name, "zip") == 0) {
		return true;
	}
#endif
	return false;
}

} // namespace panduck
