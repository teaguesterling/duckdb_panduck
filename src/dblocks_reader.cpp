#include "dblocks_reader.hpp"

#include "duck_block_compact.hpp"
#include "duck_block_types.hpp"
#include "panduck_duckdb_compat.hpp"
#include "reader_registry.hpp"

#include "duckdb/common/file_system.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

namespace duckdb {
namespace {

namespace compact = duck_block::compact;

constexpr const char *FORMAT = "dblocks";

//! How much of the file the reader holds at once, before a line that is longer
//! grows the buffer to fit it. A line is never split: the codec decodes whole lines.
constexpr idx_t READ_PIECE = 1 << 20;

void DblocksColumns(vector<LogicalType> &types, panduck::BindNames &names) {
	types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	         LogicalType::INTEGER, LogicalType::VARCHAR, LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR),
	         LogicalType::INTEGER};
	names = {"kind", "element_type", "content", "level", "encoding", "attributes", "element_order"};
}

struct DblocksBindData : public TableFunctionData {
	string path;
	bool from_string = false;
	string text;
};

//! ONE THREAD. element_order is a line's position among the element lines, and the
//! reader counts it as it reads -- so the file is read front to back, in pieces, and
//! the count needs no help from scan order or preserve_insertion_order.
struct DblocksGlobalState : public GlobalTableFunctionState {
	unique_ptr<FileHandle> handle;
	string buffer;
	idx_t pos = 0;
	bool eof = false;
	int32_t order = 0;
	idx_t MaxThreads() const override {
		return 1;
	}
};

unique_ptr<FunctionData> DblocksFileBind(ClientContext &context, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, panduck::BindNames &names) {
	readers::RequireReaderEnabled(context, FORMAT);
	DblocksColumns(return_types, names);
	auto result = make_uniq<DblocksBindData>();
	result->path = input.inputs[0].GetValue<string>();
	return std::move(result);
}

unique_ptr<FunctionData> DblocksStringBind(ClientContext &context, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types, panduck::BindNames &names) {
	readers::RequireReaderEnabled(context, FORMAT);
	DblocksColumns(return_types, names);
	auto result = make_uniq<DblocksBindData>();
	result->from_string = true;
	result->text = input.inputs[0].IsNull() ? string() : input.inputs[0].GetValue<string>();
	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> DblocksInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<DblocksBindData>();
	auto state = make_uniq<DblocksGlobalState>();
	if (bind.from_string) {
		state->buffer = bind.text;
		state->eof = true;
	} else {
		state->handle = readers::OpenFileThroughVFS(context, bind.path, "read_dblocks_blocks");
	}
	return std::move(state);
}

//! Read the next piece of the file onto the end of the buffer, dropping what has
//! been consumed. Sets eof when the file has no more.
void ReadPiece(DblocksGlobalState &state) {
	state.buffer.erase(0, state.pos);
	state.pos = 0;
	const idx_t old_size = state.buffer.size();
	state.buffer.resize(old_size + READ_PIECE);
	auto got = state.handle->Read(&state.buffer[old_size], READ_PIECE);
	state.buffer.resize(old_size + (got > 0 ? idx_t(got) : 0));
	if (got <= 0) {
		state.eof = true;
	}
}

void DblocksScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<DblocksGlobalState>();
	const auto &block_type = DuckBlockTypes::DuckBlockType();
	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE) {
		auto nl = state.buffer.find('\n', state.pos);
		const char *line;
		idx_t len;
		if (nl == string::npos) {
			if (!state.eof) {
				ReadPiece(state);
				continue;
			}
			if (state.pos >= state.buffer.size()) {
				break; // done
			}
			line = state.buffer.data() + state.pos; // a last line without a newline
			len = state.buffer.size() - state.pos;
			state.pos = state.buffer.size();
		} else {
			line = state.buffer.data() + state.pos;
			len = nl - state.pos;
			state.pos = nl + 1;
		}
		Value element;
		if (!compact::ReadLine(line, len, state.order, block_type, element)) {
			continue; // an empty or header line
		}
		auto &fields = StructValue::GetChildren(element);
		for (idx_t col = 0; col < 7; col++) {
			output.data[col].SetValue(count, fields[col]);
		}
		state.order++;
		count++;
	}
	output.SetCardinalityUnsafe(count);
}

void DblocksToBlocksFun(DataChunk &args, ExpressionState &state, Vector &result) {
	readers::RequireReaderEnabled(panduck::ClientContextOf(state), FORMAT);
	for (idx_t i = 0; i < args.size(); i++) {
		auto text = args.data[0].GetValue(i);
		if (text.IsNull()) {
			result.SetValue(i, Value());
			continue;
		}
		vector<Value> blocks;
		compact::Read(StringValue::Get(text), DuckBlockTypes::DuckBlockType(), blocks);
		result.SetValue(i, Value::LIST(DuckBlockTypes::DuckBlockType(), std::move(blocks)));
	}
}

