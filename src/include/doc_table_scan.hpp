#pragma once

// The table scan behind ATTACH ... (TYPE panduck) and doc_tables / doc_table: which tables a
// document has, in which order, under which names, and the SQL that reads each. One scan
// serves every surface, so they cannot disagree about which table is table N (#215).

#include "duckdb.hpp"

namespace duckdb {
namespace panduck_tables {

enum class TableNames { SLUGS, NUMBERED, NAMED_ONLY, BOTH };

//! One table found at ATTACH: a real `table` block, or a csv/tsv/json/jsonl code block.
struct FoundTable {
	idx_t n = 0;            //!< 1-based row in `tables`: real tables first, then embedded data
	string kind;            //!< 'table', 'csv', 'tsv', 'json' or 'jsonl'
	string content;         //!< the table JSON, or the embedded text
	string title;           //!< caption, else the nearest heading above; empty = none
	string heading_id;      //!< that heading's attributes['id'], if it had one
	vector<string> columns; //!< header cells, real tables only
	int64_t element_order = 0;
	string view; //!< the view name; empty = no view (TABLE_NAMES 'named_only')
};

//! FORMAT's value, lower-cased and checked against the reader registry.
string ParseFormat(const string &raw);
//! A local path made absolute; a local scheme passed through; a network address refused.
string ResolveDocPath(ClientContext &context, string path);
//! `read_panduck_doc(<path>, format := <format>)`, for embedding in SQL.
string ReadDocSql(const string &path, const string &format);
//! One read of the blocks, on a fresh Connection to `instance`. Real tables first, then
//! embedded data, numbered from 1.
vector<FoundTable> IdentifyTables(ClientContext &context, DatabaseInstance &instance, const string &path,
                                  const string &format);
//! The SELECT that reads one table's rows.
string TableViewSql(const FoundTable &t);
//! Names every table per `mode`, sets each table's `view`, returns the (view name, SQL) pairs.
vector<std::pair<string, string>> NameTables(vector<FoundTable> &tables, TableNames mode);
//! The `tables` listing, as literal data.
string TablesListingSql(const vector<FoundTable> &tables);

} // namespace panduck_tables
} // namespace duckdb
