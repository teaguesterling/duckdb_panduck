#pragma once

// TRANSITIVE INCLUDES ARE NOT A DEPENDENCY. Code moved behind this seam has
// twice compiled only because a header arrived through duckdb.hpp, so every
// name used below is included here explicitly: fopen/fread/fseek from <cstdio>,
// memcpy from <cstring>, the fixed-width integers from <cstdint>, std::string
// from <string>.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#if !defined(_WIN32)
// off_t, for the POSIX large-file seek used by FileSource below. <cstdio>
// declares fseeko/ftello but the TYPE comes from here.
#include <sys/types.h>
#endif

namespace panduck {

//! A seekable source of bytes, with no filesystem and no DuckDB behind it.
//!
//! WHY POSITIONAL AND NOT STREAM-SHAPED. This is not a style preference; it is
//! the shape both endpoints already speak, so the interface adds no state of
//! its own to keep in sync:
//!
//!     miniz:  typedef size_t (*mz_file_read_func)(void *pOpaque, mz_uint64 file_ofs,
//!                                                void *pBuf, size_t n);
//!     duckdb: int64_t FileHandle::Read(void *buffer, int64_t nr_bytes, idx_t location);
//!
//! miniz's `mz_zip_reader_init(zip, size, flags)` with `m_pRead` set IS the
//! callback entry point -- `mz_zip_reader_init_file` is only its convenience
//! wrapper -- and the host engine's file handle takes an explicit location too.
//! A `Seek`/`Tell` pair would invent a THIRD convention sitting between two that
//! already agree, and every implementation would then have to keep its cursor
//! consistent with a reader that does not use one. So: one size, one positional
//! read, one name. Do not add a Seek, and do not add an Open or a Close --
//! lifetime belongs to whoever constructed the source.
//!
//! THIS INTERFACE MUST NOT THROW. `ZipContainer` REPORTS failure instead of
//! throwing, for the reasons its own header documents at length: the exception
//! types the readers used to raise are DuckDB API types, and the type IS the
//! SQL-visible `IO Error:` / `Invalid Input Error:` prefix that the
//! malformed-fixture tests match on. A source that throws would cross a C
//! callback frame inside miniz, which leaks miniz's internal state, and would
//! surface as whichever exception the host wraps a foreign `std::exception` in.
//! So an implementation catches at its own boundary and reports:
//!
//!   - `Read` returning FEWER bytes than asked is a REPORT, not an error. It is
//!     normal at end of file, and conflating it with failure would change the
//!     behaviour of every truncated-fixture test.
//!   - `Read` returning 0 means nothing could be read at that offset.
//!   - `Size` returning 0 for an unopened or unreadable source is likewise a
//!     report; `mz_zip_reader_init` with a size of 0 simply fails to open.
//!
//! DELIBERATELY NOT SPELLED `noexcept`, and the reason is worth recording. The
//! stronger declaration would make a leaked exception call std::terminate --
//! that is, it would trade a reportable `IO Error` for a dead session, in a
//! class whose whole header is about not disturbing SQL-visible error
//! behaviour. The contract above is the requirement; terminate() is not the
//! enforcement mechanism we want for it.
class ByteSource {
public:
	virtual ~ByteSource() {
	}

	//! Total bytes available, or 0 when the source is not readable.
	//!
	//! CONST, AND `Read` IS NOT -- the asymmetry is deliberate rather than an
	//! oversight. Asking a source how big it is observes it and nothing more, so
	//! a `const ByteSource &` that could not answer would be a wart every
	//! implementation downstream inherited. Reading is a different matter: a
	//! file-backed source moves an OS file position to serve a positional read,
	//! which is real mutation of real state even though no data member changes.
	//! Spelling that honestly here keeps `mutable` out of the implementations.
	virtual uint64_t Size() const = 0;

	//! Reads up to `nr_bytes` into `buffer` starting at `offset`. Returns the
	//! number of bytes actually copied -- see the short-read rule above. Must
	//! not throw.
	virtual size_t Read(void *buffer, size_t nr_bytes, uint64_t offset) = 0;

