# read_latex_blocks Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Read `.tex` / `.latex` into duck_blocks so a LaTeX document and the same document in DOCX, ODT, EPUB or RTF yield the same table of contents and the same prose.

**Architecture:** A char scanner emits a token stream knowing nothing about duck_blocks; a static disposition table says what each macro means; a parser walks the tokens with an explicit scope stack, so nesting depth falls out of stack depth. Mirrors `rtf_reader.cpp`'s `GroupState` pattern for a brace-delimited format, except the stack carries open scopes rather than a flag set.

**Tech Stack:** C++17, DuckDB table function API, no new dependencies (no pugixml, no miniz — LaTeX is plain text). Tests are DuckDB sqllogictest files under `test/sql/`.

**Spec:** `docs/superpowers/specs/2026-08-31-latex-reader-design.md`

## Global Constraints

- **duck_block spec 4.0.** `src/include/duck_block_vocabulary.hpp` is vendored at `a91d00a`. Use `DuckBlockTypes::TYPE_*` constants, never string literals.
- **Every element carries a structural `level`.** Never NULL, never below 1, never jumping by more than one from the previous element. Top-level blocks are 1; a container's children are `parent+1`; an inline is a child of its block at `block_level+1`, nesting deeper among themselves.
- **`heading_level` is separate from `level`.** Semantic rank goes in `attributes['heading_level']` (1–6); structural depth goes in `level`. Both are always present on a heading.
- **Containers carry no content.** `content` is populated **if and only if** the container has a single text child.
- **`plain` vs `paragraph`.** A bare block-level text run is `plain`; a run the source wrapped in a real paragraph is `paragraph`. This is a property of the RUN, not of whether its container has block children.
- **Lists carry both attribute spellings.** `list_type` (canonical: `bullet`/`ordered`) AND `ordered` (legacy: `true`/`false`). Ordered lists also carry `start`, `number_style`, `number_delim` **always**, including at defaults (`1`/`Decimal`/`Period`).
- **Never emit `generic`.** Unrecognised input is dropped, not wrapped.
- **Never invent an attribute value.** If duck_block has not specified one (e.g. a `list_type` for `description`), route the question to the `duck_block_utils` agent rather than guessing.
- **Style:** tabs for indentation, `clang-format` via `make format`. Match the comment density of `src/epub_reader.cpp` — explain *why*, not *what*.

---

### Task 1: Tokenizer — text, groups, control sequences

**Files:**
- Create: `src/include/latex_tokenizer.hpp`
- Create: `src/latex_tokenizer.cpp`
- Modify: `CMakeLists.txt:34` (add `src/latex_tokenizer.cpp` to `EXTENSION_SOURCES`)
- Test: `test/sql/latex_tokenizer.test`

**Interfaces:**
- Produces: `namespace duckdb::latex`, `enum class TokenKind { TEXT, CONTROL_WORD, CONTROL_SYMBOL, BEGIN_GROUP, END_GROUP, PAR_BREAK, MATH_SHIFT, END }`, `struct Token { TokenKind kind; std::string text; bool display_math = false; }`, and `std::vector<Token> Tokenize(const std::string &src)`.
- A debug table function `panduck_latex_tokens(VARCHAR) -> (kind VARCHAR, text VARCHAR)` so the tokenizer is testable from SQL without any duck_block machinery. This is permanent, not scaffolding: it is the only way to test the fiddly rules in isolation.

- [ ] **Step 1: Write the failing test**

Create `test/sql/latex_tokenizer.test`:

```
# name: test/sql/latex_tokenizer.test
# description: the LaTeX tokenizer, tested without any duck_block machinery
# group: [sql]

require panduck

# A control word is backslash plus LETTERS, and the whitespace after it is CONSUMED --
# `\LaTeX foo` is one control word then `foo`, not the word plus " foo". Getting this
# wrong inserts a space before every macro's following text.
query II
SELECT kind, text FROM panduck_latex_tokens('\textbf{a} b');
----
control_word	textbf
begin_group	{
text	a
end_group	}
text	 b
```

- [ ] **Step 2: Run it and watch it fail**

Run: `make release && build/release/test/unittest "test/sql/latex_tokenizer.test"`
Expected: FAIL — `Catalog Error: Table Function with name panduck_latex_tokens does not exist`.

- [ ] **Step 3: Write the header**

Create `src/include/latex_tokenizer.hpp`:

```cpp
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
```

- [ ] **Step 4: Implement text, groups and control sequences**

Create `src/latex_tokenizer.cpp` implementing only what Step 1's test needs:

```cpp
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
} // namespace duckdb
```

Then add the table function in the same file. Follow `src/supported_extensions.cpp:66-114` exactly for the bind/init/scan shape — it is the smallest table function in the repo and this one has the same "materialise a vector, emit it in chunks" structure:

```cpp
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
```

Register it: in `src/panduck_extension.cpp`, next to the other `Register*` calls, add `RegisterLatexTokensFunction(loader);` and include `latex_tokenizer.hpp`. Add `src/latex_tokenizer.cpp` to `EXTENSION_SOURCES` in `CMakeLists.txt`.

