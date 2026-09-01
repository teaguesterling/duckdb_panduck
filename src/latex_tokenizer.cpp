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

namespace {

//! Called with `i` just past a `\begin` control word. If the environment being opened is
//! one whose body is BYTES rather than tokens, emit the whole `{name} body \end{name}` run
//! and advance `i` past it; otherwise leave both untouched and return false so the normal
//! scanner reads the brace group.
//!
//! The environment names live here rather than in the macro table on purpose: which
//! environments suspend tokenization is a LEXICAL fact, and the table's job is to say what
//! a construct MEANS. `verbatim` still appears there as a SEMANTIC code block, because
//! both facts are true of it.
bool LexVerbatim(const std::string &src, size_t &i, std::vector<Token> &out) {
	size_t k = i;
	while (k < src.size() && (src[k] == ' ' || src[k] == '\t')) {
		k++;
	}
	if (k >= src.size() || src[k] != '{') {
		return false;
	}
	auto close = src.find('}', k + 1);
	if (close == std::string::npos) {
		return false;
	}
	std::string env = src.substr(k + 1, close - k - 1);
	// verbatim* is verbatim with visible spaces -- the same lexical mode, and it arrives
	// with its star intact because a brace group is not a control word. The TERMINATOR
	// keeps the star: \begin{verbatim*} is closed by \end{verbatim*} and by nothing else.
	const std::string base = !env.empty() && env.back() == '*' ? env.substr(0, env.size() - 1) : env;
	if (base != "verbatim" && base != "lstlisting") {
		return false;
	}
	out.push_back(Token {TokenKind::BEGIN_GROUP, "{", false});
	out.push_back(Token {TokenKind::TEXT, env, false});
	out.push_back(Token {TokenKind::END_GROUP, "}", false});
	size_t body = close + 1;
	// \begin{lstlisting}[language=C] -- the options describe the listing, they are not in it.
	if (body < src.size() && src[body] == '[') {
		auto opt = src.find(']', body);
		if (opt != std::string::npos) {
			body = opt + 1;
		}
	}
	// The newline that ends the \begin line is layout. The next line's INDENTATION is not,
	// so only the break itself is eaten.
	size_t skip = body;
	while (skip < src.size() && (src[skip] == ' ' || src[skip] == '\t' || src[skip] == '\r')) {
		skip++;
	}
	if (skip < src.size() && src[skip] == '\n') {
		body = skip + 1;
	}
	const std::string terminator = "\\end{" + env + "}";
	auto stop = src.find(terminator, body);
	std::string text = src.substr(body, (stop == std::string::npos ? src.size() : stop) - body);
	while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '\t')) {
		text.pop_back();
	}
	if (!text.empty()) {
		out.push_back(Token {TokenKind::TEXT, text, false});
	}
	// The closer is emitted as ordinary tokens even when the source ran out without one, so
	// the reader's environment stack always sees a balanced pair and cannot leak a level.
	out.push_back(Token {TokenKind::CONTROL_WORD, "end", false});
	out.push_back(Token {TokenKind::BEGIN_GROUP, "{", false});
	out.push_back(Token {TokenKind::TEXT, env, false});
	out.push_back(Token {TokenKind::END_GROUP, "}", false});
	i = stop == std::string::npos ? src.size() : stop + terminator.size();
	return true;
}

//! Called with `i` just past an opening math shift. Math is opaque, the same way verbatim
//! is: its content is bytes, not tokens, so the body has to be cut out HERE -- before the
//! ligature rule (`---` -> em dash) and the comment rule (`%` to end of line) get a chance
//! to run over it -- rather than reconstructed downstream from tokens that have already
//! lost the information. Without this, `$a --- b$` tokenizes `---` exactly like prose does
//! and math stops being verbatim by the time the reader ever sees it.
//!
//! `closer` is the exact byte sequence that ends the span ("$", "$$", "\)" or "\]"). The
//! closer is matched BEFORE the escape rule runs (see below), so a `\$`-escaped dollar
//! inside `$..$` is a literal dollar sign rather than the end of the formula, while `\)`
//! and `\]` -- which ARE their own closer, backslash included -- still close. Returns the
//! position just past the closer, or src.size() if the source ran out first; `body` is a
//! raw substring of `src` between the two, byte for byte -- nothing is unescaped, decoded,
//! or otherwise interpreted, because math has no shape here to interpret it INTO.
size_t LexMathBody(const std::string &src, size_t i, const std::string &closer, std::string &body) {
	size_t start = i;
	while (i < src.size()) {
		// THE CLOSER IS CHECKED BEFORE THE ESCAPE RULE, and the order is load-bearing: `\)`
		// and `\]` ARE the closer for the parenthesis/bracket spellings, backslash and all,
		// so testing the escape rule first would consume the very bytes this function exists
		// to find and math would never close. `\$` has no such conflict -- its closer is a
		// single `$` with no leading backslash -- so this ordering costs it nothing.
		if (src.compare(i, closer.size(), closer) == 0) {
			body = src.substr(start, i - start);
			return i + closer.size();
		}
		if (src[i] == '\\' && i + 1 < src.size()) {
			i += 2;
			continue;
		}
		i++;
	}
	body = src.substr(start);
	return src.size();
}

} // namespace

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
			if (name == "begin") {
				// VERBATIM IS A LEXICAL MODE, not an environment with a body: inside it `%`
				// is a percent sign, `\emph` is five characters and `---` is three hyphens.
				// Every one of those rules has ALREADY run by the time the reader sees
				// tokens, so the body has to be cut out here, where the bytes still exist.
				LexVerbatim(src, i, out);
			}
			continue;
		}
		// \( \[ are the OTHER spelling of math, and pandoc prefers them. They lex as
		// control symbols, so without this branch -- placed BEFORE the generic
		// control-symbol case below -- display math is silently dropped -- the macro table
		// never sees math at all, because math is a tokenizer construct rather than a
		// macro. Like `$`, the body is cut out with LexMathBody so ligatures and comments
		// cannot run over it.
		if (c == '\\' && i + 1 < src.size() && (src[i + 1] == '(' || src[i + 1] == '[')) {
			flush();
			bool display = src[i + 1] == '[';
			out.push_back(Token {TokenKind::MATH_SHIFT, std::string(), display});
			std::string body;
			size_t after = LexMathBody(src, i + 2, display ? "\\]" : "\\)", body);
			if (!body.empty()) {
				out.push_back(Token {TokenKind::TEXT, body, false});
			}
			out.push_back(Token {TokenKind::MATH_SHIFT, std::string(), display});
			i = after;
			continue;
		}
		// A STRAY \) or \] -- no opener seen -- is emitted as a bare MATH_SHIFT rather
		// than falling through to ordinary text, so a reader degrades it the same way it
		// already degrades any other unbalanced construct instead of leaking a spurious
		// `)` or `]` into the surrounding paragraph.
		if (c == '\\' && i + 1 < src.size() && (src[i + 1] == ')' || src[i + 1] == ']')) {
			flush();
			bool display = src[i + 1] == ']';
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
			std::string body;
			size_t after = LexMathBody(src, i + (display ? 2 : 1), display ? "$$" : "$", body);
			if (!body.empty()) {
				out.push_back(Token {TokenKind::TEXT, body, false});
			}
			out.push_back(Token {TokenKind::MATH_SHIFT, std::string(), display});
			i = after;
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
