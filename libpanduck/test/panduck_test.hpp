#pragma once

#include "panduck/block.hpp"

#include <sstream>
#include <string>
#include <vector>

// libpanduck's standalone test harness -- deliberately not a test framework.
//
// WHY NO GTEST / CATCH2. libpanduck's entire claim is that it has almost no
// dependencies: the comment at the top of libpanduck/CMakeLists.txt explains
// that even yyjson was a real dependency DECISION rather than a path setting,
// and the ipynb module is off by default in the standalone build because that
// decision has not been taken. Linking a test framework in order to prove the
// library needs nothing would undo the thing being proved. So: two counters, a
// Fail() and two macros.
//
// WHY THE COUNTERS ARE PRINTED. A runner that prints only "OK" is
// indistinguishable from a runner that ran nothing, and this project has been
// bitten by exactly that shape of false green six times (stale artifacts
// reporting TEST_EXIT=0 for a build that had failed). main() prints the number
// of checks EXECUTED, per module and in total, and exits non-zero if that total
// is zero. The count is what makes a green run mean something.

namespace panduck_test {

extern int checks;
extern int failures;

void Fail(const char *file, int line, const std::string &what);

//! Render a failed CHECK_EQ. Templated so int/size_t/std::string/const char *
//! all describe themselves; C++11, so no generic lambdas and no auto returns.
template <class A, class B>
std::string Describe(const char *a_text, const A &a_value, const char *b_text, const B &b_value) {
	std::ostringstream os;
	os << a_text << " == " << b_text << " (left=<" << a_value << "> right=<" << b_value << ">)";
	return os.str();
}

//! THE CENTREPIECE OF THIS SUITE, and the reason it exists at all.
//!
//! Four properties, asserted over EVERY row the reader returned:
//!
//!   1. kind is non-empty
//!   2. encoding is non-empty          <-- the one that matters, see below
//!   3. level >= 1
//!   4. element_order is 0,1,2,... in vector order
//!
//! On (2): the per-reader row structs that L1 deleted (IpynbRow, OrgRow, MwRow,
//! ...) each defaulted `encoding` to ENCODING_TEXT. `::panduck::Block` defaults
//! it to the EMPTY STRING. So a flatten that moves behind the seam and forgets
//! to assign encoding ships a blank encoding on every row: no compile error, and
//! -- until this function existed -- no failing test either, because every
//! assertion about these readers ran through SQL in the extension's
//! sqllogictest suite, which did not look at the column. That trap was live in
//! all six readers and was caught six times by hand. This is the automation of
//! that hand-check.
//!
//! Counted as four checks per call rather than four-per-row on purpose: the
//! assertion covers every row either way, and a count that does not move with
//! the size of a fixture is a count a report can quote. The failure message
//! names the first offending row and its contents.
void CheckVocabularyShape(const char *label, const std::vector<::panduck::Block> &rows);

//! First row matching kind+element_type, or null. `kind` or `element_type` may
//! be null to mean "any".
const ::panduck::Block *FindFirst(const std::vector<::panduck::Block> &rows, const char *kind,
                                  const char *element_type);

//! attributes[key], or "" when absent -- so a test can CHECK_EQ against the
//! expected value without first asserting presence.
std::string Attr(const ::panduck::Block &block, const char *key);

//! Content as a plain string: "<null>" when absent, so a NULL never silently
//! compares equal to "".
std::string Content(const ::panduck::Block &block);

//! Copy a vocabulary constant into a std::string. NOT cosmetic, and the reason
//! is worth writing down because the failure mode is a link error, not a
//! compile error.
//!
//! duck_block_vocabulary.hpp is a VENDORED BYTE-EXACT copy (docs/architecture.md)
//! and declares its constants as `static constexpr const char *` with no
//! out-of-line definition. At C++11 that is fine as long as the value is only
//! READ -- an lvalue-to-rvalue conversion in a constant expression does not
//! odr-use the member. Binding one to a REFERENCE does, and CHECK_EQ's
//! Describe() takes `const B &`, so
//!
//!     CHECK_EQ(row.encoding, V::ENCODING_TEXT);
//!
//! compiles and then fails to link with
//!
//!     undefined reference to `duckdb::DuckBlockVocabulary::ENCODING_TEXT'
//!
//! C++17 made such members implicitly inline, which is why nothing upstream has
//! hit this; libpanduck is C++11 by contract (libpanduck/CMakeLists.txt), and
//! the vendored header must stay byte-exact, so the fix belongs at the call
//! site. Write `CHECK_EQ(row.encoding, Vocab(V::ENCODING_TEXT))`.
inline std::string Vocab(const char *value) {
	return std::string(value);
}

} // namespace panduck_test

#define CHECK(cond)                                                                                                    \
	do {                                                                                                               \
		++::panduck_test::checks;                                                                                      \
		if (!(cond)) {                                                                                                 \
			::panduck_test::Fail(__FILE__, __LINE__, #cond);                                                           \
		}                                                                                                              \
	} while (0)

#define CHECK_MSG(cond, msg)                                                                                           \
	do {                                                                                                               \
		++::panduck_test::checks;                                                                                      \
		if (!(cond)) {                                                                                                 \
			::panduck_test::Fail(__FILE__, __LINE__, std::string(#cond) + " -- " + (msg));                             \
		}                                                                                                              \
	} while (0)

#define CHECK_EQ(a, b)                                                                                                 \
	do {                                                                                                               \
		++::panduck_test::checks;                                                                                      \
		if (!((a) == (b))) {                                                                                           \
			::panduck_test::Fail(__FILE__, __LINE__, ::panduck_test::Describe(#a, (a), #b, (b)));                      \
		}                                                                                                              \
	} while (0)