- [ ] **Step 5: Run the test and watch it pass**

Run: `make release && build/release/test/unittest "test/sql/latex_tokenizer.test"`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/include/latex_tokenizer.hpp src/latex_tokenizer.cpp src/panduck_extension.cpp CMakeLists.txt test/sql/latex_tokenizer.test
git commit -m "Add a LaTeX tokenizer with no duck_block dependency"
```

---

### Task 2: Tokenizer — comments, paragraph breaks, ligatures, math

**Files:**
- Modify: `src/latex_tokenizer.cpp` (the `Tokenize` loop)
- Test: `test/sql/latex_tokenizer.test`

**Interfaces:**
- Consumes: `latex::Tokenize` from Task 1.
- Produces: no new signatures. `Token::display_math` becomes meaningful.

- [ ] **Step 1: Write the failing tests**

Append to `test/sql/latex_tokenizer.test`:

```
# `%` DISCARDS TO END OF LINE, THEN THE NEWLINE, THEN THE NEXT LINE'S LEADING WHITESPACE.
# That is TeX's actual rule and the reason pandoc writes a trailing `%` -- it exists to
# suppress the space the line break would otherwise produce. A tokenizer that strips the
# comment but keeps the newline injects a leading space into every pandoc heading.
query II
SELECT kind, text FROM panduck_latex_tokens(e'a% note\n   b');
----
text	ab

# \% is a literal percent, not a comment.
query II
SELECT kind, text FROM panduck_latex_tokens('100\% sure');
----
control_symbol	%
text	 sure

# A BLANK LINE IS A PARAGRAPH BREAK; A SINGLE NEWLINE IS JUST A SPACE. That is LaTeX's
# only paragraph signal. Asserted as kinds and counts rather than as token values: a text
# run spanning a single newline CONTAINS one, and sqllogictest uses newlines as row
# boundaries, so asserting the value would be testing the harness's escaping rather than
# the tokenizer.
query II
SELECT kind, count(*) FROM panduck_latex_tokens(e'one\ntwo\n\nthree')
GROUP BY kind ORDER BY kind;
----
par_break	1
text	2

# ...and the discriminating half: NO blank line means NO break, so the whole thing is one
# text run. A tokenizer that split on every newline would pass the assertion above and
# fail this one.
query II
SELECT kind, count(*) FROM panduck_latex_tokens(e'one\ntwo')
GROUP BY kind ORDER BY kind;
----
text	1

# Ligatures are resolved in the text scanner, where the source spells them.
query II
SELECT kind, text FROM panduck_latex_tokens('a --- b -- c');
----
text	a — b – c

# Math is opaque: the shift is a token, the content is text, nothing is parsed.
query I
SELECT count(*) FROM panduck_latex_tokens('$x^2$') WHERE kind = 'math_shift';
----
2

