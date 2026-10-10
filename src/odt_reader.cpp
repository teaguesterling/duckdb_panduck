#include "odt_reader.hpp"
#include "container_status.hpp"
#include "panduck_duckdb_compat.hpp"
#include "reader_registry.hpp"

#include "duck_block_types.hpp"

#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

#include <string>
#include <vector>

// The DuckDB-facing half of the ODT reader. The parse core and the row
// flattening moved to libpanduck (issue #104, L2); odt_reader.hpp re-exports
// their types into this namespace so nothing else had to change.
//
// WHAT THIS FILE LOST. `struct OdtRow`, a private copy of the duck_block row,
// and ~50 lines of flatten that were WELDED INTO OdtBind between the
// ParseOdtFile call and the `return std::move(result)`. Both are gone: the bind
// now does the two things only DuckDB can do -- the enablement check and the
// column schema -- and calls ::panduck::odt::ReadOdt for the rows. OdtRow's
// `encoding = ENCODING_TEXT` default went with it, which is why ReadOdt assigns
// encoding explicitly on every row and every child; see the comments there.
//
// AND WHAT IT GAINED: the raise. ParseOdtFile used to throw IOException (bad
// ZIP) and InvalidInputException (no content.xml, or content.xml not
// well-formed). Those types are DuckDB API and their type IS the SQL-visible
// `IO Error:` / `Invalid Input Error:` prefix three assertions in
// test/sql/odt_reader.test match, so they could not cross the seam and could not
// be swapped for a panduck exception either -- DuckDB would wrap it, keeping the
// text and changing the type, which the seam contract's rule 4 forbids. The core
// reports a ::panduck::ContainerStatus; RaiseContainerStatus turns it back into
// the same exception with the same message. See src/include/container_status.hpp.
//
// NO FileSystem HERE, unlike rtf, and that is unchanged behaviour rather than a
// choice made in this commit: read_odt_blocks has always opened the archive with
// miniz directly (ZipContainer -> mz_zip_reader_init_file), so it has never read
// through a client-attached filesystem. Routing it through FileSystem would be a
// behaviour change, and a widening one; it is not this commit's business.

namespace duckdb {

namespace {

struct OdtBindData : public TableFunctionData {
	std::vector<::panduck::Block> rows;
};

struct OdtGlobalState : public GlobalTableFunctionState {
	idx_t offset = 0;
	static unique_ptr<GlobalTableFunctionState> Init(ClientContext &, TableFunctionInitInput &) {
		return make_uniq<OdtGlobalState>();
	}
};

unique_ptr<FunctionData> OdtBind(ClientContext &context, TableFunctionBindInput &input,
                                 vector<LogicalType> &return_types, panduck::BindNames &names) {
	readers::RequireReaderEnabled(context, "odt");
	names = {"kind", "element_type", "content", "level", "encoding", "attributes", "element_order"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::INTEGER,
	                LogicalType::VARCHAR, LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR),
	                LogicalType::INTEGER};

	auto path = input.inputs[0].GetValue<string>();
	auto result = make_uniq<OdtBindData>();
	// THE ONE ROW EMITTER, now ::panduck::odt::ReadOdt, which opens the archive
	// itself and REPORTS failure instead of throwing it.
	::panduck::ContainerStatus status;
	result->rows = ::panduck::odt::ReadOdt(path, status);
	if (!status.Ok()) {
		RaiseContainerStatus(status, "read_odt_blocks", path);
	}
	return std::move(result);
}

void OdtScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<OdtBindData>();
	auto &state = input.global_state->Cast<OdtGlobalState>();
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

void RegisterOdtReaderFunction(ExtensionLoader &loader) {
	TableFunction fn("read_odt_blocks", {LogicalType::VARCHAR}, OdtScan, OdtBind, OdtGlobalState::Init);
	CreateTableFunctionInfo info(std::move(fn));
	info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
	FunctionDescription desc;
	desc.parameter_names = {"file_path"};
	desc.description = "Read an ODT document and return structured document blocks.";
	desc.examples = {"SELECT * FROM read_odt_blocks('document.odt')"};
	desc.categories = {"panduck"};
	info.descriptions.push_back(desc);
	loader.RegisterFunction(std::move(info));
}

} // namespace duckdb
