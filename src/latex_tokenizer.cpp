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
		// \( \) \[ \] are the OTHER spelling of math, and pandoc prefers them. They
		// lex as control symbols, so without this branch -- placed BEFORE the generic
		// control-symbol case below -- display math is silently dropped -- the macro table
		// never sees math at all, because math is a tokenizer construct rather than a
		// macro.
		if (c == '\\' && i + 1 < src.size() && (src[i + 1] == '(' || src[i + 1] == ')' ||
		                                        src[i + 1] == '[' || src[i + 1] == ']')) {
			flush();
			bool display = src[i + 1] == '[' || src[i + 1] == ']';
			out.push_back(Token {TokenKind::MATH_SHIFT, std::string(), display});
			i += 2;
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
		if (c == '%') {
			// TeX's comment rule in full: to end of line, THEN the newline, THEN the next
			// line's leading whitespace. pandoc's trailing `%` in \hypertarget{id}{% is
			// there precisely to suppress the space the break would produce, so a reader
			// that keeps the newline puts a leading space in every heading it writes.
			while (i < src.size() && src[i] != '\n') {
				i++;
			}
			if (i < src.size()) {
				i++; // the newline itself
			}
			while (i < src.size() && (src[i] == ' ' || src[i] == '\t')) {
				i++;
			}
			continue;
		}
		if (c == '\n') {
			// A blank line -- two newlines separated only by whitespace -- is a paragraph
			// break. A single newline is just a space; it is LaTeX's only paragraph signal.
			size_t j = i + 1;
			while (j < src.size() && (src[j] == ' ' || src[j] == '\t' || src[j] == '\r')) {
				j++;
			}
			if (j < src.size() && src[j] == '\n') {
				flush();
				while (j < src.size() && isspace((unsigned char)src[j])) {
					j++;
				}
				out.push_back(Token {TokenKind::PAR_BREAK, std::string(), false});
				i = j;
				continue;
			}
			text.push_back('\n');
			i++;
			continue;
		}
		if (c == '$') {
			flush();
			bool display = i + 1 < src.size() && src[i + 1] == '$';
			out.push_back(Token {TokenKind::MATH_SHIFT, std::string(), display});
			i += display ? 2 : 1;
			continue;
		}
		if (c == '-' && src.compare(i, 3, "---") == 0) {
			text += "—"; // em dash
			i += 3;
			continue;
		}
		if (c == '-' && src.compare(i, 2, "--") == 0) {
			text += "–"; // en dash
			i += 2;
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
