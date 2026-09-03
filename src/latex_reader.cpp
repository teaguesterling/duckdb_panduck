#include "block_json.hpp"
#include "latex_reader.hpp"

#include "duck_block_types.hpp"
#include "latex_macros.hpp"
#include "latex_tokenizer.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <map>

namespace duckdb {
namespace latex {

namespace {

bool IsSpace(char c) {
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

bool IsBlank(const std::string &s) {
	for (char c : s) {
		if (!IsSpace(c)) {
			return false;
		}
	}
	return true;
}

std::string Trim(const std::string &s) {
	size_t b = 0, e = s.size();
	while (b < e && IsSpace(s[b])) {
		b++;
	}
	while (e > b && IsSpace(s[e - 1])) {
		e--;
	}
	return s.substr(b, e - b);
}

void RightTrim(std::string &s) {
	while (!s.empty() && IsSpace(s.back())) {
		s.pop_back();
	}
}

//! A control symbol that stands for the literal character it escapes. Everything else --
//! \, \; \! and the accents -- is spacing or diacritics, which duck_block has no place
//! for, so it contributes nothing rather than leaking a stray character into the text.
bool IsEscapedLiteral(const std::string &sym) {
	return sym.size() == 1 && std::string("%&_$#{}").find(sym[0]) != std::string::npos;
}

// The sectioning ladder, coarsest first. LaTeX has no absolute heading rank: \section is
// the top of an article and the second rank of a book, so the rank is only knowable once
// the documentclass is. Index into this array IS the depth in the ladder.
const char *const SECTIONING[] = {"part",          "chapter",   "section",     "subsection",
                                  "subsubsection", "paragraph", "subparagraph"};
constexpr int SECTIONING_COUNT = 7;

} // namespace

int HeadingLevelFor(const std::string &macro_name, const std::string &document_class) {
	int rung = -1;
	for (int i = 0; i < SECTIONING_COUNT; i++) {
		if (macro_name == SECTIONING[i]) {
			rung = i;
			break;
		}
	}
	if (rung < 0) {
		return 1;
	}
	// article has no \chapter, so its ladder starts one rung lower: \section is 1. book and
	// report do, so theirs starts at \chapter and every later macro shifts down by one.
	// \part sits above both and has nowhere to go, so it clamps onto 1 with whatever is
	// below it -- a document that uses \part AND \chapter loses that one distinction,
	// which is strictly better than emitting a heading_level of 0 that no consumer allows.
	bool has_chapter = document_class == "book" || document_class == "report" || document_class == "memoir" ||
	                   document_class == "scrbook" || document_class == "scrreprt";
	int level = has_chapter ? rung : rung - 1;
	if (level < 1) {
		return 1;
	}
	return level > 6 ? 6 : level;
}

namespace {

//! The parser. Holds a cursor over a token vector and the paragraph being accumulated;
//! Run() is re-entrant over a SUB-vector so that descending into a TRANSPARENT macro's
//! content argument shares the same block output and the same pending run. That sharing is
//! the whole trick: \hypertarget{id}{\section{X}} emits its heading into the enclosing
//! document rather than into a nested one, which is why no pandoc-specific rule is needed.
class Parser {
public:
	explicit Parser(const std::string &src) : tokens(Tokenize(src)) {
	}

	std::vector<LatexBlock> Parse();

private:
	//! One open \begin{...}. The stack exists so that \end can be matched BY NAME: a
	//! generic begin/end counter lets \end{document} close an unclosed \begin{itemize},
	//! after which the document's real ending has been spent as a nest level and the reader
	//! runs to end of input emitting the tail it was supposed to stop before.
	struct EnvFrame {
		std::string name;
		int saved_depth = 1; //!< depth to restore when this environment closes
		int list_depth = 1;  //!< lists only: the depth the `list` block itself sits at
		bool is_list = false;
		//! lists only: a `description`, whose \item[label] is a TERM rather than a custom
		//! bullet, and whose every \item therefore emits TWO list_items.
		bool is_definition = false;
		bool item_open = false; //!< lists only: an \item is accumulating
		//! TIGHTNESS IS A PROPERTY OF THE LIST, NOT OF THE ITEM. A paragraph break anywhere
		//! after the first \item loosens EVERY item, because that is what the spacing it
		//! produces means. Tracking it per item instead makes the LAST item of every loose
		//! list come out tight -- nothing follows it, so nothing can loosen it -- which is
		//! wrong on the canonical spelling of a loose list rather than on an odd one.
		bool list_loose = false;
		//! lists only: every list_item's index in `blocks`. Kept because the content rule
		//! cannot be applied until the list closes and its tightness is known.
		std::vector<size_t> item_indices;
		//! Index in `blocks` of the container this environment emitted, or NO_BLOCK for a
		//! TRANSPARENT or DROPPED environment that emitted none. Kept so that the container
		//! can be WITHDRAWN if it turns out to have closed empty -- see PopEnvironment.
		static constexpr size_t NO_BLOCK = (size_t)-1;
		size_t block_index = NO_BLOCK;
	};

	std::vector<Token> tokens;
	std::string document_class = "article";
	std::vector<LatexBlock> blocks;
	std::vector<EnvFrame> env_stack;
	//! Structural depth of the block currently being built. A container pushes this and its
	//! \end restores it, so every block emitted between the two is at least one deeper.
	int depth = 1;
	//! How many INLINE scopes are open around the run being accumulated. Zero while reading
	//! a block's own text; one inside \textbf{...}; two inside the \emph within it.
	int inline_depth = 0;
	bool stopped = false;

	// The run being accumulated. `pending` is loose text; `pending_inlines` is non-empty
	// once any formatted run has appeared, at which point the block emits children instead
	// of a flat content string. Both are SWAPPED OUT for the duration of an inline scope,
	// so a nested scope accumulates its own children without seeing its parent's.
	std::string pending;
	std::vector<LatexInline> pending_inlines;

	//! Absolute level for a run pushed into the scope currently open. A block sits at
	//! `depth`, a run directly inside it one deeper, and a run inside another run deeper
	//! again -- which is the only reason nesting is expressible in a flat row stream.
	int InlineLevel() const {
		return depth + 1 + inline_depth;
	}

