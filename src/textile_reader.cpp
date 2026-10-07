#include "textile_reader.hpp"
#include "panduck_duckdb_compat.hpp"
#include "reader_registry.hpp"

#include "duck_block_types.hpp"

#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

#include <fstream>
#include <string>
#include <vector>

// The DuckDB-facing half of the textile reader. The parse core, the scanner and
// the row flattening moved to libpanduck (issue #104, L2); textile_reader.hpp
// re-exports their types into this namespace so nothing else had to change.

namespace duckdb {
namespace textile {

namespace {

struct TxBindData : public TableFunctionData {
	std::vector<::panduck::Block> rows;
};

struct TxGlobalState : public GlobalTableFunctionState {
	idx_t offset = 0;
	static unique_ptr<GlobalTableFunctionState> Init(ClientContext &, TableFunctionInitInput &) {
		return make_uniq<TxGlobalState>();
	}
};

void TxColumns(vector<LogicalType> &types, panduck::BindNames &names) {
	types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	         LogicalType::INTEGER, LogicalType::VARCHAR, LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR),
	         LogicalType::INTEGER};
	names = {"kind", "element_type", "content", "level", "encoding", "attributes", "element_order"};
}
unique_ptr<FunctionData> TxFileBind(ClientContext &context, TableFunctionBindInput &input,
                                    vector<LogicalType> &return_types, panduck::BindNames &names) {
	readers::RequireReaderEnabled(context, "textile");
	TxColumns(return_types, names);
	auto path = input.inputs[0].GetValue<string>();
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		throw IOException("read_textile_blocks: cannot open %s", path);
	}
	std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	auto result = make_uniq<TxBindData>();
	result->rows = ::panduck::textile::ReadTextile(src);
	return std::move(result);
}

unique_ptr<FunctionData> TxStringBind(ClientContext &context, TableFunctionBindInput &input,
                                      vector<LogicalType> &return_types, panduck::BindNames &names) {
	readers::RequireReaderEnabled(context, "textile");
	TxColumns(return_types, names);
	auto result = make_uniq<TxBindData>();
	result->rows = ::panduck::textile::ReadTextile(input.inputs[0].GetValue<string>());
	return std::move(result);
}

void TxScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<TxBindData>();
	auto &state = input.global_state->Cast<TxGlobalState>();
	idx_t count = 0;
	while (state.offset < data.rows.size() && count < STANDARD_VECTOR_SIZE) {
		const auto &row = data.rows[state.offset];
		output.SetValue(0, count, Value(row.kind));
		output.SetValue(1, count, Value(row.element_type));
		// Behaviour unchanged: HasContent() is `present AND non-empty`, so absent and
		// empty still both become SQL NULL. See panduck/block.hpp.
		output.SetValue(2, count, row.content.HasContent() ? Value(row.content.value) : Value(LogicalType::VARCHAR));
		output.SetValue(3, count, Value::INTEGER(row.level));
		output.SetValue(4, count, Value(row.encoding));
		output.SetValue(5, count, DuckBlockTypes::CreateAttributesMap(row.attributes));
		output.SetValue(6, count, Value::INTEGER(row.element_order));
		state.offset++;
		count++;
	}
	output.SetCardinality(count);
}

} // namespace

void RegisterTextileReader(ExtensionLoader &loader) {
	{
		TableFunction file_fn("read_textile_blocks", {LogicalType::VARCHAR}, TxScan, TxFileBind, TxGlobalState::Init);
		CreateTableFunctionInfo info(std::move(file_fn));
		info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
		FunctionDescription desc;
		desc.parameter_names = {"file_path"};
		desc.description = "Read a Textile document and return structured document blocks.";
		desc.examples = {"SELECT * FROM read_textile_blocks('document.textile')"};
		desc.categories = {"panduck"};
		info.descriptions.push_back(desc);
		loader.RegisterFunction(std::move(info));
	}

	{
		TableFunction string_fn("read_textile_blocks_string", {LogicalType::VARCHAR}, TxScan, TxStringBind,
		                        TxGlobalState::Init);
		CreateTableFunctionInfo info(std::move(string_fn));
		info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
		FunctionDescription desc;
		desc.parameter_names = {"textile_text"};
		desc.description = "Parse a Textile string and return structured document blocks.";
		desc.examples = {"SELECT * FROM read_textile_blocks_string('h1. Header\\n\\n*bold*')"};
		desc.categories = {"panduck"};
		info.descriptions.push_back(desc);
		loader.RegisterFunction(std::move(info));
	}
}

} // namespace textile
} // namespace duckdb
