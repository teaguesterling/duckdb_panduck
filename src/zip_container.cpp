#include "zip_container.hpp"

#include "container_status.hpp"

namespace duckdb {

// THE DUCKDB TAIL, AND ALL OF IT: two throws. The miniz reading moved to
// libpanduck/src/zip_container.cpp (issue #104, L2); what could not move is
// the exception TYPES, which are DuckDB API and are SQL-visible as the
// `IO Error:` / `Invalid Input Error:` prefix three tests match on. See
// src/include/zip_container.hpp for why that forced a wrapper rather than a
// `using` shim.
//
// THE TWO THROWS NOW DELEGATE to RaiseContainerStatus (src/container_status.cpp)
// rather than spelling their own format strings. docx, odt and epub moved behind
// the seam and use ::panduck::ZipContainer directly, so their failures arrive as
// a ContainerStatus and are raised there -- and the same two messages being
// built in two places is exactly how they would drift apart. One copy each,
// used by this class and by the three readers.
ZipContainer::ZipContainer(const std::string &path, const char *reader_name_p) : zip(path), reader_name(reader_name_p) {
	if (!zip.IsOpen()) {
		RaiseContainerStatus(::panduck::ContainerStatus::NotAZip(), reader_name.c_str(), path);
	}
}

bool ZipContainer::Read(const char *member, std::string &out) {
	return zip.Read(member, out);
}

std::string ZipContainer::ReadRequired(const char *member) {
	std::string out;
	if (!Read(member, out)) {
		RaiseContainerStatus(::panduck::ContainerStatus::MissingMember(member), reader_name.c_str(), zip.Path());
	}
	return out;
}

} // namespace duckdb
