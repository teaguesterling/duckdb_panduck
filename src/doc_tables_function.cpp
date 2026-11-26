// doc_tables(src) and doc_table(src, n | id := ...): a document's tables in one SQL
// expression, with no ATTACH first (#215).
//
// Both are bind-replace table functions over ATTACH's own table scan (doc_table_scan.hpp).
// At bind they run the scan on the constant `src` and return the SQL ATTACH generates for
// `r.tables` and `r.table_N`. So the rows are ATTACH's by construction: the two surfaces
// cannot disagree about which table is table N.

#include "doc_tables_function.hpp"
#include "doc_table_scan.hpp"

#include "panduck_duckdb_compat.hpp"

#include "duckdb/function/table_function.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

namespace duckdb {

namespace {

using namespace panduck_tables; // NOLINT

//! `src`: one local document. A glob would read several documents under one numbering, so
//! it is refused rather than expanded.
string DocSource(ClientContext &context, const TableFunctionBindInput &input, const string &fn) {
	if (input.inputs.empty() || input.inputs[0].IsNull()) {
		throw BinderException("%s: src must be a constant path, not NULL", fn);
	}
	auto src = input.inputs[0].ToString();
	if (src.find_first_of("*?[") != string::npos && src.find("://") == string::npos) {
		throw InvalidInputException("%s reads one document; '%s' is a glob pattern", fn, src);
	}
	return ResolveDocPath(context, src);
}

string DocFormat(const TableFunctionBindInput &input) {
	auto it = input.named_parameters.find("format");
	if (it == input.named_parameters.end() || it->second.IsNull()) {
		return "auto";
	}
	return ParseFormat(it->second.ToString());
}

//! The tables, numbered and named as ATTACH's default TABLE_NAMES 'slugs' names them.
vector<FoundTable> ScanTables(ClientContext &context, const string &path, const string &format) {
	auto tables = IdentifyTables(context, DatabaseInstance::GetDatabase(context), path, format);
	NameTables(tables, TableNames::SLUGS);
	return tables;
}

unique_ptr<TableRef> SelectRef(ClientContext &context, const string &sql) {
	Parser parser(context);
	parser.ParseQuery(sql);
	if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::SELECT_STATEMENT) {
		throw InternalException("panduck: doc_tables generated a statement that is not one SELECT");
	}
	auto select = unique_ptr_cast<SQLStatement, SelectStatement>(std::move(parser.statements[0]));
	return make_uniq<SubqueryRef>(std::move(select));
}

unique_ptr<TableRef> DocTablesBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	auto path = DocSource(context, input, "doc_tables");
	auto format = DocFormat(input);
	return SelectRef(context, TablesListingSql(ScanTables(context, path, format)));
}

} // namespace

void RegisterDocTablesFunctions(ExtensionLoader &loader) {
	TableFunction doc_tables("doc_tables", {LogicalType::VARCHAR}, nullptr, nullptr);
	doc_tables.bind_replace = DocTablesBindReplace;
	panduck::AddNamedParameter(doc_tables, "format", LogicalType::VARCHAR);
	CreateTableFunctionInfo info(std::move(doc_tables));
	info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
	FunctionDescription desc;
	desc.parameter_names = {"src"};
	desc.description = "List a document's tables: the same rows as ATTACH's r.tables (n, view, kind, title, columns, "
	                   "element_order, rows, id). src must be a constant path to one local document.";
	desc.examples = {"SELECT * FROM doc_tables('report.docx')"};
	desc.categories = {"panduck"};
	info.descriptions.push_back(desc);
	loader.RegisterFunction(std::move(info));
}

} // namespace duckdb
