#include "panduck/zip_container.hpp"

#include <cstring>
#include <miniz.h>

namespace panduck {

struct ZipContainer::Impl {
	mz_zip_archive zip;
	bool open = false;

	~Impl() {
		if (open) {
			mz_zip_reader_end(&zip);
		}
	}
};

// `new` RATHER THAN make_uniq/make_unique: libpanduck is C++11
// (CMAKE_CXX_STANDARD 11), where std::make_unique does not exist yet, and
// make_uniq is DuckDB's.
ZipContainer::ZipContainer(const std::string &path_p) : impl(std::unique_ptr<Impl>(new Impl())), path(path_p) {
	memset(&impl->zip, 0, sizeof(impl->zip));
	if (mz_zip_reader_init_file(&impl->zip, path.c_str(), 0)) {
		impl->open = true;
	}
}

ZipContainer::~ZipContainer() = default;

bool ZipContainer::IsOpen() const {
	return impl->open;
}

const std::string &ZipContainer::Path() const {
	return path;
}

bool ZipContainer::Read(const char *member, std::string &out) {
	// GUARD ON `open`, which the src/ version did not need: there, the
	// constructor threw, so Read was unreachable on a failed open. Here the
	// constructor returns and a caller may not have checked IsOpen(), and
	// mz_zip_reader_locate_file on a zeroed archive is not a defined read.
	if (!impl->open) {
		return false;
	}
	int index = mz_zip_reader_locate_file(&impl->zip, member, nullptr, 0);
	if (index < 0) {
		return false;
	}
	size_t size = 0;
	void *data = mz_zip_reader_extract_to_heap(&impl->zip, static_cast<mz_uint>(index), &size, 0);
	if (!data) {
		return false;
	}
	out.assign(static_cast<char *>(data), size);
	mz_free(data);
	return true;
}

} // namespace panduck