# BOTH SPELLINGS OF MATH. \( \) and \[ \] lex as control symbols, so without an explicit
# branch they fall through to the generic control-symbol case and the math vanishes.
# pandoc PREFERS these forms, so a reader that handles only $ loses math on exactly the
# documents most likely to contain it. Math is a tokenizer construct, not a macro -- the
# disposition table never sees it.
query I
SELECT count(*) FROM panduck_latex_tokens('\\(a\\) and \\[b\\]') WHERE kind = 'math_shift';
----
4
```

- [ ] **Step 2: Run and watch them fail**

Run: `make release && build/release/test/unittest "test/sql/latex_tokenizer.test"`
Expected: FAIL — the comment test returns `text a% note\n   b` because comments are not handled yet.

- [ ] **Step 3: Implement the four rules**

In `Tokenize`, before the `text.push_back(c)` fallthrough, add these branches in this order:

```cpp
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
		// \( \) \[ \] are the OTHER spelling of math, and pandoc prefers them. They
		// lex as control symbols, so without this branch they fall through to the generic
		// control-symbol case and display math is silently dropped -- the macro table
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
```

- [ ] **Step 4: Run and watch them pass**

Run: `make release && build/release/test/unittest "test/sql/latex_tokenizer.test"`
Expected: PASS, all tokenizer assertions.

- [ ] **Step 5: Commit**

```bash
git add src/latex_tokenizer.cpp test/sql/latex_tokenizer.test
git commit -m "Handle comments, paragraph breaks, ligatures and math shifts"
```

---

### Task 3: The macro disposition table

**Files:**
- Create: `src/include/latex_macros.hpp`
- Create: `src/latex_macros.cpp`
- Modify: `CMakeLists.txt` (add `src/latex_macros.cpp`)
- Test: exercised through `test/sql/latex_reader.test` (Tasks 4-6) — see below

**Interfaces:**
- Produces: `namespace duckdb::latex`, `enum class Disposition { SEMANTIC, TRANSPARENT, DROPPED, TEXT }`, `struct MacroEntry { const char *name; Disposition disposition; const char *element_type; int args; int content_arg; const char *expansion; }`, `const MacroEntry *LookupMacro(const std::string &name)`, `const MacroEntry *LookupEnvironment(const std::string &name)`.

**NO TABLE FUNCTION.** The design doc argued for exposing this as `panduck_latex_macros()` on auditability grounds — an allowlist nobody can enumerate being a backstop with extra steps. That argument does not survive contact: a static table in a source file IS enumerable by reading it, every entry that matters is exercised by the reader's own tests, and a public function is a permanent API commitment no consumer has asked for. `panduck_latex_tokens` earns its place for a reason that does not transfer here — comment-eats-newline and control-word whitespace cannot be observed any other way, whereas a wrong table entry shows up directly as a wrong block. Add it later if something actually wants runtime introspection; it is ~40 lines against a table that will not have moved.

- [ ] **Step 1: Write the table**

Create `src/include/latex_macros.hpp` and `src/latex_macros.cpp`. The table is static data in the shape of `src/supported_extensions.cpp:34` and `src/pandoc_ast_map.cpp:18` — this codebase states its coverage as a queryable table, and this one is that for LaTeX.

`content_arg` is the 0-based index of the argument to descend into for `TRANSPARENT`, or `-1`.

Entries, exactly:

```cpp
// SEMANTIC -- inline
{"textbf", SEMANTIC, "bold", 1, 0, nullptr},   {"bf", SEMANTIC, "bold", 1, 0, nullptr},
{"emph", SEMANTIC, "italic", 1, 0, nullptr},   {"textit", SEMANTIC, "italic", 1, 0, nullptr},
{"it", SEMANTIC, "italic", 1, 0, nullptr},     {"underline", SEMANTIC, "underline", 1, 0, nullptr},
{"uline", SEMANTIC, "underline", 1, 0, nullptr},
{"sout", SEMANTIC, "strikethrough", 1, 0, nullptr}, {"st", SEMANTIC, "strikethrough", 1, 0, nullptr},
{"texttt", SEMANTIC, "code", 1, 0, nullptr},   {"textsc", SEMANTIC, "smallcaps", 1, 0, nullptr},
{"textsuperscript", SEMANTIC, "superscript", 1, 0, nullptr},
{"textsubscript", SEMANTIC, "subscript", 1, 0, nullptr},
{"href", SEMANTIC, "link", 2, 1, nullptr},     {"url", SEMANTIC, "link", 1, 0, nullptr},
{"includegraphics", SEMANTIC, "image", 1, 0, nullptr},
{"footnote", SEMANTIC, "note", 1, 0, nullptr}, {"cite", SEMANTIC, "cite", 1, 0, nullptr},

// SEMANTIC -- sectioning. heading_level is resolved at parse time from the
// documentclass, so element_type alone is not enough; the reader consults
// HeadingLevelFor() rather than a level column here.
{"part", SEMANTIC, "heading", 1, 0, nullptr},        {"chapter", SEMANTIC, "heading", 1, 0, nullptr},
{"section", SEMANTIC, "heading", 1, 0, nullptr},     {"subsection", SEMANTIC, "heading", 1, 0, nullptr},
{"subsubsection", SEMANTIC, "heading", 1, 0, nullptr}, {"paragraph", SEMANTIC, "heading", 1, 0, nullptr},
{"subparagraph", SEMANTIC, "heading", 1, 0, nullptr},

// TRANSPARENT -- drop the macro, DESCEND into the content argument.
{"hypertarget", TRANSPARENT, nullptr, 2, 1, nullptr},
{"texorpdfstring", TRANSPARENT, nullptr, 2, 0, nullptr},
{"textnormal", TRANSPARENT, nullptr, 1, 0, nullptr}, {"mbox", TRANSPARENT, nullptr, 1, 0, nullptr},
{"text", TRANSPARENT, nullptr, 1, 0, nullptr},       {"protect", TRANSPARENT, nullptr, 0, -1, nullptr},

// DROPPED -- macro AND arguments. Presentational or metadata.
{"label", DROPPED, nullptr, 1, -1, nullptr},      {"tightlist", DROPPED, nullptr, 0, -1, nullptr},
{"maketitle", DROPPED, nullptr, 0, -1, nullptr},  {"title", DROPPED, nullptr, 1, -1, nullptr},
{"author", DROPPED, nullptr, 1, -1, nullptr},     {"date", DROPPED, nullptr, 1, -1, nullptr},
{"vspace", DROPPED, nullptr, 1, -1, nullptr},     {"hspace", DROPPED, nullptr, 1, -1, nullptr},
{"newpage", DROPPED, nullptr, 0, -1, nullptr},    {"clearpage", DROPPED, nullptr, 0, -1, nullptr},
{"noindent", DROPPED, nullptr, 0, -1, nullptr},   {"centering", DROPPED, nullptr, 0, -1, nullptr},
{"pagestyle", DROPPED, nullptr, 1, -1, nullptr},  {"setlength", DROPPED, nullptr, 2, -1, nullptr},
{"index", DROPPED, nullptr, 1, -1, nullptr},      {"tableofcontents", DROPPED, nullptr, 0, -1, nullptr},
{"newcommand", DROPPED, nullptr, 2, -1, nullptr}, {"renewcommand", DROPPED, nullptr, 2, -1, nullptr},
{"bibliography", DROPPED, nullptr, 1, -1, nullptr},
{"bibliographystyle", DROPPED, nullptr, 1, -1, nullptr},