	//! A label for an error message -- a path, a URL, or a placeholder for bytes
	//! that never had a name. Not a handle and not required to be openable: the
	//! only caller is a message, so `ZipContainer::Path()` keeps working when a
	//! container is opened from memory.
	virtual const std::string &Name() const = 0;

protected:
	ByteSource() {
	}

private:
	//! Copy and assignment are suppressed on the base because an implementation
	//! may own a FILE * (see FileSource); copying one would double-close it.
	ByteSource(const ByteSource &) = delete;
	ByteSource &operator=(const ByteSource &) = delete;
};

//! A ByteSource over bytes already in hand.
//!
//! LIFETIME RULE: MemorySource REFERS to the caller's buffer and NEVER COPIES
//! IT. The bytes must outlive the MemorySource, and the MemorySource must
//! outlive anything built on it (a ZipContainer keeps the source alive only by
//! reference -- see zip_container.hpp).
//!
//! This is a decision, not an accident, and both planned callers want it:
//!
//!   - a table function already holding a document as a value or a string has a
//!     buffer with a longer life than the read, so a copy would double the
//!     memory of every archive read for nothing;
//!   - the standalone tests slurp a fixture into a local std::string and then
//!     open it, which is exactly this lifetime.
//!
//! The cost is that constructing from a TEMPORARY dangles:
//!
//!     MemorySource bad(ReadWholeFile(path));   // WRONG: buffer dies here
//!     std::string bytes = ReadWholeFile(path);
//!     MemorySource good(bytes);                // right
//!
//! A copying variant would be a different class with a different name, and
//! nothing needs one yet.
class MemorySource : public ByteSource {
public:
	//! `data` must remain valid and unmodified for this object's lifetime.
	MemorySource(const char *data, size_t size, const std::string &name = std::string("<memory>"))
	    : data(data), size(size), name(name) {
	}

	//! Binds to `bytes`' buffer. `bytes` must outlive this object -- see the
	//! lifetime rule above; do not pass a temporary.
	explicit MemorySource(const std::string &bytes, const std::string &name = std::string("<memory>"))
	    : data(bytes.data()), size(bytes.size()), name(name) {
	}

	uint64_t Size() const {
		return static_cast<uint64_t>(size);
	}

	size_t Read(void *buffer, size_t nr_bytes, uint64_t offset) {
		if (!data || offset >= static_cast<uint64_t>(size)) {
			// PAST THE END IS A SHORT READ OF ZERO, not a failure. miniz probes
			// beyond the end of a truncated archive while hunting for the
			// central directory, and that probe has to come back empty rather
			// than loud.
			return 0;
		}
		size_t available = size - static_cast<size_t>(offset);
		size_t n = nr_bytes < available ? nr_bytes : available;
		if (n > 0) {
			memcpy(buffer, data + static_cast<size_t>(offset), n);
		}
		return n;
	}

	const std::string &Name() const {
		return name;
	}

private:
	const char *data;
	size_t size;
	std::string name;
};

//! A ByteSource over a stdio file, for a consumer with no host filesystem.
//!
//! This is what the standalone library gets for free, and it is also what the
//! existing `ZipContainer(path)` constructor is implemented over, so the
//! path-based behaviour the readers have always had is one code path rather than
//! two.
//!
//! 64-BIT OFFSETS, EXPLICITLY. `fseek` takes a `long`, which is 32 bits on
//! Windows and on any 32-bit target, so an archive past 2 GB would seek to a
//! truncated offset and read the wrong bytes -- silently. The large-file seek is
//! selected below per platform. On a 32-bit Unix build this additionally needs
//! `_FILE_OFFSET_BITS=64` for `off_t` itself to be 64 bits; that is a property
//! of the composition's compile flags, not something a header can fix, and it is
//! recorded here so the next person does not have to rediscover it.
class FileSource : public ByteSource {
public:
	//! Opens `path` for reading. NEVER THROWS: ask `IsOpen()`.
	explicit FileSource(const std::string &path) : handle(0), size(0), name(path) {
		handle = fopen(path.c_str(), "rb");
		if (!handle) {
			return;
		}
		if (Seek(handle, 0, SEEK_END) != 0) {
			fclose(handle);
			handle = 0;
			return;
		}
		int64_t end = Tell(handle);
		if (end < 0) {
			fclose(handle);
			handle = 0;
			return;
		}
		size = static_cast<uint64_t>(end);
	}

