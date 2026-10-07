#pragma once

// The LaTeX tokenizer knows NOTHING about duck_blocks, deliberately. Comment
// handling, control-word whitespace and verbatim are the fiddly parts of
// reading TeX, and keeping them here means they are testable without
// constructing a single block -- and that libpanduck/src/latex.cpp is about
// meaning rather than bytes.
//
// Moved behind the libpanduck seam (issue #104, L2). It never named a DuckDB
// type: it sat in `namespace duckdb` by convention only, alongside the
// panduck_latex_tokens() table function that shared its .cpp. That function
// stayed in src/latex_tokenizer.cpp, and src/include/latex_tokenizer.hpp
// re-exports everything below so no existing `duckdb::latex::Token` spelling
// had to change.
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace panduck {
namespace latex {

enum class TokenKind { TEXT, CONTROL_WORD, CONTROL_SYMBOL, BEGIN_GROUP, END_GROUP, PAR_BREAK, MATH_SHIFT, END };

struct Token {
	TokenKind kind;
	std::string text;          //!< the word for CONTROL_WORD, the char for CONTROL_SYMBOL,
	                           //!< the run for TEXT
	bool display_math = false; //!< MATH_SHIFT only: $$..$$ and \[..\] rather than $..$
	//! TEXT only, and only when a ligature rewrote the run: the SOURCE SPELLING
	//! of `text`. Empty means the run already is its own source.
	//!
	//! A ligature is a fact about PROSE. `Dr.~Smith` is two words with an
	//! unbreakable space between them, but `http://example.com/~bob` is a tilde
	//! in a machine-readable string, and once the tokenizer has folded it to
	//! U+00A0 no consumer downstream can tell the two apart or undo either -- the
	//! URL is simply broken, and invisibly so. Carrying the source alongside the
	//! resolved text costs one string per run that actually contains a ligature
	//! and lets the reader take a link target or an image path AS WRITTEN while
	//! every other use of the same token keeps the typography.
	std::string raw;

	// EXPLICIT CONSTRUCTORS BECAUSE libpanduck IS C++11, and this is the one
	// thing the move to the seam actually had to change in the lexer.
	//
	// 22 call sites write `Token {kind, text, display_math}` as an AGGREGATE
	// initialisation, and that is well-formed from C++14 on -- but in C++11 a
	// class with a brace-or-equal-initializer for a non-static data member is not
	// an aggregate at all ([dcl.init.aggr]/1; N3653 relaxed it for C++14). So
	// `bool display_math = false;` above, three lines up, silently disqualified
	// every one of those 22 sites. The extension compiles at C++17
	// (CMakeLists.txt sets CXX_STANDARD 17), where this is legal, so NOTHING in
	// the extension build could see it: `cmake -S libpanduck` is what reported
	// it, 22 errors, which is precisely the class of defect the standalone
	// project exists to catch.
	//
	// Both constructors reproduce the old initialisation exactly. The defaulted
	// one leaves `kind` indeterminate and `display_math` false, as
	// `Token piece;` did; the three-argument one leaves `raw` empty, as a
	// three-element braced list did. Behaviour unchanged -- the alternative,
	// dropping the default member initialiser to keep the aggregate, would have
	// left `display_math` indeterminate at the two default-construction sites in
	// latex.cpp, which is a real change dressed up as the smaller edit.
	Token() = default;
	Token(TokenKind k, std::string t, bool dm) : kind(k), text(std::move(t)), display_math(dm) {
	}
};

//! Tokenize a whole LaTeX source. Never throws: malformed input degrades.
std::vector<Token> Tokenize(const std::string &src);

//! Byte length of the UTF-8 code point starting at `pos`: a lead byte plus its
//! continuation bytes, never a fraction of one. Every place that takes "the
//! next single character" out of LaTeX source has to go through this, because a
//! half code point put into a DuckDB VARCHAR raises Invalid unicode -- and
//! `{\bf émile}` is ordinary LaTeX, not malformed input, so erroring on it
//! would break the promise that the reader degrades rather than throws. An
//! invalid or truncated sequence reports 1, so a caller stepping over malformed
//! bytes still advances.
size_t Utf8SequenceLength(const std::string &s, size_t pos);

//! Lowercase token-kind name, for panduck_latex_tokens() and for tests.
const char *KindName(TokenKind kind);

} // namespace latex
} // namespace panduck