// TEXT -- expands to literal characters.
{"ldots", TEXT, nullptr, 0, -1, "…"},  {"dots", TEXT, nullptr, 0, -1, "…"},
{"textbackslash", TEXT, nullptr, 0, -1, "\\"},
{"LaTeX", TEXT, nullptr, 0, -1, "LaTeX"},   {"TeX", TEXT, nullptr, 0, -1, "TeX"},
```

Environments, a separate table looked up by `LookupEnvironment`:

```cpp
{"itemize", SEMANTIC, "list", 0, -1, "bullet"},
{"enumerate", SEMANTIC, "list", 0, -1, "ordered"},
{"quote", SEMANTIC, "blockquote", 0, -1, nullptr},
{"quotation", SEMANTIC, "blockquote", 0, -1, nullptr},
{"verbatim", SEMANTIC, "code", 0, -1, nullptr},
{"lstlisting", SEMANTIC, "code", 0, -1, nullptr},
{"center", TRANSPARENT, nullptr, 0, -1, nullptr},
{"abstract", TRANSPARENT, nullptr, 0, -1, nullptr},
{"document", TRANSPARENT, nullptr, 0, -1, nullptr},
// `description` is HELD: duck_block has no settled list_type for a definition list and
// emitting one would invent a value no consumer can read. TRANSPARENT keeps its text.
{"description", TRANSPARENT, nullptr, 0, -1, nullptr},
// Dropped whole: descending yields mangled cell and coordinate text as prose.
{"tabular", DROPPED, nullptr, 0, -1, nullptr},  {"tikzpicture", DROPPED, nullptr, 0, -1, nullptr},
{"equation", DROPPED, nullptr, 0, -1, nullptr}, {"align", DROPPED, nullptr, 0, -1, nullptr},
{"displaymath", DROPPED, nullptr, 0, -1, nullptr}, {"figure", DROPPED, nullptr, 0, -1, nullptr},
{"table", DROPPED, nullptr, 0, -1, nullptr},
```

`LookupMacro` and `LookupEnvironment` are linear scans over these arrays returning `nullptr` when absent — the tables are ~60 entries and are consulted once per token, so a map buys nothing and costs a static initialiser.

- [ ] **Step 2: Verify it compiles and links**

Run: `make release`
Expected: builds clean. There is deliberately no behavioural assertion at this task — the table is inert data until Task 4 consumes it, and an assertion here would test the compiler rather than the reader. Tasks 4-6 exercise every disposition: SEMANTIC via headings and lists, TRANSPARENT via the hypertarget case, DROPPED via maketitle and tightlist, TEXT via the em-dash and ldots.

- [ ] **Step 3: Commit**

```bash
git add src/include/latex_macros.hpp src/latex_macros.cpp CMakeLists.txt
git commit -m "Add the LaTeX macro disposition table"
```

---

### Task 4: Reader — headings, paragraphs, and the two-writer equivalence

**Files:**
- Create: `src/include/latex_reader.hpp`
- Create: `src/latex_reader.cpp`
- Modify: `CMakeLists.txt`, `src/panduck_extension.cpp`
- Modify: `src/supported_extensions.cpp:50` (flip `latex` to `STATUS_IMPLEMENTED`, reader `read_latex_blocks`)
- Test: `test/sql/latex_reader.test`

**Interfaces:**
- Consumes: `latex::Tokenize`, `latex::LookupMacro`, `latex::LookupEnvironment`.
- Produces, all in `namespace duckdb::latex`:

```cpp
//! An inline run. `level` is absolute, not relative: a run directly inside a block sits
//! at that block's level + 1, and a run nested inside another run is one deeper again.
struct LatexInline {
	std::string element_type; //!< bold, italic, text, link, image, math, ...
	std::string content;      //!< empty when this run has structured children
	std::string href;         //!< link only
	std::string src;          //!< image only
	std::string display;      //!< math only: "inline" or "block"
	int level = 2;
};

struct LatexBlock {
	std::string element_type;
	std::string content;      //!< empty when a container, or when inlines are populated
	std::string list_type;    //!< list only: "bullet" / "ordered"
	std::string list_start, number_style, number_delim; //!< ordered lists, always set
	std::string display;      //!< math only
	int heading_level = 0;    //!< 1-6 for headings, 0 otherwise
	int level = 1;            //!< structural depth; never 0, never NULL on emission
	std::vector<LatexInline> inlines;
};

//! Parse LaTeX source into blocks. Never throws: malformed input degrades.
std::vector<LatexBlock> ParseLatexString(const std::string &src);

