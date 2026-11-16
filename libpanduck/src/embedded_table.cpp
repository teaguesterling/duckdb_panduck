#include "panduck/embedded_table.hpp"

#include "panduck/block_json.hpp"
#include "panduck/yyjson_compat.hpp"

// WHAT IS ACTUALLY USED: std::isspace and std::tolower from <cctype>, size_t from
// <cstddef>, free from <cstdlib>, std::unordered_map, std::string, std::vector.
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

namespace panduck {

namespace {

using Rows = std::vector<std::vector<std::string>>;

std::string Trim(const std::string &s) {
	size_t b = 0, e = s.size();
	while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) {
		b++;
	}
	while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
		e--;
	}
	return s.substr(b, e - b);
}

// --- csv / tsv ---------------------------------------------------------------

bool ParseDelimited(const std::string &s, char delim, const char *fmt, std::vector<std::string> &headers, Rows &rows,
                    std::string &error) {
	Rows records;
	std::vector<std::string> record;
	std::string field;
	bool in_quotes = false;
	bool quoted = false;
	size_t record_no = 1;

	auto end_field = [&]() {
		record.push_back(field);
		field.clear();
		quoted = false;
	};
	auto end_record = [&]() {
		// A line that is empty or only whitespace, outside quotes, is not a record.
		bool blank = record.empty() && !quoted && Trim(field).empty();
		if (!blank) {
			end_field();
			records.push_back(std::move(record));
		}
		record.clear();
		field.clear();
		quoted = false;
		record_no++;
	};

	for (size_t i = 0; i < s.size(); i++) {
		char c = s[i];
		if (in_quotes) {
			if (c == '"') {
				if (i + 1 < s.size() && s[i + 1] == '"') {
					field += '"';
					i++;
				} else {
					in_quotes = false;
				}
			} else {
				field += c;
			}
		} else if (c == '"' && field.empty() && !quoted) {
			in_quotes = true;
			quoted = true;
		} else if (c == delim) {
			end_field();
		} else if (c == '\r' && i + 1 < s.size() && s[i + 1] == '\n') {
			continue; // the \n ends the record
		} else if (c == '\n' || c == '\r') {
			end_record();
		} else {
			field += c;
		}
	}
	if (in_quotes) {
		error = std::string(fmt) + ": unterminated quote in record " + std::to_string(record_no);
		return false;
	}
	end_record();

	if (records.empty()) {
		error = std::string(fmt) + ": no header row";
		return false;
	}
	headers = std::move(records[0]);
	for (size_t r = 1; r < records.size(); r++) {
		if (records[r].size() != headers.size()) {
			error = std::string(fmt) + ": data row " + std::to_string(r) + " has " + std::to_string(records[r].size()) +
			        " fields, the header has " + std::to_string(headers.size());
			return false;
		}
		rows.push_back(std::move(records[r]));
	}
	return true;
}

// --- json / jsonl ------------------------------------------------------------

//! One complete JSON value, which must be an array or an object. NUMBERS ARE READ AS
//! RAW TEXT, so a cell keeps "1.50" as the author wrote it rather than yyjson's
//! re-printed double. yyjson rejects trailing content and unclosed brackets itself,
//! which is what dbu's separate bracket check was for.
yyjson_doc *ParseWhole(const std::string &s) {
	yyjson_doc *doc = yyjson_read(s.data(), s.size(), YYJSON_READ_NUMBER_AS_RAW);
	if (!doc) {
		return nullptr;
	}
	yyjson_val *root = yyjson_doc_get_root(doc);
	if (!yyjson_is_arr(root) && !yyjson_is_obj(root)) {
		yyjson_doc_free(doc);
		return nullptr;
	}
	return doc;
}

std::string Cell(yyjson_val *v) {
	if (yyjson_is_str(v)) {
		return std::string(yyjson_get_str(v), yyjson_get_len(v));
	}
	if (yyjson_is_null(v)) {
		return std::string();
	}
	if (yyjson_is_raw(v)) {
		return std::string(yyjson_get_raw(v), yyjson_get_len(v));
	}
	if (yyjson_is_bool(v)) {
		return yyjson_get_bool(v) ? "true" : "false";
	}
	// A nested array or object is kept as compact JSON, so a cell never loses data.
	size_t len = 0;
	char *json = yyjson_val_write(v, 0, &len);
	if (!json) {
		return std::string();
	}
	std::string out(json, len);
	free(json);
	return out;
}

// The most empty cells a key union may add, unless the objects already hold more real
// cells than that.
const size_t MAX_PADDED_CELLS = 1000000;

