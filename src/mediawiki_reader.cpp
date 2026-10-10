#include "mediawiki_reader.hpp"
#include "panduck_duckdb_compat.hpp"
#include "reader_registry.hpp"

#include "duck_block_types.hpp"

#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

#include <string>
#include <vector>

// The DuckDB-facing half of the MediaWiki reader. The parse core, the scanner
// and the row flattening moved to libpanduck (issue #104, L2);
// mediawiki_reader.hpp re-exports their types into this namespace so nothing
// else had to change.

namespace duckdb {
namespace mediawiki {

namespace {

struct MwBindData : public TableFunctionData {
	std::vector<::panduck::Block> rows;
};

struct MwGlobalState : public GlobalTableFunctionState {
	idx_t offset = 0;
	static unique_ptr<GlobalTableFunctionState> Init(ClientContext &, TableFunctionInitInput &) {
		return make_uniq<MwGlobalState>();
	}
};

void MwColumns(vector<LogicalType> &types, panduck::BindNames &names) {
	types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	         LogicalType::INTEGER, LogicalType::VARCHAR, LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR),
	         LogicalType::INTEGER};
	names = {"kind", "element_type", "content", "level", "encoding", "attributes", "element_order"};
}

unique_ptr<FunctionData> MwFileBind(ClientContext &context, TableFunctionBindInput &input,
                                    vector<LogicalType> &return_types, panduck::BindNames &names) {
	readers::RequireReaderEnabled(context, "mediawiki");
	MwColumns(return_types, names);
	auto path = input.inputs[0].GetValue<string>();
	auto src = readers::ReadFileThroughVFS(context, path, "read_mediawiki_blocks");
	auto result = make_uniq<MwBindData>();
	result->rows = ::panduck::mediawiki::ReadMediaWiki(src);
	return std::move(result);
}

unique_ptr<FunctionData> MwStringBind(ClientContext &context, TableFunctionBindInput &input,
                                      vector<LogicalType> &return_types, panduck::BindNames &names) {
	readers::RequireReaderEnabled(context, "mediawiki");
	MwColumns(return_types, names);
	auto result = make_uniq<MwBindData>();
	result->rows = ::panduck::mediawiki::ReadMediaWiki(input.inputs[0].GetValue<string>());
	return std::move(result);
}

void MwScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<MwBindData>();
	auto &state = input.global_state->Cast<MwGlobalState>();
	idx_t count = 0;
	while (state.offset < data.rows.size() && count < STANDARD_VECTOR_SIZE) {
		const auto &row = data.rows[state.offset];
		output.SetValue(0, count, Value(row.kind));
		output.SetValue(1, count, Value(row.element_type));
		// Behaviour unchanged: HasContent() is `present AND non-empty`, so absent and
		// empty still both become SQL NULL, exactly as `row.content.empty()` did here
		// before the move. See panduck/block.hpp.
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

void RegisterMediaWikiReader(ExtensionLoader &loader) {
	{
		TableFunction file_fn("read_mediawiki_blocks", {LogicalType::VARCHAR}, MwScan, MwFileBind, MwGlobalState::Init);
		CreateTableFunctionInfo info(std::move(file_fn));
		info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
		FunctionDescription desc;
		desc.parameter_names = {"file_path"};
		desc.description = "Read a MediaWiki wikitext document and return "
		                   "structured document blocks.";
		desc.examples = {"SELECT * FROM read_mediawiki_blocks('document.wiki')"};
		desc.categories = {"panduck"};
		info.descriptions.push_back(desc);
		loader.RegisterFunction(std::move(info));
	}

	{
		TableFunction string_fn("read_mediawiki_blocks_string", {LogicalType::VARCHAR}, MwScan, MwStringBind,
		                        MwGlobalState::Init);
		CreateTableFunctionInfo info(std::move(string_fn));
		info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
		FunctionDescription desc;
		desc.parameter_names = {"mediawiki_text"};
		desc.description = "Parse a MediaWiki wikitext string and return "
		                   "structured document blocks.";
		desc.examples = {"SELECT * FROM read_mediawiki_blocks_string('== Section "
		                 "==\\n\'\'\'Bold\'\'\'')"};
		desc.categories = {"panduck"};
		info.descriptions.push_back(desc);
		loader.RegisterFunction(std::move(info));
	}
}

} // namespace mediawiki
} // namespace duckdb
