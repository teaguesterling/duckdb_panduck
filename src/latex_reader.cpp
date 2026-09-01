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
	std::vector<Token> tokens;
	std::string document_class = "article";
	std::vector<LatexBlock> blocks;
	//! Structural depth of the block currently being built. Containers (Task 6) push this;
	//! for now every block is top-level and every inline is one deeper.
	int depth = 1;
	bool stopped = false;

	// The run being accumulated. `pending` is loose text; `pending_inlines` is non-empty
	// once any formatted run has appeared, at which point the block emits children instead
	// of a flat content string.
	std::string pending;
	std::vector<LatexInline> pending_inlines;
	//! Returned for an argument a macro declared but the source did not supply. Held on the
	//! parser rather than as a function-local static so an absent argument is empty per
	//! parse rather than shared between them.
	std::vector<Token> no_arg;

	void Run(std::vector<Token> &toks);
	void ControlWord(std::vector<Token> &toks, size_t &i, const std::string &name);
	bool ReadGroup(std::vector<Token> &toks, size_t &i, std::vector<Token> &out);
	void SkipOptional(std::vector<Token> &toks, size_t &i);
	void SkipEnvironment(std::vector<Token> &toks, size_t &i);
	void AppendText(const std::string &text);
	void PushInline(LatexInline inl);
	void FlushRun();
	void EmitHeading(const std::string &name, std::vector<Token> &arg);
};

//! Flatten tokens to plain text, resolving the dispositions that can contribute
//! characters. Used for anything that is text BY CONSTRUCTION -- a heading's title, a
//! link's target, the documentclass -- where structure cannot survive anyway.
void Flatten(const std::vector<Token> &toks, std::string &out);

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

void Flatten(const std::vector<Token> &toks, std::string &out) {
	for (size_t i = 0; i < toks.size(); i++) {
		const auto &tok = toks[i];
		switch (tok.kind) {
		case TokenKind::TEXT:
			FlattenAppend(out, tok.text);
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
				Flatten(args[content_arg], out);
			}
			break;
		}
		default:
			break; // groups and math shifts carry no characters of their own
		}
	}
}

