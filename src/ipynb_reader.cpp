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

struct IpynbRow {
	std::string kind, element_type, content;
	std::string encoding = DuckBlockTypes::ENCODING_TEXT;
	int32_t level = 0;
	std::map<std::string, std::string> attributes;
	int32_t element_order = 0;
};

struct IpynbBindData : public TableFunctionData {
	std::vector<IpynbRow> rows;
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

void BuildRows(const std::string &src, std::vector<IpynbRow> &rows) {
	int32_t order = 0;
	for (auto &block : ParseIpynbString(src)) {
		IpynbRow row;
		row.kind = block.kind.empty() ? DuckBlockTypes::KIND_BLOCK : block.kind;
		row.element_type = block.element_type;
		row.content = block.content;
		if (!block.encoding.empty()) {
			row.encoding = block.encoding;
		}
		row.element_order = order++;
		if (!block.key.empty()) {
			row.attributes[DuckBlockTypes::ATTR_KEY] = block.key;
		}
		if (!block.raw_format.empty()) {
			row.attributes["format"] = block.raw_format;
		}
		if (!block.source_type.empty()) {
			// A div's cell or output kind, or a metadata field's original path. On
			// metadata it is what keeps a format-derived field distinguishable from a
			// pandoc-derived one, which is the condition attached to exceeding the
			// reference.
			row.attributes[DuckBlockTypes::ATTR_SOURCE_TYPE] = block.source_type;
		}
		if (!block.language.empty()) {
			row.attributes["language"] = block.language;
		}
		// NO heading, role or list branches here, unlike every other reader: a
		// notebook's block structure is cells, code and raw content. Carrying
		// fields the format cannot produce would be dead weight that reads as an
		// oversight.
		const int32_t block_level = block.level > 0 ? block.level : 1;
		row.level = block_level;
		rows.push_back(std::move(row));

		for (auto &inl : block.inlines) {
			IpynbRow child;
			child.kind = DuckBlockTypes::KIND_INLINE;
			child.element_type = inl.element_type;
			child.content = inl.content;
			child.level = inl.level > 0 ? inl.level : block_level + 1;
			child.element_order = order++;
			// No href branch: the only inlines this reader emits are the metadata
			// values' text runs. Markdown cells are held raw, so their links never
			// become inlines here -- they are still inside the raw content until
			// expand_embedded parses it. That helper LANDED (#39):
			// read_panduck_doc(src, expand_embedded := true), and the same parameter
			// on doc_toc/doc_section/doc_search_sections/doc_container. It parses in
			// the SQL layer where delegation lives, so this reader keeps the
			// independence the comment above defends -- the default is still one raw
			// block.
			rows.push_back(std::move(child));
		}
	}
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
	BuildRows(src, result->rows);
	return std::move(result);
}

unique_ptr<FunctionData> IpynbStringBind(ClientContext &context, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, panduck::BindNames &names) {
	readers::RequireReaderEnabled(context, "ipynb");
	IpynbColumns(return_types, names);
	auto result = make_uniq<IpynbBindData>();
	BuildRows(input.inputs[0].GetValue<string>(), result->rows);
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
		output.SetValue(2, count, row.content.empty() ? Value(LogicalType::VARCHAR) : Value(row.content));
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
