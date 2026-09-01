#include "pandoc_reader.hpp"

#include "duck_block_types.hpp"
#include "pandoc_block_convert.hpp"

#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <fstream>

namespace duckdb {
namespace {

struct PandocBindData : public TableFunctionData {
	vector<Value> blocks;
};

struct PandocGlobalState : public GlobalTableFunctionState {
	idx_t offset = 0;
	static unique_ptr<GlobalTableFunctionState> Init(ClientContext &, TableFunctionInitInput &) {
		return make_uniq<PandocGlobalState>();
	}
};

void PandocColumns(vector<LogicalType> &types, vector<string> &names) {
	types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::INTEGER,
	         LogicalType::VARCHAR, LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR),
	         LogicalType::INTEGER};
	names = {"kind", "element_type", "content", "level", "encoding", "attributes", "element_order"};
}

unique_ptr<FunctionData> BindFromJson(const string &json, vector<LogicalType> &return_types, vector<string> &names) {
	PandocColumns(return_types, names);
	auto result = make_uniq<PandocBindData>();
	// The converter's own entry point, reached in C++ rather than through a SQL name --
	// which is what lets panduck expose this without registering a name upstream owns.
	PandocBlockConvert::ConvertPandocAstToBlocks(json, result->blocks);
	return std::move(result);
}

unique_ptr<FunctionData> PandocFileBind(ClientContext &, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<string> &names) {
	auto path = input.inputs[0].GetValue<string>();
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		throw IOException("read_pandoc_blocks: cannot open %s", path);
	}
	std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	return BindFromJson(json, return_types, names);
}

unique_ptr<FunctionData> PandocStringBind(ClientContext &, TableFunctionBindInput &input,
                                          vector<LogicalType> &return_types, vector<string> &names) {
	return BindFromJson(input.inputs[0].GetValue<string>(), return_types, names);
}

//! Unpack each duck_block STRUCT into the flat columns every panduck reader emits. The
//! converter produces a LIST of STRUCTs; a table function's job here is to make those rows
//! so a predicate has something to push into.
void PandocScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<PandocBindData>();
	auto &state = input.global_state->Cast<PandocGlobalState>();
	idx_t count = 0;
	while (state.offset < data.blocks.size() && count < STANDARD_VECTOR_SIZE) {
		auto &children = StructValue::GetChildren(data.blocks[state.offset]);
		for (idx_t col = 0; col < 7 && col < children.size(); col++) {
			output.SetValue(col, count, children[col]);
		}
		state.offset++;
		count++;
	}
	output.SetCardinality(count);
}

} // namespace

void RegisterPandocReader(ExtensionLoader &loader) {
	TableFunction file_fn("read_pandoc_blocks", {LogicalType::VARCHAR}, PandocScan, PandocFileBind,
	                      PandocGlobalState::Init);
	loader.RegisterFunction(file_fn);

	// The string form, as every other panduck reader has: asserting a construct without a
	// fixture on disk is how the mapping rules stay readable in the tests.
	TableFunction string_fn("read_pandoc_blocks_string", {LogicalType::VARCHAR}, PandocScan, PandocStringBind,
	                        PandocGlobalState::Init);
	loader.RegisterFunction(string_fn);
}

} // namespace duckdb
