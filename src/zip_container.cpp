#include "zip_container.hpp"

#include <cstring>
#include <miniz.h>

namespace duckdb {

struct ZipContainer::Impl {
	mz_zip_archive zip;
	bool open = false;

	~Impl() {
		if (open) {
			mz_zip_reader_end(&zip);
		}
	}
};

ZipContainer::ZipContainer(const std::string &path_p, const char *reader_name_p)
    : impl(make_uniq<Impl>()), path(path_p), reader_name(reader_name_p) {
	memset(&impl->zip, 0, sizeof(impl->zip));
	if (!mz_zip_reader_init_file(&impl->zip, path.c_str(), 0)) {
		throw IOException("%s: not a readable ZIP archive: %s", reader_name, path);
	}
	impl->open = true;
}

ZipContainer::~ZipContainer() = default;

bool ZipContainer::Read(const char *member, std::string &out) {
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

std::string ZipContainer::ReadRequired(const char *member) {
	std::string out;
	if (!Read(member, out)) {
		throw InvalidInputException("%s: %s is a ZIP but has no %s, so it is not the expected format", reader_name,
		                            path, member);
	}
	return out;
}

} // namespace duckdb
