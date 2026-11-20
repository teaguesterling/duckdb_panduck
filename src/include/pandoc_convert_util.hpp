#pragma once

#include "duck_block_types.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/value.hpp"
#include "yyjson.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace duckdb {

// ============================================================================
// Shared utilities for the Pandoc-AST converters
// (pandoc_block_convert.cpp / pandoc_inline_convert.cpp)
// ============================================================================

// Maximum nesting depth for Pandoc-AST conversion, applied both when parsing
// Pandoc JSON into duck_blocks and when emitting Pandoc JSON from
// duck_blocks. Every recursive converter path must call CheckPandocDepth at
// entry so a deeply nested document produces a clean DuckDB error instead of
// exhausting the call stack.
static constexpr int32_t PANDOC_MAX_NESTING_DEPTH = 128;

inline void CheckPandocDepth(idx_t depth) {
	if (depth > idx_t(PANDOC_MAX_NESTING_DEPTH)) {
		throw InvalidInputException("Pandoc AST conversion exceeded the maximum nesting depth of %d",
		                            PANDOC_MAX_NESTING_DEPTH);
	}
}

// Parse a base-10 integer, returning `def` on empty, malformed, or
// out-of-range input. std::stoi throws std::invalid_argument /
// std::out_of_range on such input, which escapes as an unclean error when the
// value comes from user data (e.g. a heading_level attribute).
inline int32_t ParseInt32OrDefault(const std::string &s, int32_t def) {
	if (s.empty()) {
		return def;
	}
	errno = 0;
	char *end = nullptr;
	const long value = std::strtol(s.c_str(), &end, 10);
	if (end == s.c_str() || errno == ERANGE || value > long(std::numeric_limits<int32_t>::max()) ||
	    value < long(std::numeric_limits<int32_t>::min())) {
		return def;
	}
	return int32_t(value);
}

// ----------------------------------------------------------------------------
// Pandoc Attr: [id, [class], [[key, value]]], read into and written from a
// duck_block attributes map. The block converter used these alone, as static
// functions; the inline converter needs the same mapping for Link, Image, Span
// and Code (#208), so there is one copy of it here.
// ----------------------------------------------------------------------------

// A parsed Pandoc attr triple: ["id", ["class", ...], [["key","value"], ...]]
struct PandocAttr {
	string id;
	vector<string> classes;
	vector<std::pair<string, string>> key_values;
};

inline void ParsePandocAttrVal(duckdb_yyjson::yyjson_val *attr_val, PandocAttr &attr) {
	if (!attr_val || !duckdb_yyjson::yyjson_is_arr(attr_val)) {
		return;
	}
	// Element 0: id
	duckdb_yyjson::yyjson_val *id_val = duckdb_yyjson::yyjson_arr_get(attr_val, 0);
	if (id_val && duckdb_yyjson::yyjson_is_str(id_val)) {
		attr.id = string(duckdb_yyjson::yyjson_get_str(id_val), duckdb_yyjson::yyjson_get_len(id_val));
	}
	// Element 1: classes
	duckdb_yyjson::yyjson_val *classes_val = duckdb_yyjson::yyjson_arr_get(attr_val, 1);
	if (classes_val && duckdb_yyjson::yyjson_is_arr(classes_val)) {
		size_t idx, max;
		duckdb_yyjson::yyjson_val *cls;
		yyjson_arr_foreach(classes_val, idx, max, cls) {
			if (duckdb_yyjson::yyjson_is_str(cls)) {
				attr.classes.emplace_back(duckdb_yyjson::yyjson_get_str(cls), duckdb_yyjson::yyjson_get_len(cls));
			}
		}
	}
	// Element 2: key-values
	duckdb_yyjson::yyjson_val *kvs_val = duckdb_yyjson::yyjson_arr_get(attr_val, 2);
	if (kvs_val && duckdb_yyjson::yyjson_is_arr(kvs_val)) {
		size_t idx, max;
		duckdb_yyjson::yyjson_val *kv;
		yyjson_arr_foreach(kvs_val, idx, max, kv) {
			if (duckdb_yyjson::yyjson_is_arr(kv) && duckdb_yyjson::yyjson_arr_size(kv) >= 2) {
				duckdb_yyjson::yyjson_val *k = duckdb_yyjson::yyjson_arr_get(kv, 0);
				duckdb_yyjson::yyjson_val *v = duckdb_yyjson::yyjson_arr_get(kv, 1);
				if (k && duckdb_yyjson::yyjson_is_str(k) && v && duckdb_yyjson::yyjson_is_str(v)) {
					attr.key_values.emplace_back(
					    string(duckdb_yyjson::yyjson_get_str(k), duckdb_yyjson::yyjson_get_len(k)),
					    string(duckdb_yyjson::yyjson_get_str(v), duckdb_yyjson::yyjson_get_len(v)));
				}
			}
		}
	}
}

