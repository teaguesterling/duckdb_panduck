// ATTACH '<doc>' AS r (TYPE panduck): a document as a read-only database of views.
//
// Spec: docs/superpowers/specs/2026-11-23-attach-documents-design.md. The governing rule is
// Teague's (2026-11-13): "eager to identify and list, lazy to run". ATTACH builds only
// (view name -> SQL) pairs. The DefaultGenerator below turns a name into a view when
// something asks for it, so listing names binds nothing (measured by the 2026-11-23 spike).
//
// The shape copies DuckDB's own OpenFileStorageExtension
// (duckdb/src/storage/open_file_storage_extension.cpp): an in-memory DuckCatalog, made
// read-only, whose view set has a default generator.

#include "attach_storage.hpp"

#include "reader_registry.hpp"

#include "duckdb/catalog/catalog_entry/duck_schema_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/catalog/default/default_generator.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/parser/parsed_data/create_view_info.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"
#include "duckdb/main/connection.hpp"
#include "yyjson.hpp"

#include <algorithm>

namespace duckdb {

namespace {

enum class TableNames { SLUGS, NUMBERED, NAMED_ONLY, BOTH };

//! Everything ATTACH decides. Later stages only append to `views`.
struct AttachPlan {
	string path;   //!< absolute, or a local scheme passed through unchanged
	string format; //!< 'auto' or a reader-registry format name
	TableNames mode = TableNames::SLUGS;
	vector<std::pair<string, string>> views; //!< (view name, SELECT ...), in listing order
};

string Quote(const string &s) {
	return SQLString::ToString(s);
}

//! One table found at ATTACH: a real `table` block, or a csv/tsv/json/jsonl code block.
struct FoundTable {
	idx_t n = 0;          //!< 1-based row in `tables`: real tables first, then embedded data
	string kind;          //!< 'table', 'csv', 'tsv', 'json' or 'jsonl'
	string content;       //!< the table JSON, or the embedded text
	string title;         //!< caption, else the nearest heading above; empty = none
	string heading_id;    //!< that heading's attributes['id'], if it had one
	vector<string> columns; //!< header cells, real tables only
	int64_t element_order = 0;
	string view;          //!< the view name; empty = no view (TABLE_NAMES 'named_only')
};

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

TableNames ParseTableNames(const string &raw) {
	auto v = StringUtil::Lower(raw);
	if (v == "slugs") {
		return TableNames::SLUGS;
	}
	if (v == "numbered") {
		return TableNames::NUMBERED;
	}
	if (v == "named_only") {
		return TableNames::NAMED_ONLY;
	}
	if (v == "both") {
		return TableNames::BOTH;
	}
	throw InvalidInputException("panduck: TABLE_NAMES must be one of slugs, numbered, named_only, both; got '%s'",
	                            raw);
}

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
		throw InvalidInputException("panduck: FORMAT must be one of %s; got '%s'", StringUtil::Join(valid, ", "),
		                            raw);
	}
	return v;
}

