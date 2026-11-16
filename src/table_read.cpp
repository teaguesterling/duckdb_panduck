#include "table_read.hpp"
#include "duckdb_compat.hpp"
#include "panduck_duckdb_compat.hpp"

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "yyjson.hpp"

namespace duckdb {
namespace panduck {

using namespace duckdb_yyjson; // NOLINT -- the spelling the json extension uses

namespace {

// The sniffer's ladder, narrowest first. A column takes the first type that
// every non-empty cell casts to under a strict cast; otherwise VARCHAR.
//
// BOOLEAN is last and accepts only "true" / "false" (any case). DuckDB's own
// VARCHAR -> BOOLEAN cast also takes 't', 'yes', '1' and more, which would
// turn a column of single letters into booleans.
//
// DATE comes before TIMESTAMP because every date also casts to a timestamp.
// The strict DATE cast rejects a time part, so a column with one timestamp
// in it stays TIMESTAMP rather than narrowing to DATE.
enum class Sniffed { BIGINT, DOUBLE, DATE, TIMESTAMP, BOOLEAN, VARCHAR };

bool IsBooleanWord(const string &s) {
	return StringUtil::CIEquals(s, "true") || StringUtil::CIEquals(s, "false");
}

bool Fits(Sniffed t, const string &s) {
	string_t in(s.c_str(), UnsafeNumericCast<uint32_t>(s.size()));
	switch (t) {
	case Sniffed::BIGINT: {
		int64_t r;
		return TryCast::Operation(in, r, true);
	}
	case Sniffed::DOUBLE: {
		double r;
		return TryCast::Operation(in, r, true);
	}
	case Sniffed::DATE: {
		date_t r;
		return TryCast::Operation(in, r, true);
	}
	case Sniffed::TIMESTAMP: {
		timestamp_t r;
		return TryCast::Operation(in, r, true);
	}
	case Sniffed::BOOLEAN:
		return IsBooleanWord(s);
	case Sniffed::VARCHAR:
		return true;
	}
	return false;
}

LogicalType ToLogical(Sniffed t) {
	switch (t) {
	case Sniffed::BIGINT:
		return LogicalType::BIGINT;
	case Sniffed::DOUBLE:
		return LogicalType::DOUBLE;
	case Sniffed::DATE:
		return LogicalType::DATE;
	case Sniffed::TIMESTAMP:
		return LogicalType::TIMESTAMP;
	case Sniffed::BOOLEAN:
		return LogicalType::BOOLEAN;
	default:
		return LogicalType::VARCHAR;
	}
}

Value ToValue(Sniffed t, const string &s) {
	string_t in(s.c_str(), UnsafeNumericCast<uint32_t>(s.size()));
	switch (t) {
	case Sniffed::BIGINT: {
		int64_t r = 0;
		TryCast::Operation(in, r, true);
		return Value::BIGINT(r);
	}
	case Sniffed::DOUBLE: {
		double r = 0;
		TryCast::Operation(in, r, true);
		return Value::DOUBLE(r);
	}
	case Sniffed::DATE: {
		date_t r;
		TryCast::Operation(in, r, true);
		return Value::DATE(r);
	}
	case Sniffed::TIMESTAMP: {
		timestamp_t r;
		TryCast::Operation(in, r, true);
		return Value::TIMESTAMP(r);
	}
	case Sniffed::BOOLEAN:
		return Value::BOOLEAN(StringUtil::CIEquals(s, "true"));
	default:
		return Value(s);
	}
}

// A cell's text, or false for NULL. The forward direction
// (duck_block_json_to_table) writes NULL as an empty string, so an empty cell
// reads back as NULL in every column type, VARCHAR included. A JSON null, a bare
// number (read as its literal text) or a boolean is accepted as well, for
// content another producer wrote. A nested array or object is not a cell value
// and reads as NULL.
bool CellText(yyjson_val *v, string &out) {
	if (yyjson_is_str(v) || yyjson_is_raw(v)) {
		out = string(yyjson_is_str(v) ? yyjson_get_str(v) : yyjson_get_raw(v), yyjson_get_len(v));
		return !out.empty();
	}
	if (yyjson_is_bool(v)) {
		out = yyjson_get_bool(v) ? "true" : "false";
		return true;
	}
	return false;
}

// The elements of a JSON array, in order. yyjson_arr_get is a linear walk, so
// a row is read once into a vector rather than indexed cell by cell.
vector<yyjson_val *> Elements(yyjson_val *arr) {
	vector<yyjson_val *> out;
	size_t idx, max;
	yyjson_val *item;
	yyjson_arr_foreach(arr, idx, max, item) {
		out.push_back(item);
	}
	return out;
}

// The most NULL cells a ragged table may gain from padding, unless the table
// already holds more real cells than that.
constexpr idx_t MAX_PADDED_CELLS = 1000000;

struct TableReadBindData : public TableFunctionData {
	vector<vector<Value>> columns; // columns[col][row]
	idx_t row_count = 0;
};

struct TableReadState : public GlobalTableFunctionState {
	idx_t offset = 0;
};

// Frees the parsed document on every exit from the bind, the throws included.
struct DocGuard {
	yyjson_doc *doc;
	~DocGuard() {
		if (doc) {
			yyjson_doc_free(doc);
		}
	}
};

unique_ptr<FunctionData> TableReadBind(ClientContext &context, TableFunctionBindInput &input,
                                       vector<LogicalType> &return_types, BindNames &names) {
	auto &arg = input.inputs[0];
	if (arg.IsNull()) {
		throw InvalidInputException("panduck_table_read: the table content is NULL");
	}
	auto text = arg.GetValue<string>();
	// Numbers are read as raw text, so a cell keeps "2.50", not 2.5.
	DocGuard guard {yyjson_read(text.data(), text.size(), YYJSON_READ_NUMBER_AS_RAW)};
	yyjson_val *root = guard.doc ? yyjson_doc_get_root(guard.doc) : nullptr;
	yyjson_val *headers_v = yyjson_is_obj(root) ? yyjson_obj_get(root, "headers") : nullptr;
	if (!yyjson_is_arr(headers_v)) {
		throw InvalidInputException("panduck_table_read: expected a table block's content, "
		                            "{\"headers\": [...], \"rows\": [[...]]}, with \"headers\" an array");
	}
	auto headers = Elements(headers_v);
	yyjson_val *rows_v = yyjson_obj_get(root, "rows");
	vector<vector<yyjson_val *>> rows;
	vector<bool> row_is_array;
	if (yyjson_is_arr(rows_v)) {
		size_t idx, max;
		yyjson_val *row;
		yyjson_arr_foreach(rows_v, idx, max, row) {
			row_is_array.push_back(yyjson_is_arr(row));
			rows.push_back(yyjson_is_arr(row) ? Elements(row) : vector<yyjson_val *>());
		}
	}

	// The column count is the widest of the headers and every row, so a long
	// row adds a column rather than lose cells. A short row pads with NULL.
	idx_t ncols = headers.size();
	idx_t present = 0;
	for (auto &row : rows) {
		ncols = MaxValue<idx_t>(ncols, row.size());
		present += row.size();
	}

	// Padding is the one place output can outgrow input: one wide row and many
	// short ones make rows x width cells from a few kilobytes. Present cells
	// are bounded by the input's size; invented ones are capped here.
	idx_t padded = rows.size() * ncols - present;
	if (padded > MaxValue<idx_t>(MAX_PADDED_CELLS, present)) {
		throw InvalidInputException("panduck_table_read: %llu rows x %llu columns would need %llu padding cells "
		                            "for %llu present cells; the limit is max(%llu, present)",
		                            (unsigned long long)rows.size(), (unsigned long long)ncols,
		                            (unsigned long long)padded, (unsigned long long)present,
		                            (unsigned long long)MAX_PADDED_CELLS);
	}

	// Cell text per column; an absent string is NULL.
	auto result = make_uniq<TableReadBindData>();
	result->row_count = rows.size();
	vector<vector<std::pair<bool, string>>> cells(ncols);
	for (idx_t c = 0; c < ncols; c++) {
		cells[c].resize(rows.size());
	}
	for (idx_t r = 0; r < rows.size(); r++) {
		for (idx_t c = 0; c < rows[r].size(); c++) {
			cells[c][r].first = CellText(rows[r][c], cells[c][r].second);
		}
	}

	// Names: an empty or duplicate header becomes column_<n>, n the 1-based
	// position. DuckDB compares column names case-insensitively, so the
	// duplicate check does too.
	case_insensitive_set_t taken;
	for (idx_t c = 0; c < ncols; c++) {
		string name;
		if (c < headers.size()) {
			CellText(headers[c], name);
		}
		if (name.empty() || taken.count(name)) {
			name = "column_" + std::to_string(c + 1);
			while (taken.count(name)) {
				name += "_";
			}
		}
		taken.insert(name);
		// A runtime name needs the explicit conversion; see BindNames.
		names.push_back(typename BindNames::value_type(name));
	}

	static const Sniffed LADDER[] = {Sniffed::BIGINT, Sniffed::DOUBLE, Sniffed::DATE, Sniffed::TIMESTAMP,
	                                 Sniffed::BOOLEAN};
	result->columns.resize(ncols);
	for (idx_t c = 0; c < ncols; c++) {
		bool any = false;
		for (auto &cell : cells[c]) {
			any = any || cell.first;
		}
		Sniffed type = Sniffed::VARCHAR;
		if (any) {
			for (auto candidate : LADDER) {
				bool all_fit = true;
				for (auto &cell : cells[c]) {
					if (cell.first && !Fits(candidate, cell.second)) {
						all_fit = false;
						break;
					}
				}
				if (all_fit) {
					type = candidate;
					break;
				}
			}
		}
		auto logical = ToLogical(type);
		return_types.push_back(logical);
		auto &col = result->columns[c];
		col.reserve(cells[c].size());
		for (auto &cell : cells[c]) {
			col.push_back(cell.first ? ToValue(type, cell.second) : Value(logical));
		}
	}
	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> TableReadInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<TableReadState>();
}

void TableReadScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &bind = data.bind_data->Cast<TableReadBindData>();
	auto &state = data.global_state->Cast<TableReadState>();
	idx_t count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, bind.row_count - state.offset);
	for (idx_t c = 0; c < bind.columns.size(); c++) {
		for (idx_t i = 0; i < count; i++) {
			output.data[c].SetValue(i, bind.columns[c][state.offset + i]);
		}
	}
	state.offset += count;
	CompatSetOutputCardinality(output, count);
}

} // namespace

void RegisterTableReadFunctions(ExtensionLoader &loader) {
	TableFunction fn("panduck_table_read", {LogicalType::VARCHAR}, TableReadScan, TableReadBind, TableReadInit);
	CreateTableFunctionInfo info(std::move(fn));
	info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
	FunctionDescription desc;
	desc.parameter_names = {"content"};
	desc.description =
	    "Read a table block's content ({\"headers\": [...], \"rows\": [[...]]}) back into a relation. Column types "
	    "are sniffed: BIGINT, DOUBLE, DATE, TIMESTAMP, BOOLEAN, else VARCHAR. An empty cell reads as NULL. The "
	    "argument must be a constant, such as getvariable().";
	desc.examples = {"SET VARIABLE t = (SELECT content FROM blocks WHERE element_type = 'table' LIMIT 1); "
	                 "SELECT * FROM panduck_table_read(getvariable('t'))"};
	desc.categories = {"panduck"};
	info.descriptions.push_back(desc);
	loader.RegisterFunction(std::move(info));
}

} // namespace panduck
} // namespace duckdb
