#pragma once

// The LaTeX tokenizer knows NOTHING about duck_blocks, deliberately. Comment handling,
// control-word whitespace and verbatim are the fiddly parts of reading TeX, and keeping
// them here means they are testable without constructing a single block -- and that
// latex_reader.cpp is about meaning rather than bytes.
#include <cstddef>
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

//! Byte length of the UTF-8 code point starting at `pos`: a lead byte plus its
//! continuation bytes, never a fraction of one. Every place that takes "the next single
//! character" out of LaTeX source has to go through this, because a half code point put
//! into a DuckDB VARCHAR raises Invalid unicode -- and `{\bf émile}` is ordinary LaTeX,
//! not malformed input, so erroring on it would break the promise that the reader
//! degrades rather than throws. An invalid or truncated sequence reports 1, so a caller
//! stepping over malformed bytes still advances.
size_t Utf8SequenceLength(const std::string &s, size_t pos);

//! Lowercase token-kind name, for panduck_latex_tokens() and for tests.
const char *KindName(TokenKind kind);

} // namespace latex

class ExtensionLoader;
void RegisterLatexTokensFunction(ExtensionLoader &loader);

} // namespace duckdb