//! A plain path becomes absolute, so the views do not depend on the cwd later. A local
//! scheme (zim://, git://...@rev) passes through: making it absolute would break it. A
//! network scheme fails: the brief rules out network access for this feature.
string ResolvePath(ClientContext &context, string path) {
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

//! Erase panduck's own options: DuckDB's storage manager rejects any key it does not know
//! (storage_manager.cpp:147), and it runs after this callback. Keys arrive lower-cased.
void TakeOptions(AttachOptions &options, AttachPlan &plan) {
	string format = "auto";
	for (auto it = options.options.begin(); it != options.options.end();) {
		if (it->first == "table_names") {
			plan.mode = ParseTableNames(it->second.ToString());
			it = options.options.erase(it);
		} else if (it->first == "format") {
			format = it->second.ToString();
			it = options.options.erase(it);
		} else {
			++it;
		}
	}
	plan.format = ParseFormat(format);
}

string ReadDocSql(const AttachPlan &plan) {
	return "read_panduck_doc(" + Quote(plan.path) + ", format := " + Quote(plan.format) + ")";
}

//! IDENTIFY: one read of the blocks, on a fresh Connection to the same database. This is
//! identifying, not running: the table views' names depend on how many tables there are
//! and on their titles, so it must happen at ATTACH.
vector<FoundTable> IdentifyTables(DatabaseInstance &instance, const AttachPlan &plan) {
	Connection con(instance);
	auto res = con.Query("SELECT element_type, kind, element_order, content, "
	                     "coalesce(lower(attributes['language']), ''), coalesce(attributes['id'], '') FROM " +
	                     ReadDocSql(plan) +
	                     " WHERE element_type IN ('heading', 'table', 'code', 'caption') ORDER BY element_order");
	if (res->HasError()) {
		throw InvalidInputException("panduck: cannot attach '%s': %s", plan.path, res->GetError());
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

//! The view names. Every table is table_N here; TABLE_NAMES modes come next.
void NameTables(vector<FoundTable> &tables, TableNames) {
	for (auto &t : tables) {
		t.view = "table_" + std::to_string(t.n);
	}
}

string OrNull(const string &s) {
	return s.empty() ? "NULL::VARCHAR" : Quote(s);
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
		sql += (i ? ", (" : "(") + std::to_string(t.n) + "::BIGINT, " + OrNull(t.view) + ", " + Quote(t.kind) +
		       ", " + OrNull(t.title) + ", " + columns + ", " + std::to_string(t.element_order) + "::BIGINT)";
	}
	return sql + ")" + cols;
}

class DocViewGenerator : public DefaultGenerator {
public:
	DocViewGenerator(Catalog &catalog, SchemaCatalogEntry &schema, vector<std::pair<string, string>> views_p)
	    : DefaultGenerator(catalog), schema(schema), views(std::move(views_p)) {
	}

	unique_ptr<CatalogEntry> CreateDefaultEntry(ClientContext &, const Identifier &entry_name) override {
		for (auto &v : views) {
			if (entry_name == Identifier(v.first)) {
				auto info = make_uniq<CreateViewInfo>();
				info->SetQualifiedName(QualifiedName({Identifier::DefaultSchema()}, Identifier(v.first)));
				info->sql = v.second;
				auto parser = Parser::GetBuiltinParser();
				auto view_info = CreateViewInfo::FromSelect(parser, std::move(info));
				return make_uniq_base<CatalogEntry, ViewCatalogEntry>(catalog, schema, *view_info);
			}
		}
		return nullptr;
	}

	vector<Identifier> GetDefaultEntries() override {
		vector<Identifier> names;
		for (auto &v : views) {
			names.emplace_back(v.first);
		}
		return names;
	}

private:
	SchemaCatalogEntry &schema;
	vector<std::pair<string, string>> views;
};

unique_ptr<Catalog> PanduckAttach(optional_ptr<StorageExtensionInfo>, ClientContext &context, AttachedDatabase &db,
                                  const string &, AttachInfo &info, AttachOptions &options) {
	AttachPlan plan;
	TakeOptions(options, plan);
	plan.path = ResolvePath(context, info.path);

	plan.views.emplace_back("blocks", "SELECT * FROM " + ReadDocSql(plan));

	auto tables = IdentifyTables(db.GetDatabase(), plan);
	NameTables(tables, plan.mode);
	plan.views.emplace_back("tables", TablesListingSql(tables));
	for (auto &t : tables) {
		if (!t.view.empty()) {
			plan.views.emplace_back(t.view, TableViewSql(t));
		}
	}

	db.SetReadOnlyDatabase();
	options.default_table = QualifiedName::Parse("blocks");
	info.path = ":memory:";
	options.access_mode = AccessMode::READ_WRITE;

	auto catalog = make_uniq<DuckCatalog>(db);
	catalog->Initialize(false);
	auto sys = CatalogTransaction::GetSystemTransaction(db.GetDatabase());
	auto &schema = catalog->GetSchema(sys, Identifier::DefaultSchema());
	auto &set = schema.Cast<DuckSchemaEntry>().GetCatalogSet(CatalogType::VIEW_ENTRY);
	set.SetDefaultGenerator(make_uniq<DocViewGenerator>(*catalog, schema, std::move(plan.views)));
	return std::move(catalog);
}

unique_ptr<TransactionManager> PanduckTransactionManager(optional_ptr<StorageExtensionInfo>, AttachedDatabase &db,
                                                         Catalog &) {
	return make_uniq<DuckTransactionManager>(db);
}

} // namespace

void RegisterAttachStorage(ExtensionLoader &loader) {
	auto ext = make_shared_ptr<StorageExtension>();
	ext->attach = PanduckAttach;
	ext->create_transaction_manager = PanduckTransactionManager;
	StorageExtension::Register(DBConfig::GetConfig(loader.GetDatabaseInstance()), "panduck", std::move(ext));
}

} // namespace duckdb