	~FileSource() {
		if (handle) {
			fclose(handle);
		}
	}

	//! False when the path is missing or unreadable. Every `Read` then returns
	//! 0, so a caller that ignores this sees an empty source rather than
	//! undefined behaviour -- the same bargain ZipContainer::IsOpen() makes.
	bool IsOpen() const {
		return handle != 0;
	}

	uint64_t Size() const {
		return size;
	}

	size_t Read(void *buffer, size_t nr_bytes, uint64_t offset) {
		if (!handle || nr_bytes == 0) {
			return 0;
		}
		if (Seek(handle, static_cast<int64_t>(offset), SEEK_SET) != 0) {
			return 0;
		}
		// A SHORT fread IS NOT CHECKED AGAINST ferror HERE on purpose: the
		// contract is "bytes actually read", and both end of file and a read
		// error report the same way to the caller, which then fails to parse an
		// archive it could not read. Distinguishing them would need an error
		// channel this interface deliberately does not have.
		return fread(buffer, 1, nr_bytes, handle);
	}

	const std::string &Name() const {
		return name;
	}

private:
	//! The large-file seek/tell pair, chosen per platform. `fseeko`/`ftello` are
	//! POSIX; MSVC spells them `_fseeki64`/`_ftelli64`.
	//! The PLAIN-fseek fallback is the third branch and not an oversight:
	//! fseeko/ftello are POSIX, not ISO C, so a libc that exposes only the
	//! standard set hides them. There, plain fseek is used -- correct wherever
	//! `long` is 64 bits, which is every LP64 target, and limited to 2 GB where
	//! it is not. Noted rather than static_asserted: a header has no business
	//! failing a 32-bit build that will never see a 2 GB archive.
	//!
	//! MEASURED, because the condition is easy to get wrong by reading: glibc
	//! defines _DEFAULT_SOURCE and _POSIX_C_SOURCE even under `-std=c++11`, so
	//! `__STRICT_ANSI__` alone does NOT hide fseeko there and this project's
	//! builds take the POSIX branch in both dialects. PANDUCK_NO_LARGE_FILE_SEEK
	//! exists so the fallback can be compiled and run on purpose rather than
	//! waiting for a platform to discover it -- an unexercised preprocessor
	//! branch is the same half-check this suite refuses everywhere else.
	static int Seek(FILE *f, int64_t offset, int origin) {
#if defined(_WIN32)
		return _fseeki64(f, offset, origin);
#elif defined(PANDUCK_NO_LARGE_FILE_SEEK) ||                                                                           \
    (defined(__STRICT_ANSI__) && !defined(_POSIX_C_SOURCE) && !defined(_DEFAULT_SOURCE))
		return fseek(f, static_cast<long>(offset), origin);
#else
		return fseeko(f, static_cast<off_t>(offset), origin);
#endif
	}

	static int64_t Tell(FILE *f) {
#if defined(_WIN32)
		return _ftelli64(f);
#elif defined(PANDUCK_NO_LARGE_FILE_SEEK) ||                                                                           \
    (defined(__STRICT_ANSI__) && !defined(_POSIX_C_SOURCE) && !defined(_DEFAULT_SOURCE))
		return static_cast<int64_t>(ftell(f));
#else
		return static_cast<int64_t>(ftello(f));
#endif
	}

	FILE *handle;
	uint64_t size;
	std::string name;
};

} // namespace panduck
