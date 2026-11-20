#pragma once
// ---------------------------------------------------------------------------
// VENDORING THIS FILE
//
// The compact duck_blocks line format, as C++: its code tables and a codec for
// it. Copy it beside duck_block_vocabulary.hpp. It needs DuckDB's Value API and
// DuckDB's bundled yyjson, which an extension already has, and nothing else
// from this repo.
//
// THE TABLES BELOW ARE THE FORMAT'S SOURCE. duck_block_utils builds its SQL
// macros (duck_blocks_to_compact, duck_blocks_from_compact,
// read_compact_blocks) from them at load, so the SQL and anything vendoring
// this file use one set of codes. duck_block_utils does not call the codec
// below itself; test/fixtures/compact/golden is what holds the two in step,
// and a consumer's tests should read the same files.
//
// THE FORMAT. One JSON object per line, one line per element, in
// element_order order, after a header line {"@":"<TABLE_VERSION>"}:
//   t  kind + element_type as a type code   always
//   c  content                              when not NULL ("" is written)
//   l  level                                always
//   e  encoding                             when not 'text'; 'json' is "j"
//   a  known attribute keys, by code        when the element has one
//   m  every other attribute key            when the element has one
// An element's position among the element lines is its element_order. Header
// lines (an "@" and no "t") and empty lines are skipped wherever they appear,
// so files concatenate.
//
// TYPE CODES: a block is UPPERCASE, an inline lowercase, a value a symbol; a
// type that is both a block and an inline has one letter in both cases. A type
// with no code is written as kind:element_type.
//
// VERSIONING. Codes are only ever ADDED, never changed or reused, and a reader
// refuses a type or attribute code it does not know -- so a file from a newer
// table fails loudly on an older reader instead of losing a field.
// TABLE_VERSION is the duck_block_utils release that last changed the tables.
// ---------------------------------------------------------------------------

