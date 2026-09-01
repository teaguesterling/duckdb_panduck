#include "latex_tokenizer.hpp"

#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <cctype>

namespace duckdb {
namespace latex {

const char *KindName(TokenKind kind) {
	switch (kind) {
	case TokenKind::TEXT:
		return "text";
	case TokenKind::CONTROL_WORD:
		return "control_word";
	case TokenKind::CONTROL_SYMBOL:
		return "control_symbol";
	case TokenKind::BEGIN_GROUP:
		return "begin_group";
	case TokenKind::END_GROUP:
		return "end_group";
	case TokenKind::PAR_BREAK:
		return "par_break";
	case TokenKind::MATH_SHIFT:
		return "math_shift";
	default:
		return "end";
	}
}

std::vector<Token> Tokenize(const std::string &src) {
	std::vector<Token> out;
	std::string text;
	auto flush = [&]() {
		if (!text.empty()) {
			out.push_back(Token {TokenKind::TEXT, text, false});
			text.clear();
		}
	};

	for (size_t i = 0; i < src.size();) {
		char c = src[i];
		if (c == '\\' && i + 1 < src.size() && isalpha((unsigned char)src[i + 1])) {
			flush();
			size_t start = ++i;
			while (i < src.size() && isalpha((unsigned char)src[i])) {
				i++;
			}
			std::string name = src.substr(start, i - start);
			// A starred form is the same macro: \section* is looked up as \section.
			if (i < src.size() && src[i] == '*') {
				i++;
			}
			// TeX CONSUMES the whitespace after a control word. Keeping it would put a
			// space before the text following every macro.
			while (i < src.size() && (src[i] == ' ' || src[i] == '\t')) {
				i++;
			}
			out.push_back(Token {TokenKind::CONTROL_WORD, name, false});
			continue;
		}
		if (c == '\\' && i + 1 < src.size()) {
			flush();
			out.push_back(Token {TokenKind::CONTROL_SYMBOL, std::string(1, src[i + 1]), false});
			i += 2;
			continue;
		}
		if (c == '{' || c == '}') {
			flush();
			out.push_back(Token {c == '{' ? TokenKind::BEGIN_GROUP : TokenKind::END_GROUP, std::string(1, c), false});
			i++;
			continue;
		}
		text.push_back(c);
		i++;
	}
	flush();
	out.push_back(Token {TokenKind::END, std::string(), false});
	return out;
}

} // namespace latex

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
                                         vector<LogicalType> &return_types, vector<string> &names) {
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
	// The END token is a parser convenience, not a fact about the document, so it is not
	// emitted here -- a test asserting a token list should not have to carry it.
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
	loader.RegisterFunction(fn);
}

} // namespace duckdb
