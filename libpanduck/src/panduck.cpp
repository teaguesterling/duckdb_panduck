#include "panduck/panduck.hpp"

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
#ifdef PANDUCK_WITH_IPYNB
	if (std::strcmp(name, "ipynb") == 0) {
		return true;
	}
#endif
	return false;
}

} // namespace panduck
