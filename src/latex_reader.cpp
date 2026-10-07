#include "latex_reader.hpp"
#include "panduck_duckdb_compat.hpp"
#include "reader_registry.hpp"

#include "duck_block_types.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

#include <string>
#include <vector>

// The DuckDB-facing half of the LaTeX reader. The parse core, the tokenizer, the
// macro table and the row flattening moved to libpanduck (issue #104, L2);
// latex_reader.hpp re-exports their types into this namespace so nothing else
// had to change.
//
// FileSystem STAYS. Unlike textile, which reads with std::ifstream, this reader
// goes through DuckDB's FileSystem -- so it reads from whatever filesystem the
// client has attached, httpfs included. Swapping it for ifstream behind the seam
// would be a behaviour change riding inside a refactor; it is a DuckDB
// affordance and it belongs on the DuckDB side.

namespace duckdb {

namespace {

struct LatexReaderBindData : public TableFunctionData {
	std::vector<::panduck::Block> rows;
};

struct LatexReaderGlobalState : public GlobalTableFunctionState {
	idx_t offset = 0;

	static unique_ptr<GlobalTableFunctionState> Init(ClientContext &, TableFunctionInitInput &) {
		return make_uniq<LatexReaderGlobalState>();
	}
};

void LatexColumns(vector<LogicalType> &return_types, panduck::BindNames &names) {
	// Column order mirrors the duck_block struct so a row casts straight to
	// duck_block.
	names = {"kind", "element_type", "content", "level", "encoding", "attributes", "element_order"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::INTEGER,
	                LogicalType::VARCHAR, LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR),
	                LogicalType::INTEGER};
}

unique_ptr<FunctionData> LatexFileBind(ClientContext &context, TableFunctionBindInput &input,
                                       vector<LogicalType> &return_types, panduck::BindNames &names) {
	readers::RequireReaderEnabled(context, "latex");
	LatexColumns(return_types, names);

	auto path = input.inputs[0].GetValue<string>();
	auto &fs = FileSystem::GetFileSystem(context);
	if (!fs.FileExists(path)) {
		throw IOException("read_latex_blocks: file not found: %s", path);
	}
	auto handle = fs.OpenFile(path, FileOpenFlags::FILE_FLAGS_READ);
	auto size = fs.GetFileSize(*handle);
	std::string data;
	data.resize(size);
	if (size > 0) {
		fs.Read(*handle, const_cast<char *>(data.data()), size);
	}

	auto result = make_uniq<LatexReaderBindData>();
	// THE ONE ROW EMITTER, now ::panduck::latex::ReadLatex. read_latex_blocks and
	// read_latex_blocks_string differ only in where the bytes come from, so
	// anything past this point -- level arithmetic, attribute spelling,
	// element_order -- cannot drift between them.
	result->rows = ::panduck::latex::ReadLatex(data);
	return std::move(result);
}

unique_ptr<FunctionData> LatexStringBind(ClientContext &context, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, panduck::BindNames &names) {
	readers::RequireReaderEnabled(context, "latex");
	LatexColumns(return_types, names);
	auto result = make_uniq<LatexReaderBindData>();
	result->rows = ::panduck::latex::ReadLatex(input.inputs[0].GetValue<string>());
	return std::move(result);
}

void LatexReaderScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<LatexReaderBindData>();
	auto &state = input.global_state->Cast<LatexReaderGlobalState>();

	idx_t count = 0;
	while (state.offset < data.rows.size() && count < STANDARD_VECTOR_SIZE) {
		const auto &row = data.rows[state.offset];

		output.SetValue(0, count, Value(row.kind));
		output.SetValue(1, count, Value(row.element_type));
		// Empty content is NULL, per the duck_block convention for containers whose
		// text lives in structured inline children. Behaviour unchanged:
		// HasContent() is `present AND non-empty`, so absent and empty still both
		// become SQL NULL, exactly as `row.content.empty()` did here before the
		// move. See panduck/block.hpp.
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

void RegisterLatexReaderFunction(ExtensionLoader &loader) {
	{
		TableFunction file_fn("read_latex_blocks", {LogicalType::VARCHAR}, LatexReaderScan, LatexFileBind,
		                      LatexReaderGlobalState::Init);
		CreateTableFunctionInfo info(std::move(file_fn));
		info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
		FunctionDescription desc;
		desc.parameter_names = {"file_path"};
		desc.description = "Read a LaTeX document and return structured document blocks.";
		desc.examples = {"SELECT * FROM read_latex_blocks('document.tex')"};
		desc.categories = {"panduck"};
		info.descriptions.push_back(desc);
		loader.RegisterFunction(std::move(info));
	}

	// NOT TEST SCAFFOLDING. Asserting a two-line snippet is how the nesting rules
	// stay readable, and every other panduck reader needs a fixture file on disk
	// for the same job -- which is why none of them can state a rule in one line
	// of SQL.
	{
		TableFunction string_fn("read_latex_blocks_string", {LogicalType::VARCHAR}, LatexReaderScan, LatexStringBind,
		                        LatexReaderGlobalState::Init);
		CreateTableFunctionInfo info(std::move(string_fn));
		info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
		FunctionDescription desc;
		desc.parameter_names = {"latex_text"};
		desc.description = "Parse a LaTeX string and return structured document blocks.";
		desc.examples = {"SELECT * FROM "
		                 "read_latex_blocks_string('\\section{Title}\\n\\textbf{Bold}')"};
		desc.categories = {"panduck"};
		info.descriptions.push_back(desc);
		loader.RegisterFunction(std::move(info));
	}
}

} // namespace duckdb
