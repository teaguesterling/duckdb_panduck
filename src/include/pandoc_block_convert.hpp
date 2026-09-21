#pragma once

#include "duckdb.hpp"
#include "duckdb/function/scalar_function.hpp"

namespace duckdb {

class PandocBlockConvert {
public:
	// Register all Pandoc block conversion functions
	static void Register(ExtensionLoader &loader);

	// Core conversion function (exposed for reuse)
	static void ConvertPandocAstToBlocks(const string &json, vector<Value> &result);

private:
	// pandoc_ast_to_blocks(json VARCHAR) -> LIST(duck_block)
	// Convert Pandoc JSON AST blocks to duck_block list
	static void PandocAstToBlocksFun(DataChunk &args, ExpressionState &state, Vector &result);

	// duck_blocks_to_pandoc_blocks(blocks LIST(duck_block)) -> VARCHAR
	// Convert duck_block list to Pandoc JSON blocks array
	static void DuckBlocksToPandocBlocksFun(DataChunk &args, ExpressionState &state, Vector &result);

	// read_pandoc_ast(file_path VARCHAR) -> LIST(duck_block)
	// Read a Pandoc JSON file and convert to duck_blocks
	static void ReadPandocAstFun(DataChunk &args, ExpressionState &state, Vector &result);
};

} // namespace duckdb