//! heading_level for a sectioning macro under a given document class. `article` (and the
//! default, and a fragment with no preamble) puts \section at 1; `book` and `report` put
//! \chapter at 1 and shift the rest down by one. Returns 1..6, capped.
int HeadingLevelFor(const std::string &macro_name, const std::string &document_class);
```

- Two table functions: `read_latex_blocks(VARCHAR path)` and `read_latex_blocks_string(VARCHAR source)`. Both emit the canonical duck_block column set — `(kind, element_type, content, level, encoding, attributes, element_order)` — and share one row-emitting helper, so the only difference is where the bytes come from. The string form is not test scaffolding: asserting a two-line snippet is how the nesting rules in Task 5 stay readable, and every other panduck reader needs a fixture file for the same job.

- [ ] **Step 1: Write the failing test**

Create `test/sql/latex_reader.test`:

```
# name: test/sql/latex_reader.test
# description: read_latex_blocks() over the same document written by a person and by pandoc
# group: [sql]

require panduck

# THE CENTRAL ASSERTION. handwritten.tex and pandoc.tex are the SAME DOCUMENT written
# twice. pandoc buries \section two brace-levels deep inside \hypertarget{id}{% behind a
# comment token; a person writes \section at the top level. Both must yield the same
# headings at the same levels, and this is what proves TRANSPARENT works without any
# pandoc-specific rule in the reader.
query I
SELECT count(*) FROM (
  (SELECT content, attributes['heading_level'] FROM read_latex_blocks('test/fixtures/handwritten.tex')
   WHERE element_type = 'heading')
  EXCEPT
  (SELECT content, attributes['heading_level'] FROM read_latex_blocks('test/fixtures/pandoc.tex')
   WHERE element_type = 'heading')
);
----
0

# POSITIVE COMPANION, and it is load-bearing: the EXCEPT above is satisfied by MUTUAL
# EMPTINESS -- two readers that both return nothing agree perfectly. This is the same trap
# reader_registry.test documents for the dispatcher identity.
query II
SELECT content, attributes['heading_level'] FROM read_latex_blocks('test/fixtures/pandoc.tex')
WHERE element_type = 'heading' ORDER BY element_order;
----
Heading One	1
Heading Two	2

# \maketitle MUST EMIT NOTHING. handwritten.tex has \title/\author/\maketitle and
# pandoc.tex has none of them in its body, so the two documents are equivalent ONLY if
# \maketitle produces no block. Consistent with every other panduck reader: none extracts
# document metadata yet.
query I
SELECT count(*) FROM read_latex_blocks('test/fixtures/handwritten.tex')
WHERE content = 'Handwritten' OR content = 'Nobody';
----
0

# THE COMMENT MUST NOT LEAK, and the leading space it would leave is invisible on
# inspection. handwritten.tex carries "This comment must not appear in any block."
query I
SELECT count(*) FROM read_latex_blocks('test/fixtures/handwritten.tex')
WHERE content LIKE '%comment must not appear%';
----
0

# The preamble is discarded wholesale: no \usepackage line becomes a paragraph.
query I
SELECT count(*) FROM read_latex_blocks('test/fixtures/pandoc.tex')
WHERE content LIKE '%usepackage%' OR content LIKE '%documentclass%';
----
0
```

- [ ] **Step 2: Run and watch it fail**

Run: `make release && build/release/test/unittest "test/sql/latex_reader.test"`
Expected: FAIL — `read_latex_blocks does not exist`.

- [ ] **Step 3: Implement the parser core**

Create `src/latex_reader.cpp`. The parser holds a cursor over `std::vector<Token>` and a `depth` counter, exactly as `epub_reader.cpp`'s `WalkBlocks` does.

Required behaviour for this task only:

1. **Skip the preamble.** Scan for `CONTROL_WORD "begin"` followed by a group containing `document`. Everything before it is discarded except `\documentclass`, whose first required argument sets the sectioning base (`article` default; `book`/`report` shift by one). If no `\begin{document}` exists, parse the whole token stream as body — handwritten fragments are common and erroring on them helps nobody.
2. **Argument scanning.** `ReadGroup()` consumes a balanced `{...}` and returns its tokens; `SkipOptional()` consumes a `[...]` if present. Driven by `MacroEntry::args`.
3. **Sectioning.** A `SEMANTIC` macro with `element_type == "heading"` reads its content argument, flattens it to text, and emits `heading` with `attributes['heading_level']` from `HeadingLevelFor(name, documentclass)`.
4. **Paragraphs.** Text accumulates until `PAR_BREAK` or a block-level construct, then flushes as one block. A run with no formatting emits `content`; a run with formatting emits NULL content plus inline children.
5. **Dispositions.** `TRANSPARENT` recurses into `content_arg`; `DROPPED` consumes and discards `args` groups; `TEXT` appends `expansion` to the current run.

Levels: top-level blocks are 1; inlines are `block_level + 1`, nesting deeper among themselves. Emit rows the way `epub_reader.cpp:636-680` does — block row then its inline children, `element_order` incrementing across both.

- [ ] **Step 4: Run and watch it pass**

Run: `make release && build/release/test/unittest "test/sql/latex_reader.test"`
Expected: PASS.

- [ ] **Step 5: Run the whole suite**

Run: `build/release/test/unittest "test/sql/*"`
Expected: PASS. `reader_registry.test` picks `.tex` up automatically — the registry is derived, so flipping `STATUS_IMPLEMENTED` is the only change dispatch needs, and `reader_registry.test:26` (which asserts `.tex` is NOT routable) must be flipped in this task.

- [ ] **Step 6: Commit**

```bash
git add src/include/latex_reader.hpp src/latex_reader.cpp src/supported_extensions.cpp src/panduck_extension.cpp CMakeLists.txt test/sql/latex_reader.test test/sql/reader_registry.test
git commit -m "Add read_latex_blocks: headings, paragraphs, and two-writer equivalence"
```

---

### Task 5: Reader — inline formatting and genuine nesting

**Files:**
- Modify: `src/latex_reader.cpp`
- Test: `test/sql/latex_reader.test`

**Interfaces:**
- Consumes: everything from Task 4.
- Produces: no new signatures.

- [ ] **Step 1: Write the failing tests**

Append to `test/sql/latex_reader.test`:

```
# Inline formatting, both fixtures, including the two spellings of strike -- \sout in the
# handwritten file and \st in pandoc's output.
query II
SELECT element_type, content FROM read_latex_blocks('test/fixtures/pandoc.tex')
WHERE kind = 'inline' AND element_type <> 'text' ORDER BY element_order;
----
bold	bold
italic	italic
strikethrough	strike

