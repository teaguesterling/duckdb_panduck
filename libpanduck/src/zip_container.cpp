#include "panduck/zip_container.hpp"

// WHAT IS ACTUALLY USED, named here rather than arriving transitively: int32_t and
// int64_t from <cstdint>, std::unique_ptr from <memory> (also reached through the
// header, repeated because this file calls reset()).
#include <cstdint>
#include <memory>

// minizip-ng, the zip library duckdb-excel uses. panduck used miniz until 2026-11-12;
// the two both define `mz_zip_writer_add_file`, so a static link holding panduck and
// excel failed with a duplicate symbol. vcpkg_ports/README.md says why the version is
// pinned to excel's. The `minizip-ng/` prefix is the include layout excel uses too.
#include "minizip-ng/mz.h"
#include "minizip-ng/mz_strm.h"
#include "minizip-ng/mz_zip.h"
#include "minizip-ng/mz_zip_rw.h"

namespace panduck {

namespace {

// A minizip-ng STREAM OVER A ByteSource. minizip-ng reads through a cursor
// (read/tell/seek); ByteSource is positional (byte_source.hpp). So this struct
// keeps the cursor that ByteSource deliberately does not have.
//
// THE `base` MEMBER COMES FIRST and is a plain `mz_stream`, because minizip-ng
// casts the `void *stream` it is given to `mz_stream *` to find the vtable.
//
// NOTHING HERE MAY THROW: a C frame sits between these callbacks and the caller.
// ByteSource::Read promises not to throw (byte_source.hpp); the rest is arithmetic.
struct SourceStream {
	mz_stream base;
	ByteSource *source = nullptr;
	int64_t position = 0;
};

SourceStream &Self(void *stream) {
	return *static_cast<SourceStream *>(stream);
}

// open and close are no-ops: the ByteSource is already open, and its owner closes it.
int32_t SourceOpen(void *, const char *, int32_t) {
	return MZ_OK;
}

int32_t SourceIsOpen(void *stream) {
	return Self(stream).source ? MZ_OK : MZ_STREAM_ERROR;
}

int32_t SourceRead(void *stream, void *buf, int32_t size) {
	auto &self = Self(stream);
	if (!self.source || size < 0) {
		return MZ_STREAM_ERROR;
	}
	size_t got = self.source->Read(buf, static_cast<size_t>(size), static_cast<uint64_t>(self.position));
	self.position += static_cast<int64_t>(got);
	return static_cast<int32_t>(got);
}

int32_t SourceWrite(void *, const void *, int32_t) {
	return MZ_STREAM_ERROR; // read-only
}

int64_t SourceTell(void *stream) {
	return Self(stream).position;
}

int32_t SourceSeek(void *stream, int64_t offset, int32_t origin) {
	auto &self = Self(stream);
	if (!self.source) {
		return MZ_STREAM_ERROR;
	}
	int64_t size = static_cast<int64_t>(self.source->Size());
	int64_t target;
	switch (origin) {
	case MZ_SEEK_SET:
		target = offset;
		break;
	case MZ_SEEK_CUR:
		target = self.position + offset;
		break;
	case MZ_SEEK_END:
		target = size + offset;
		break;
	default:
		return MZ_SEEK_ERROR;
	}
	// A seek outside the source is an error, not a clamp. minizip-ng seeks to find
	// the end-of-central-directory record, and a file too short to hold one must
	// fail the open rather than read from offset 0.
	if (target < 0 || target > size) {
		return MZ_SEEK_ERROR;
	}
	self.position = target;
	return MZ_OK;
}

int32_t SourceClose(void *) {
	return MZ_OK;
}

int32_t SourceError(void *) {
	return MZ_OK;
}

// create and destroy are never called: the stream lives inside Impl, and is passed
// to mz_zip_reader_open by address rather than made by mz_stream_create.
void *SourceCreate(void) {
	return nullptr;
}

void SourceDestroy(void **) {
}

mz_stream_vtbl source_stream_vtbl = {SourceOpen,  SourceIsOpen, SourceRead,   SourceWrite,   SourceTell, SourceSeek,
                                     SourceClose, SourceError,  SourceCreate, SourceDestroy, nullptr,    nullptr};

} // namespace

struct ZipContainer::Impl {
	SourceStream stream;
	void *reader = nullptr;
	bool open = false;

	// THE SOURCE THIS CONTAINER OWNS, which is only the path constructor's
	// FileSource. A container opened from a caller's ByteSource owns nothing:
	// lifetime belongs to whoever constructed the source (byte_source.hpp), and
	// the header says so where a caller will read it.
	std::unique_ptr<FileSource> owned_source;

	~Impl() {
		// ORDER MATTERS. Closing and deleting the reader stops any further call
		// into the stream; only then may the source it points at go away.
		// Releasing the reader here keeps that order explicit instead of resting
		// on member destruction order.
		if (reader) {
			mz_zip_reader_close(reader);
			mz_zip_reader_delete(&reader);
		}
		open = false;
	}
};

// `new` RATHER THAN make_uniq/make_unique: libpanduck is C++11
// (CMAKE_CXX_STANDARD 11), where std::make_unique does not exist yet, and
// make_uniq is DuckDB's.
ZipContainer::ZipContainer(const std::string &path_p) : impl(std::unique_ptr<Impl>(new Impl())), path(path_p) {
	// THE PATH CASE IS THE BYTE SOURCE CASE (issue #120): open a FileSource this
	// container owns and go through the same stream as every other source, so
	// there is one archive code path to keep correct rather than two. A missing
	// file, a directory, or a non-ZIP all leave IsOpen() false.
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
	impl->stream.base.vtbl = &source_stream_vtbl;
	impl->stream.base.base = nullptr;
	impl->stream.source = &source;
	impl->stream.position = 0;
	impl->reader = mz_zip_reader_create();
	if (!impl->reader) {
		return;
	}
	// A ZERO SIZE IS NOT SPECIAL-CASED: the reader rejects an archive too small to
	// hold a central directory, so a missing file (FileSource::Size() == 0) reports
	// exactly what a truncated one does -- IsOpen() false.
	if (mz_zip_reader_open(impl->reader, &impl->stream) == MZ_OK) {
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
	// GUARD ON `open`: the constructor returns rather than throws, and a caller may
	// not have checked IsOpen().
	if (!impl->open) {
		return false;
	}
	// CASE-INSENSITIVE, as miniz's mz_zip_reader_locate_file was with flags 0
	// (MZ_ZIP_FLAG_CASE_SENSITIVE unset). Keeping it means a member spelled
	// `Word/document.xml` by an odd writer still reads, exactly as before.
	if (mz_zip_reader_locate_entry(impl->reader, member, 1) != MZ_OK) {
		return false;
	}
	int32_t length = mz_zip_reader_entry_save_buffer_length(impl->reader);
	if (length < 0) {
		return false;
	}
	std::string buffer(static_cast<size_t>(length), '\0');
	if (length > 0 && mz_zip_reader_entry_save_buffer(impl->reader, &buffer[0], length) != MZ_OK) {
		return false;
	}
	out.swap(buffer);
	return true;
}

} // namespace panduck