bool ObjectsToTable(const std::vector<yyjson_val *> &objects, const char *fmt, std::vector<std::string> &headers,
                    Rows &rows, std::string &error) {
	// JSON keys are case-sensitive, so the index is too.
	std::unordered_map<std::string, size_t> index;
	size_t present = 0;
	for (auto *obj : objects) {
		present += yyjson_obj_size(obj);
		size_t i, n;
		yyjson_val *key, *val;
		yyjson_obj_foreach(obj, i, n, key, val) {
			std::string k(yyjson_get_str(key), yyjson_get_len(key));
			if (index.emplace(k, headers.size()).second) {
				headers.push_back(k);
			}
		}
	}
	size_t total = objects.size() * headers.size();
	size_t padded = total > present ? total - present : 0;
	size_t limit = present > MAX_PADDED_CELLS ? present : MAX_PADDED_CELLS;
	if (padded > limit) {
		error = std::string(fmt) + ": " + std::to_string(objects.size()) + " objects x " +
		        std::to_string(headers.size()) + " keys would need " + std::to_string(padded) +
		        " padding cells; the limit is max(" + std::to_string(MAX_PADDED_CELLS) + ", present cells)";
		return false;
	}
	for (auto *obj : objects) {
		std::vector<std::string> row(headers.size());
		size_t i, n;
		yyjson_val *key, *val;
		yyjson_obj_foreach(obj, i, n, key, val) {
			row[index[std::string(yyjson_get_str(key), yyjson_get_len(key))]] = Cell(val);
		}
		rows.push_back(std::move(row));
	}
	return true;
}

bool ParseJson(const std::string &s, std::vector<std::string> &headers, Rows &rows, std::string &error) {
	yyjson_doc *doc = ParseWhole(s);
	if (!doc) {
		error = "json: not a complete JSON value";
		return false;
	}
	yyjson_val *root = yyjson_doc_get_root(doc);
	bool ok = false;
	if (!yyjson_is_arr(root)) {
		error = "json: expected an array of objects";
	} else {
		std::vector<yyjson_val *> objects;
		bool all_objects = true;
		size_t i, n;
		yyjson_val *elem;
		yyjson_arr_foreach(root, i, n, elem) {
			if (!yyjson_is_obj(elem)) {
				error = "json: element " + std::to_string(i + 1) + " is not an object";
				all_objects = false;
				break;
			}
			objects.push_back(elem);
		}
		if (all_objects && objects.empty()) {
			error = "json: the array is empty";
		} else if (all_objects) {
			ok = ObjectsToTable(objects, "json", headers, rows, error);
		}
	}
	yyjson_doc_free(doc);
	return ok;
}

bool ParseJsonl(const std::string &s, std::vector<std::string> &headers, Rows &rows, std::string &error) {
	std::vector<yyjson_doc *> docs;
	auto free_all = [&]() {
		for (auto *d : docs) {
			yyjson_doc_free(d);
		}
	};
	size_t line_no = 0;
	size_t start = 0;
	while (start <= s.size()) {
		size_t end = s.find('\n', start);
		if (end == std::string::npos) {
			end = s.size();
		}
		line_no++;
		std::string line = Trim(s.substr(start, end - start));
		start = end + 1;
		if (line.empty()) {
			continue;
		}
		yyjson_doc *doc = ParseWhole(line);
		if (!doc || !yyjson_is_obj(yyjson_doc_get_root(doc))) {
			if (doc) {
				yyjson_doc_free(doc);
			}
			free_all();
			error = "jsonl: line " + std::to_string(line_no) + " is not a JSON object";
			return false;
		}
		docs.push_back(doc);
	}
	if (docs.empty()) {
		error = "jsonl: no objects";
		return false;
	}
	std::vector<yyjson_val *> objects;
	for (auto *d : docs) {
		objects.push_back(yyjson_doc_get_root(d));
	}
	bool ok = ObjectsToTable(objects, "jsonl", headers, rows, error);
	free_all();
	return ok;
}

} // namespace

bool EmbeddedToTable(const std::string &text, const std::string &format_in, std::string &content, std::string &error) {
	std::string format;
	for (char c : format_in) {
		format += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	std::vector<std::string> headers;
	Rows rows;
	bool ok;
	if (format == "csv") {
		ok = ParseDelimited(text, ',', "csv", headers, rows, error);
	} else if (format == "tsv") {
		ok = ParseDelimited(text, '\t', "tsv", headers, rows, error);
	} else if (format == "json") {
		ok = ParseJson(text, headers, rows, error);
	} else if (format == "jsonl") {
		ok = ParseJsonl(text, headers, rows, error);
	} else {
		error = "unsupported format: " + format + " (supported: csv, tsv, json, jsonl)";
		return false;
	}
	if (ok) {
		content = BuildTableJson(headers, rows);
	}
	return ok;
}

} // namespace panduck