# NESTING IS EMITTED GENUINELY. \textbf{\emph{x}} is a tree in the source and stays one:
# bold owns italic owns the text. Flattening to "strongest format wins" -- which every
# other panduck reader still does, and rtf_reader.cpp:77 documents as a limitation --
# would discard the one thing LaTeX states unambiguously.
query III
SELECT element_type, content, level FROM read_latex_blocks_string('\textbf{\emph{x}}')
WHERE kind = 'inline' ORDER BY element_order;
----
bold	NULL	2
italic	NULL	3
text	x	4

# ...AND THE SIMPLE CASE STAYS FLAT. \textbf{x} is ONE run, byte-identical to what the
# other four readers emit. Asserting only the nested case would pass against a reader that
# over-nested everything; the two together are what discriminate.
query III
SELECT element_type, content, level FROM read_latex_blocks_string('\textbf{x}')
WHERE kind = 'inline' ORDER BY element_order;
----
bold	x	2
```

- [ ] **Step 2: Run and watch them fail**

Run: `make release && build/release/test/unittest "test/sql/latex_reader.test"`
Expected: FAIL — inline children are absent or flattened.

- [ ] **Step 3: Implement the inline scope stack**

Maintain `std::vector<std::string> inline_scopes`. A `SEMANTIC` macro whose `element_type` is an inline pushes a scope, recurses into its content argument, and pops.

**You do not nest if and only if the only child is plain text.** That is the whole rule, and it is duck_block's own convention — `src/include/duck_block_types.hpp` states it as *content is populated if and only if the container has a single text child*.

So `\textbf{x}` has one text child and emits ONE run carrying `x`. `\textbf{\emph{x}}` has a child that is not plain text, so it emits a container with NULL content and a child at `level+1`. The condition is on the CHILD, not on the depth or the macro: a scope with two text runs and no formatting is still a single run, and a scope with one formatted child still nests.

Note this is the same rule as the block-side `plain`-versus-`paragraph` decision in Task 6, seen from the inline side — in both cases the question is what the container's only child actually is, and in both cases deciding it from anything else (depth, macro name, whether the container has children at all) collapses two distinct inputs into one output.

- [ ] **Step 4: Run and watch them pass**

Run: `make release && build/release/test/unittest "test/sql/latex_reader.test"`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/latex_reader.cpp test/sql/latex_reader.test
git commit -m "Emit LaTeX inline formatting, nested genuinely rather than flattened"
```

---

### Task 6: Reader — environments, lists, and the plain/paragraph distinction

**Files:**
- Modify: `src/latex_reader.cpp`
- Test: `test/sql/latex_reader.test`

**Interfaces:**
- Consumes: everything from Tasks 4 and 5.
- Produces: no new signatures.

- [ ] **Step 1: Write the failing tests**

Append to `test/sql/latex_reader.test`:

