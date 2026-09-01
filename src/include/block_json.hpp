#pragma once

#include <cstdio>
#include <string>
#include <vector>

namespace duckdb {

//! JSON string escaping for `table` content.
//!
//! Spec 5.0 makes `table` the ONE element_type whose content is a JSON document
//! ({"headers": [...], "rows": [[...]]}); everything else is text. Two readers emit it --
//! EPUB from <table>, LaTeX from tabular -- so this lives here rather than as a copy in
//! each. Two copies of one rule cannot detect their own disagreement, which is the defect
//! this project has spent a day finding in other people's code.
//!
//! Deliberately dependency-free: no DuckDB types, no JSON library. Pulling in a library to
//! serialise two arrays of strings would be a dependency per element type, and this header
//! is the shape a reader-side helper needs to keep if the parsers are ever extracted from
//! the extension.
inline std::string JsonEscapeString(const std::string &in) {
	std::string out;
	out.reserve(in.size() + 8);
	for (unsigned char c : in) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (c < 0x20) {
				// A control character has no raw JSON spelling; \u keeps the document
				// parseable rather than emitting bytes a reader rejects.
				char buf[7];
				snprintf(buf, sizeof(buf), "\\u%04x", c);
				out += buf;
			} else {
				out += static_cast<char>(c);
			}
		}
	}
	return out;
}

//! Build the spec 5.0 native table schema from already-flattened cell text.
//!
//! `headers` may be empty -- that is "this table has no header row", spelled as an empty
//! array rather than by omitting the key, so a consumer reads the same shape either way.
inline std::string BuildTableJson(const std::vector<std::string> &headers,
                                  const std::vector<std::vector<std::string>> &rows) {
	std::string json = "{\"headers\":[";
	for (size_t h = 0; h < headers.size(); h++) {
		json += (h ? ",\"" : "\"") + JsonEscapeString(headers[h]) + "\"";
	}
	json += "],\"rows\":[";
	for (size_t r = 0; r < rows.size(); r++) {
		json += (r ? ",[" : "[");
		for (size_t c = 0; c < rows[r].size(); c++) {
			json += (c ? ",\"" : "\"") + JsonEscapeString(rows[r][c]) + "\"";
		}
		json += "]";
	}
	json += "]}";
	return json;
}

} // namespace duckdb
