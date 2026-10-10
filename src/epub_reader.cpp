#include "epub_reader.hpp"
#include "container_status.hpp"
#include "panduck_duckdb_compat.hpp"
#include "reader_registry.hpp"

#include "duck_block_types.hpp"

#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

#include <string>
#include <vector>

// The DuckDB-facing half of the EPUB reader. The parse core and the row
// flattening moved to libpanduck (issue #104, L2); epub_reader.hpp re-exports
// their types into this namespace so nothing else had to change.
//
// WHAT THIS FILE LOST. `struct EpubRow`, a private copy of the duck_block row,
// and ~65 lines of flatten that were WELDED INTO EpubBind between the
// ParseEpubFile call and the `return std::move(result)`. Both are gone: the bind
// now does the two things only DuckDB can do -- the enablement check and the
// column schema -- and calls ::panduck::epub::ReadEpub for the rows. EpubRow's
// `encoding = ENCODING_TEXT` default went with it, which is why ReadEpub assigns
// encoding explicitly on every row and every child; see the comments there.
//
// AND WHAT IT GAINED: the raise, for FIVE failures rather than docx's and odt's
// three. ParseEpubFile threw on a bad ZIP, on a missing META-INF/container.xml,
// on any of four members not being well-formed XML, on a container.xml naming no
// rootfile, and on the named package document being absent. Those types are
// DuckDB API and their type IS the SQL-visible `IO Error:` / `Invalid Input
// Error:` prefix test/sql/epub_reader.test matches, so they could not cross the
// seam and could not be swapped for a panduck exception either -- DuckDB would
// wrap it, keeping the text and changing the type, which the seam contract's
// rule 4 forbids. The core reports a ::panduck::ContainerStatus;
// RaiseContainerStatus turns it back into the same exception with the same
// message, and epub is why that type has five codes. See
// src/include/container_status.hpp.
//
// THE ARCHIVE NOW OPENS THROUGH DuckDB's FileSystem (issue #120 step 2), like
// docx and odt and like the other ten readers since #122. read_epub_blocks
// handed a path straight to miniz, so it could not see an `s3://` or `https://`
// file. The bind opens the handle here and passes a
// `readers::FileHandleSource` to the core's `ByteSource` overload; the core still
// knows nothing about DuckDB.
//
// EPUB IS THE FORMAT THAT MAKES THE HANDLE WORTH HAVING. A book is a SPINE of
// members read out of one open archive, so slurping the whole file to satisfy
// miniz would copy an entire book into memory to walk its contents. That is why
// readers::OpenFileThroughVFS exists beside readers::ReadFileThroughVFS rather
// than these three readers reusing the whole-file helper.
//
// THE MISSING-FILE ERROR IS THE SHARED ONE, not a local copy:
// OpenFileThroughVFS raises the same `file not found` IOException
// ReadFileThroughVFS raises for the other ten readers. All five of the core's
// reported failures still come from the core, which keeps every assertion in
// test/sql/epub_reader.test exactly as it was.

namespace duckdb {

namespace {

struct EpubBindData : public TableFunctionData {
	std::vector<::panduck::Block> rows;
};

struct EpubGlobalState : public GlobalTableFunctionState {
	idx_t offset = 0;
	static unique_ptr<GlobalTableFunctionState> Init(ClientContext &, TableFunctionInitInput &) {
		return make_uniq<EpubGlobalState>();
	}
};

unique_ptr<FunctionData> EpubBind(ClientContext &context, TableFunctionBindInput &input,
                                  vector<LogicalType> &return_types, panduck::BindNames &names) {
	readers::RequireReaderEnabled(context, "epub");
	names = {"kind", "element_type", "content", "level", "encoding", "attributes", "element_order"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::INTEGER,
	                LogicalType::VARCHAR, LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR),
	                LogicalType::INTEGER};

	auto path = input.inputs[0].GetValue<string>();
	auto result = make_uniq<EpubBindData>();
	// OPENED THROUGH THE CLIENT'S FILESYSTEM, so any path DuckDB can reach works.
	// The handle and the source are LOCALS that outlive the read below, and epub is
	// where that matters most: the core keeps one container open and reads it once
	// per spine member, so the source is still being read long after the call
	// begins (see panduck/byte_source.hpp).
	auto handle = readers::OpenFileThroughVFS(context, path, "read_epub_blocks");
	readers::FileHandleSource source(*handle);
	// THE ONE ROW EMITTER, now ::panduck::epub::ReadEpub, which reads the archive
	// out of the source, follows the spine, and REPORTS failure instead of
	// throwing it.
	::panduck::ContainerStatus status;
	result->rows = ::panduck::epub::ReadEpub(source, status);
	if (!status.Ok()) {
		RaiseContainerStatus(status, "read_epub_blocks", path);
	}
	return std::move(result);
}

void EpubScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<EpubBindData>();
	auto &state = input.global_state->Cast<EpubGlobalState>();
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

void RegisterEpubReaderFunction(ExtensionLoader &loader) {
	TableFunction fn("read_epub_blocks", {LogicalType::VARCHAR}, EpubScan, EpubBind, EpubGlobalState::Init);
	CreateTableFunctionInfo info(std::move(fn));
	info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
	FunctionDescription desc;
	desc.parameter_names = {"file_path"};
	desc.description = "Read an EPUB document and return structured document blocks.";
	desc.examples = {"SELECT * FROM read_epub_blocks('book.epub')"};
	desc.categories = {"panduck"};
	info.descriptions.push_back(desc);
	loader.RegisterFunction(std::move(info));
}

} // namespace duckdb