```
# LISTS ARE CONTAINERS AND SO ARE THEIR ITEMS. A bare \item text is a TIGHT item: its run
# is a `plain`, a block-level text run with NO paragraph semantics -- Pandoc's Plain
# constructor, and exactly what the source wrote. Both attribute spellings are emitted:
# list_type is canonical, `ordered` is the legacy alias a v1-era consumer reads.
query IIIII
SELECT element_type, content, level, attributes['list_type'], attributes['ordered']
FROM read_latex_blocks('test/fixtures/pandoc.tex')
WHERE element_type IN ('list', 'list_item', 'plain') ORDER BY element_order;
----
list	NULL	1	bullet	false
list_item	NULL	2	NULL	NULL
plain	bullet one	3	NULL	NULL
list_item	NULL	2	NULL	NULL
plain	bullet two	3	NULL	NULL

# A blockquote owns its paragraph one level deeper and carries no content itself.
query III
SELECT element_type, content, level FROM read_latex_blocks('test/fixtures/pandoc.tex')
WHERE element_type = 'blockquote' OR content = 'A block quote.' ORDER BY element_order;
----
blockquote	NULL	1
paragraph	A block quote.	2

# \tightlist emits nothing -- it is pandoc's spacing hint, not content. If it leaked the
# two fixtures would stop being equivalent, since handwritten.tex has no such macro.
query I
SELECT count(*) FROM read_latex_blocks('test/fixtures/pandoc.tex')
WHERE content LIKE '%tightlist%';
----
0

# THE ASYMMETRY FOR UNKNOWN INPUT. An unknown MACRO is dropped with its arguments, because
# most unrecognised macros are presentational and descending would flood the output. An
# unknown ENVIRONMENT is descended into, because most still wrap prose. `tabular` is the
# documented exception: descending yields mangled cell text as sentences.
query II
SELECT element_type, content FROM read_latex_blocks_string(
  '\begin{document}\vspace{2em}\begin{unknownenv}kept\end{unknownenv}\begin{tabular}{ll}a&b\\\end{tabular}\end{document}')
WHERE kind = 'block' ORDER BY element_order;
----
plain	kept

# UTF-8 and the em-dash ligature both survive. handwritten.tex carries "café --- em-dash".
query I
SELECT count(*) FROM read_latex_blocks('test/fixtures/handwritten.tex')
WHERE content LIKE '%café — em-dash%';
----
1
```

- [ ] **Step 2: Run and watch them fail**

Run: `make release && build/release/test/unittest "test/sql/latex_reader.test"`
Expected: FAIL — environments are not handled.

- [ ] **Step 3: Implement environments**

`\begin{name}` looks `name` up via `LookupEnvironment`:

- `SEMANTIC` → emit the container at `depth`, parse the body at `depth+1` until the matching `\end{name}`. For `list`, set `list_type` from `expansion` AND `ordered` (`"true"`/`"false"`), and for ordered lists emit `start` (from an optional `[N]`, else `1`), `number_style` (`Decimal`) and `number_delim` (`Period`) **always**.
- `TRANSPARENT` → parse the body at the SAME depth, emitting no container.
- `DROPPED` → skip to the matching `\end{name}`, emitting nothing.
- **Unknown environment → TRANSPARENT.** Most wrap prose; dropping them loses real content.
- **Unknown macro → DROPPED** with its arguments. Most are presentational.

`\item` closes any open item and opens a `list_item` at `list_level+1`. Its body parses at `list_level+2`. A body that is a **bare run** emits `plain`; a body containing a genuine paragraph emits `paragraph`. **Do not decide this from whether the item has block children** — an item can hold a nested list and still be tight; the distinction is a property of the run.

`verbatim`/`lstlisting` take bytes raw to the matching `\end`, with no tokenizing, and emit `code`.

- [ ] **Step 4: Run and watch them pass**

Run: `make release && build/release/test/unittest "test/sql/latex_reader.test"`
Expected: PASS.

- [ ] **Step 5: Run the whole suite and the tree invariant**

Run: `build/release/test/unittest "test/sql/*"`
Expected: PASS. `reader_registry.test`'s cross-reader tree invariant now covers five readers — add `read_latex_blocks('test/fixtures/pandoc.tex')` to its UNION so LaTeX is held to the same no-NULL/no-jump rule as the rest.

- [ ] **Step 6: Commit**

```bash
git add src/latex_reader.cpp test/sql/latex_reader.test test/sql/reader_registry.test
git commit -m "Read LaTeX environments, lists and the tight/loose item distinction"
```

---

### Task 7: Robustness, math, and the third fixture

**Files:**
- Create: `test/fixtures/edge_cases.tex`
- Modify: `src/latex_reader.cpp`
- Test: `test/sql/latex_reader.test`

**Interfaces:**
- Consumes: everything above.
- Produces: no new signatures.

- [ ] **Step 1: Write the fixture**

Create `test/fixtures/edge_cases.tex`. The two existing fixtures are a MATCHED PAIR and must not be edited — the two-writer equivalence assertion is exactly their value, and adding cases to one breaks it.

```latex
% Cases the matched pair cannot carry: math, verbatim, and malformed input.
\documentclass{article}
\begin{document}
Inline $x^2$ and display $$y = mx + b$$ here.
\begin{verbatim}
  \section{not a heading}  % not a comment
\end{verbatim}
\textbf{unclosed brace
\end{document}
```