#include "duck_block_vocabulary.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/value.hpp"
#include "yyjson.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace duckdb {
namespace duck_block {
namespace compact {

static constexpr const char *TABLE_VERSION = "4.2.0";

struct TypeCode {
	const char *kind;
	const char *element_type;
	const char *code;
};

static constexpr TypeCode TYPE_CODES[] = {
    {"block", "document", "O"},
    {"block", "heading", "H"},
    {"block", "paragraph", "P"},
    {"block", "plain", "N"},
    {"block", "code", "C"},
    {"block", "blockquote", "Q"},
    {"block", "list", "L"},
    {"block", "list_item", "I"},
    {"block", "deflist", "E"},
    {"block", "lineblock", "B"},
    {"block", "table", "T"},
    {"block", "hr", "R"},
    {"block", "page_break", "K"},
    {"block", "metadata", "M"},
    {"block", "image", "G"},
    {"block", "raw", "W"},
    {"block", "div", "D"},
    {"block", "section", "S"},
    {"block", "figure", "F"},
    {"block", "caption", "A"},
    {"block", "generic", "X"},
    {"inline", "text", "t"},
    {"inline", "space", "s"},
    {"inline", "softbreak", "j"},
    {"inline", "linebreak", "r"},
    {"inline", "code", "c"},
    {"inline", "math", "m"},
    {"inline", "raw", "w"},
    {"inline", "bold", "b"},
    {"inline", "italic", "i"},
    {"inline", "strikethrough", "k"},
    {"inline", "superscript", "u"},
    {"inline", "subscript", "d"},
    {"inline", "smallcaps", "z"},
    {"inline", "underline", "l"},
    {"inline", "link", "a"},
    {"inline", "image", "g"},
    {"inline", "quoted", "q"},
    {"inline", "span", "p"},
    {"inline", "cite", "e"},
    {"inline", "note", "f"},
    {"inline", "generic", "x"},
    {"value", "string", "$"},
    {"value", "bool", "?"},
    {"value", "list", "["},
    {"value", "map", "{"},
    {"value", "inlines", "~"},
    {"value", "blocks", "#"},
    {"value", "version", "@"},
    {"value", "generic", "*"},
};

struct AttrCode {
	const char *key;
	const char *code;
};

// Codes are STRUCT field names in the SQL reader, which DuckDB treats without
// regard to case, so they must be unique when case is ignored.
static constexpr AttrCode ATTR_CODES[] = {
    {"class", "c"},         {"id", "i"},       {"source_type", "s"},  {"heading_level", "h"}, {"list_type", "t"},
    {"ordered", "o"},       {"key", "k"},      {"role", "r"},         {"href", "w"},          {"format", "f"},
    {"src", "u"},           {"language", "g"}, {"number_delim", "d"}, {"number_style", "y"},  {"start", "n"},
    {"template_name", "m"}, {"alt", "a"},      {"link_type", "l"},    {"pandoc_ast", "p"},    {"execution_count", "x"},
    {"title", "e"},
};

static constexpr idx_t TYPE_CODE_COUNT = sizeof(TYPE_CODES) / sizeof(TYPE_CODES[0]);
static constexpr idx_t ATTR_CODE_COUNT = sizeof(ATTR_CODES) / sizeof(ATTR_CODES[0]);

//! The header line, with its newline.
inline std::string Header() {
	return std::string("{\"@\":\"") + TABLE_VERSION + "\"}\n";
}

namespace detail {

inline const std::unordered_map<std::string, std::string> &TypeEncode() {
	static const std::unordered_map<std::string, std::string> m = [] {
		std::unordered_map<std::string, std::string> r;
		for (idx_t i = 0; i < TYPE_CODE_COUNT; i++) {
			r[std::string(TYPE_CODES[i].kind) + ":" + TYPE_CODES[i].element_type] = TYPE_CODES[i].code;
		}
		return r;
	}();
	return m;
}

inline const std::unordered_map<std::string, std::string> &TypeDecode() {
	static const std::unordered_map<std::string, std::string> m = [] {
		std::unordered_map<std::string, std::string> r;
		for (idx_t i = 0; i < TYPE_CODE_COUNT; i++) {
			r[TYPE_CODES[i].code] = std::string(TYPE_CODES[i].kind) + ":" + TYPE_CODES[i].element_type;
		}
		return r;
	}();
	return m;
}

inline const std::unordered_map<std::string, std::string> &AttrEncode() {
	static const std::unordered_map<std::string, std::string> m = [] {
		std::unordered_map<std::string, std::string> r;
		for (idx_t i = 0; i < ATTR_CODE_COUNT; i++) {
			r[ATTR_CODES[i].key] = ATTR_CODES[i].code;
		}
		return r;
	}();
	return m;
}

inline const std::unordered_map<std::string, std::string> &AttrDecode() {
	static const std::unordered_map<std::string, std::string> m = [] {
		std::unordered_map<std::string, std::string> r;
		for (idx_t i = 0; i < ATTR_CODE_COUNT; i++) {
			r[ATTR_CODES[i].code] = ATTR_CODES[i].key;
		}
		return r;
	}();
	return m;
}

inline std::string OrderText(const Value &order) {
	return order.IsNull() ? std::string("NULL") : order.ToString();
}

struct MutDoc {
	duckdb_yyjson::yyjson_mut_doc *doc;
	MutDoc() : doc(duckdb_yyjson::yyjson_mut_doc_new(nullptr)) {
	}
	~MutDoc() {
		duckdb_yyjson::yyjson_mut_doc_free(doc);
	}
};

struct Doc {
	duckdb_yyjson::yyjson_doc *doc;
	explicit Doc(duckdb_yyjson::yyjson_doc *d) : doc(d) {
	}
	~Doc() {
		if (doc) {
			duckdb_yyjson::yyjson_doc_free(doc);
		}
	}
};

inline std::string KeysJson(duckdb_yyjson::yyjson_val *obj) {
	std::string out = "[";
	size_t idx, max;
	duckdb_yyjson::yyjson_val *k, *v;
	bool first = true;
	yyjson_obj_foreach(obj, idx, max, k, v) {
		out += first ? "\"" : ",\"";
		out += duckdb_yyjson::yyjson_get_str(k);
		out += "\"";
		first = false;
	}
	return out + "]";
}

} // namespace detail

//! One element as one line, with its newline. Refuses what the format cannot
//! carry rather than writing a default: a NULL level, encoding, attributes map
//! or attribute value, and an encoding named "t" or "j" (those are codes).
inline std::string WriteLine(const Value &block) {
	auto &f = StructValue::GetChildren(block);
	auto &order = f[DuckBlockVocabulary::ELEMENT_ORDER_IDX];
	const std::string where = " at element_order " + detail::OrderText(order);

	detail::MutDoc md;
	auto *doc = md.doc;
	auto *obj = duckdb_yyjson::yyjson_mut_obj(doc);
	duckdb_yyjson::yyjson_mut_doc_set_root(doc, obj);

	const std::string kind =
	    f[DuckBlockVocabulary::KIND_IDX].IsNull() ? "" : StringValue::Get(f[DuckBlockVocabulary::KIND_IDX]);
	const std::string type = f[DuckBlockVocabulary::ELEMENT_TYPE_IDX].IsNull()
	                             ? ""
	                             : StringValue::Get(f[DuckBlockVocabulary::ELEMENT_TYPE_IDX]);
	const std::string kt = kind + ":" + type;
	auto &enc = detail::TypeEncode();
	auto it = enc.find(kt);
	const std::string t = it == enc.end() ? kt : it->second;
	duckdb_yyjson::yyjson_mut_obj_add_strncpy(doc, obj, "t", t.data(), t.size());

	auto &content = f[DuckBlockVocabulary::CONTENT_IDX];
	if (!content.IsNull()) {
		auto &c = StringValue::Get(content);
		duckdb_yyjson::yyjson_mut_obj_add_strncpy(doc, obj, "c", c.data(), c.size());
	}

	auto &level = f[DuckBlockVocabulary::LEVEL_IDX];
	if (level.IsNull()) {
		throw InvalidInputException("compact: NULL level%s", where);
	}
	duckdb_yyjson::yyjson_mut_obj_add_int(doc, obj, "l", level.GetValue<int32_t>());

	auto &encoding = f[DuckBlockVocabulary::ENCODING_IDX];
	if (encoding.IsNull()) {
		throw InvalidInputException("compact: NULL encoding%s", where);
	}
	const std::string e = StringValue::Get(encoding);
	if (e == "t" || e == "j") {
		throw InvalidInputException("compact: encoding '%s' is a reserved code", e);
	}
	if (e == "json") {
		duckdb_yyjson::yyjson_mut_obj_add_str(doc, obj, "e", "j");
	} else if (e != "text") {
		duckdb_yyjson::yyjson_mut_obj_add_strncpy(doc, obj, "e", e.data(), e.size());
	}

	auto &attrs = f[DuckBlockVocabulary::ATTRIBUTES_IDX];
	if (attrs.IsNull()) {
		throw InvalidInputException("compact: NULL attributes%s", where);
	}
	auto &acode = detail::AttrEncode();
	auto *a = duckdb_yyjson::yyjson_mut_obj(doc);
	auto *m = duckdb_yyjson::yyjson_mut_obj(doc);
	// Keys go out in the MAP's own order, in both objects: the reader sorts.
	for (auto &entry : MapValue::GetChildren(attrs)) {
		auto &kv = StructValue::GetChildren(entry);
		if (kv[1].IsNull()) {
			throw InvalidInputException("compact: NULL attribute value%s", where);
		}
		const std::string key = StringValue::Get(kv[0]);
		const std::string &val = StringValue::Get(kv[1]);
		auto known = acode.find(key);
		if (known != acode.end()) {
			duckdb_yyjson::yyjson_mut_obj_add(
			    a, duckdb_yyjson::yyjson_mut_strncpy(doc, known->second.data(), known->second.size()),
			    duckdb_yyjson::yyjson_mut_strncpy(doc, val.data(), val.size()));
		} else {
			duckdb_yyjson::yyjson_mut_obj_add(m, duckdb_yyjson::yyjson_mut_strncpy(doc, key.data(), key.size()),
			                                  duckdb_yyjson::yyjson_mut_strncpy(doc, val.data(), val.size()));
		}
	}
	if (duckdb_yyjson::yyjson_mut_obj_size(a) > 0) {
		duckdb_yyjson::yyjson_mut_obj_add_val(doc, obj, "a", a);
	}
	if (duckdb_yyjson::yyjson_mut_obj_size(m) > 0) {
		duckdb_yyjson::yyjson_mut_obj_add_val(doc, obj, "m", m);
	}

	size_t len = 0;
	char *json = duckdb_yyjson::yyjson_mut_write(doc, 0, &len);
	if (!json) {
		throw InternalException("compact: could not serialise an element%s", where);
	}
	std::string line(json, len);
	free(json);
	line += '\n';
	return line;
}

//! A whole list: the header, then one line per element in element_order order
//! (ties keep list order; a NULL element_order sorts last).
inline std::string Write(const vector<Value> &blocks) {
	vector<idx_t> idx(blocks.size());
	for (idx_t i = 0; i < idx.size(); i++) {
		idx[i] = i;
	}
	std::stable_sort(idx.begin(), idx.end(), [&](idx_t x, idx_t y) {
		auto &ox = StructValue::GetChildren(blocks[x])[DuckBlockVocabulary::ELEMENT_ORDER_IDX];
		auto &oy = StructValue::GetChildren(blocks[y])[DuckBlockVocabulary::ELEMENT_ORDER_IDX];
		if (ox.IsNull() || oy.IsNull()) {
			return !ox.IsNull() && oy.IsNull();
		}
		return ox.GetValue<int32_t>() < oy.GetValue<int32_t>();
	});
	std::string out = Header();
	for (auto i : idx) {
		out += WriteLine(blocks[i]);
	}
	return out;
}

//! One line. Returns false for an empty or header line; otherwise sets `out` to
//! the element, numbered `element_order`. Refuses an unknown type code, an
//! unknown attribute code, and a line without "l".
inline bool ReadLine(const char *data, size_t len, int32_t element_order, const LogicalType &block_type, Value &out) {
	if (len == 0) {
		return false;
	}
	const std::string where = " at element_order " + std::to_string(element_order);
	detail::Doc d(duckdb_yyjson::yyjson_read(data, len, 0));
	auto *root = d.doc ? duckdb_yyjson::yyjson_doc_get_root(d.doc) : nullptr;
	if (!root || !duckdb_yyjson::yyjson_is_obj(root)) {
		throw InvalidInputException("compact: a line is not a JSON object%s", where);
	}
	auto *t = duckdb_yyjson::yyjson_obj_get(root, "t");
	auto *at = duckdb_yyjson::yyjson_obj_get(root, "@");
	const bool has_t = t && !duckdb_yyjson::yyjson_is_null(t);
	if (!has_t && at && !duckdb_yyjson::yyjson_is_null(at)) {
		return false; // a header
	}
	if (!has_t || !duckdb_yyjson::yyjson_is_str(t)) {
		throw InvalidInputException("compact: unknown type code NULL%s", where);
	}
	const std::string code(duckdb_yyjson::yyjson_get_str(t), duckdb_yyjson::yyjson_get_len(t));
	std::string kt;
	auto &dec = detail::TypeDecode();
	auto hit = dec.find(code);
	if (hit != dec.end()) {
		kt = hit->second;
	} else if (code.find(':') != std::string::npos) {
		kt = code;
	} else {
		throw InvalidInputException("compact: unknown type code \"%s\"%s", code, where);
	}
	const auto colon = kt.find(':');

	auto *a = duckdb_yyjson::yyjson_obj_get(root, "a");
	std::map<std::string, std::string> attrs;
	auto *m = duckdb_yyjson::yyjson_obj_get(root, "m");
	if (m && duckdb_yyjson::yyjson_is_obj(m)) {
		size_t idx, max;
		duckdb_yyjson::yyjson_val *k, *v;
		yyjson_obj_foreach(m, idx, max, k, v) {
			if (duckdb_yyjson::yyjson_is_str(v)) {
				attrs[std::string(duckdb_yyjson::yyjson_get_str(k), duckdb_yyjson::yyjson_get_len(k))] =
				    std::string(duckdb_yyjson::yyjson_get_str(v), duckdb_yyjson::yyjson_get_len(v));
			}
		}
	}
	if (a && duckdb_yyjson::yyjson_is_obj(a)) {
		auto &adec = detail::AttrDecode();
		size_t idx, max;
		duckdb_yyjson::yyjson_val *k, *v;
		yyjson_obj_foreach(a, idx, max, k, v) {
			auto key = adec.find(std::string(duckdb_yyjson::yyjson_get_str(k), duckdb_yyjson::yyjson_get_len(k)));
			if (key == adec.end()) {
				throw InvalidInputException("compact: unknown attribute code%s: %s", where, detail::KeysJson(a));
			}
			if (duckdb_yyjson::yyjson_is_str(v)) {
				attrs[key->second] = std::string(duckdb_yyjson::yyjson_get_str(v), duckdb_yyjson::yyjson_get_len(v));
			}
		}
	}

	auto *l = duckdb_yyjson::yyjson_obj_get(root, "l");
	if (!l || !duckdb_yyjson::yyjson_is_int(l)) {
		throw InvalidInputException("compact: missing level%s", where);
	}
	auto *c = duckdb_yyjson::yyjson_obj_get(root, "c");
	auto *e = duckdb_yyjson::yyjson_obj_get(root, "e");
	std::string encoding = "text";
	if (e && duckdb_yyjson::yyjson_is_str(e)) {
		encoding = std::string(duckdb_yyjson::yyjson_get_str(e), duckdb_yyjson::yyjson_get_len(e));
		encoding = encoding == "t" ? "text" : encoding == "j" ? "json" : encoding;
	}

	vector<Value> keys, vals;
	for (auto &kv : attrs) { // std::map: keys sorted, as the SQL reader sorts them
		keys.emplace_back(kv.first);
		vals.emplace_back(kv.second);
	}
	vector<Value> fields(7);
	fields[DuckBlockVocabulary::KIND_IDX] = Value(kt.substr(0, colon));
	fields[DuckBlockVocabulary::ELEMENT_TYPE_IDX] = Value(kt.substr(colon + 1));
	fields[DuckBlockVocabulary::CONTENT_IDX] =
	    c && duckdb_yyjson::yyjson_is_str(c)
	        ? Value(std::string(duckdb_yyjson::yyjson_get_str(c), duckdb_yyjson::yyjson_get_len(c)))
	        : Value(LogicalType::VARCHAR);
	fields[DuckBlockVocabulary::LEVEL_IDX] = Value::INTEGER(int32_t(duckdb_yyjson::yyjson_get_sint(l)));
	fields[DuckBlockVocabulary::ENCODING_IDX] = Value(encoding);
	fields[DuckBlockVocabulary::ATTRIBUTES_IDX] =
	    Value::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR, std::move(keys), std::move(vals));
	fields[DuckBlockVocabulary::ELEMENT_ORDER_IDX] = Value::INTEGER(element_order);
	out = Value::STRUCT(block_type, std::move(fields));
	return true;
}

//! A whole text: every element line, numbered from 0.
inline void Read(const std::string &text, const LogicalType &block_type, vector<Value> &out) {
	int32_t order = 0;
	size_t start = 0;
	while (start <= text.size()) {
		size_t end = text.find('\n', start);
		if (end == std::string::npos) {
			end = text.size();
		}
		Value v;
		if (ReadLine(text.data() + start, end - start, order, block_type, v)) {
			out.push_back(std::move(v));
			order++;
		}
		if (end == text.size()) {
			break;
		}
		start = end + 1;
	}
}

} // namespace compact
} // namespace duck_block
} // namespace duckdb
