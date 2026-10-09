#include "zip_container.hpp"

namespace duckdb {

// THE DUCKDB TAIL, AND ALL OF IT: two throws. The miniz reading moved to
// libpanduck/src/zip_container.cpp (issue #104, L2); what could not move is
// the exception TYPES, which are DuckDB API and are SQL-visible as the
// `IO Error:` / `Invalid Input Error:` prefix three tests match on. See
// src/include/zip_container.hpp for why that forced a wrapper rather than a
// `using` shim.
ZipContainer::ZipContainer(const std::string &path, const char *reader_name_p) : zip(path), reader_name(reader_name_p) {
	if (!zip.IsOpen()) {
		throw IOException("%s: not a readable ZIP archive: %s", reader_name, path);
	}
}

bool ZipContainer::Read(const char *member, std::string &out) {
	return zip.Read(member, out);
}

std::string ZipContainer::ReadRequired(const char *member) {
	std::string out;
	if (!Read(member, out)) {
		throw InvalidInputException("%s: %s is a ZIP but has no %s, so it is not the expected format", reader_name,
		                            zip.Path(), member);
	}
	return out;
}

} // namespace duckdb
