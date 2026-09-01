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
	//! TEXT only, and only when a ligature rewrote the run: the SOURCE SPELLING of `text`.
	//! Empty means the run already is its own source.
	//!
	//! A ligature is a fact about PROSE. `Dr.~Smith` is two words with an unbreakable space
	//! between them, but `http://example.com/~bob` is a tilde in a machine-readable string,
	//! and once the tokenizer has folded it to U+00A0 no consumer downstream can tell the
	//! two apart or undo either -- the URL is simply broken, and invisibly so. Carrying the
	//! source alongside the resolved text costs one string per run that actually contains a
	//! ligature and lets the reader take a link target or an image path AS WRITTEN while
	//! every other use of the same token keeps the typography.
	std::string raw;
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
