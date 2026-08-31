#define DUCKDB_EXTENSION_MAIN

#include "panduck_extension.hpp"
#include "duck_block_types.hpp"
#include "pandoc_ast_map.hpp"

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
inline void PanduckPandocApiVersionFun(DataChunk &args, ExpressionState &state, Vector &result) {
	const string version = to_string(pandoc_ast::API_VERSION_MAJOR) + "." + to_string(pandoc_ast::API_VERSION_MINOR);
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::GetData<string_t>(result)[0] = StringVector::AddString(result, version);
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

	loader.RegisterFunction(
	    ScalarFunction("panduck_pandoc_api_version", {}, LogicalType::VARCHAR, PanduckPandocApiVersionFun));

	loader.RegisterFunction(
	    ScalarFunction("panduck_duck_block_type", {}, LogicalType::VARCHAR, PanduckDuckBlockTypeFun));

	RegisterPandocAstMapFunction(loader);
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
