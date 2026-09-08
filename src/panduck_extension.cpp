#define DUCKDB_EXTENSION_MAIN

#include "panduck_extension.hpp"
#include "duck_block_types.hpp"
#include "docx_reader.hpp"
#include "epub_reader.hpp"
#include "latex_reader.hpp"
#include "org_reader.hpp"
#include "ipynb_reader.hpp"
#include "mediawiki_reader.hpp"
#include "pandoc_block_convert.hpp"
#include "pandoc_reader.hpp"
#include "rst_reader.hpp"
#include "textile_reader.hpp"
#include "latex_tokenizer.hpp"
#include "odt_reader.hpp"
#include "pandoc_ast_map.hpp"
#include "reader_registry.hpp"
#include "rtf_reader.hpp"
#include "supported_extensions.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

namespace duckdb {

//! panduck_version() -- the extension version. Exists so that "did the extension build,
//! register and load?" is answerable without any reader being implemented yet.
inline void PanduckVersionFun(DataChunk &args, ExpressionState &state, Vector &result) {
	string version;
#ifdef EXT_VERSION_PANDUCK
	version = EXT_VERSION_PANDUCK;
#else
	version = "dev";
#endif
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::GetData<string_t>(result)[0] = StringVector::AddString(result, version);
}

//! panduck_pandoc_api_version() -- the pandoc-types AST version this build's mapping
//! targets, as "major.minor". The conformance harness compares this against the
//! "pandoc-api-version" a real pandoc binary emits.
//! RETURNS INTEGER[], the shape pandoc's `pandoc-api-version` field actually takes.
//!
//! It returned the VARCHAR '1.23' until #7. That is not a formatting preference -- it is
//! wrong for the one thing the name promises. Building a pandoc envelope with it yields
//!
//!     parsing [] failed, expected Array, but encountered String
//!
//! and pandoc rejects the document. Reported by a consumer who reached for it exactly as
//! the name suggests, then had to read the value off the AST struct instead.
//!
//! It also DROPPED THE PATCH COMPONENT: '1.23' from MAJOR and MINOR, while the struct
//! emitted {MAJOR, MINOR, PATCH} = [1,23,1]. Two spellings of one fact, disagreeing. Built
//! from the same three constants now, and a test asserts the helper equals the struct's
//! field so they cannot drift again.
inline void PanduckPandocApiVersionFun(DataChunk &args, ExpressionState &state, Vector &result) {
	vector<Value> parts = {Value::INTEGER(pandoc_ast::API_VERSION_MAJOR), Value::INTEGER(pandoc_ast::API_VERSION_MINOR),
	                       Value::INTEGER(pandoc_ast::API_VERSION_PATCH)};
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	result.SetValue(0, Value::LIST(LogicalType::INTEGER, parts));
}

//! panduck_duck_block_type() -- the SQL type panduck's readers will emit, rendered as a
//! string. Two jobs in Phase 1: it forces duck_block_types.hpp to actually be compiled
//! (a header nothing includes is a header nothing checks), and it makes the struct
//! layout assertable from SQL, so a drift between panduck's copy of the contract and
//! duck_block_utils' definition fails a test rather than surfacing in Phase 2.
inline void PanduckDuckBlockTypeFun(DataChunk &args, ExpressionState &state, Vector &result) {
	const string type_str = DuckBlockTypes::DuckBlockType().ToString();
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::GetData<string_t>(result)[0] = StringVector::AddString(result, type_str);
}

static void LoadInternal(ExtensionLoader &loader) {
	loader.RegisterFunction(ScalarFunction("panduck_version", {}, LogicalType::VARCHAR, PanduckVersionFun));

	loader.RegisterFunction(ScalarFunction("panduck_pandoc_api_version", {}, LogicalType::LIST(LogicalType::INTEGER),
	                                       PanduckPandocApiVersionFun));

	loader.RegisterFunction(
	    ScalarFunction("panduck_duck_block_type", {}, LogicalType::VARCHAR, PanduckDuckBlockTypeFun));

	RegisterPandocAstMapFunction(loader);
	RegisterRtfReaderFunction(loader);
	RegisterDocxReaderFunction(loader);
	RegisterOdtReaderFunction(loader);
	RegisterEpubReaderFunction(loader);
	RegisterLatexTokensFunction(loader);
	RegisterLatexReaderFunction(loader);
	org::RegisterOrgReader(loader);
	rst::RegisterRstReader(loader);
	ipynb::RegisterIpynbReader(loader);
	mediawiki::RegisterMediaWikiReader(loader);
	textile::RegisterTextileReader(loader);
	RegisterPandocReader(loader);
	// The WRITE direction -- panduck_blocks_to_pandoc_ast and friends. Registered here
	// rather than from RegisterPandocReader because the two are independent surfaces: the
	// reader is a table function over a path, this is scalars over a block list. See the
	// comment on PandocBlockConvert::Register for why these carry panduck_ names while
	// duck_block_utils still owns the canonical ones.
	PandocBlockConvert::Register(loader);

	RegisterSupportedExtensionsFunction(loader);
	RegisterReaderRegistry(loader);
}

void PanduckExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string PanduckExtension::Name() {
	return "panduck";
}

std::string PanduckExtension::Version() const {
#ifdef EXT_VERSION_PANDUCK
	return EXT_VERSION_PANDUCK;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(panduck, loader) {
	duckdb::LoadInternal(loader);
}
}