std::string FlattenTrimmed(const std::vector<Token> &toks) {
	std::string out;
	Flatten(toks, out);
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

void Parser::PushInline(LatexInline inl) {
	if (!pending.empty()) {
		LatexInline text;
		text.element_type = DuckBlockTypes::INLINE_TEXT;
		text.content = pending;
		text.level = depth + 1;
		pending.clear();
		pending_inlines.push_back(std::move(text));
	}
	inl.level = depth + 1;
	pending_inlines.push_back(std::move(inl));
}

void Parser::FlushRun() {
	if (pending_inlines.empty()) {
		RightTrim(pending);
		if (pending.empty()) {
			pending.clear();
			return;
		}
		LatexBlock block;
		block.element_type = DuckBlockTypes::TYPE_PARAGRAPH;
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
	block.element_type = DuckBlockTypes::TYPE_PARAGRAPH;
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
			out.push_back(Token {TokenKind::TEXT, std::string(1, toks[j].text[0]), false});
			toks[j].text.erase(0, 1);
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

void Parser::SkipOptional(std::vector<Token> &toks, size_t &i) {
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
		text = text.substr(close + 1);
		i = start;
		return;
	}
	// A `[` whose `]` is in a LATER token is still a real optional argument --
	// `\\includegraphics[width=\\textwidth]{img.png}` lexes as TEXT "[width=",
	// CONTROL_WORD textwidth, TEXT "]" -- so the search has to cross tokens. It must not
	// cross a construct an optional argument can never CONTAIN, though, and that boundary is
	// the whole safety of this function: an UNCLOSED `[` otherwise scans to the first `]`
	// anywhere in the rest of the document, deletes both spans and welds two paragraphs into
	// one. Run in the preamble the same runaway consumes \\begin{document}, `body_start` is
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
		text.clear();
		i = j;
		return;
	}
	// Unclosed, or closed only beyond a boundary an optional argument cannot cross: leave the
	// `[` exactly where it is. It is then ordinary text, which is what it turned out to be.
}

void Parser::SkipEnvironment(std::vector<Token> &toks, size_t &i) {
	// PLACEHOLDER FOR TASK 6, which gives environments their own dispositions. Skipping to
	// the matching \end loses the content, which is wrong -- but it is wrong in a way that
	// degrades rather than crashes, and every alternative available before the environment
	// table is consumed invents a shape.
	int nest = 1;
	while (i < toks.size() && toks[i].kind != TokenKind::END && nest > 0) {
		if (toks[i].kind == TokenKind::CONTROL_WORD) {
			if (toks[i].text == "begin") {
				nest++;
				i++;
				std::vector<Token> name;
				ReadGroup(toks, i, name);
				continue;
			}
			if (toks[i].text == "end") {
				nest--;
				i++;
				std::vector<Token> name;
				ReadGroup(toks, i, name);
				continue;
			}
		}
		i++;
	}
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
		SkipEnvironment(toks, i);
		return;
	}
	if (name == "end") {
		FlushRun();
		std::vector<Token> env;
		ReadGroup(toks, i, env);
		if (FlattenTrimmed(env) == "document") {
			stopped = true;
		}
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
	auto arg_at = [&](int index) -> std::vector<Token> & {
		if (index < 0 || index >= (int)args.size()) {
			no_arg.clear();
			return no_arg;
		}
		return args[index];
	};

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
	if (element_type == DuckBlockTypes::INLINE_IMAGE) {
		inl.src = FlattenTrimmed(arg_at(0));
	} else if (element_type == DuckBlockTypes::INLINE_LINK) {
		// \href{url}{text} and \url{url} differ only in whether the target is also the
		// label, which content_arg already encodes: argument 0 is the target either way.
		inl.href = FlattenTrimmed(arg_at(0));
		inl.content = FlattenTrimmed(arg_at(entry->content_arg));
	} else {
		inl.content = FlattenTrimmed(arg_at(entry->content_arg >= 0 ? entry->content_arg : 0));
	}
	if (inl.content.empty() && inl.src.empty() && inl.href.empty()) {
		return; // an empty formatted run is not an element, it is punctuation
	}
	PushInline(std::move(inl));
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
			FlushRun();
			i++;
			break;
		case TokenKind::BEGIN_GROUP:
		case TokenKind::END_GROUP:
			// A bare group is a SCOPE, and scopes are Task 5. Until then the braces are
			// transparent: keeping the text they wrap is the smaller error.
			i++;
			break;
		case TokenKind::MATH_SHIFT: {
			// Math is Task 7. Skip to the closing shift so the formula's guts do not leak
			// into the surrounding paragraph as prose.
			i++;
			while (i < toks.size() && toks[i].kind != TokenKind::MATH_SHIFT && toks[i].kind != TokenKind::END) {
				i++;
			}
			if (i < toks.size() && toks[i].kind == TokenKind::MATH_SHIFT) {
				i++;
			}
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
		row.kind = DuckBlockTypes::KIND_BLOCK;
		row.element_type = block.element_type;
		row.content = block.content;
		row.element_order = order++;
		if (block.heading_level > 0) {
			// SEMANTIC RANK, not structural depth. A top-level \subsection is level 1 and
			// heading_level 2; the two are different facts and conflating them renders a
			// heading by where it sits rather than what it is.
			row.attributes[DuckBlockTypes::ATTR_HEADING_LEVEL] = std::to_string(block.heading_level);
		}
		if (!block.list_type.empty()) {
			// BOTH SPELLINGS, matching every other panduck reader: `list_type` is canonical
			// under spec 4.0 and `ordered` is the legacy alias consumers written against v1
			// still read.
			row.attributes["ordered"] = block.list_type == "ordered" ? "true" : "false";
			row.attributes["list_type"] = block.list_type;
			if (!block.list_start.empty()) {
				row.attributes["start"] = block.list_start;
				row.attributes["number_style"] = block.number_style;
				row.attributes["number_delim"] = block.number_delim;
			}
		}
		if (!block.display.empty()) {
			row.attributes["display"] = block.display;
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
		output.SetValue(4, count, Value(DuckBlockTypes::ENCODING_TEXT));
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
