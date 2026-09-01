#pragma once

// The LaTeX tokenizer knows NOTHING about duck_blocks, deliberately. Comment handling,
// control-word whitespace and verbatim are the fiddly parts of reading TeX, and keeping
// them here means they are testable without constructing a single block -- and that
// latex_reader.cpp is about meaning rather than bytes.
#include <string>
#include <vector>

namespace duckdb {
namespace latex {

enum class TokenKind { TEXT, CONTROL_WORD, CONTROL_SYMBOL, BEGIN_GROUP, END_GROUP, PAR_BREAK, MATH_SHIFT, END };

struct Token {
	TokenKind kind;
	std::string text;         //!< the word for CONTROL_WORD, the char for CONTROL_SYMBOL, the run for TEXT
	bool display_math = false; //!< MATH_SHIFT only: $$..$$ and \[..\] rather than $..$
};

//! Tokenize a whole LaTeX source. Never throws: malformed input degrades.
std::vector<Token> Tokenize(const std::string &src);

//! Lowercase token-kind name, for panduck_latex_tokens() and for tests.
const char *KindName(TokenKind kind);

} // namespace latex

class ExtensionLoader;
void RegisterLatexTokensFunction(ExtensionLoader &loader);

} // namespace duckdb