	void Run(std::vector<Token> &toks);
	void ControlWord(std::vector<Token> &toks, size_t &i, const std::string &name);
	bool ReadGroup(std::vector<Token> &toks, size_t &i, std::vector<Token> &out);
	void BeginEnvironment(std::vector<Token> &toks, size_t &i, const std::string &name);
	void EndEnvironment(const std::string &name);
	void PopEnvironment();
	void ScanToEnd(std::vector<Token> &toks, size_t &i, const std::string &name, std::string *raw);
	//! tabular -> a spec 5.0 native table. Consumes through \end{<name>}.
	void EmitTabular(std::vector<Token> &toks, size_t &i, const std::string &name);
	//! \title/\author/\date -> kind='value'. Returns true if `name` was one of them.
	bool CaptureMetadata(const std::string &name, std::vector<Token> &arg);
	void EmitMetadata();
	//! title/date -> one flattened string. author -> one entry per \and-separated name.
	std::map<std::string, std::vector<std::string>> meta;
	void StartItem(std::vector<Token> &toks, size_t &i);
	void ResolveList(EnvFrame &frame);
	void NoteParagraphBreak();
	const char *RunElementType() const;
	void AppendText(const std::string &text);
	void PushInline(LatexInline inl, std::vector<LatexInline> children);
	void ParseInlineScope(std::vector<Token> &content, std::string &text, std::vector<LatexInline> &children);
	void FlushRun();
	void EmitHeading(const std::string &name, std::vector<Token> &arg);
};

//! Cut an optional `[...]` argument out of the token stream. `captured` receives its
//! contents when it lay wholly inside one TEXT token, which is the only shape a caller
//! that wants to READ the argument (\begin{enumerate}[3]) can rely on; a cross-token
//! optional is skipped but reported as empty rather than half-reconstructed.
//!
//! ONE COPY, CALLED FROM BOTH PATHS. Flatten() used to scan a macro's arguments itself,
//! and its copy never learned about optional ones -- so `\section{Fig
//! \includegraphics[width=2cm]{f.png} end}` came out titled "Fig [width=2cm]f.png end"
//! while the parsing path read the same macro correctly. Headings are what doc_toc
//! returns, so the divergence surfaced in the reader's primary output. Free function
//! rather than a Parser member for exactly that reason: it touches no parser state, and
//! Flatten is not a member.
void SkipOptional(std::vector<Token> &toks, size_t &i, std::string *captured = nullptr) {
	if (captured) {
		captured->clear();
	}
	// `[` and `]` are ORDINARY CHARACTERS to the tokenizer -- they are not TeX syntax, only
	// a convention macros implement themselves -- so an optional argument is not a token
	// boundary and has to be cut out of the TEXT run it lives inside. pandoc's
	// `\documentclass[\n]{article}` is exactly this shape.
	size_t start = i;
	while (start < toks.size() && toks[start].kind == TokenKind::TEXT && IsBlank(toks[start].text)) {
		start++;
	}
	if (start >= toks.size() || toks[start].kind != TokenKind::TEXT) {
		return;
	}
	auto &text = toks[start].text;
	size_t k = 0;
	while (k < text.size() && IsSpace(text[k])) {
		k++;
	}
	if (k >= text.size() || text[k] != '[') {
		return;
	}
	size_t close = text.find(']', k);
	if (close != std::string::npos) {
		if (captured) {
			*captured = text.substr(k + 1, close - k - 1);
		}
		text = text.substr(close + 1);
		// Token::raw is the SOURCE of the whole run, so cutting the run leaves it describing
		// bytes that are no longer there. Dropping it says "no source spelling is known any
		// more", and a literal Flatten then falls back to the resolved text -- which is what
		// an option list is read as anyway. Keeping a stale copy would be the silent bug.
		toks[start].raw.clear();
		i = start;
		return;
	}
	// A `[` whose `]` is in a LATER token is still a real optional argument --
	// `\includegraphics[width=\textwidth]{img.png}` lexes as TEXT "[width=",
	// CONTROL_WORD textwidth, TEXT "]" -- so the search has to cross tokens. It must not
	// cross a construct an optional argument can never CONTAIN, though, and that boundary is
	// the whole safety of this function: an UNCLOSED `[` otherwise scans to the first `]`
	// anywhere in the rest of the document, deletes both spans and welds two paragraphs into
	// one. Run in the preamble the same runaway consumes \begin{document}, `body_start` is
	// never found, and the reader returns NOTHING for a document it could mostly have read.
	//
	// The budget is a second belt: a pathological source with a bracket and a boundary-free
	// tail should cost a bounded scan, not a quadratic one.
	constexpr size_t OPTIONAL_ARG_BUDGET = 64;
	const size_t limit = toks.size() < start + 1 + OPTIONAL_ARG_BUDGET ? toks.size() : start + 1 + OPTIONAL_ARG_BUDGET;
	for (size_t j = start + 1; j < limit; j++) {
		auto kind = toks[j].kind;
		if (kind == TokenKind::END || kind == TokenKind::PAR_BREAK || kind == TokenKind::MATH_SHIFT ||
		    kind == TokenKind::BEGIN_GROUP || kind == TokenKind::END_GROUP) {
			break;
		}
		if (kind == TokenKind::CONTROL_WORD && (toks[j].text == "begin" || toks[j].text == "end")) {
			break;
		}
		if (kind != TokenKind::TEXT) {
			continue;
		}
		auto found = toks[j].text.find(']');
		if (found == std::string::npos) {
			continue;
		}
		toks[j].text = toks[j].text.substr(found + 1);
		toks[j].raw.clear(); // stale once the run is cut -- see the in-token case above
		text.clear();
		toks[start].raw.clear();
		i = j;
		return;
	}
	// Unclosed, or closed only beyond a boundary an optional argument cannot cross: leave the
	// `[` exactly where it is. It is then ordinary text, which is what it turned out to be.
}

//! Flatten tokens to plain text, resolving the dispositions that can contribute
//! characters. Used for anything that is text BY CONSTRUCTION -- a heading's title, a
//! link's target, the documentclass -- where structure cannot survive anyway.
//!
//! NON-CONST because it shares SkipOptional with the parsing path, and that helper cuts
//! the optional argument out of the TEXT token it lives inside. Both callers already hand
//! over a token group they own and do not read again.
//! `literal` takes every TEXT run as the SOURCE spelled it rather than as the tokenizer
//! resolved it -- see Token::raw. It is set for exactly the arguments that are machine
//! readable rather than prose: a link target and an image path. `\href{http://x/~bob}{y}`
//! must yield a tilde, because U+00A0 makes the URL unresolvable and does not look wrong;
//! the LABEL beside it is prose and goes through the ordinary path, ligatures and all.
void Flatten(std::vector<Token> &toks, std::string &out, bool literal = false);

void FlattenAppend(std::string &out, const std::string &text) {
	for (char c : text) {
		if (IsSpace(c)) {
			if (!out.empty() && out.back() != ' ') {
				out.push_back(' ');
			}
			continue;
		}
		out.push_back(c);
	}
}

void Flatten(std::vector<Token> &toks, std::string &out, bool literal) {
	for (size_t i = 0; i < toks.size(); i++) {
		const auto &tok = toks[i];
		switch (tok.kind) {
		case TokenKind::TEXT:
			FlattenAppend(out, literal && !tok.raw.empty() ? tok.raw : tok.text);
			break;
		case TokenKind::CONTROL_SYMBOL:
			if (IsEscapedLiteral(tok.text)) {
				out += tok.text;
			} else if (tok.text == "\\") {
				FlattenAppend(out, " ");
			}
			break;
		case TokenKind::CONTROL_WORD: {
			auto *entry = LookupMacro(tok.text);
			// Collect the macro's brace groups whatever its disposition is: the difference
			// between DROPPED and TRANSPARENT is which of them get flattened, not whether
			// they are consumed.
			std::vector<std::vector<Token>> args;
			int want = entry ? entry->args : 0;
			size_t j = i + 1;
			for (int a = 0; a < want; a++) {
				// The SAME skip the parsing path performs before every required argument.
				// Without it a heading title picks up \includegraphics's [width=2cm] as
				// prose, and the two paths disagree about the same macro.
				SkipOptional(toks, j);
				while (j < toks.size() && toks[j].kind == TokenKind::TEXT && IsBlank(toks[j].text)) {
					j++;
				}
				if (j >= toks.size() || toks[j].kind != TokenKind::BEGIN_GROUP) {
					break;
				}
				std::vector<Token> group;
				int nest = 1;
				j++;
				while (j < toks.size() && nest > 0) {
					if (toks[j].kind == TokenKind::BEGIN_GROUP) {
						nest++;
					} else if (toks[j].kind == TokenKind::END_GROUP) {
						nest--;
						if (nest == 0) {
							j++;
							break;
						}
					}
					group.push_back(toks[j]);
					j++;
				}
				args.push_back(std::move(group));
			}
			i = j - 1;
			if (!entry) {
				break; // unknown macro: the name is not text, so nothing is contributed
			}
			if (entry->disposition == Disposition::TEXT && entry->expansion) {
				out += entry->expansion;
				break;
			}
			if (entry->disposition == Disposition::DROPPED) {
				break;
			}
			// SEMANTIC and TRANSPARENT both have a content argument; flattening loses the
			// formatting, which is the point of flattening.
			int content_arg = entry->content_arg >= 0 ? entry->content_arg : 0;
			if (content_arg < (int)args.size()) {
				Flatten(args[content_arg], out, literal);
			}
			break;
		}
		default:
			break; // groups and math shifts carry no characters of their own
		}
	}
}

std::string FlattenTrimmed(std::vector<Token> &toks) {
	std::string out;
	Flatten(toks, out);
	return Trim(out);
}

//! FlattenTrimmed for an argument that is a machine-readable string rather than prose.
std::string FlattenLiteralTrimmed(std::vector<Token> &toks) {
	std::string out;
	Flatten(toks, out, true);
	return Trim(out);
}

//! Concatenate a math span's tokens back into its content. This is deliberately NOT
//! Flatten(): Flatten resolves macros and drops braces because it is building prose. Math
//! needs none of that reconstruction any more -- LexMathBody (latex_tokenizer.cpp) already
//! cut the raw source out before ligatures or comment-stripping could run over it, the same
//! way LexVerbatim does for \begin{verbatim}, so a well-formed span is always exactly one
//! TEXT token here. Non-TEXT tokens are skipped rather than reconstructed: they should not
//! occur inside a span LexMathBody produced, and math is opaque regardless -- there is no
//! shape here worth guessing at for a token that reaches this function unexpectedly. The
//! one departure from byte-exact is the trailing Trim(): leading/trailing whitespace is cut
//! the same way it is from every other block and inline `content` in this reader, so `$
//! x^2 $` and `$x^2$` are the same element -- internal whitespace is untouched either way.
std::string MathText(const std::vector<Token> &toks, size_t begin, size_t end) {
	std::string out;
	for (size_t k = begin; k < end; k++) {
		if (toks[k].kind == TokenKind::TEXT) {
			out += toks[k].text;
		}
	}
	return Trim(out);
}

void Parser::AppendText(const std::string &text) {
	for (char c : text) {
		if (!IsSpace(c)) {
			pending.push_back(c);
			continue;
		}
		if (pending.empty()) {
			// Whitespace at the very start of a block is layout and is dropped; whitespace
			// after a formatted run is a real word boundary -- `\emph{a} \emph{b}` is two
			// words -- so it survives even though `pending` is empty.
			if (pending_inlines.empty()) {
				continue;
			}
			pending.push_back(' ');
			continue;
		}
		if (pending.back() != ' ') {
			pending.push_back(' ');
		}
	}
}

void Parser::PushInline(LatexInline inl, std::vector<LatexInline> children) {
	if (!pending.empty()) {
		LatexInline text;
		text.element_type = DuckBlockTypes::INLINE_TEXT;
		text.content = pending;
		text.level = InlineLevel();
		pending.clear();
		pending_inlines.push_back(std::move(text));
	}
	inl.level = InlineLevel();
	pending_inlines.push_back(std::move(inl));
	// The children were levelled by the scope that produced them, one deeper than `inl`,
	// and they follow it in document order. A flat, ordered, level-carrying stream is how
	// duck_block spells a tree; there is no separate child list to fill in.
	for (auto &child : children) {
		pending_inlines.push_back(std::move(child));
	}
}

//! Read a macro's content argument AS INLINE CONTENT rather than flattening it to text.
//! Returns the scope's loose text in `text` and its structured runs in `children`, and
//! exactly one of the two is populated when the scope has a single child.
//!
//! THE CALLER'S RULE, which this function deliberately does not decide: content is carried
//! iff the only child is plain text. So `children` empty means the caller carries `text`;
//! `children` non-empty means the caller nests and carries nothing.
void Parser::ParseInlineScope(std::vector<Token> &content, std::string &text, std::vector<LatexInline> &children) {
	// The enclosing run is set aside rather than shared: a nested scope must not see its
	// parent's half-built text as its own leading content, and must not flush it either.
	std::string outer_pending;
	std::vector<LatexInline> outer_inlines;
	outer_pending.swap(pending);
	outer_inlines.swap(pending_inlines);

	inline_depth++;
	Run(content);
	RightTrim(pending);
	if (!pending_inlines.empty() && !pending.empty()) {
		// Trailing text alongside formatted siblings is a child in its own right -- the
		// scope has more than one child, so it nests and every one of them must be a row.
		LatexInline tail;
		tail.element_type = DuckBlockTypes::INLINE_TEXT;
		tail.content = pending;
		tail.level = InlineLevel();
		pending_inlines.push_back(std::move(tail));
		pending.clear();
	}
	inline_depth--;

	text = std::move(pending);
	children = std::move(pending_inlines);
	pending = std::move(outer_pending);
	pending_inlines = std::move(outer_inlines);
}

//! `paragraph` or `plain` for the run about to be flushed. THE DIFFERENCE IS NOT
//! COSMETIC: `plain` is Pandoc's Plain constructor -- block-level text carrying NO
//! paragraph semantics -- and it is the whole of what makes a tight list item tight.
const char *Parser::RunElementType() const {
	if (env_stack.empty()) {
		return DuckBlockTypes::TYPE_PARAGRAPH;
	}
	const auto &frame = env_stack.back();
	// A LIST ITEM IS THE ONLY SCOPE THAT PRODUCES `plain`. Its run is provisional: emitted
	// as `plain`, then re-typed to `paragraph` by ResolveList() if the list turns out
	// loose, or folded onto the item's own content if it turns out to be the item's only
	// child. Neither is knowable until the list closes.
	//
	// EVERY OTHER SCOPE GETS `paragraph`, including a transparent environment's, and
	// including one whose name the table never claimed. The same bytes must not read
	// differently for being wrapped in a name we happen to recognise -- and body text
	// accumulating into a paragraph until a blank line IS what LaTeX does inside every
	// environment, whether or not panduck models the environment itself.
	if (frame.is_list && frame.item_open) {
		return DuckBlockTypes::TYPE_PLAIN;
	}
	return DuckBlockTypes::TYPE_PARAGRAPH;
}

//! A blank line, or \par, which is the same fact spelled as a control word. Only a list
//! cares: it is what makes the list LOOSE, and it counts only once an \item has opened --
//! a blank line between \begin{itemize} and the first \item is a layout habit rather than
//! an authorial one, and reading it as spacing would loosen half the lists ever written.
void Parser::NoteParagraphBreak() {
	if (env_stack.empty()) {
		return;
	}
	auto &frame = env_stack.back();
	if (frame.is_list && !frame.item_indices.empty()) {
		frame.list_loose = true;
	}
}

void Parser::FlushRun() {
	if (inline_depth > 0) {
		// AN INLINE SCOPE HAS NO BLOCK BOUNDARY INSIDE IT. Emitting a block from here would
		// publish one whose inline children sit at an inline depth the block never had,
		// which breaks the invariant that a child is exactly one level below its parent.
		// The callers that can reach this -- \begin, \end and a sectioning macro inside
		// \textbf{...} -- are malformed LaTeX; keeping the run intact degrades better than
		// splitting it.
		return;
	}
	if (pending_inlines.empty()) {
		RightTrim(pending);
		if (pending.empty()) {
			pending.clear();
			return;
		}
		LatexBlock block;
		block.element_type = RunElementType();
		block.content = pending;
		block.level = depth;
		pending.clear();
		blocks.push_back(std::move(block));
		return;
	}
	RightTrim(pending);
	if (!pending.empty()) {
		LatexInline text;
		text.element_type = DuckBlockTypes::INLINE_TEXT;
		text.content = pending;
		text.level = depth + 1;
		pending_inlines.push_back(std::move(text));
	}
	pending.clear();
	LatexBlock block;
	block.element_type = RunElementType();
	block.level = depth;
	block.inlines = std::move(pending_inlines);
	pending_inlines.clear();
	if (!block.inlines.empty()) {
		blocks.push_back(std::move(block));
	}
}

bool Parser::ReadGroup(std::vector<Token> &toks, size_t &i, std::vector<Token> &out) {
	out.clear();
	size_t j = i;
	// TeX skips whitespace before an argument. The tokenizer already ate the spaces after a
	// control word, but a newline survives as a TEXT token and would otherwise hide the
	// brace -- which is exactly the shape pandoc writes \hypertarget in.
	while (j < toks.size() && toks[j].kind == TokenKind::TEXT && IsBlank(toks[j].text)) {
		j++;
	}
	if (j >= toks.size() || toks[j].kind == TokenKind::END) {
		return false;
	}
	if (toks[j].kind != TokenKind::BEGIN_GROUP) {
		// An unbraced argument is a SINGLE TOKEN in TeX -- `\section x` takes just the `x`.
		// Honouring that beats consuming the rest of the line, which would swallow a
		// paragraph whenever a macro's braces were omitted.
		if (toks[j].kind == TokenKind::CONTROL_WORD || toks[j].kind == TokenKind::CONTROL_SYMBOL) {
			out.push_back(toks[j]);
			i = j + 1;
			return true;
		}
		if (toks[j].kind == TokenKind::TEXT && !toks[j].text.empty()) {
			// A SINGLE TOKEN IS A SINGLE CHARACTER, AND A CHARACTER IS NOT A BYTE. Taking
			// one byte cuts `\textbf émile` between the two bytes of `é`, which is not a
			// degraded reading of well-formed LaTeX -- it is an Invalid unicode error out
			// of DuckDB, on input this reader promises to read.
			const size_t n = Utf8SequenceLength(toks[j].text, 0);
			out.push_back(Token {TokenKind::TEXT, toks[j].text.substr(0, n), false});
			toks[j].text.erase(0, n);
			i = j;
			return true;
		}
		return false;
	}
	int nest = 1;
	j++;
	while (j < toks.size() && toks[j].kind != TokenKind::END) {
		if (toks[j].kind == TokenKind::BEGIN_GROUP) {
			nest++;
		} else if (toks[j].kind == TokenKind::END_GROUP) {
			nest--;
			if (nest == 0) {
				i = j + 1;
				return true;
			}
		}
		out.push_back(toks[j]);
		j++;
	}
	i = j; // unbalanced braces: take what there is rather than rewinding forever
	return true;
}

//! Advance past the body of `\begin{name}`, stopping just after its OWN `\end{name}`.
//! Nesting is counted only for environments of the SAME name, and that is the whole point:
//! a generic begin/end counter treats \end{document} as the closer of an unclosed
//! \begin{itemize}, spends the document's real ending on a nest level, and then runs to end
//! of input emitting the tail it was supposed to stop before.
//!
//! `raw`, when given, collects the body's TEXT tokens unfolded -- for verbatim, whose body
//! the tokenizer has already delivered as one uninterpreted run.
void Parser::ScanToEnd(std::vector<Token> &toks, size_t &i, const std::string &name, std::string *raw) {
	int nest = 1;
	while (i < toks.size() && toks[i].kind != TokenKind::END) {
		if (toks[i].kind == TokenKind::CONTROL_WORD && (toks[i].text == "begin" || toks[i].text == "end")) {
			bool is_begin = toks[i].text == "begin";
			size_t j = i + 1;
			std::vector<Token> env;
			ReadGroup(toks, j, env);
			auto env_name = FlattenTrimmed(env);
			i = j;
			if (env_name != name) {
				if (!is_begin && env_name == "document") {
					// The DOCUMENT ending, reached inside an environment that never closed.
					// Reading on past it is how the old counter emitted the tail.
					stopped = true;
					return;
				}
				continue;
			}
			if (is_begin) {
				nest++;
				continue;
			}
			if (--nest == 0) {
				return;
			}
			continue;
		}
		if (raw && toks[i].kind == TokenKind::TEXT) {
			*raw += toks[i].text;
		}
		i++;
	}
}

//! The first number of an ordered list, from `\begin{enumerate}`'s optional argument.
//!
//! ONLY TWO SPELLINGS ARE A START, and the narrowness is the point: an enumerate optional
//! is usually enumitem KEY-VALUE OPTIONS, and reading "the first integer anywhere in it"
//! turns `[itemsep=0pt]` into start=0 -- a number no list starts at -- and
//! `[topsep=2pt,start=5]` into start=2. Both are routine markup, so the loose reading is
//! wrong on ordinary documents rather than on odd ones. Anything else defaults to 1.
std::string ParseListStart(const std::string &optional) {
	auto text = Trim(optional);
	if (!text.empty() && text.find_first_not_of("0123456789") == std::string::npos) {
		return text; // the bare form, \begin{enumerate}[3]
	}
	auto at = text.find("start=");
	if (at != std::string::npos) {
		size_t k = at + 6;
		while (k < text.size() && IsSpace(text[k])) {
			k++;
		}
		size_t end = k;
		while (end < text.size() && text[end] >= '0' && text[end] <= '9') {
			end++;
		}
		if (end > k) {
			return text.substr(k, end - k);
		}
	}
	return "1";
}

void Parser::BeginEnvironment(std::vector<Token> &toks, size_t &i, const std::string &name) {
	auto *entry = LookupEnvironment(name);
	if (entry && entry->disposition == Disposition::DROPPED) {
		// tabular, figure, tikzpicture: descending yields cell and coordinate text as
		// sentences, which reads as prose the document never contained.
		ScanToEnd(toks, i, name, nullptr);
		return;
	}

	EnvFrame frame;
	frame.name = name;
	frame.saved_depth = depth;
	if (!entry || entry->disposition != Disposition::SEMANTIC) {
		// UNKNOWN ENVIRONMENT -> TRANSPARENT, the exact opposite of the rule for an unknown
		// MACRO. A macro is usually presentational and usually wraps a fragment, so
		// descending into every one of them floods the output; an environment usually wraps
		// PROSE, and dropping it loses paragraphs. Whether the NAME was in the table changes
		// nothing further: its body reads exactly as a known transparent environment's does.
		env_stack.push_back(std::move(frame));
		return;
	}

	std::string element_type = entry->element_type ? entry->element_type : "";
	if (element_type.empty()) {
		// A SEMANTIC entry with no element_type would emit a block with an empty type,
		// which is the same defect as emitting `generic`: a row no consumer can dispatch
		// on. Reading it as TRANSPARENT keeps the prose and claims nothing.
		env_stack.push_back(std::move(frame));
		return;
	}
	if (element_type == DuckBlockTypes::TYPE_TABLE) {
		// A table's cells are not prose and must not go through the run machinery: `&` and
		// `\\` are structure, and descending would emit them as sentences.
		EmitTabular(toks, i, name);
		return;
	}
	if (element_type == DuckBlockTypes::TYPE_CODE) {
		// verbatim and lstlisting have no body to parse: the tokenizer took theirs as bytes.
		std::string body;
		ScanToEnd(toks, i, name, &body);
		if (!body.empty()) {
			LatexBlock block;
			block.element_type = DuckBlockTypes::TYPE_CODE;
			block.content = std::move(body);
			block.level = depth;
			blocks.push_back(std::move(block));
		}
		return;
	}

	LatexBlock block;
	block.element_type = element_type;
	block.level = depth;
	if (element_type == DuckBlockTypes::TYPE_LIST) {
		// The list TYPE rides in `expansion` rather than in a field of its own: for an
		// environment there is no expansion text, and duck_block owns the two spellings --
		// minting a local constant for a value the spec defines would be this project
		// inventing a name for someone else's vocabulary.
		block.list_type = entry->expansion ? entry->expansion : DuckBlockTypes::LIST_TYPE_BULLET;
		std::string optional;
		SkipOptional(toks, i, &optional);
		if (block.list_type == DuckBlockTypes::LIST_TYPE_ORDERED) {
			// ALWAYS, not only when the source said so. A consumer that renders numbering
			// otherwise has to invent the default the reader already knows.
			block.list_start = ParseListStart(optional);
			block.number_style = "Decimal";
			block.number_delim = "Period";
		}
		frame.is_list = true;
		// A literal, because duck_block declares ATTR_LIST_TYPE but none of its VALUES --
		// the same hole duck_block_utils just closed for `role`. A misspelled "definiton"
		// here would be valid, conformant and lint-clean. Raised upstream.
		frame.is_definition = block.list_type == DuckBlockTypes::LIST_TYPE_DEFINITION;
		frame.list_depth = depth;
	}
	blocks.push_back(std::move(block));
	frame.block_index = blocks.size() - 1;
	depth++;
	env_stack.push_back(std::move(frame));
}

void Parser::PopEnvironment() {
	auto &frame = env_stack.back();
	if (frame.is_list) {
		ResolveList(frame);
	}
	// AN EMPTY CONTAINER IS WITHDRAWN, not published. `\begin{itemize}\end{itemize}` and
	// `\begin{quote}\end{quote}` otherwise emit a block with NULL content and no children,
	// which is the one shape a duck_block consumer can neither render nor index -- the same
	// objection that already makes `\section{}` not a heading, `\href{url}{}` not a link and
	// an empty formatted run not an element. Being the LAST block is what proves it is
	// empty: every block emitted since it opened was emitted while it was open, so it is a
	// descendant. Innermost environments pop first, so a container holding nothing but
	// another empty container is withdrawn in the same sweep.
	if (frame.block_index != EnvFrame::NO_BLOCK && frame.block_index + 1 == blocks.size()) {
		blocks.erase(blocks.begin() + (long)frame.block_index);
	}
	depth = frame.saved_depth;
	env_stack.pop_back();
}

void Parser::EndEnvironment(const std::string &name) {
	size_t found = env_stack.size();
	for (size_t k = env_stack.size(); k > 0; k--) {
		if (env_stack[k - 1].name == name) {
			found = k - 1;
			break;
		}
	}
	if (found == env_stack.size()) {
		// A STRAY \end CLOSES NOTHING -- popping a level that was never pushed would lift
		// the rest of the document out of whatever container it really sits in. The one
		// exception is \end{document}, which ends the document from wherever it is found,
		// including from inside environments the source forgot to close.
		if (name == "document") {
			while (!env_stack.empty()) {
				PopEnvironment();
			}
			stopped = true;
		}
		return;
	}
	while (env_stack.size() > found) {
		PopEnvironment();
	}
	if (name == "document") {
		stopped = true;
	}
}

//! Settle every item of a closing list, which is the earliest moment either question can
//! be answered -- both depend on facts the list only finishes supplying at its \end.
//!
//! LOOSE: every item's own run becomes a `paragraph`, because the spacing a blank line
//! produces applies to the whole list. Nothing is folded: the source wrote paragraphs.
//!
//! TIGHT: THE CONTENT RULE. content is carried iff the container's only child is a plain
//! text run -- and a lone bare run IS that single text child, so it becomes the item's own
//! content and no child row is emitted at all. Decided from WHAT the child is, not from
//! whether the item has block children: an item can hold a nested list and still be tight,
//! so a rule keyed on "has block children" reads `\item text` and `\item \par text`
//! correctly and then gets `\item text` + sublist wrong.
//!
//! Items are settled LAST FIRST so that folding one away cannot move the next one's index.
void Parser::ResolveList(EnvFrame &frame) {
	frame.item_open = false;
	// A run belongs to the item it sits DIRECTLY under. Anything deeper is inside a nested
	// container -- a sublist's own items, a quote's paragraphs -- and is that container's
	// business, not this list's.
	const int child_level = frame.list_depth + 2;
	for (size_t n = frame.item_indices.size(); n > 0; n--) {
		const size_t item = frame.item_indices[n - 1];
		const size_t end = n < frame.item_indices.size() ? frame.item_indices[n] : blocks.size();
		if (frame.list_loose) {
			for (size_t b = item + 1; b < end; b++) {
				if (blocks[b].level == child_level && blocks[b].element_type == DuckBlockTypes::TYPE_PLAIN) {
					blocks[b].element_type = DuckBlockTypes::TYPE_PARAGRAPH;
				}
			}
			continue;
		}
		if (end != item + 2) {
			continue; // no children, or a block sibling: the run keeps its own row
		}
		auto &child = blocks[item + 1];
		if (child.element_type != DuckBlockTypes::TYPE_PLAIN || !child.inlines.empty()) {
			// A formatted run has no single string to carry, so it stays a `plain` with its
			// inline tree intact -- which is also what Pandoc emits for `\item \textbf{a} b`.
			continue;
		}
		blocks[item].content = std::move(child.content);
		blocks.erase(blocks.begin() + (long)(item + 1));
	}
}

bool Parser::CaptureMetadata(const std::string &name, std::vector<Token> &arg) {
	// \title, \author and \date were DROPPED with their arguments -- the text was parsed
	// and thrown away. A discard, not a fidelity gap, and the only unrecoverable one this
	// reader had.
	//
	// They stay OFF the block path: \maketitle must still emit nothing, or a document with
	// a title block stops being equivalent to the same document without one. The text goes
	// to the metadata axis instead, which is where pandoc puts it too.
	if (name != "title" && name != "author" && name != "date") {
		return false;
	}
	if (name != "author") {
		meta[name] = {FlattenTrimmed(arg)};
		return true;
	}
	// \author IS A LIST, ALWAYS. Measured: pandoc yields MetaList even for a single
	// author, where \title of the same document yields MetaInlines. Two shapes for what
	// looks like the same kind of field, and both are pandoc's -- so `author` cannot be
	// generalised from `title` in this reader, nor from Org's, where two \#+AUTHOR: lines
	// concatenate into ONE MetaInlines instead.
	std::vector<std::string> names;
	std::vector<Token> current;
	for (auto &tok : arg) {
		if (tok.kind == TokenKind::CONTROL_WORD && tok.text == "and") {
			auto one = FlattenTrimmed(current);
			if (!one.empty()) {
				names.push_back(std::move(one));
			}
			current.clear();
			continue;
		}
		current.push_back(tok);
	}
	auto last = FlattenTrimmed(current);
	if (!last.empty()) {
		names.push_back(std::move(last));
	}
	// `\author{}` yields an EMPTY list, not a list holding one empty name -- pandoc emits
	// MetaList [] for it. The key is still present: an empty field is a field, and a
	// consumer cannot recover "declared and left blank" from silence.
	meta[name] = std::move(names);
	return true;
}

void Parser::EmitMetadata() {
	// AFTER the blocks -- spec 6.2 makes the ordering a contract. std::map iterates sorted,
	// which is also pandoc's Meta serialisation order, so a consumer comparing panduck's
	// output against pandoc_ast_to_blocks sees the same sequence.
	for (auto &kv : meta) {
		LatexBlock block;
		block.kind = DuckBlockTypes::KIND_VALUE;
		block.key = kv.first;
		block.level = 1;
		if (kv.first != "author") {
			block.element_type = DuckBlockTypes::VALUE_INLINES;
			if (!kv.second.empty() && !kv.second[0].empty()) {
				LatexInline text;
				text.element_type = DuckBlockTypes::INLINE_TEXT;
				text.content = kv.second[0];
				text.level = 2;
				block.inlines.push_back(std::move(text));
			}
			blocks.push_back(std::move(block));
			continue;
		}
		block.element_type = DuckBlockTypes::VALUE_LIST;
		blocks.push_back(std::move(block));
		for (auto &one : kv.second) {
			LatexBlock item;
			item.kind = DuckBlockTypes::KIND_VALUE;
			item.element_type = DuckBlockTypes::VALUE_INLINES;
			item.level = 2;
			LatexInline text;
			text.element_type = DuckBlockTypes::INLINE_TEXT;
			text.content = one;
			text.level = 3;
			item.inlines.push_back(std::move(text));
			blocks.push_back(std::move(item));
		}
	}
}

void Parser::EmitTabular(std::vector<Token> &toks, size_t &i, const std::string &name) {
	FlushRun();
	SkipOptional(toks, i); // \begin{tabular}[t]{...}: vertical positioning, presentational
	std::vector<Token> spec;
	// The COLUMN SPEC is read and discarded. `{|l|r|}` carries alignment and rules, and the
	// native {headers, rows} projection has nowhere to put either -- the same loss the EPUB
	// reader takes, and for the same reason: attributes['pandoc_ast'] is where a producer
	// WITH a Pandoc tuple keeps them, and panduck never has one. Reading it rather than
	// leaving it is what stops `{|l|r|}` arriving as a first table cell.
	ReadGroup(toks, i, spec);

	std::vector<std::vector<std::string>> rows;
	std::vector<bool> ruled_after;
	bool leading_rule = false;
	std::vector<std::vector<Token>> row_cells;
	std::vector<Token> cell;

	auto end_cell = [&]() {
		row_cells.push_back(std::move(cell));
		cell.clear();
	};
	auto end_row = [&]() {
		end_cell();
		std::vector<std::string> flat;
		bool any = false;
		for (auto &c : row_cells) {
			auto t = FlattenTrimmed(c);
			if (!t.empty()) {
				any = true;
			}
			flat.push_back(std::move(t));
		}
		row_cells.clear();
		if (any) {
			// A row of nothing but whitespace contributes NO row. A trailing `\\` produces
			// one of these, and pandoc emits it as an empty row -- measured. Matching that
			// would put a blank line in every table that ends the way LaTeX tables usually
			// do, so this diverges deliberately and matches the EPUB reader's cell-less
			// <tr> rule instead.
			rows.push_back(std::move(flat));
			ruled_after.push_back(false);
		}
	};

	for (; i < toks.size(); i++) {
		auto &tok = toks[i];
		if (tok.kind == TokenKind::END) {
			break;
		}
		if (tok.kind == TokenKind::CONTROL_WORD && tok.text == "end") {
			// THE DOCUMENT ENDING STOPS THE SCAN, exactly as ScanToEnd does it. An unclosed
			// `\begin{tabular}` otherwise runs to end of input and welds the rest of the
			// file into a cell: `\begin{tabular}{ll}a\end{document} after this` produced a
			// table containing "adocument after this", with the document's remaining prose
			// swallowed into it.
			//
			// Same runaway shape SkipOptional documents a few hundred lines up. It
			// reappeared because a NEW scanner needs the boundary reasoning again from
			// scratch -- the old DROPPED path inherited it from ScanToEnd for free, so
			// replacing that path with a hand-written walker silently gave the guarantee
			// back. The `stopped` flag is ScanToEnd's own mechanism, reused rather than
			// re-invented.
			size_t j = i + 1;
			std::vector<Token> nm;
			ReadGroup(toks, j, nm);
			auto env_name = FlattenTrimmed(nm);
			if (env_name == name) {
				i = j;
				break;
			}
			if (env_name == "document") {
				stopped = true;
				i = j;
				break;
			}
			// Some OTHER environment's \end, inside a cell. Not ours to act on: fall through
			// and let the tokens be cell content, so a nested environment cannot truncate
			// the table.
		}
		if (tok.kind == TokenKind::CONTROL_WORD &&
		    (tok.text == "hline" || tok.text == "toprule" || tok.text == "midrule" || tok.text == "bottomrule")) {
			// A RULE IS THE ONLY HEADER SIGNAL LaTeX HAS. tabular has no thead: pandoc makes
			// the first row a header exactly when the table opens with a rule AND the first
			// row is followed by one. Measured against pandoc 3.1.3 -- without the rules it
			// emits no header at all, so this is its convention rather than an invention.
			// booktabs' toprule/midrule are accepted for the same reason.
			if (rows.empty()) {
				leading_rule = true;
			} else {
				ruled_after.back() = true;
			}
			continue;
		}
		if (tok.kind == TokenKind::CONTROL_SYMBOL && tok.text == "\\") {
			end_row();
			continue;
		}
		if (tok.kind == TokenKind::TEXT && tok.text.find('&') != std::string::npos) {
			// `&` is an ordinary character to the tokenizer, so a cell boundary lives INSIDE
			// a TEXT run and the run has to be cut. An ESCAPED `\&` never reaches here: it
			// arrives as a CONTROL_SYMBOL, so a raw `&` in a TEXT token is always a
			// separator and never content.
			std::string seg;
			for (char ch : tok.text) {
				if (ch == '&') {
					Token piece;
					piece.kind = TokenKind::TEXT;
					piece.text = seg;
					cell.push_back(piece);
					end_cell();
					seg.clear();
				} else {
					seg.push_back(ch);
				}
			}
			Token piece;
			piece.kind = TokenKind::TEXT;
			piece.text = seg;
			cell.push_back(piece);
			continue;
		}
		cell.push_back(tok);
	}
	end_row(); // a final row with no trailing `\\`

	if (rows.empty()) {
		return; // an empty tabular is scaffolding, matching the EPUB reader's empty table
	}

	std::vector<std::string> headers;
	size_t first = 0;
	if (leading_rule && !ruled_after.empty() && ruled_after[0] && rows.size() > 1) {
		headers = rows[0];
		first = 1;
	}
	std::vector<std::vector<std::string>> body(rows.begin() + (long)first, rows.end());

	LatexBlock block;
	block.element_type = DuckBlockTypes::TYPE_TABLE;
	block.content = BuildTableJson(headers, body);
	block.encoding = DuckBlockTypes::ENCODING_JSON;
	block.level = depth;
	blocks.push_back(std::move(block));
}

void Parser::StartItem(std::vector<Token> &toks, size_t &i) {
	if (env_stack.empty() || !env_stack.back().is_list) {
		// \item WITH NO LIST TO BELONG TO. This used to be reached by \begin{description},
		// which was TRANSPARENT while duck_block had no settled list_type for a definition
		// list. Since spec 5.0 a description IS a list, so that route is gone -- but the
		// branch is still live for a bare \item and for one inside a genuinely transparent
		// environment such as `center`.
		//
		// Ending the run is all that can be honoured; the label is left as text, because
		// deleting text on a guess is worse than losing the structure.
		return;
	}
	auto &frame = env_stack.back();
	// \item[label] IS TWO DIFFERENT THINGS depending on the environment it is in. In
	// itemize/enumerate it is a CUSTOM BULLET: presentational, with no duck_block field to
	// hold it, so it is dropped inside a list and kept as text outside one where it is the
	// only text there is. In a `description` it is the TERM -- the half of the pair that
	// carries the meaning -- and dropping it would discard the document's content.
	std::string label;
	SkipOptional(toks, i, &label);
	if (frame.is_definition) {
		// Tokenized and flattened rather than taken as the raw substring: SkipOptional cuts
		// the label out of a TEXT run as source text, so \item[\textbf{T}] would otherwise
		// reach `content` as literal TeX. Emitting markup as text is the same defect as the
		// `~` that once arrived in a URL as a non-breaking space.
		auto label_toks = Tokenize(label);
		auto term_text = FlattenTrimmed(label_toks);
		if (!term_text.empty()) {
			LatexBlock term;
			term.element_type = DuckBlockTypes::TYPE_LIST_ITEM;
			term.role = DuckBlockTypes::ROLE_TERM;
			term.content = std::move(term_text);
			term.level = frame.list_depth + 1;
			blocks.push_back(std::move(term));
			// Recorded as an item even though it is already complete. ResolveList derives
			// each item's child range from the NEXT entry in this vector; leaving the term
			// out would make the following pair's term read as the previous definition's
			// child, and the content rule would then decline to fire on every tight item.
			frame.item_indices.push_back(blocks.size() - 1);
		}
	}
	LatexBlock item;
	item.element_type = DuckBlockTypes::TYPE_LIST_ITEM;
	if (frame.is_definition) {
		item.role = DuckBlockTypes::ROLE_DEFINITION;
	}
	item.level = frame.list_depth + 1;
	blocks.push_back(std::move(item));
	frame.item_indices.push_back(blocks.size() - 1);
	frame.item_open = true;
	depth = frame.list_depth + 2;
}

void Parser::EmitHeading(const std::string &name, std::vector<Token> &arg) {
	FlushRun();
	auto title = FlattenTrimmed(arg);
	if (title.empty()) {
		// `\\section{}`, and `\\section` at end of input, are not headings: they would emit a
		// block with NULL content and no children to justify the NULL, which is the one shape
		// a duck_block consumer cannot render or index. The inline path already refuses an
		// empty formatted run for the same reason; the two have to agree.
		return;
	}
	LatexBlock block;
	block.element_type = DuckBlockTypes::TYPE_HEADING;
	block.content = title;
	block.heading_level = HeadingLevelFor(name, document_class);
	block.level = depth;
	blocks.push_back(std::move(block));
}

void Parser::ControlWord(std::vector<Token> &toks, size_t &i, const std::string &name) {
	if (name == "begin") {
		FlushRun();
		std::vector<Token> env;
		ReadGroup(toks, i, env);
		BeginEnvironment(toks, i, FlattenTrimmed(env));
		return;
	}
	if (name == "end") {
		// FLUSH FIRST, and the order is load-bearing: the run still belongs to the scope
		// that is about to close, so an item's last run has to be finished while the item
		// is still open -- ResolveList settles every item from the blocks that sit between
		// them, and a run left pending here would not be among those blocks.
		FlushRun();
		std::vector<Token> env;
		ReadGroup(toks, i, env);
		EndEnvironment(FlattenTrimmed(env));
		return;
	}
	if (name == "item") {
		FlushRun();
		StartItem(toks, i);
		return;
	}
	if (name == "par") {
		// \par IS A BLANK LINE spelled as a control word, and the tokenizer only produces
		// PAR_BREAK from the blank-line spelling. Keying the tight/loose rule on the token
		// alone would read `\item \par text` as tight, because the macro table never
		// claimed \par and an unclaimed macro is simply dropped.
		if (inline_depth > 0) {
			AppendText(" ");
			return;
		}
		NoteParagraphBreak();
		FlushRun();
		return;
	}

	auto *entry = LookupMacro(name);
	if (!entry) {
		// A macro panduck does not claim: drop the NAME, keep everything around it. The
		// alternative -- consuming arguments we cannot count -- deletes text on the
		// strength of a guess.
		return;
	}

	std::vector<std::vector<Token>> args;
	for (int a = 0; a < entry->args; a++) {
		SkipOptional(toks, i);
		std::vector<Token> group;
		if (!ReadGroup(toks, i, group)) {
			break;
		}
		args.push_back(std::move(group));
	}
	// Stands in for an argument the macro declared but the source did not supply. It is a
	// LOCAL, one per invocation, because ControlWord is now re-entrant through inline
	// scopes as well as TRANSPARENT descent: a parser-wide buffer handed to a nested Run()
	// would be the same object an outer frame still holds a reference to.
	std::vector<Token> absent_arg;
	auto arg_at = [&](int index) -> std::vector<Token> & {
		if (index < 0 || index >= (int)args.size()) {
			absent_arg.clear();
			return absent_arg;
		}
		return args[index];
	};

	if (entry->disposition == Disposition::DROPPED &&
	    CaptureMetadata(name, const_cast<std::vector<Token> &>(arg_at(0)))) {
		return;
	}

	switch (entry->disposition) {
	case Disposition::DROPPED:
		// The macro AND its arguments vanish. \maketitle is the load-bearing case: it must
		// emit NOTHING, or a document with a title block stops being equivalent to the same
		// document without one.
		return;
	case Disposition::TEXT:
		if (entry->expansion) {
			AppendText(entry->expansion);
		}
		return;
	case Disposition::TRANSPARENT: {
		// Descend into the content argument and keep reading as if the macro were never
		// there. Shared state means a block-level construct inside it lands in the
		// enclosing document, which is what makes pandoc's \hypertarget-wrapped headings
		// indistinguishable from a person's bare \section.
		auto &content = arg_at(entry->content_arg);
		if (!content.empty()) {
			Run(content);
		}
		return;
	}
	case Disposition::SEMANTIC:
		break;
	}

	std::string element_type = entry->element_type ? entry->element_type : "";
	if (element_type == DuckBlockTypes::TYPE_HEADING) {
		EmitHeading(name, arg_at(entry->content_arg >= 0 ? entry->content_arg : 0));
		return;
	}

	LatexInline inl;
	inl.element_type = element_type;
	std::vector<LatexInline> children;
	if (element_type == DuckBlockTypes::INLINE_IMAGE) {
		// An image's argument is a FILENAME, not content: there is no child to nest, and no
		// ligature either -- `\includegraphics{~/img.png}` is a path with a tilde in it.
		inl.src = FlattenLiteralTrimmed(arg_at(0));
	} else {
		if (element_type == DuckBlockTypes::INLINE_LINK) {
			// \href{url}{text} and \url{url} differ only in whether the target is also the
			// label, which content_arg already encodes: argument 0 is the target either way.
			// Read BEFORE descending, because descending mutates the token group it reads.
			// LITERAL: the target is a URL, so `~` and `--` are the bytes pandoc wrote and
			// not typography. The label -- argument 1, parsed below -- is prose and is not.
			inl.href = FlattenLiteralTrimmed(arg_at(0));
		}
		// NESTING, AND THE RULE THAT DECIDES IT. \textbf{\emph{x}} is a tree in the source;
		// flattening the argument to text dropped the inner \emph without trace. So the
		// argument is PARSED, and then: content is carried iff the only child is plain text.
		// The condition is on what the child IS -- not on the depth, not on the macro, and
		// not on whether there are children at all -- because deciding it any other way
		// collapses \textbf{x} and \textbf{\emph{x}} onto the same shape.
		std::string text;
		ParseInlineScope(arg_at(entry->content_arg >= 0 ? entry->content_arg : 0), text, children);
		if (children.empty()) {
			inl.content = std::move(text);
		}
		if (element_type == DuckBlockTypes::INLINE_LINK && children.empty() &&
		    (entry->content_arg <= 0 || inl.content.empty())) {
			// TWO CASES, ONE ANSWER: THE LABEL IS THE TARGET. `\url{x}` says so by
			// construction -- its content argument IS argument 0 -- and there the label has
			// to be the target AS WRITTEN, or a URL with a `~` in it renders with a
			// non-breaking space that no reader can see and no browser can follow.
			// `\href{url}{}` has no label at all, and a link with NULL content and no
			// children is the one shape a consumer can neither render nor index -- the same
			// objection that makes \section{} not a heading -- so it gets the same answer
			// rather than a second convention.
			inl.content = inl.href;
		}
	}
	if (inl.content.empty() && children.empty() && inl.src.empty() && inl.href.empty()) {
		return; // an empty formatted run is not an element, it is punctuation
	}
	PushInline(std::move(inl), std::move(children));
}

void Parser::Run(std::vector<Token> &toks) {
	size_t i = 0;
	while (i < toks.size() && !stopped) {
		auto kind = toks[i].kind;
		if (kind == TokenKind::END) {
			break;
		}
		switch (kind) {
		case TokenKind::TEXT:
			AppendText(toks[i].text);
			i++;
			break;
		case TokenKind::PAR_BREAK:
			if (inline_depth > 0) {
				// A blank line inside \textbf{...} cannot start a paragraph -- the run would
				// have to end mid-argument -- so it is the word boundary it looks like.
				AppendText(" ");
			} else {
				// NOTED BEFORE THE FLUSH: the run ENDING at a blank line is itself a
				// paragraph, so the scope has to know before the run is typed.
				NoteParagraphBreak();
				FlushRun();
			}
			i++;
			break;
		case TokenKind::BEGIN_GROUP:
		case TokenKind::END_GROUP:
			// A BARE GROUP IS TRANSPARENT. In TeX it is a scope, and a font switch inside it
			// (`{\bfseries x}`) applies to exactly the text it wraps -- but the reader keeps
			// no scope stack, so the choice is between keeping the text and losing the
			// formatting, or dropping both. Keeping the text is the smaller error, and it is
			// the same answer \strong and every other unclaimed macro already gets.
			i++;
			break;
		case TokenKind::MATH_SHIFT: {
			// MATH IS OPAQUE. LexMathBody (latex_tokenizer.cpp) already cut this span's
			// content out as raw bytes -- untouched by ligatures, comment-stripping or macro
			// lookup -- so this only carries it into an inline `math` run, exactly where it
			// sits in whatever text surrounds it. `display` distinguishes $$..$$ / \[..\]
			// from $..$ / \(..\), which is the one fact the tokenizer already decided and the
			// reader only has to relay.
			bool display = toks[i].display_math;
			size_t start = i + 1;
			size_t j = start;
			while (j < toks.size() && toks[j].kind != TokenKind::MATH_SHIFT && toks[j].kind != TokenKind::END) {
				j++;
			}
			auto content = MathText(toks, start, j);
			if (!content.empty()) {
				LatexInline inl;
				inl.element_type = DuckBlockTypes::INLINE_MATH;
				inl.content = content;
				inl.display = display ? "block" : "inline";
				PushInline(std::move(inl), {});
			}
			i = j < toks.size() && toks[j].kind == TokenKind::MATH_SHIFT ? j + 1 : j;
			break;
		}
		case TokenKind::CONTROL_SYMBOL: {
			auto sym = toks[i].text;
			i++;
			if (IsEscapedLiteral(sym)) {
				pending.push_back(sym[0]);
			} else if (sym == "\\") {
				AppendText(" "); // an explicit line break is a word boundary, not a block
			}
			break;
		}
		case TokenKind::CONTROL_WORD: {
			auto name = toks[i].text;
			i++;
			ControlWord(toks, i, name);
			break;
		}
		default:
			i++;
			break;
		}
	}
}

std::vector<LatexBlock> Parser::Parse() {
	// THE PREAMBLE IS DISCARDED WHOLESALE except for \documentclass. Parsing it would turn
	// every \usepackage into a paragraph, and there is nothing in it a duck_block wants:
	// \title and \author are metadata, which no panduck reader extracts yet.
	size_t body_start = 0;
	for (size_t i = 0; i < tokens.size(); i++) {
		if (tokens[i].kind == TokenKind::END) {
			break;
		}
		if (tokens[i].kind != TokenKind::CONTROL_WORD) {
			continue;
		}
		if (tokens[i].text == "documentclass") {
			size_t j = i + 1;
			SkipOptional(tokens, j);
			std::vector<Token> arg;
			if (ReadGroup(tokens, j, arg)) {
				auto cls = FlattenTrimmed(arg);
				if (!cls.empty()) {
					document_class = cls;
				}
			}
			// EVIDENCE OF A PREAMBLE, even when \\begin{document} never arrives. Falling all
			// the way back to token 0 for such a source re-parses \\documentclass itself as
			// prose -- its class name is an unknown macro's transparent brace group -- so a
			// truncated document leads with a paragraph reading "article". Claiming the
			// narrowest thing the evidence supports (the preamble runs AT LEAST this far)
			// beats claiming there was no preamble at all.
			body_start = j;
			i = j > i ? j - 1 : i;
			continue;
		}
		if (tokens[i].text == "title" || tokens[i].text == "author" || tokens[i].text == "date") {
			// METADATA LIVES IN THE PREAMBLE, which the body pass never sees: Parse() runs
			// tokens from body_start onward, so \title{X} before \begin{document} reached
			// ControlWord exactly never. Capturing it in the body pass alone emitted nothing
			// and looked like the macros were still being dropped -- the capture worked, the
			// tokens simply never arrived.
			//
			// Scanned here rather than by widening the body, because the preamble is not
			// prose: running it as body is what the body_start machinery above exists to
			// prevent.
			size_t j = i + 1;
			std::vector<Token> arg;
			if (ReadGroup(tokens, j, arg)) {
				CaptureMetadata(tokens[i].text, arg);
				i = j > i ? j - 1 : i;
			}
			continue;
		}
		if (tokens[i].text == "begin") {
			size_t j = i + 1;
			std::vector<Token> arg;
			if (ReadGroup(tokens, j, arg) && FlattenTrimmed(arg) == "document") {
				body_start = j;
				break;
			}
			i = j > i ? j - 1 : i;
		}
	}

	// No \begin{document} means a FRAGMENT, and a fragment is a body -- from token 0 when
	// nothing suggested a preamble, or from just past \documentclass when something did.
	// read_latex_blocks on a snippet is the common case for read_latex_blocks_string, and
	// erroring on it would make the string form useless for exactly what it exists for.
	std::vector<Token> body(tokens.begin() + body_start, tokens.end());
	Run(body);
	FlushRun();
	// A TRUNCATED SOURCE still has to finish its containers: `\begin{itemize}\item a` with
	// no \end reaches here with the item open, and an item closed by end-of-input gets the
	// same content rule as one closed by \end -- the document being cut short is not a
	// statement about how its last item was written.
	while (!env_stack.empty()) {
		PopEnvironment();
	}
	// Metadata is appended AFTER every block, including the ones a truncated source leaves
	// to be closed above -- spec 6.2 makes body-then-metadata a contract, and closing the
	// containers first is what keeps it true for a document that was cut short.
	EmitMetadata();
	return std::move(blocks);
}

} // namespace

std::vector<LatexBlock> ParseLatexString(const std::string &src) {
	Parser parser(src);
	return parser.Parse();
}

} // namespace latex

namespace {

//! One emitted row, already shaped like a duck_block.
struct BlockRow {
	//! Defaults to `text`; only `table` differs. Column 4 emitted a hardcoded constant
	//! until spec 5.0 gave table a JSON content schema, so the one element_type needing a
	//! different encoding was the one the emitter could not express.
	std::string encoding = DuckBlockTypes::ENCODING_TEXT;
	std::string kind;
	std::string element_type;
	std::string content;
	bool has_level = false;
	int32_t level = 0;
	std::map<std::string, std::string> attributes;
	int32_t element_order = 0;
};

struct LatexReaderBindData : public TableFunctionData {
	std::vector<BlockRow> rows;
};

struct LatexReaderGlobalState : public GlobalTableFunctionState {
	idx_t offset = 0;

