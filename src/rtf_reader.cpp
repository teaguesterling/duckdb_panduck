#include "rtf_reader.hpp"
#include "panduck_duckdb_compat.hpp"
#include "reader_registry.hpp"

#include "duck_block_types.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

#include <string>
#include <vector>

// The DuckDB-facing half of the RTF reader. The parse core and the row
// flattening moved to libpanduck (issue #104, L2); rtf_reader.hpp re-exports
// their types into this namespace so nothing else had to change.
//
// FileSystem STAYS. Unlike textile, which reads with std::ifstream, this reader
// goes through DuckDB's FileSystem -- so it reads from whatever filesystem the
// client has attached, httpfs included. Swapping it for ifstream behind the seam
// would be a behaviour change riding inside a refactor; it is a DuckDB
// affordance and it belongs on the DuckDB side.
//
// WHAT THIS FILE LOST THAT THE TIER 1 TAILS DID NOT. Those readers each had a
// `BuildRows` free function, so the cut line was already drawn. RTF's flatten was
// WELDED INTO RtfReaderBind -- ~17 lines appending into `result->rows` between
// the FileSystem read and the return -- along with `struct BlockRow`, a twelfth
// private copy of the duck_block row. Both are gone: the bind now does the three
// things only DuckDB can do (enablement check, column schema, read the path) and
// calls ::panduck::rtf::ReadRtf for the rows. BlockRow's `encoding =
// ENCODING_TEXT` default went with it, which is why ReadRtf assigns encoding
// explicitly on every row and every child -- see the comments there.

namespace duckdb {

namespace {

struct RtfReaderBindData : public TableFunctionData {
	std::vector<::panduck::Block> rows;
};

struct RtfReaderGlobalState : public GlobalTableFunctionState {
	idx_t offset = 0;

	static unique_ptr<GlobalTableFunctionState> Init(ClientContext &, TableFunctionInitInput &) {
		return make_uniq<RtfReaderGlobalState>();
	}
};

unique_ptr<FunctionData> RtfReaderBind(ClientContext &context, TableFunctionBindInput &input,
                                       vector<LogicalType> &return_types, panduck::BindNames &names) {
	readers::RequireReaderEnabled(context, "rtf");
	// Column order mirrors the duck_block struct so a row casts straight to
	// duck_block.
	names = {"kind", "element_type", "content", "level", "encoding", "attributes", "element_order"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::INTEGER,
	                LogicalType::VARCHAR, LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR),
	                LogicalType::INTEGER};

	auto path = input.inputs[0].GetValue<string>();
	auto &fs = FileSystem::GetFileSystem(context);
	if (!fs.FileExists(path)) {
		throw IOException("read_rtf_blocks: file not found: %s", path);
	}
	auto handle = fs.OpenFile(path, FileOpenFlags::FILE_FLAGS_READ);
	auto size = fs.GetFileSize(*handle);
	std::string data;
	data.resize(size);
	if (size > 0) {
		fs.Read(*handle, const_cast<char *>(data.data()), size);
	}

	auto result = make_uniq<RtfReaderBindData>();
	// THE ONE ROW EMITTER, now ::panduck::rtf::ReadRtf. rtf has a single table
	// function today, so unlike latex there is no second bind to drift from -- but
	// that is the reason the flatten was in here in the first place, and the
	// reason it was the least portable code in the reader rather than the
	// most-shared.
	result->rows = ::panduck::rtf::ReadRtf(data);
	return std::move(result);
}

void RtfReaderScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<RtfReaderBindData>();
	auto &state = input.global_state->Cast<RtfReaderGlobalState>();

	idx_t count = 0;
	while (state.offset < data.rows.size() && count < STANDARD_VECTOR_SIZE) {
		const auto &row = data.rows[state.offset];

		output.SetValue(0, count, Value(row.kind));
		output.SetValue(1, count, Value(row.element_type));
		// Empty content is NULL, per the duck_block convention for containers whose
		// text lives in structured inline children. Behaviour unchanged:
		// HasContent() is `present AND non-empty`, so absent and empty still both
		// become SQL NULL, exactly as `row.content.empty()` did here before the
		// move -- and it is the PREFERRED spelling, not a compromise: duck_block
		// ruled on 2026-09-01 that consumers MUST treat NULL and '' as the same
		// absence and producers SHOULD emit NULL, with `coalesce(content,'') <> ''`
		// as the portable test. HasContent() is that test. See panduck/block.hpp.
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

void RegisterRtfReaderFunction(ExtensionLoader &loader) {
	TableFunction fn("read_rtf_blocks", {LogicalType::VARCHAR}, RtfReaderScan, RtfReaderBind,
	                 RtfReaderGlobalState::Init);
	CreateTableFunctionInfo info(std::move(fn));
	info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
	FunctionDescription desc;
	desc.parameter_names = {"file_path"};
	desc.description = "Read an RTF file and return structured document blocks.";
	desc.examples = {"SELECT * FROM read_rtf_blocks('document.rtf')"};
	desc.categories = {"panduck"};
	info.descriptions.push_back(desc);
	loader.RegisterFunction(std::move(info));
}

} // namespace duckdb
