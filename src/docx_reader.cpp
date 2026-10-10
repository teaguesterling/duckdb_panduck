#include "docx_reader.hpp"
#include "container_status.hpp"
#include "panduck_duckdb_compat.hpp"
#include "reader_registry.hpp"

#include "duck_block_types.hpp"

#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

#include <string>
#include <vector>

// The DuckDB-facing half of the DOCX reader. The parse core and the row
// flattening moved to libpanduck (issue #104, L2); docx_reader.hpp re-exports
// their types into this namespace so nothing else had to change.
//
// WHAT THIS FILE LOST. `struct DocxRow`, a private copy of the duck_block row,
// and ~50 lines of flatten that were WELDED INTO DocxBind between the
// ParseDocxFile call and the `return std::move(result)`. Both are gone: the bind
// now does the two things only DuckDB can do -- the enablement check and the
// column schema -- and calls ::panduck::docx::ReadDocx for the rows. DocxRow's
// `encoding = ENCODING_TEXT` default went with it, which is why ReadDocx assigns
// encoding explicitly on every row and every child; see the comments there.
//
// AND WHAT IT GAINED: the raise. ParseDocxFile used to throw IOException (bad
// ZIP) and InvalidInputException (no word/document.xml, or it not being
// well-formed). Those types are DuckDB API and their type IS the SQL-visible
// `IO Error:` / `Invalid Input Error:` prefix four assertions in
// test/sql/docx_reader.test match, so they could not cross the seam and could
// not be swapped for a panduck exception either -- DuckDB would wrap it, keeping
// the text and changing the type, which the seam contract's rule 4 forbids. The
// core reports a ::panduck::ContainerStatus; RaiseContainerStatus turns it back
// into the same exception with the same message. See
// src/include/container_status.hpp.
//
// THE ARCHIVE NOW OPENS THROUGH DuckDB's FileSystem (issue #120 step 2), which
// closed the last gap this reader had: read_docx_blocks used to hand a path
// straight to miniz, so it was one of three readers that could not see an
// `s3://` or `https://` file while the other ten could. The bind opens the handle
// here and passes a `readers::FileHandleSource` to the core's `ByteSource`
// overload; the core still knows nothing about DuckDB.
//
// THE MISSING-FILE ERROR IS THE SHARED ONE, not a local copy:
// readers::OpenFileThroughVFS raises the same `file not found` IOException
// readers::ReadFileThroughVFS raises for the other ten readers, so a user cannot
// tell the two groups apart by the message. What DOES still come from the core is
// everything after a successful open -- a file that is not a ZIP, a missing
// word/document.xml -- which keeps the four assertions in
// test/sql/docx_reader.test exactly as they were.

namespace duckdb {

namespace {

struct DocxBindData : public TableFunctionData {
	std::vector<::panduck::Block> rows;
};

struct DocxGlobalState : public GlobalTableFunctionState {
	idx_t offset = 0;
	static unique_ptr<GlobalTableFunctionState> Init(ClientContext &, TableFunctionInitInput &) {
		return make_uniq<DocxGlobalState>();
	}
};

unique_ptr<FunctionData> DocxBind(ClientContext &context, TableFunctionBindInput &input,
                                  vector<LogicalType> &return_types, panduck::BindNames &names) {
	readers::RequireReaderEnabled(context, "docx");
	// Column order mirrors the duck_block struct, so a row casts straight to
	// duck_block and read_panduck_doc's flat branch can SELECT * it through.
	names = {"kind", "element_type", "content", "level", "encoding", "attributes", "element_order"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::INTEGER,
	                LogicalType::VARCHAR, LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR),
	                LogicalType::INTEGER};

	auto path = input.inputs[0].GetValue<string>();
	auto result = make_uniq<DocxBindData>();
	// OPENED THROUGH THE CLIENT'S FILESYSTEM, so any path DuckDB can reach works.
	// The handle and the source are LOCALS that outlive the read below -- which is
	// the requirement, not an accident: the core's ZipContainer holds the source by
	// reference and reads members on demand, so both must still be alive for the
	// whole call (see panduck/byte_source.hpp).
	auto handle = readers::OpenFileThroughVFS(context, path, "read_docx_blocks");
	readers::FileHandleSource source(*handle);
	// THE ONE ROW EMITTER, now ::panduck::docx::ReadDocx, which reads the archive
	// out of the source and REPORTS failure instead of throwing it.
	::panduck::ContainerStatus status;
	result->rows = ::panduck::docx::ReadDocx(source, status);
	if (!status.Ok()) {
		RaiseContainerStatus(status, "read_docx_blocks", path);
	}
	return std::move(result);
}

void DocxScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<DocxBindData>();
	auto &state = input.global_state->Cast<DocxGlobalState>();
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

void RegisterDocxReaderFunction(ExtensionLoader &loader) {
	TableFunction fn("read_docx_blocks", {LogicalType::VARCHAR}, DocxScan, DocxBind, DocxGlobalState::Init);
	CreateTableFunctionInfo info(std::move(fn));
	info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
	FunctionDescription desc;
	desc.parameter_names = {"file_path"};
	desc.description = "Read a DOCX document and return structured document blocks.";
	desc.examples = {"SELECT * FROM read_docx_blocks('document.docx')"};
	desc.categories = {"panduck"};
	info.descriptions.push_back(desc);
	loader.RegisterFunction(std::move(info));
}

} // namespace duckdb
