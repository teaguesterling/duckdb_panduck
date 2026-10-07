#include "panduck_test.hpp"

#include "panduck/vocabulary.hpp"

#include <cstdio>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace panduck_test {

int checks = 0;
int failures = 0;

void Fail(const char *file, int line, const std::string &what) {
	++failures;
	fprintf(stderr, "FAIL %s:%d: %s\n", file, line, what.c_str());
}

std::string Attr(const ::panduck::Block &block, const char *key) {
	std::map<std::string, std::string>::const_iterator it = block.attributes.find(key);
	return it == block.attributes.end() ? std::string() : it->second;
}

std::string Content(const ::panduck::Block &block) {
	return block.content.is_null ? std::string("<null>") : block.content.value;
}

const ::panduck::Block *FindFirst(const std::vector<::panduck::Block> &rows, const char *kind,
                                  const char *element_type) {
	for (size_t i = 0; i < rows.size(); ++i) {
		if (kind && rows[i].kind != kind) {
			continue;
		}
		if (element_type && rows[i].element_type != element_type) {
			continue;
		}
		return &rows[i];
	}
	return NULL;
}

//! Where the offending row is, and what it held. Without this a failure says
//! only "some row had a blank encoding", which is the kind of message that
//! sends someone back to a debugger.
//!
//! Bounds-checked because CHECK_MSG evaluates its message eagerly, so this is
//! called on the passing path too -- with index == rows.size(), the sentinel
//! for "no offending row".
static std::string RowDescription(const char *label, size_t index, const std::vector<::panduck::Block> &rows) {
	if (index >= rows.size()) {
		return std::string(label) + " (no offending row)";
	}
	const ::panduck::Block &row = rows[index];
	std::ostringstream os;
	os << label << " row " << index << ": kind=<" << row.kind << "> element_type=<" << row.element_type
	   << "> level=" << row.level << " encoding=<" << row.encoding << "> element_order=" << row.element_order
	   << " content=<" << Content(row) << ">";
	return os.str();
}

void CheckVocabularyShape(const char *label, const std::vector<::panduck::Block> &rows) {
	// Each property is ONE check over all rows; the message names the first row
	// that broke it. See the header for why it is counted this way.
	size_t bad_kind = rows.size();
	size_t bad_encoding = rows.size();
	size_t bad_level = rows.size();
	size_t bad_order = rows.size();

	for (size_t i = 0; i < rows.size(); ++i) {
		const ::panduck::Block &row = rows[i];
		if (bad_kind == rows.size() && row.kind.empty()) {
			bad_kind = i;
		}
		if (bad_encoding == rows.size() && row.encoding.empty()) {
			bad_encoding = i;
		}
		if (bad_level == rows.size() && row.level < 1) {
			bad_level = i;
		}
		if (bad_order == rows.size() && row.element_order != static_cast<int32_t>(i)) {
			bad_order = i;
		}
	}

	CHECK_MSG(bad_kind == rows.size(), std::string("blank kind, ") + RowDescription(label, bad_kind, rows));

	// THE CENTREPIECE. ::panduck::Block defaults encoding to "", the deleted
	// per-reader row structs defaulted it to ENCODING_TEXT.
	CHECK_MSG(bad_encoding == rows.size(), std::string("blank encoding, ") + RowDescription(label, bad_encoding, rows));

	CHECK_MSG(bad_level == rows.size(), std::string("level < 1, ") + RowDescription(label, bad_level, rows));
	CHECK_MSG(bad_order == rows.size(),
	          std::string("element_order not sequential from 0, ") + RowDescription(label, bad_order, rows));
}

} // namespace panduck_test
