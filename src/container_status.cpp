#include "container_status.hpp"

namespace duckdb {

void RaiseContainerStatus(const ::panduck::ContainerStatus &status, const char *reader_name, const std::string &path) {
	// EVERY FORMAT STRING BELOW IS CARRIED VERBATIM from where it used to live,
	// and the argument order with it. The whole value of this function is that
	// the messages did not get retyped per reader during the move:
	//
	//   NOT_A_ZIP              src/zip_container.cpp, ZipContainer's constructor
	//   MISSING_MEMBER         src/zip_container.cpp, ZipContainer::ReadRequired
	//   MALFORMED_XML          src/{docx,odt,epub}_reader.cpp -- three throws
	//                          that were already one sentence with the member
	//                          spelled as a literal in docx/odt and as %s in
	//                          epub, so %s here reproduces all three
	//   MANIFEST_NO_ROOT       src/epub_reader.cpp
	//   MANIFEST_ROOT_MISSING  src/epub_reader.cpp (the '%s' quoting is the
	//                          original's, and the line break inside the literal
	//                          is too -- it is a source-formatting artifact that
	//                          concatenates away, not part of the message)
	switch (status.code) {
	case ::panduck::ContainerStatus::NOT_A_ZIP:
		throw IOException("%s: not a readable ZIP archive: %s", reader_name, path);
	case ::panduck::ContainerStatus::MISSING_MEMBER:
		throw InvalidInputException("%s: %s is a ZIP but has no %s, so it is not the expected format", reader_name,
		                            path, status.member);
	case ::panduck::ContainerStatus::MALFORMED_XML:
		throw InvalidInputException("%s: %s is not well-formed XML in %s: %s", reader_name, status.member, path,
		                            status.detail);
	case ::panduck::ContainerStatus::MANIFEST_NO_ROOT:
		throw InvalidInputException("%s: %s has a container.xml with no rootfile full-path", reader_name, path);
	case ::panduck::ContainerStatus::MANIFEST_ROOT_MISSING:
		throw InvalidInputException("%s: %s names a package document "
		                            "'%s' that is not in the archive",
		                            reader_name, path, status.member);
	case ::panduck::ContainerStatus::OK:
	default:
		// An OK status reaching here means a caller tested the wrong thing. Not
		// an IOException: nothing is wrong with the user's file.
		throw InternalException("%s: RaiseContainerStatus called with no failure to raise (%s)", reader_name, path);
	}
}

} // namespace duckdb