void BlocksToDblocksFun(DataChunk &args, ExpressionState &, Vector &result) {
	for (idx_t i = 0; i < args.size(); i++) {
		auto blocks = args.data[0].GetValue(i);
		if (blocks.IsNull()) {
			result.SetValue(i, Value());
			continue;
		}
		result.SetValue(i, Value(compact::Write(ListValue::GetChildren(blocks))));
	}
}

void WriteDblocksFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = panduck::ClientContextOf(state);
	auto &fs = FileSystem::GetFileSystem(context);
	for (idx_t i = 0; i < args.size(); i++) {
		auto path = args.data[0].GetValue(i);
		auto blocks = args.data[1].GetValue(i);
		if (path.IsNull()) {
			result.SetValue(i, Value(false));
			continue;
		}
		const string text = blocks.IsNull() ? compact::Header() : compact::Write(ListValue::GetChildren(blocks));
		auto handle = fs.OpenFile(StringValue::Get(path),
		                          FileOpenFlags::FILE_FLAGS_WRITE | FileOpenFlags::FILE_FLAGS_FILE_CREATE_NEW);
		fs.Write(*handle, const_cast<char *>(text.data()), static_cast<int64_t>(text.size()));
		handle->Close();
		result.SetValue(i, Value(true));
	}
}

void RegisterTable(ExtensionLoader &loader, const char *name, table_function_bind_t bind, const char *param,
                   const char *description, const char *example) {
	TableFunction fn(name, {LogicalType::VARCHAR}, DblocksScan, bind, DblocksInit);
	CreateTableFunctionInfo info(std::move(fn));
	info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
	FunctionDescription desc;
	desc.parameter_names = {param};
	desc.description = description;
	desc.examples = {example};
	desc.categories = {"panduck"};
	info.descriptions.push_back(desc);
	loader.RegisterFunction(std::move(info));
}

void RegisterScalar(ExtensionLoader &loader, ScalarFunction fn, vector<string> params, const char *description,
                    const char *example) {
	// FALLIBLE: the codec refuses bad input with InvalidInputException, and DuckDB
	// v2.0.0 turns an execution error from a function not marked SetFallible() into
	// an INTERNAL error.
	fn.SetFallible();
	CreateScalarFunctionInfo info(std::move(fn));
	info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
	FunctionDescription desc;
	desc.parameter_names = std::move(params);
	desc.description = description;
	desc.examples = {example};
	desc.categories = {"panduck"};
	info.descriptions.push_back(desc);
	loader.RegisterFunction(std::move(info));
}

} // namespace

void RegisterDblocksFunctions(ExtensionLoader &loader) {
	RegisterTable(loader, "read_dblocks_blocks", DblocksFileBind, "file_path",
	              "Read a .dblocks file (the compact duck_blocks line format) as document blocks, streaming it.",
	              "SELECT * FROM read_dblocks_blocks('document.dblocks')");
	RegisterTable(loader, "read_dblocks_blocks_string", DblocksStringBind, "dblocks_text",
	              "Read compact duck_blocks lines from a string as document blocks.",
	              "SELECT * FROM read_dblocks_blocks_string('{\"@\":\"4.2.0\"}\n{\"t\":\"P\",\"c\":\"x\",\"l\":1}\n')");
	RegisterScalar(loader,
	               ScalarFunction("panduck_dblocks_to_blocks", {LogicalType::VARCHAR},
	                              DuckBlockTypes::DuckBlockListType(), DblocksToBlocksFun),
	               {"dblocks_text"}, "Convert compact duck_blocks lines into a list of duck_blocks.",
	               "panduck_dblocks_to_blocks(content)");
	RegisterScalar(loader,
	               ScalarFunction("panduck_blocks_to_dblocks", {DuckBlockTypes::DuckBlockListType()},
	                              LogicalType::VARCHAR, BlocksToDblocksFun),
	               {"blocks"}, "Write a list of duck_blocks as compact duck_blocks lines, header first.",
	               "panduck_blocks_to_dblocks(blocks)");
	RegisterScalar(loader,
	               ScalarFunction("panduck_write_dblocks", {LogicalType::VARCHAR, DuckBlockTypes::DuckBlockListType()},
	                              LogicalType::BOOLEAN, WriteDblocksFun),
	               {"file_path", "blocks"}, "Write a list of duck_blocks to a .dblocks file.",
	               "panduck_write_dblocks('output.dblocks', blocks)");
}

} // namespace duckdb
