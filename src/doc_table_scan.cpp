// The shared table scan: see doc_table_scan.hpp. Moved from attach_storage.cpp (#215).

#include "doc_table_scan.hpp"

#include "reader_registry.hpp"

#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/connection.hpp"
#include "yyjson.hpp"
#include "panduck/slugify.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace duckdb {
namespace panduck_tables {

namespace {

string Quote(const string &s) {
	return SQLString::ToString(s);
}

vector<string> HeaderCells(const string &table_json) {
	using namespace duckdb_yyjson; // NOLINT -- the spelling the json extension uses
	vector<string> out;
	auto doc = yyjson_read(table_json.data(), table_json.size(), 0);
	if (!doc) {
		return out;
	}
	auto root = yyjson_doc_get_root(doc);
	auto headers = yyjson_is_obj(root) ? yyjson_obj_get(root, "headers") : nullptr;
	if (yyjson_is_arr(headers)) {
		size_t idx, max;
		yyjson_val *h;
		yyjson_arr_foreach(headers, idx, max, h) {
			out.emplace_back(yyjson_is_str(h) ? string(yyjson_get_str(h), yyjson_get_len(h)) : string());
		}
	}
	yyjson_doc_free(doc);
	return out;
}

//! The fixed views. A table slug never takes one of these names.
const vector<string> &FixedViews() {
	static const vector<string> names {"blocks", "tables",   "headings", "sections",
	                                   "code",   "metadata", "links",    "images"};
	return names;
}

//! A table's slug: its heading's own id when it has one, else panduck's heading-id rule
//! over the title (libpanduck/include/panduck/slugify.hpp). Any ASCII character that is
//! not a letter, a digit or `_` becomes `_` (Teague, 2026-11-23: hyphens become
//! underscores, so every view name is a plain SQL identifier). Empty = no slug.
string TableSlug(const FoundTable &t) {
	string raw = !t.heading_id.empty() ? t.heading_id : (t.title.empty() ? "" : ::panduck::HeadingSlug(t.title, '_'));
	for (auto &c : raw) {
		auto u = static_cast<unsigned char>(c);
		if (u < 0x80 && !std::isalnum(u) && c != '_') {
			c = '_';
		}
	}
	return StringUtil::Lower(raw);
}

string OrNull(const string &s) {
	return s.empty() ? "NULL::VARCHAR" : Quote(s);
}

} // namespace

//! FORMAT's valid values are the registry's document formats plus 'auto', so a reader
//! registered later is attachable without a change here. 'pandoc' and 'code' are not
//! registry rows: read_panduck_doc reaches them only through `format :=` (its dispatch in
//! reader_registry.cpp has a branch for each), so they are listed by name.
string ParseFormat(const string &raw) {
	auto v = StringUtil::Lower(raw);
	vector<string> valid {"auto", "pandoc", "code"};
	for (auto &e : readers::ReaderRegistry::Get().Entries()) {
		if (e.format == "data" || std::find(valid.begin(), valid.end(), e.format) != valid.end()) {
			continue;
		}
		valid.push_back(e.format);
	}
	if (std::find(valid.begin(), valid.end(), v) == valid.end()) {
		throw InvalidInputException("panduck: FORMAT must be one of %s; got '%s'", StringUtil::Join(valid, ", "), raw);
	}
	return v;
}

//! A plain path becomes absolute, so the views do not depend on the cwd later. A local
//! scheme (zim://, git://...@rev) passes through: making it absolute would break it. A
//! network scheme fails: the brief rules out network access for this feature.
string ResolveDocPath(ClientContext &context, string path) {
	if (StringUtil::StartsWith(path, "panduck:")) {
		path = path.substr(8);
	}
	if (FileSystem::IsRemoteFile(path) || StringUtil::StartsWith(StringUtil::Lower(path), "http://") ||
	    StringUtil::StartsWith(StringUtil::Lower(path), "https://") ||
	    StringUtil::StartsWith(StringUtil::Lower(path), "s3://")) {
		throw InvalidInputException("panduck: ATTACH reads local files only; '%s' is a network address", path);
	}
	if (path.find("://") != string::npos) {
		return path;
	}
	auto &fs = FileSystem::GetFileSystem(context);
	auto expanded = fs.ExpandPath(path);
	if (!fs.IsPathAbsolute(expanded)) {
		expanded = fs.JoinPath(FileSystem::GetWorkingDirectory(), expanded);
	}
	if (!fs.FileExists(expanded)) {
		throw InvalidInputException("panduck: cannot attach '%s': file not found", path);
	}
	return expanded;
}

string ReadDocSql(const string &path, const string &format) {
	return "read_panduck_doc(" + Quote(path) + ", format := " + Quote(format) + ")";
}

//! IDENTIFY: one read of the blocks, on a fresh Connection to the same database. This is
//! identifying, not running: the table views' names depend on how many tables there are
//! and on their titles, so it must happen at ATTACH.
//!
//! A fresh Connection does not see the caller's SESSION settings (measured: with
//! SET SESSION panduck_disabled_readers = 'odt', the odt was read anyway, and its tables
//! would have been baked into views). So the caller's reader-policy settings, registered
//! in src/reader_registry.cpp, are copied to it first.
vector<FoundTable> IdentifyTables(ClientContext &context, DatabaseInstance &instance, const string &path,
                                  const string &format) {
	Connection con(instance);
	for (auto name : {"panduck_allow_registration", "panduck_enabled_readers", "panduck_disabled_readers"}) {
		// Only a value that differs is SET. Under lock_configuration any SET fails, and an
		// unconditional one made every ATTACH fail (found by the final review).
		Value caller, nested;
		if (!context.TryGetCurrentSetting(Identifier(name), caller) || caller.IsNull()) {
			continue;
		}
		if (con.context->TryGetCurrentSetting(Identifier(name), nested) && !nested.IsNull() &&
		    nested.ToString() == caller.ToString()) {
			continue;
		}
		auto set = con.Query(string("SET SESSION ") + name + " = " + caller.ToSQLString());
		if (set->HasError()) {
			// An InvalidInputException, not an InternalException: an internal error during
			// ATTACH invalidates the whole database.
			throw InvalidInputException("panduck: cannot attach '%s': cannot pass %s to the ATTACH read: %s", path,
			                            name, set->GetError());
		}
	}
	auto res = con.Query("SELECT element_type, kind, element_order, content, "
	                     "coalesce(lower(attributes['language']), ''), coalesce(attributes['id'], '') FROM " +
	                     ReadDocSql(path, format) +
	                     " WHERE element_type IN ('heading', 'table', 'code', 'caption') ORDER BY element_order");
	if (res->HasError()) {
		throw InvalidInputException("panduck: cannot attach '%s': %s", path, res->GetError());
	}
	vector<FoundTable> real, embedded;
	string heading, heading_id;
	FoundTable *last_real = nullptr;
	int64_t last_real_order = -2;
	while (auto chunk = res->Fetch()) {
		if (chunk->size() == 0) {
			break;
		}
		for (idx_t i = 0; i < chunk->size(); i++) {
			auto type = chunk->GetValue(0, i).ToString();
			auto kind = chunk->GetValue(1, i).ToString();
			auto order = chunk->GetValue(2, i).GetValue<int64_t>();
			auto content_v = chunk->GetValue(3, i);
			auto content = content_v.IsNull() ? string() : content_v.ToString();
			auto language = chunk->GetValue(4, i).ToString();
			if (type == "heading") {
				heading = content;
				heading_id = chunk->GetValue(5, i).ToString();
			} else if (type == "caption") {
				// The vocabulary's caption belongs to the container just before it.
				if (last_real && order == last_real_order + 1) {
					last_real->title = content;
					last_real->heading_id.clear();
				}
			} else if (type == "table") {
				FoundTable t;
				t.kind = "table";
				t.content = content;
				t.title = heading;
				t.heading_id = heading_id;
				t.columns = HeaderCells(content);
				t.element_order = order;
				real.push_back(std::move(t));
				last_real = &real.back();
				last_real_order = order;
			} else if (kind == "block" &&
			           (language == "csv" || language == "tsv" || language == "json" || language == "jsonl")) {
				FoundTable t;
				t.kind = language;
				t.content = content;
				t.title = heading;
				t.heading_id = heading_id;
				t.element_order = order;
				embedded.push_back(std::move(t));
			}
		}
	}
	vector<FoundTable> all;
	for (auto &t : real) {
		all.push_back(std::move(t));
	}
	for (auto &t : embedded) {
		all.push_back(std::move(t));
	}
	for (idx_t i = 0; i < all.size(); i++) {
		all[i].n = i + 1;
	}
	return all;
}

string TableViewSql(const FoundTable &t) {
	if (t.kind == "table") {
		return "SELECT * FROM panduck_table_read(" + Quote(t.content) + ")";
	}
	// A failed parse leaves content NULL. error() raises the parse reason instead of
	// panduck_table_read's "the table content is NULL" (src/table_read.cpp).
	auto e = "panduck_embedded_to_table(" + Quote(t.content) + ", " + Quote(t.kind) + ")";
	return "SELECT * FROM panduck_table_read(coalesce(" + e + ".content, error(" + e + ".error)))";
}

//! Names every table per TABLE_NAMES and returns the (view name, SQL) pairs. Sets each
//! table's `view` (the name `tables` lists): the slug, else table_N; empty under
//! 'named_only' for a table with no slug.
vector<std::pair<string, string>> NameTables(vector<FoundTable> &tables, TableNames mode) {
	// table_K is reserved for every K in every mode, so table_N is the same table in
	// 'numbered' and 'both', and a heading named "Table 1" cannot take it.
	std::unordered_set<string> taken(FixedViews().begin(), FixedViews().end());
	for (auto &t : tables) {
		taken.insert("table_" + std::to_string(t.n));
	}
	vector<std::pair<string, string>> views;
	for (auto &t : tables) {
		auto numbered = "table_" + std::to_string(t.n);
		string slug;
		if (mode != TableNames::NUMBERED) {
			auto base = TableSlug(t);
			if (!base.empty()) {
				slug = base;
				for (idx_t k = 2; taken.count(slug); k++) {
					slug = base + "_" + std::to_string(k);
				}
				taken.insert(slug);
			}
		}
		auto sql = TableViewSql(t);
		switch (mode) {
		case TableNames::NUMBERED:
			t.view = numbered;
			views.emplace_back(numbered, sql);
			break;
		case TableNames::SLUGS:
			t.view = slug.empty() ? numbered : slug;
			views.emplace_back(t.view, sql);
			break;
		case TableNames::NAMED_ONLY:
			t.view = slug;
			if (!slug.empty()) {
				views.emplace_back(slug, sql);
			}
			break;
		case TableNames::BOTH:
			t.view = slug.empty() ? numbered : slug;
			views.emplace_back(numbered, sql);
			if (!slug.empty()) {
				views.emplace_back(slug, sql);
			}
			break;
		}
	}
	return views;
}

//! `tables` is literal data baked at ATTACH, so listing it parses nothing.
string TablesListingSql(const vector<FoundTable> &tables) {
	const string cols = " t(n, view, kind, title, columns, element_order)";
	if (tables.empty()) {
		return "SELECT * FROM (VALUES (NULL::BIGINT, NULL::VARCHAR, NULL::VARCHAR, NULL::VARCHAR, "
		       "NULL::VARCHAR[], NULL::BIGINT))" +
		       cols + " WHERE false";
	}
	string sql = "SELECT * FROM (VALUES ";
	for (idx_t i = 0; i < tables.size(); i++) {
		auto &t = tables[i];
		string columns = "NULL::VARCHAR[]";
		if (t.kind == "table") {
			vector<string> quoted;
			for (auto &c : t.columns) {
				quoted.push_back(Quote(c));
			}
			columns = "[" + StringUtil::Join(quoted, ", ") + "]::VARCHAR[]";
		}
		sql += (i ? ", (" : "(") + std::to_string(t.n) + "::BIGINT, " + OrNull(t.view) + ", " + Quote(t.kind) + ", " +
		       OrNull(t.title) + ", " + columns + ", " + std::to_string(t.element_order) + "::BIGINT)";
	}
	return sql + ")" + cols;
}

} // namespace panduck_tables
} // namespace duckdb
