#include "pandoc_reader.hpp"
#include "reader_registry.hpp"
#include "panduck_duckdb_compat.hpp"

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

void PandocColumns(vector<LogicalType> &types, panduck::BindNames &names) {
	types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	         LogicalType::INTEGER, LogicalType::VARCHAR, LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR),
	         LogicalType::INTEGER};
	names = {"kind", "element_type", "content", "level", "encoding", "attributes", "element_order"};
}

unique_ptr<FunctionData> BindFromJson(const string &json, vector<LogicalType> &return_types,
                                      panduck::BindNames &names) {
	PandocColumns(return_types, names);
	auto result = make_uniq<PandocBindData>();
	// The converter's own entry point, reached in C++ rather than through a SQL name --
	// which is what lets panduck expose this without registering a name upstream owns.
	PandocBlockConvert::ConvertPandocAstToBlocks(json, result->blocks);
	return std::move(result);
}

unique_ptr<FunctionData> PandocFileBind(ClientContext &context, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, panduck::BindNames &names) {
	readers::RequireReaderEnabled(context, "pandoc");
	auto path = input.inputs[0].GetValue<string>();
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		throw IOException("read_pandoc_blocks: cannot open %s", path);
	}
	std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	return BindFromJson(json, return_types, names);
}

unique_ptr<FunctionData> PandocStringBind(ClientContext &context, TableFunctionBindInput &input,
                                          vector<LogicalType> &return_types, panduck::BindNames &names) {
	readers::RequireReaderEnabled(context, "pandoc");
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

//! panduck_pandoc_ast_to_blocks(json) -- pandoc AST text to duck_blocks, as a SCALAR.
//!
//! EXISTS BECAUSE read_pandoc_blocks_string CANNOT TAKE A COLUMN. It is a table function,
//! and DuckDB refuses a column argument to one even through LATERAL:
//!
//!   Binder Error: Table function "read_pandoc_blocks_string" does not support lateral
//!   join column parameters ... The function only supports literals as parameters.
//!
//! So a consumer holding pandoc JSON in a column -- rows fetched from a table, an API
//! response, a pipe read into a value -- had no route at all. Reported by duckeye, which
//! was unaffected only because it passes a literal '/dev/stdin'.
//!
//! THE GENERAL SHAPE, which is worth more than this function: swapping a scalar for a table
//! function is a SILENT change for every literal call site and a HARD BREAK for every column
//! one, so the blast radius is invisible at the swap and shows up per-consumer later. The
//! same wall broke duckeye's ZIM path for weeks, and its suite stayed green because the one
//! assertion expected a non-zero exit -- and a Binder Error is also non-zero.
//!
//! Returns a LIST rather than rows: that is what a scalar can do, and it composes with
//! unnest() when rows are wanted. Named panduck_-prefixed because duck_block_utils owns the
//! bare `pandoc_ast_to_blocks`, and a name belongs to exactly one extension in this family.
static void PandocAstToBlocksFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	readers::RequireReaderEnabled(context, "pandoc");
	UnifiedVectorFormat input;
	args.data[0].ToUnifiedFormat(args.size(), input);
	auto json_text = UnifiedVectorFormat::GetData<string_t>(input);
	for (idx_t i = 0; i < args.size(); i++) {
		auto idx = input.sel->get_index(i);
		if (!input.validity.RowIsValid(idx)) {
			result.SetValue(i, Value());
			continue;
		}
		vector<Value> blocks;
		PandocBlockConvert::ConvertPandocAstToBlocks(json_text[idx].GetString(), blocks);
		result.SetValue(i, Value::LIST(DuckBlockTypes::DuckBlockType(), blocks));
	}
}

void RegisterPandocReader(ExtensionLoader &loader) {
	TableFunction file_fn("read_pandoc_blocks", {LogicalType::VARCHAR}, PandocScan, PandocFileBind,
	                      PandocGlobalState::Init);
	loader.RegisterFunction(file_fn);

	// The string form, as every other panduck reader has: asserting a construct without a
	// fixture on disk is how the mapping rules stay readable in the tests.
	TableFunction string_fn("read_pandoc_blocks_string", {LogicalType::VARCHAR}, PandocScan, PandocStringBind,
	                        PandocGlobalState::Init);
	loader.RegisterFunction(string_fn);

	loader.RegisterFunction(ScalarFunction("panduck_pandoc_ast_to_blocks", {LogicalType::VARCHAR},
	                                       DuckBlockTypes::DuckBlockListType(), PandocAstToBlocksFun));
}

} // namespace duckdb
