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
#include "doc_table_scan.hpp"

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

namespace duckdb {

namespace {

using namespace panduck_tables; // NOLINT -- the scan ATTACH shares with doc_tables (#215)

//! Everything ATTACH decides. Later stages only append to `views`.
struct AttachPlan {
	string path;   //!< absolute, or a local scheme passed through unchanged
	string format; //!< 'auto' or a reader-registry format name
	TableNames mode = TableNames::SLUGS;
	vector<std::pair<string, string>> views; //!< (view name, SELECT ...), in listing order
};

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
	throw InvalidInputException("panduck: TABLE_NAMES must be one of slugs, numbered, named_only, both; got '%s'", raw);
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

//! The content views. Each reads the document again through read_panduck_doc, not the
//! `blocks` view, so it does not depend on how the attached name resolves. Attribute keys
//! and the metadata shape were measured across readers first (attach_views.test header).
vector<std::pair<string, string>> ContentViews(const AttachPlan &plan) {
	const string b = "WITH b AS (SELECT * FROM " + ReadDocSql(plan.path, plan.format) + ") ";
	const string h = "h AS (SELECT TRY_CAST(attributes['heading_level'] AS INTEGER) AS level, content AS title, "
	                 "attributes['id'] AS id, element_order FROM b WHERE element_type = 'heading') ";
	return {
	    {"headings", b + ", " + h + "SELECT level, title, id, element_order FROM h ORDER BY element_order"},
	    // A section ends at the next heading of the same or a higher level (doc_section's rule),
	    // else at the last row of the document.
	    {"sections", b + ", " + h +
	                     "SELECT id, level, title, element_order AS start_order, "
	                     "coalesce((SELECT min(h2.element_order) FROM h h2 WHERE h2.element_order > h.element_order "
	                     "AND h2.level <= h.level) - 1, (SELECT max(element_order) FROM b)) AS end_order "
	                     "FROM h ORDER BY element_order"},
	    {"code", b + ", " + h +
	                 "SELECT attributes['language'] AS language, content, element_order, "
	                 "(SELECT arg_max(h.title, h.element_order) FROM h WHERE h.element_order < c.element_order) "
	                 "AS section FROM b c WHERE c.kind = 'block' AND c.element_type = 'code' ORDER BY element_order"},
	    // A metadata value is a subtree: the keyed value row, then the inline text under it,
	    // up to the next row at the same or a lower level.
	    {"metadata", b + ", m AS (SELECT attributes['key'] AS key, level, element_order FROM b "
	                     "WHERE kind = 'value' AND attributes['key'] IS NOT NULL) "
	                     "SELECT m.key, (SELECT string_agg(x.content, '' ORDER BY x.element_order) FROM b x "
	                     "WHERE x.kind = 'inline' AND x.element_order > m.element_order AND x.element_order < "
	                     "coalesce((SELECT min(y.element_order) FROM b y WHERE y.element_order > m.element_order "
	                     "AND y.level <= m.level), 2147483647)) AS value FROM m ORDER BY m.element_order"},
	    {"links", b + "SELECT content AS text, attributes['href'] AS address, attributes['title'] AS title, "
	                  "element_order FROM b WHERE element_type = 'link' ORDER BY element_order"},
	    {"images", b + "SELECT content AS alt, attributes['src'] AS address, attributes['title'] AS title, "
	                   "element_order FROM b WHERE element_type = 'image' ORDER BY element_order"},
	};
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
	plan.path = ResolveDocPath(context, info.path);

	plan.views.emplace_back("blocks", "SELECT * FROM " + ReadDocSql(plan.path, plan.format));

	auto tables = IdentifyTables(context, db.GetDatabase(), plan.path, plan.format);
	auto table_views = NameTables(tables, plan.mode);
	plan.views.emplace_back("tables", TablesListingSql(tables));
	for (auto &v : ContentViews(plan)) {
		plan.views.push_back(std::move(v));
	}
	for (auto &v : table_views) {
		plan.views.push_back(std::move(v));
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
