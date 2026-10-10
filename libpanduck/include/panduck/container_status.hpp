#pragma once

#include <string>

// HOW A CONTAINER READER REPORTS FAILURE ACROSS THE SEAM (issue #104, L2).
//
// docx, odt and epub are the three readers whose failures are SQL-VISIBLE. Each
// has `statement error` tests in test/sql/<fmt>_reader.test matching an exact
// message, and test/fixtures/malformed/README.md names the two DuckDB exception
// TYPES those tests depend on -- `IO Error` and `Invalid Input Error` -- because
// the type IS the prefix on the message a user sees.
//
// So neither throwing nor message-building can live behind the seam:
//
//   * A DuckDB exception type is a DuckDB API symbol. Rule 1.
//   * A panduck exception that DuckDB wraps would keep the TEXT and change the
//     TYPE, which rule 4 (behaviour must not change) forbids. This is the same
//     reasoning that made panduck::ZipContainer non-throwing; see
//     panduck/zip_container.hpp, which this type generalises from two failure
//     modes to five.
//   * The messages name the READER (`read_odt_blocks: ...`), which is the name
//     of a DuckDB table function. The library has no business knowing it.
//
// Hence: THE CORE REPORTS A CODE, THE TAIL OWNS EVERY FORMAT STRING.
// src/container_status.cpp has the five, one per code, and is the only place in
// the build where a container error message exists. That is what makes "the SQL
// error text and type are byte-identical before and after" a property you can
// check by reading one function, rather than a claim to re-verify per reader.
//
// WHY AN OUT-PARAMETER RATHER THAN A Result<T> WRAPPER. The three entry points
// return `std::vector<XBlock>` / `std::vector<Block>` by value, and
// `ReadOdt(path, status)` keeps exactly that -- no wrapper to unwrap at the call
// site, and no need to invent a C++11 stand-in for the `optional`/`expected`
// this build does not have (rule 2). The cost is that a caller can ignore the
// status; it is paid down by every entry point also returning an EMPTY vector on
// failure, so a caller that ignores it reads an empty document rather than
// garbage -- the same bargain panduck::ZipContainer::Read() already makes.
//
// WHY ONE SHARED TYPE AND NOT THREE. These five modes are properties of reading
// a CONTAINER DOCUMENT, not of docx, odt or epub. Three private copies of one
// enum is precisely the duplication L1 collapsed for the row struct.

namespace panduck {

//! Why a container reader could not produce rows. Reported, never thrown.
struct ContainerStatus {
	enum Code {
		//! No failure. Every entry point leaves the status here on success.
		OK = 0,
		//! The file is missing, or is not a readable ZIP archive at all.
		NOT_A_ZIP,
		//! A member the format REQUIRES is absent, so the archive is not the
		//! format it claims to be. `member` names it.
		MISSING_MEMBER,
		//! `member` is present but is not well-formed XML. `detail` carries the
		//! XML parser's own words, which the message quotes verbatim.
		MALFORMED_XML,
		//! The container's manifest parsed but names no root document. EPUB only
		//! today: it is the one format panduck reads whose manifest is two levels
		//! deep (META-INF/container.xml -> the package document).
		MANIFEST_NO_ROOT,
		//! The manifest names a root document the archive does not contain.
		//! `member` is the path it named. EPUB only today, same reason.
		MANIFEST_ROOT_MISSING
	};

	Code code = OK;
	//! The archive member the failure is about, for the codes that have one.
	std::string member;
	//! Free text from a lower layer -- today only the XML parser's description.
	std::string detail;

	//! Named for what callers ask, so no site spells `code == OK` itself.
	bool Ok() const {
		return code == OK;
	}

	static ContainerStatus NotAZip() {
		ContainerStatus s;
		s.code = NOT_A_ZIP;
		return s;
	}

	static ContainerStatus MissingMember(const std::string &member) {
		ContainerStatus s;
		s.code = MISSING_MEMBER;
		s.member = member;
		return s;
	}

	static ContainerStatus MalformedXml(const std::string &member, const std::string &detail) {
		ContainerStatus s;
		s.code = MALFORMED_XML;
		s.member = member;
		s.detail = detail;
		return s;
	}

	static ContainerStatus ManifestNoRoot() {
		ContainerStatus s;
		s.code = MANIFEST_NO_ROOT;
		return s;
	}

	static ContainerStatus ManifestRootMissing(const std::string &member) {
		ContainerStatus s;
		s.code = MANIFEST_ROOT_MISSING;
		s.member = member;
		return s;
	}
};

} // namespace panduck
