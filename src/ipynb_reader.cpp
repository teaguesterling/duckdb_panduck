#include "ipynb_reader.hpp"
#include "panduck_duckdb_compat.hpp"
#include "reader_registry.hpp"

#include "duck_block_types.hpp"

#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

#include <fstream>
#include <map>
#include <string>
#include <vector>

// The DuckDB-facing half of the ipynb reader. The parse core moved to
// libpanduck/src/ipynb.cpp (issue #104, L2); ipynb_reader.hpp re-exports its
// types into this namespace so nothing else had to change.

namespace duckdb {
namespace ipynb {

namespace {

struct IpynbBindData : public TableFunctionData {
	std::vector<::panduck::Block> rows;
};

struct IpynbGlobalState : public GlobalTableFunctionState {
	idx_t offset = 0;
	static unique_ptr<GlobalTableFunctionState> Init(ClientContext &, TableFunctionInitInput &) {
		return make_uniq<IpynbGlobalState>();
	}
};

void IpynbColumns(vector<LogicalType> &types, panduck::BindNames &names) {
	types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	         LogicalType::INTEGER, LogicalType::VARCHAR, LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR),
	         LogicalType::INTEGER};
	names = {"kind", "element_type", "content", "level", "encoding", "attributes", "element_order"};
}

unique_ptr<FunctionData> IpynbFileBind(ClientContext &context, TableFunctionBindInput &input,
                                       vector<LogicalType> &return_types, panduck::BindNames &names) {
	readers::RequireReaderEnabled(context, "ipynb");
	IpynbColumns(return_types, names);
	auto path = input.inputs[0].GetValue<string>();
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		throw IOException("read_ipynb_blocks: cannot open %s", path);
	}
	std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	auto result = make_uniq<IpynbBindData>();
	result->rows = ::panduck::ipynb::ReadIpynb(src);
	return std::move(result);
}

unique_ptr<FunctionData> IpynbStringBind(ClientContext &context, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, panduck::BindNames &names) {
	readers::RequireReaderEnabled(context, "ipynb");
	IpynbColumns(return_types, names);
	auto result = make_uniq<IpynbBindData>();
	result->rows = ::panduck::ipynb::ReadIpynb(input.inputs[0].GetValue<string>());
	return std::move(result);
}

void IpynbScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<IpynbBindData>();
	auto &state = input.global_state->Cast<IpynbGlobalState>();
	idx_t count = 0;
	while (state.offset < data.rows.size() && count < STANDARD_VECTOR_SIZE) {
		const auto &row = data.rows[state.offset];
		output.SetValue(0, count, Value(row.kind));
		output.SetValue(1, count, Value(row.element_type));
		// BEHAVIOUR UNCHANGED. Nullable can now express absent-vs-empty, but this
		// still collapses both to SQL NULL exactly as before; HasContent() is
		// `present AND non-empty`. Teaching a reader to emit a real empty string
		// is a separate per-reader change with its own test (panduck/block.hpp).
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

void RegisterIpynbReader(ExtensionLoader &loader) {
	{
		TableFunction file_fn("read_ipynb_blocks", {LogicalType::VARCHAR}, IpynbScan, IpynbFileBind,
		                      IpynbGlobalState::Init);
		CreateTableFunctionInfo info(std::move(file_fn));
		info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
		FunctionDescription desc;
		desc.parameter_names = {"file_path"};
		desc.description = "Read a Jupyter Notebook (.ipynb) file and return "
		                   "structured document blocks.";
		desc.examples = {"SELECT * FROM read_ipynb_blocks('notebook.ipynb')"};
		desc.categories = {"panduck"};
		info.descriptions.push_back(desc);
		loader.RegisterFunction(std::move(info));
	}

	// The string form, as the LaTeX reader has: asserting a two-line snippet is
	// how the nesting and inline rules stay readable in the tests.
	{
		TableFunction string_fn("read_ipynb_blocks_string", {LogicalType::VARCHAR}, IpynbScan, IpynbStringBind,
		                        IpynbGlobalState::Init);
		CreateTableFunctionInfo info(std::move(string_fn));
		info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
		FunctionDescription desc;
		desc.parameter_names = {"ipynb_json"};
		desc.description = "Parse a Jupyter Notebook JSON string and return "
		                   "structured document blocks.";
		desc.examples = {"SELECT * FROM read_ipynb_blocks_string('{\"cells\": [], \"metadata\": "
		                 "{}, \"nbformat\": 4, \"nbformat_minor\": 2}')"};
		desc.categories = {"panduck"};
		info.descriptions.push_back(desc);
		loader.RegisterFunction(std::move(info));
	}
}

} // namespace ipynb
} // namespace duckdb