	static unique_ptr<GlobalTableFunctionState> Init(ClientContext &, TableFunctionInitInput &) {
		return make_uniq<LatexReaderGlobalState>();
	}
};

void LatexColumns(vector<LogicalType> &return_types, vector<string> &names) {
	// Column order mirrors the duck_block struct so a row casts straight to duck_block.
	names = {"kind", "element_type", "content", "level", "encoding", "attributes", "element_order"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::INTEGER,
	                LogicalType::VARCHAR, LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR),
	                LogicalType::INTEGER};
}

//! The ONE row emitter. read_latex_blocks and read_latex_blocks_string differ only in
//! where the bytes come from, so anything that lives past this point -- level arithmetic,
//! attribute spelling, element_order -- cannot drift between them.
void BuildRows(const std::string &source, std::vector<BlockRow> &rows) {
	int32_t order = 0;
	for (auto &block : latex::ParseLatexString(source)) {
		BlockRow row;
		row.kind = block.kind.empty() ? DuckBlockTypes::KIND_BLOCK : block.kind;
		row.element_type = block.element_type;
		row.content = block.content;
		if (!block.encoding.empty()) {
			row.encoding = block.encoding;
		}
		row.element_order = order++;
		if (block.heading_level > 0) {
			// SEMANTIC RANK, not structural depth. A top-level \subsection is level 1 and
			// heading_level 2; the two are different facts and conflating them renders a
			// heading by where it sits rather than what it is.
			row.attributes[DuckBlockTypes::ATTR_HEADING_LEVEL] = std::to_string(block.heading_level);
		}
		if (!block.key.empty()) {
			row.attributes[DuckBlockTypes::ATTR_KEY] = block.key;
		}
		if (!block.role.empty()) {
			row.attributes[DuckBlockTypes::ATTR_ROLE] = block.role;
		}
		if (!block.list_type.empty()) {
			// BOTH SPELLINGS, matching every other panduck reader: `list_type` is canonical
			// under spec 4.0 and `ordered` is the legacy alias consumers written against v1
			// still read.
			row.attributes[DuckBlockTypes::ATTR_ORDERED_LEGACY] =
			    block.list_type == DuckBlockTypes::LIST_TYPE_ORDERED ? "true" : "false";
			row.attributes[DuckBlockTypes::ATTR_LIST_TYPE] = block.list_type;
			if (!block.list_start.empty()) {
				row.attributes["start"] = block.list_start;
				row.attributes["number_style"] = block.number_style;
				row.attributes["number_delim"] = block.number_delim;
			}
		}
		// EVERY ELEMENT CARRIES A STRUCTURAL LEVEL. Top level is 1; an inline is a CHILD of
		// its block, so it is at least one deeper. The inline's own level is authoritative
		// rather than recomputed here, because a run nested inside another run is deeper
		// again and only the parser knows by how much.
		const int32_t block_level = block.level > 0 ? block.level : 1;
		row.has_level = true;
		row.level = block_level;
		rows.push_back(std::move(row));

		for (auto &inl : block.inlines) {
			BlockRow child;
			child.kind = DuckBlockTypes::KIND_INLINE;
			child.element_type = inl.element_type;
			child.content = inl.content;
			child.has_level = true;
			child.level = inl.level > block_level ? inl.level : block_level + 1;
			child.element_order = order++;
			if (!inl.href.empty()) {
				child.attributes["href"] = inl.href;
			}
			if (!inl.src.empty()) {
				child.attributes["src"] = inl.src;
			}
			if (!inl.display.empty()) {
				child.attributes["display"] = inl.display;
			}
			rows.push_back(std::move(child));
		}
	}
}

unique_ptr<FunctionData> LatexFileBind(ClientContext &context, TableFunctionBindInput &input,
                                       vector<LogicalType> &return_types, vector<string> &names) {
	LatexColumns(return_types, names);

	auto path = input.inputs[0].GetValue<string>();
	auto &fs = FileSystem::GetFileSystem(context);
	if (!fs.FileExists(path)) {
		throw IOException("read_latex_blocks: file not found: %s", path);
	}
	auto handle = fs.OpenFile(path, FileOpenFlags::FILE_FLAGS_READ);
	auto size = fs.GetFileSize(*handle);
	std::string data;
	data.resize(size);
	if (size > 0) {
		fs.Read(*handle, const_cast<char *>(data.data()), size);
	}

	auto result = make_uniq<LatexReaderBindData>();
	BuildRows(data, result->rows);
	return std::move(result);
}

unique_ptr<FunctionData> LatexStringBind(ClientContext &, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, vector<string> &names) {
	LatexColumns(return_types, names);
	auto result = make_uniq<LatexReaderBindData>();
	BuildRows(input.inputs[0].GetValue<string>(), result->rows);
	return std::move(result);
}

void LatexReaderScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<LatexReaderBindData>();
	auto &state = input.global_state->Cast<LatexReaderGlobalState>();

