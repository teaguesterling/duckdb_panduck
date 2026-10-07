#include "latex_tokenizer.hpp"
#include "panduck_duckdb_compat.hpp"

#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

#include <string>
#include <vector>

// The DuckDB-facing half of the LaTeX tokenizer: panduck_latex_tokens() and
// nothing else. The lexer itself moved to libpanduck (issue #104, L2),
// libpanduck/src/latex_tokenizer.cpp, and latex_tokenizer.hpp re-exports its
// types into this namespace so every `latex::Token` spelling below is unchanged.
//
// THE FILENAME IS DELIBERATELY THE SAME on both sides. This file held two
// unrelated things -- a byte lexer and a table function over it -- and the split
// is by dependency, not by topic, so the pair keeps one name.

namespace duckdb {

namespace {

struct LatexTokensBindData : public TableFunctionData {
	std::vector<latex::Token> tokens;
};

struct LatexTokensGlobalState : public GlobalTableFunctionState {
	idx_t offset = 0;
	static unique_ptr<GlobalTableFunctionState> Init(ClientContext &, TableFunctionInitInput &) {
		return make_uniq<LatexTokensGlobalState>();
	}
};

unique_ptr<FunctionData> LatexTokensBind(ClientContext &, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, panduck::BindNames &names) {
	names = {"kind", "text"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR};
	auto result = make_uniq<LatexTokensBindData>();
	result->tokens = latex::Tokenize(input.inputs[0].GetValue<string>());
	return std::move(result);
}

void LatexTokensScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<LatexTokensBindData>();
	auto &state = input.global_state->Cast<LatexTokensGlobalState>();
	idx_t count = 0;
	// The END token is a parser convenience, not a fact about the document, so it
	// is not emitted here -- a test asserting a token list should not have to
	// carry it.
	while (state.offset < data.tokens.size() && count < STANDARD_VECTOR_SIZE) {
		auto &tok = data.tokens[state.offset++];
		if (tok.kind == latex::TokenKind::END) {
			continue;
		}
		output.SetValue(0, count, Value(latex::KindName(tok.kind)));
		output.SetValue(1, count, Value(tok.text));
		count++;
	}
	output.SetCardinality(count);
}

} // namespace

void RegisterLatexTokensFunction(ExtensionLoader &loader) {
	TableFunction fn("panduck_latex_tokens", {LogicalType::VARCHAR}, LatexTokensScan, LatexTokensBind,
	                 LatexTokensGlobalState::Init);
	CreateTableFunctionInfo info(std::move(fn));
	info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
	FunctionDescription desc;
	desc.parameter_names = {"latex_text"};
	desc.description = "Tokenize a LaTeX string and return the sequence of LaTeX tokens.";
	desc.examples = {"SELECT * FROM panduck_latex_tokens('\\textbf{hello}')"};
	desc.categories = {"panduck"};
	info.descriptions.push_back(desc);
	loader.RegisterFunction(std::move(info));
}

} // namespace duckdb