inline bool IsReservedAttrKey(const string &key) {
	return key == "id" || key == "class" || key == DuckBlockTypes::ATTR_HEADING_LEVEL || key == "language" ||
	       key == DuckBlockTypes::ATTR_LIST_TYPE || key == "format" || key == "src" || key == "alt" || key == "title" ||
	       key == "href" || key == "quote_type" || key == "display";
}

inline void StorePandocAttr(const PandocAttr &attr, map<string, string> &attrs) {
	if (!attr.id.empty()) {
		attrs["id"] = attr.id;
	}
	if (!attr.classes.empty()) {
		string joined;
		for (auto &cls : attr.classes) {
			if (!joined.empty()) {
				joined += " ";
			}
			joined += cls;
		}
		attrs["class"] = joined;
	}
	for (auto &kv : attr.key_values) {
		if (IsReservedAttrKey(kv.first) || attrs.find(kv.first) != attrs.end()) {
			continue;
		}
		attrs[kv.first] = kv.second;
	}
}

inline string GetElementAttribute(const Value &element, const string &key) {
	auto &children = StructValue::GetChildren(element);
	auto &attrs = children[DuckBlockTypes::ATTRIBUTES_IDX];
	if (attrs.IsNull()) {
		return "";
	}

	auto &map_entries = MapValue::GetChildren(attrs);
	for (auto &entry : map_entries) {
		if (entry.IsNull()) {
			continue;
		}
		auto &kv = StructValue::GetChildren(entry);
		if (kv.size() >= 2 && !kv[0].IsNull() && kv[0].GetValue<string>() == key) {
			if (!kv[1].IsNull()) {
				return kv[1].GetValue<string>();
			}
		}
	}
	return "";
}

inline duckdb_yyjson::yyjson_mut_val *CreatePandocAttrVal(duckdb_yyjson::yyjson_mut_doc *doc, const Value &element,
                                                          const string &fallback_class) {
	auto id = GetElementAttribute(element, "id");
	auto classes = GetElementAttribute(element, "class");
	if (classes.empty()) {
		classes = fallback_class;
	}

	duckdb_yyjson::yyjson_mut_val *attr_arr = duckdb_yyjson::yyjson_mut_arr(doc);
	duckdb_yyjson::yyjson_mut_arr_add_strncpy(doc, attr_arr, id.data(), id.size());

	duckdb_yyjson::yyjson_mut_val *classes_arr = duckdb_yyjson::yyjson_mut_arr(doc);
	size_t start = 0;
	while (start < classes.length()) {
		size_t space = classes.find(' ', start);
		size_t len = (space == string::npos) ? string::npos : space - start;
		string cls = classes.substr(start, len);
		if (!cls.empty()) {
			duckdb_yyjson::yyjson_mut_arr_add_strncpy(doc, classes_arr, cls.data(), cls.size());
		}
		if (space == string::npos) {
			break;
		}
		start = space + 1;
	}
	duckdb_yyjson::yyjson_mut_arr_add_val(attr_arr, classes_arr);

	duckdb_yyjson::yyjson_mut_val *kvs_arr = duckdb_yyjson::yyjson_mut_arr(doc);
	auto &children = StructValue::GetChildren(element);
	auto &attrs = children[DuckBlockTypes::ATTRIBUTES_IDX];
	if (!attrs.IsNull()) {
		auto &map_entries = MapValue::GetChildren(attrs);
		for (auto &entry : map_entries) {
			if (entry.IsNull()) {
				continue;
			}
			auto &kv = StructValue::GetChildren(entry);
			if (kv.size() < 2 || kv[0].IsNull() || kv[1].IsNull()) {
				continue;
			}
			string key = kv[0].GetValue<string>();
			if (IsReservedAttrKey(key)) {
				continue;
			}
			string val = kv[1].GetValue<string>();
			duckdb_yyjson::yyjson_mut_val *pair_arr = duckdb_yyjson::yyjson_mut_arr(doc);
			duckdb_yyjson::yyjson_mut_arr_add_strncpy(doc, pair_arr, key.data(), key.size());
			duckdb_yyjson::yyjson_mut_arr_add_strncpy(doc, pair_arr, val.data(), val.size());
			duckdb_yyjson::yyjson_mut_arr_add_val(kvs_arr, pair_arr);
		}
	}
	duckdb_yyjson::yyjson_mut_arr_add_val(attr_arr, kvs_arr);
	return attr_arr;
}

} // namespace duckdb