	idx_t count = 0;
	while (state.offset < data.rows.size() && count < STANDARD_VECTOR_SIZE) {
		const auto &row = data.rows[state.offset];

		output.SetValue(0, count, Value(row.kind));
		output.SetValue(1, count, Value(row.element_type));
		// Empty content is NULL, per the duck_block convention for containers whose text
		// lives in structured inline children.
		output.SetValue(2, count, row.content.empty() ? Value(LogicalType::VARCHAR) : Value(row.content));
		output.SetValue(3, count, row.has_level ? Value::INTEGER(row.level) : Value(LogicalType::INTEGER));
		output.SetValue(4, count, Value(row.encoding));
		output.SetValue(5, count, DuckBlockTypes::CreateAttributesMap(row.attributes));
		output.SetValue(6, count, Value::INTEGER(row.element_order));

		state.offset++;
		count++;
	}
	output.SetCardinality(count);
}

} // namespace

void RegisterLatexReaderFunction(ExtensionLoader &loader) {
	TableFunction file_fn("read_latex_blocks", {LogicalType::VARCHAR}, LatexReaderScan, LatexFileBind,
	                      LatexReaderGlobalState::Init);
	loader.RegisterFunction(file_fn);

	// NOT TEST SCAFFOLDING. Asserting a two-line snippet is how the nesting rules stay
	// readable, and every other panduck reader needs a fixture file on disk for the same
	// job -- which is why none of them can state a rule in one line of SQL.
	TableFunction string_fn("read_latex_blocks_string", {LogicalType::VARCHAR}, LatexReaderScan, LatexStringBind,
	                        LatexReaderGlobalState::Init);
	loader.RegisterFunction(string_fn);
}

} // namespace duckdb
