#pragma once

#include "duckdb.hpp"

#include <string>

namespace duckdb {

//! Minimal RAII wrapper over a miniz ZIP reader.
//!
//! Every container format panduck reads is a ZIP with the document in a known member:
//! DOCX in word/document.xml, ODT in content.xml, EPUB in a spine of XHTML files named by
//! its .opf. The archive handling is identical; only the member names and the XML inside
//! them differ. This exists so that is written once.
//!
//! Opening and closing per member would re-read the central directory each time, which
//! matters for EPUB where a book is dozens of members rather than two.
class ZipContainer {
public:
	//! Throws IOException when the file is missing or is not a readable ZIP.
	ZipContainer(const std::string &path, const char *reader_name);
	~ZipContainer();

	ZipContainer(const ZipContainer &) = delete;
	ZipContainer &operator=(const ZipContainer &) = delete;

	//! Reads one member into `out`. Returns false when the member is absent -- normal for
	//! an optional part (a minimal DOCX may omit styles.xml) and fatal for the document
	//! body, so the caller decides which.
	bool Read(const char *member, std::string &out);

	//! Reads a member that must exist, throwing InvalidInputException naming both the file
	//! and the member when it does not. A ZIP without its body member is not the format it
	//! claims to be, and saying so beats returning zero rows.
	std::string ReadRequired(const char *member);

private:
	struct Impl;
	unique_ptr<Impl> impl;
	std::string path;
	std::string reader_name;
};

} // namespace duckdb