- [ ] **Step 2: Write the failing tests**

```
# MATH IS OPAQUE: the TeX is kept verbatim as content and nothing is parsed. `display`
# distinguishes $..$ from $$..$$.
query III
SELECT element_type, content, attributes['display'] FROM read_latex_blocks('test/fixtures/edge_cases.tex')
WHERE element_type = 'math' ORDER BY element_order;
----
math	x^2	inline
math	y = mx + b	block

# VERBATIM SUSPENDS EVERYTHING. Its body is bytes: the \section inside must NOT become a
# heading and the % must NOT be stripped as a comment.
query I
SELECT count(*) FROM read_latex_blocks('test/fixtures/edge_cases.tex')
WHERE element_type = 'heading' AND content = 'not a heading';
----
0

query I
SELECT count(*) FROM read_latex_blocks('test/fixtures/edge_cases.tex')
WHERE element_type = 'code' AND content LIKE '%not a comment%';
----
1

# MALFORMED INPUT DEGRADES, IT DOES NOT THROW. An unclosed brace closes at EOF. A reader
# that refuses a slightly-broken document is worse than one that returns most of it, and
# TeX in the wild is frequently slightly broken.
query I
SELECT count(*) > 0 FROM read_latex_blocks('test/fixtures/edge_cases.tex');
----
1
```

- [ ] **Step 3: Run and watch them fail**

Run: `make release && build/release/test/unittest "test/sql/latex_reader.test"`
Expected: FAIL — math is not emitted.

- [ ] **Step 4: Implement math and the degradation rules**

`MATH_SHIFT` opens a math span; text accumulates verbatim until the matching shift; emit `math` with `attributes['display']` = `inline` or `block`. At EOF: close every open scope and every open environment, emit what has accumulated, never throw.

- [ ] **Step 5: Run the whole suite**

Run: `build/release/test/unittest "test/sql/*"`
Expected: PASS.

- [ ] **Step 6: Update the reader's self-description and docs**

`src/supported_extensions.cpp`: replace the `latex` note with what is actually read — sectioning, paragraphs, lists, quotes, verbatim, inline formatting with genuine nesting, links, images, footnotes, opaque math; tables and `\newcommand` expansion not read. Add a `docs/readers.md` section matching the other four readers' entries.

- [ ] **Step 7: Commit**

```bash
git add test/fixtures/edge_cases.tex src/latex_reader.cpp src/supported_extensions.cpp docs/readers.md test/sql/latex_reader.test
git commit -m "Handle math, verbatim and malformed LaTeX; document the reader"
```

---

### Task 8: Vocabulary conformance and the differential harness

**Files:**
- Modify: `scripts/check_duck_block_vocabulary.py` (`SCAN_GLOBS` already covers `src/*.cpp`; verify GAPS)
- Modify: `test/roundtrip/check_roundtrip.py`
- Test: `test/sql/reader_registry.test` — RE-RUN ONLY. Task 6 already added LaTeX to the cross-reader tree invariant; do not add a second UNION arm.

**Interfaces:**
- Consumes: the finished reader.

- [ ] **Step 1: Run the vocabulary check**

Run: `make check-vocabulary`
Expected: GAPS shrinks — the reader now branches on `math`, `note`, `cite`, `plain` and others previously listed as unhandled. Remove any entry from `INTENTIONAL_GAPS` in `scripts/check_duck_block_vocabulary.py` that LaTeX now handles, and leave a reason for every entry that remains.

- [ ] **Step 2: Add LaTeX to the differential harness**

`test/roundtrip/check_roundtrip.py` reads each fixture with panduck AND pandoc and compares at declared equivalence levels. Add `handwritten.tex` and `pandoc.tex`. Run `make test_roundtrip`; it skips cleanly when pandoc is absent.

- [ ] **Step 3: Run everything**

```bash
make release
build/release/test/unittest "test/sql/*"
make check-vocabulary
make test_roundtrip
make test_pandoc_alignment
```

Expected: all pass, `check-vocabulary` green at spec 4.0.

- [ ] **Step 4: Commit**

```bash
git add scripts/check_duck_block_vocabulary.py test/roundtrip/check_roundtrip.py test/sql/reader_registry.test
git commit -m "Hold the LaTeX reader to the vocabulary check and the differential harness"
```

---

## Deferred, deliberately

1. **The other four readers are NOT retrofitted to nest inlines.** LaTeX will emit inline depth > 1 that docx/odt/rtf/epub never produce, because those formats carry formatting as a flag set. `rtf_reader.cpp:77` documents the limitation. Retrofitting is a separate task.
2. **One deliberately nested fixture through each consumer's writer.** panduck becomes the first producer of inline depth > 1; `duckdb_markdown` fixed a walker bug this class produces. Cheap insurance, separate task.
3. **Accents** (`\'{e}` → `é`). Both fixtures use literal UTF-8.
4. **Tables**, consistent with every other panduck reader.
5. **`description` list_type** — held pending a duck_block ruling. Route to the `duck_block_utils` agent.
