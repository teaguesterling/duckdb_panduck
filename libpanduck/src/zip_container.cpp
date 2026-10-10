#include "panduck/zip_container.hpp"

// WHAT IS ACTUALLY USED, named here rather than arriving transitively: memset
// from <cstring>, uint64_t from <cstdint>, std::unique_ptr from <memory> (also
// reached through the header, repeated because this file calls reset()).
#include <cstdint>
#include <cstring>
#include <memory>

#include <miniz.h>

namespace panduck {

namespace {

// THE miniz READ CALLBACK, and the whole reason ByteSource is positional rather
// than stream-shaped: this signature is already (opaque, offset, buffer, count),
// so the adapter is a cast and a forwarded call with no cursor to reconcile.
//
// `extern "C"` IS DELIBERATELY NOT WRITTEN HERE. miniz declares
// mz_file_read_func with C++ linkage when compiled as C++ (its header is inside
// the same extern "C" block the rest of the API is), and on every compiler this
// project builds with, a function pointer's language linkage is not part of its
// type -- adding it would be noise on some toolchains and an error on others.
// What matters is the other half of the contract: a C callback frame sits
// between this and the caller, so NOTHING HERE MAY THROW. ByteSource::Read
// promises that (see byte_source.hpp); this function adds no code that could
// break the promise.
size_t ZipReadCallback(void *opaque, mz_uint64 file_ofs, void *buffer, size_t n) {
	::panduck::ByteSource *source = static_cast<::panduck::ByteSource *>(opaque);
	if (!source) {
		return 0;
	}
	return source->Read(buffer, n, static_cast<uint64_t>(file_ofs));
}

} // namespace

struct ZipContainer::Impl {
	mz_zip_archive zip;
	bool open = false;

	// THE SOURCE THIS CONTAINER OWNS, which is only the path constructor's
	// FileSource. A container opened from a caller's ByteSource owns nothing:
	// lifetime belongs to whoever constructed the source (byte_source.hpp), and
	// the header says so where a caller will read it.
	std::unique_ptr<FileSource> owned_source;

	~Impl() {
		// ORDER MATTERS. mz_zip_reader_end releases miniz's state, which stops
		// any further call into the read callback; only then may the source it
		// points at go away. Declaring owned_source after `zip` and ending the
		// reader here keeps that order explicit instead of resting on member
		// destruction order.
		if (open) {
			mz_zip_reader_end(&zip);
			open = false;
		}
	}
};

// `new` RATHER THAN make_uniq/make_unique: libpanduck is C++11
// (CMAKE_CXX_STANDARD 11), where std::make_unique does not exist yet, and
// make_uniq is DuckDB's.
ZipContainer::ZipContainer(const std::string &path_p) : impl(std::unique_ptr<Impl>(new Impl())), path(path_p) {
	// THE PATH CASE IS NOW THE BYTE SOURCE CASE (issue #120). It used to call
	// mz_zip_reader_init_file; it now opens a FileSource it owns and goes
	// through the same callback as every other source, so there is one archive
	// code path to keep correct rather than two. The OBSERVABLE behaviour is
	// unchanged -- a missing file, a directory, or a non-ZIP all still leave
	// IsOpen() false -- which is why no caller under src/ or in libpanduck/src/
	// needed editing.
	impl->owned_source.reset(new FileSource(path));
	Open(*impl->owned_source);
}

ZipContainer::ZipContainer(ByteSource &source) : impl(std::unique_ptr<Impl>(new Impl())), path(source.Name()) {
	// `path` IS COPIED rather than referenced through the source, so Path()
	// keeps returning a stable `const std::string &` for both constructors and
	// a caller's error message does not depend on the source still being alive.
	Open(source);
}

void ZipContainer::Open(ByteSource &source) {
	memset(&impl->zip, 0, sizeof(impl->zip));
	// m_pRead AND m_pIO_opaque ARE SET BEFORE init, which is miniz's contract
	// for mz_zip_reader_init: that function reads the central directory through
	// the callback, so a callback installed afterwards would never be consulted.
	// mz_zip_reader_init_file is only the convenience wrapper that installs its
	// own stdio callback first.
	impl->zip.m_pRead = &ZipReadCallback;
	impl->zip.m_pIO_opaque = &source;
	// A ZERO SIZE IS NOT SPECIAL-CASED: mz_zip_reader_init rejects an archive too
	// small to hold a central directory, so a missing file (FileSource::Size()
	// == 0) reports exactly what a truncated one does -- IsOpen() false.
	if (mz_zip_reader_init(&impl->zip, source.Size(), 0)) {
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
