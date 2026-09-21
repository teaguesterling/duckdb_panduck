#pragma once

#include "duckdb.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "yyjson.hpp"

namespace duckdb {

class PandocInlineConvert {
public:
	// Register Pandoc inline conversion functions
	static void Register(ExtensionLoader &loader);

	// Convert a list of duck_block inline Values to Pandoc JSON string
	static string ConvertInlinesToPandocJson(const vector<Value> &inlines);

	// Convert a list of duck_block inline Values to Pandoc yyjson_mut_val array
	static duckdb_yyjson::yyjson_mut_val *ConvertDbInlinesToPandocVal(duckdb_yyjson::yyjson_mut_doc *doc,
	                                                                  const vector<Value> &inlines, idx_t start_idx,
	                                                                  int32_t target_level, idx_t &end_idx,
	                                                                  idx_t depth);

	// Convert nested Pandoc inline JSON to flat duck_block inline Values,
	// appending them to `result` (issue #21). `base_level` is the level given to
	// top-level inlines (children of a container at level N are at N+1), `order`
	// is the running element_order counter and is advanced past the appended
	// elements, and `depth` is the recursion budget shared with the caller.
	static void ConvertPandocInlinesToDbInlines(const string &json, int32_t base_level, int32_t &order,
	                                            vector<Value> &result, idx_t depth);

	// Convert yyjson inline AST to flat duck_block inline Values
	static void ConvertPandocInlinesValToDbInlines(duckdb_yyjson::yyjson_val *inlines_val, int32_t base_level,
	                                               int32_t &order, vector<Value> &result, idx_t depth);

private:
	// pandoc_inlines_to_db_inlines(json) -> LIST(duck_block)
	// Converts nested Pandoc inline JSON to flat duck_block inline rows
	static void PandocInlinesToDbInlinesFun(DataChunk &args, ExpressionState &state, Vector &result);

	// duck_blocks_inlines_to_pandoc(LIST(duck_block)) -> JSON
	// Converts flat duck_block inline rows back to nested Pandoc inline JSON
	static void DbInlinesToPandocFun(DataChunk &args, ExpressionState &state, Vector &result);

	// pandoc_inlines_to_text(json, mode) -> VARCHAR
	// Renders Pandoc inlines to text/markdown/html
	static void PandocInlinesToTextFun(DataChunk &args, ExpressionState &state, Vector &result);
};

} // namespace duckdb
