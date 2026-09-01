#include "block_json.hpp"
#include "epub_reader.hpp"

#include "duck_block_types.hpp"
#include "zip_container.hpp"

#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <pugixml.hpp>
#include <set>

namespace duckdb {
namespace epub {

namespace {

// --------------------------------------------------------------------------- paths
//
// Every href in an EPUB is relative to the file that names it: manifest hrefs to the OPF's
// directory, stylesheet links to the content document's. Getting this wrong reads the
// wrong member or none, so it is one shared helper rather than two ad-hoc ones.

std::string PercentDecode(const std::string &in) {
	std::string out;
	out.reserve(in.size());
	for (size_t i = 0; i < in.size(); i++) {
		if (in[i] == '%' && i + 2 < in.size() && isxdigit((unsigned char)in[i + 1]) &&
		    isxdigit((unsigned char)in[i + 2])) {
			out.push_back((char)std::stoi(in.substr(i + 1, 2), nullptr, 16));
			i += 2;
		} else {
			out.push_back(in[i]);
		}
	}
	return out;
}

std::string DirOf(const std::string &path) {
	auto slash = path.find_last_of('/');
	return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

//! Join `href` onto `base_dir` and collapse . and .. segments. A fragment (#anchor) is a
//! position within a document, not part of its name, so it is dropped.
std::string ResolvePath(const std::string &base_dir, const std::string &href) {
	std::string rel = PercentDecode(href.substr(0, href.find('#')));
	if (rel.empty()) {
		return std::string();
	}
	std::string joined = rel[0] == '/' ? rel.substr(1) : base_dir + rel;

	std::vector<std::string> parts;
	size_t start = 0;
	while (start <= joined.size()) {
		auto end = joined.find('/', start);
		auto segment = joined.substr(start, end == std::string::npos ? std::string::npos : end - start);
		if (segment == "..") {
			if (!parts.empty()) {
				parts.pop_back();
			}
		} else if (!segment.empty() && segment != ".") {
			parts.push_back(segment);
		}
		if (end == std::string::npos) {
			break;
		}
		start = end + 1;
	}
	std::string out;
	for (size_t i = 0; i < parts.size(); i++) {
		out += (i ? "/" : "") + parts[i];
	}
	return out;
}

// ----------------------------------------------------------------------------- CSS
//
// WHERE THE LINE IS DRAWN, and it is the interesting decision in this reader.
//
// LibreOffice's EPUB export emits NO semantic markup whatsoever. There is no <h1>, no
// <ul>, no <strong>: every paragraph is <p class="paraN"> and every run is
// <span class="spanN">, with the meaning banished to a CSS file. pandoc reading such a
// file finds no headings and no emphasis at all -- it yields bare Spans.
//
// So a CSS declaration is read WHEN IT NAMES THE FORMATTING and not when the formatting
// would have to be INFERRED from it:
//
//     font-weight: bold          READ.   "bold" is the answer, not evidence for it.
//     font-size: 16pt            IGNORED. 16pt is evidence that this might be a heading,
//                                and one paragraph's emphasis in a document with no
//                                headings looks exactly the same.
//
// That is the same distinction ODT's <office:automatic-styles> already forced -- a run
// names a style and the style carries fo:font-weight -- so this is the CSS spelling of
// machinery panduck already had, not a new liberty. It is also why LibreOffice EPUBs come
// out with formatting but without headings: the heading information is genuinely absent
// from the document, present only as a font size.

struct RunFormat {
	bool bold = false, italic = false, underline = false, strike = false, code = false;

	bool Plain() const {
		return !bold && !italic && !underline && !strike && !code;
	}
	//! duck_block's inline vocabulary is flat, so a run carrying several attributes is
	//! reported by its strongest. Same documented limitation as the other readers.
	std::string ElementType() const {
		if (code) {
			return DuckBlockTypes::INLINE_CODE;
		}
		if (bold) {
			return DuckBlockTypes::INLINE_BOLD;
		}
		if (italic) {
			return DuckBlockTypes::INLINE_ITALIC;
		}
		if (underline) {
			return DuckBlockTypes::INLINE_UNDERLINE;
		}
		if (strike) {
			return DuckBlockTypes::INLINE_STRIKETHROUGH;
		}
		return DuckBlockTypes::INLINE_TEXT;
	}
};

std::string Trim(const std::string &s) {
	auto b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos) {
		return std::string();
	}
	auto e = s.find_last_not_of(" \t\r\n");
	return s.substr(b, e - b + 1);
}

std::string Lower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
	return s;
}

//! Apply a `prop: value; prop: value` declaration list to `fmt`. Shared by CSS rule
//! bodies and by the style="" attribute, which is the same grammar.
void ApplyDeclarations(const std::string &decls, RunFormat &fmt) {
	size_t start = 0;
	while (start < decls.size()) {
		auto end = decls.find(';', start);
		auto decl = decls.substr(start, end == std::string::npos ? std::string::npos : end - start);
		auto colon = decl.find(':');
		if (colon != std::string::npos) {
			auto prop = Lower(Trim(decl.substr(0, colon)));
			auto value = Lower(Trim(decl.substr(colon + 1)));
			if (prop == "font-weight") {
				// Numeric weights are CSS's other spelling of the same fact; 600 is where
				// the spec puts the boundary between normal and bold.
				fmt.bold = value == "bold" || value == "bolder" ||
				           (!value.empty() && isdigit((unsigned char)value[0]) && atoi(value.c_str()) >= 600);
			} else if (prop == "font-style") {
				fmt.italic = value == "italic" || value == "oblique";
			} else if (prop == "text-decoration" || prop == "text-decoration-line") {
				fmt.strike = value.find("line-through") != std::string::npos;
				fmt.underline = value.find("underline") != std::string::npos;
			}
		}
		if (end == std::string::npos) {
			break;
		}
		start = end + 1;
	}
}

//! class name -> formatting, gathered from every stylesheet a content document links.
using CssRules = std::map<std::string, RunFormat>;

//! A deliberately small CSS reader: rule bodies keyed by a single class selector, which is
//! all any writer uses to express run formatting. Anything more selective (descendant
//! combinators, @media, specificity) would need a cascade to be correct, and being
//! half-right about a cascade is worse than ignoring the rule.
void ParseCss(const std::string &css, CssRules &rules) {
	// Comments can contain braces, so they go before any brace matching.
	std::string src;
	src.reserve(css.size());
	for (size_t i = 0; i < css.size();) {
		if (css[i] == '/' && i + 1 < css.size() && css[i + 1] == '*') {
			auto close = css.find("*/", i + 2);
			i = close == std::string::npos ? css.size() : close + 2;
		} else {
			src.push_back(css[i++]);
		}
	}

	size_t pos = 0;
	while (pos < src.size()) {
		auto open = src.find('{', pos);
		if (open == std::string::npos) {
			break;
		}
		auto close = src.find('}', open);
		if (close == std::string::npos) {
			break;
		}
		std::string selectors = src.substr(pos, open - pos);
		std::string body = src.substr(open + 1, close - open - 1);
		pos = close + 1;
		if (!selectors.empty() && selectors.find('@') != std::string::npos) {
			continue; // at-rules nest their own blocks; not read
		}

		size_t sel_start = 0;
		while (sel_start < selectors.size()) {
			auto comma = selectors.find(',', sel_start);
			auto sel =
			    Trim(selectors.substr(sel_start, comma == std::string::npos ? std::string::npos : comma - sel_start));
			// `.name` or `tag.name` only -- one class, no combinators.
			auto dot = sel.find('.');
			bool simple = dot != std::string::npos && sel.find_first_of(" >+~:[") == std::string::npos &&
			              sel.find('.', dot + 1) == std::string::npos && dot + 1 < sel.size();
			if (simple) {
				auto name = sel.substr(dot + 1);
				RunFormat fmt = rules.count(name) ? rules[name] : RunFormat();
				ApplyDeclarations(body, fmt);
				rules[name] = fmt;
			}
			if (comma == std::string::npos) {
				break;
			}
			sel_start = comma + 1;
		}
	}
}

// -------------------------------------------------------------------------- XHTML

//! Tags whose text belongs to the block itself, not to a nested block.
const std::set<std::string> BLOCK_LEAF = {"p",  "h1", "h2", "h3",  "h4",         "h5",     "h6",
                                          "li", "dt", "dd", "pre", "figcaption", "caption"};

//! Tags that contribute structure and are walked through without emitting a block of
//! their own. ul/ol/dl are here because pandoc's own model has no list-wrapper block --
//! its BulletList is the wrapper, and the items are what survive canonicalisation.
const std::set<std::string> TRANSPARENT = {"body", "figure", "hgroup", "ul", "ol"};

//! HTML5's sectioning set, which is EXACTLY duck_block's `role` vocabulary -- the spec
//! adopted it deliberately rather than by coincidence. Each becomes a `section` naming its
//! own role, following heading+heading_level rather than minting a type per variant.
//!
//! These were transparent, emitting no block at all, which is worse than the <section>-as
//! -div bug they sat beside: a wrong-but-visible element can be corrected downstream or
//! caught by a lint, while an element that emits nothing is indistinguishable from a
//! document that never had one. Mislabelled information is recoverable; absent is not.
const std::set<std::string> SECTIONING = {"section", "article", "aside", "nav",
                                          "header",  "footer",  "main"};

//! Not read. `table` is skipped in every panduck reader alike -- modelling tables needs
//! table/row/cell blocks, and doing it in one reader only would make the vocabulary mean
//! different things per format.
// NOTE WHAT IS *NOT* HERE. `table` was, alongside script and style, which meant every
// cell's TEXT was discarded rather than its structure. duck_block still has no structural
// table. When this was written duck_block had no table_row or table_cell and `table` itself
// carried an opaque Pandoc tuple, so panduck could not represent the grid either way -- but
// skipping was shipping a BUG (the words are gone) to avoid a GAP (the shape is
// unrepresented) that it had regardless. Falling through to the transparent branch yielded
// the cells as paragraphs: honestly wrong about shape, honestly right about content.
//
// That is history now. Spec 5.0 gave `table` the native {headers, rows} schema and the
// table branch in WalkBlocks emits it, so <table> is neither skipped nor transparent. Kept
// as a record of why it must never go back into this set: whatever is unrepresentable about
// a construct, discarding its text is the one response that is always wrong.
const std::set<std::string> SKIPPED = {"head", "script", "style", "template", "svg", "math"};

bool IsBlockTag(const std::string &tag) {
	return BLOCK_LEAF.count(tag) || TRANSPARENT.count(tag) || SECTIONING.count(tag) || SKIPPED.count(tag) || tag == "div" ||
	       tag == "blockquote" || tag == "hr";
}

struct Run {
	RunFormat fmt;
	std::string text;
	std::string href, src;
};

//! Resolve the formatting a start tag contributes: the element's own semantics, then its
//! class, then its style attribute -- least to most specific, which is the order CSS
//! itself resolves them in.
RunFormat FormatFor(const pugi::xml_node &node, const std::string &tag, const RunFormat &inherited,
                    const CssRules &rules) {
	RunFormat fmt = inherited;
	if (tag == "strong" || tag == "b") {
		fmt.bold = true;
	} else if (tag == "em" || tag == "i" || tag == "cite" || tag == "var") {
		fmt.italic = true;
	} else if (tag == "u" || tag == "ins") {
		fmt.underline = true;
	} else if (tag == "del" || tag == "s" || tag == "strike") {
		fmt.strike = true;
	} else if (tag == "code" || tag == "kbd" || tag == "samp" || tag == "tt") {
		fmt.code = true;
	}

	// class="a b" applies each rule in turn.
	std::string classes = node.attribute("class").value();
	size_t start = 0;
	while (start < classes.size()) {
		auto end = classes.find_first_of(" \t", start);
		auto name = classes.substr(start, end == std::string::npos ? std::string::npos : end - start);
		auto it = rules.find(name);
		if (it != rules.end()) {
			// A class rule states the formatting outright, so it REPLACES rather than
			// ORs: LibreOffice's .span1 {} means plain, and OR-ing would make every run
			// after a bold one bold.
			RunFormat from_class = it->second;
			fmt.bold = from_class.bold || fmt.bold;
			fmt.italic = from_class.italic || fmt.italic;
			fmt.underline = from_class.underline || fmt.underline;
			fmt.strike = from_class.strike || fmt.strike;
		}
		if (end == std::string::npos) {
			break;
		}
		start = end + 1;
	}

	std::string style = node.attribute("style").value();
	if (!style.empty()) {
		ApplyDeclarations(style, fmt);
	}
	return fmt;
}

void AppendRun(std::vector<Run> &runs, const RunFormat &fmt, const std::string &text, const std::string &href,
               const std::string &src) {
	if (!runs.empty() && href.empty() && src.empty() && runs.back().href.empty() && runs.back().src.empty() &&
	    runs.back().fmt.ElementType() == fmt.ElementType()) {
		runs.back().text += text;
		return;
	}
	runs.push_back(Run {fmt, text, href, src});
}

void CollectRuns(const pugi::xml_node &node, const RunFormat &inherited, const std::string &href, const CssRules &rules,
                 const std::string &base_dir, std::vector<Run> &runs) {
	for (auto child : node.children()) {
		if (child.type() == pugi::node_pcdata || child.type() == pugi::node_cdata) {
			std::string text = child.value();
			if (!text.empty()) {
				AppendRun(runs, inherited, text, href, std::string());
			}
			continue;
		}
		if (child.type() != pugi::node_element) {
			continue;
		}
		std::string tag = Lower(child.name());
		if (SKIPPED.count(tag)) {
			continue;
		}
		if (tag == "br") {
			AppendRun(runs, inherited, " ", href, std::string());
			continue;
		}
		if (tag == "img") {
			// Alt text is the image's text content. src is resolved against the content
			// document so it names a MEMBER OF THE ARCHIVE -- a consumer extracting the
			// image needs that, and "../images/cover.png" is meaningless without knowing
			// which chapter said it. href below is deliberately NOT resolved: a link may
			// point outside the book and carries a #fragment that resolution would drop,
			// so it is reported exactly as authored.
			std::string src = child.attribute("src").value();
			if (src.find("://") == std::string::npos && src.compare(0, 5, "data:") != 0) {
				src = ResolvePath(base_dir, src);
			}
			runs.push_back(Run {inherited, child.attribute("alt").value(), std::string(), src});
			continue;
		}
		std::string child_href = href;
		if (tag == "a" && child.attribute("href")) {
			child_href = child.attribute("href").value();
		}
		CollectRuns(child, FormatFor(child, tag, inherited, rules), child_href, rules, base_dir, runs);
	}
}

//! True when this element carries nested blocks, which decides whether its own text is
//! read here or by the recursion. <li>text</li> and <li><p>text</p></li> are both legal
//! and mean the same thing.
bool HasBlockChildren(const pugi::xml_node &node) {
	for (auto child : node.children()) {
		if (child.type() == pugi::node_element && IsBlockTag(Lower(child.name()))) {
			return true;
		}
	}
	return false;
}

struct DocContext {
	CssRules rules;
	std::string base_dir;
};

//! A cell's text, through the same run collection every other block uses -- so entities,
//! nested formatting and whitespace behave identically to a paragraph's. The native table
//! projection is TEXT ONLY by design: it is the renderable form, and a cell's inline tree
//! has nowhere to live in `{"headers": [...], "rows": [[...]]}`.
std::string CellText(const pugi::xml_node &cell, const DocContext &ctx) {
	std::vector<Run> runs;
	CollectRuns(cell, RunFormat(), std::string(), ctx.rules, ctx.base_dir, runs);
	std::string all;
	for (auto &r : runs) {
		all += r.text;
	}
	return Trim(all);
}

//! True when every cell in the row is a <th>. Used only when the table has no <thead>:
//! a leading all-<th> row is the header row, and a row with a mix is not.
bool IsHeaderRow(const pugi::xml_node &row) {
	bool any = false;
	for (auto cell : row.children()) {
		if (cell.type() != pugi::node_element) {
			continue;
		}
		std::string tag = cell.name();
		if (tag != "th" && tag != "td") {
			continue;
		}
		any = true;
		if (tag != "th") {
			return false;
		}
	}
	return any;
}

//! Collect every <tr> under a table, at any depth -- thead/tbody/tfoot are optional in
//! HTML and real EPUBs use all the spellings.
void CollectRows(const pugi::xml_node &node, std::vector<pugi::xml_node> &rows) {
	for (auto child : node.children()) {
		if (child.type() != pugi::node_element) {
			continue;
		}
		std::string tag = child.name();
		if (tag == "tr") {
			rows.push_back(child);
		} else if (tag == "thead" || tag == "tbody" || tag == "tfoot") {
			CollectRows(child, rows);
		}
	}
}

void EmitBlock(const pugi::xml_node &node, const std::string &element_type, int heading_level, const DocContext &ctx,
               std::vector<EpubBlock> &out, int depth, const std::string &role = std::string()) {
	std::vector<Run> runs;
	CollectRuns(node, RunFormat(), std::string(), ctx.rules, ctx.base_dir, runs);

	std::string all;
	bool any_format = false;
	for (auto &r : runs) {
		all += r.text;
		if (!r.fmt.Plain() || !r.href.empty() || !r.src.empty()) {
			any_format = true;
		}
	}
	if (Trim(all).empty() && !any_format) {
		return; // whitespace-only
	}

	EpubBlock block;
	block.element_type = element_type;
	block.heading_level = heading_level;
	block.level = depth;
	block.role = role;
	// Headings always flatten -- see the RTF, DOCX and ODT readers for why.
	if (!any_format || heading_level > 0) {
		block.content = Trim(all);
	} else {
		for (auto &r : runs) {
			EpubInline inl;
			inl.content = r.text;
			inl.href = r.href;
			inl.src = r.src;
			inl.element_type = !r.src.empty()    ? DuckBlockTypes::INLINE_IMAGE
			                   : !r.href.empty() ? DuckBlockTypes::INLINE_LINK
			                                     : r.fmt.ElementType();
			block.inlines.push_back(std::move(inl));
		}
	}
	out.push_back(std::move(block));
}

void WalkBlocks(const pugi::xml_node &node, const DocContext &ctx, std::vector<EpubBlock> &out, int depth) {
	for (auto child : node.children()) {
		if (child.type() == pugi::node_pcdata || child.type() == pugi::node_cdata) {
			// Bare text directly inside a container is rare but legal, and dropping it
			// would be the ODT text:list mistake again: losing structure is a gap, losing
			// text is a bug.
			//
			// It becomes a `plain` -- a block-level run with NO paragraph semantics --
			// because that is exactly what it is: the author wrote a naked run and no <p>.
			// This is what discriminates a TIGHT list item from a loose one, and the
			// discrimination is a property of the RUN rather than of its container: an
			// item can hold block children and still be tight, which pandoc confirms by
			// emitting Plain rather than Para for `- outer` with an indented sublist. A
			// reader that decided tightness from "does the item have block children"
			// collapses the two spellings into one and loses the distinction silently.
			std::string text = Trim(child.value());
			if (!text.empty()) {
				EpubBlock block;
				block.element_type = DuckBlockTypes::TYPE_PLAIN;
				block.level = depth;
				block.content = text;
				out.push_back(std::move(block));
			}
			continue;
		}
		if (child.type() != pugi::node_element) {
			continue;
		}
		std::string tag = Lower(child.name());
		if (SKIPPED.count(tag)) {
			continue;
		}

		if (tag.size() == 2 && tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6') {
			EmitBlock(child, DuckBlockTypes::TYPE_HEADING, tag[1] - '0', ctx, out, depth);
		} else if (tag == "hr") {
			EpubBlock block;
			block.element_type = DuckBlockTypes::TYPE_HR;
				block.level = depth;
			block.container = true;
			out.push_back(std::move(block));
		} else if (tag == "pre") {
			EpubBlock block;
			block.element_type = DuckBlockTypes::TYPE_CODE;
				block.level = depth;
			// A <pre> means its whitespace is the content, so it is taken raw rather than
			// through the run collector, which folds runs of spaces.
			std::string text = child.text().get();
			if (text.empty()) {
				text = Trim(child.child_value());
			}
			block.content = text;
			out.push_back(std::move(block));
		} else if (tag == "ul" || tag == "ol" || tag == "dl") {
			// A LIST IS A CONTAINER. Previously these fell through to the
			// unknown-element branch, which recursed and dropped the grouping: the
			// <li>s came out as bare list_items with nothing saying they belonged
			// together, or whether they were bulleted or numbered.
			//
			// <dl> IS HERE NOW, and the route it took matters. It was briefly emitted as
			// list_type='bullet' -- a straight falsehood about the document -- then moved
			// to the transparent path deliberately, because duck_block had no settled
			// shape for definition lists and guessing a parent type is how a format grows
			// a shape nobody can consume.
			//
			// Spec 5.0 settled it: a definition list IS a list kind. `deflist` is
			// deprecated and duck_blocks_lint warns on it. So the deferral is DISCHARGED
			// rather than abandoned -- the condition it was waiting on ("until there is a
			// real answer to conform to") was met upstream, and this comment is the only
			// thing that would have said so.
			EpubBlock block;
			block.element_type = DuckBlockTypes::TYPE_LIST;
			block.container = true;
			block.level = depth;
			block.list_type = (tag == "ol")   ? DuckBlockTypes::LIST_TYPE_ORDERED
			                  : (tag == "dl") ? DuckBlockTypes::LIST_TYPE_DEFINITION
			                                  : DuckBlockTypes::LIST_TYPE_BULLET;
			if (tag == "ol") {
				std::string start = child.attribute("start").value();
				block.list_start = start.empty() ? "1" : start;
				block.number_style = "Decimal";
				block.number_delim = "Period";
			}
			out.push_back(std::move(block));
			WalkBlocks(child, ctx, out, depth + 1);
		} else if (tag == "blockquote" || tag == "div" || SECTIONING.count(tag)) {
			bool structural = tag != "blockquote";
			EpubBlock block;
			// <section> IS SEMANTIC AND <div> IS NOT, and duck_block distinguishes them:
			// HTML's own spec calls div "an element of last resort", while `section` says
			// the author marked structure. Mapping <section> onto div asserts the document
			// had no semantic sectioning when it did -- a placeholder standing in for a
			// real type, which is the same defect as <dl> wearing the <ul> branch.
			// Which kind of section lives in attributes['role'], following the
			// heading+heading_level convention rather than minting a type per variant.
			block.element_type = tag == "blockquote"     ? DuckBlockTypes::TYPE_BLOCKQUOTE
			                     : SECTIONING.count(tag) ? DuckBlockTypes::TYPE_SECTION
			                                             : DuckBlockTypes::TYPE_DIV;
			if (SECTIONING.count(tag)) {
				block.role = tag;
			}
			block.container = true;
			block.level = depth;
			auto before = out.size();
			out.push_back(std::move(block));
			WalkBlocks(child, ctx, out, depth + 1);
			if (structural && out.size() == before + 1) {
				// A div or section that turned out to contain nothing is layout
				// scaffolding, not structure -- pandoc's own EPUB writer emits an empty
				// <section class="titlepage"> in every book. An empty blockquote is
				// kept, because writing one is an authorial act rather than a wrapper.
				out.pop_back();
			}
		} else if (!std::string(child.attribute("epub:type").value()).empty() &&
		           std::string(child.attribute("epub:type").value()).find("pagebreak") != std::string::npos) {
			// EPUB 3 PRINT-EQUIVALENT PAGINATION. `<span epub:type="pagebreak" title="42"/>`
			// is how a reflowable book records where the PRINT edition's page 42 began, and
			// it is what citation and library workflows depend on -- "page 42 of the print
			// edition" is unanswerable without it.
			//
			// The allowlist previously said no panduck source exposes page boundaries
			// because "epub paginates by spine document". Half right, and the wrong half was
			// doing the work: spine items are DOCUMENT boundaries -- chapters -- and
			// correctly are not pages. This construct is a page and was being discarded,
			// because the reader never read epub:type at all. No fixture contained one,
			// which is why nothing noticed.
			//
			// The number comes from `title`, which is where EPUB 3 puts the label; `id` is
			// an anchor target and often carries a prefix like "pg42", so it is a fallback
			// rather than a peer.
			std::string label = child.attribute("title").value();
			if (label.empty()) {
				label = child.attribute("id").value();
			}
			EpubBlock block;
			block.element_type = DuckBlockTypes::TYPE_PAGE;
			block.level = depth;
			block.page_number = label;
			out.push_back(std::move(block));
		} else if (tag == "table") {
			// SPEC 5.0: `table` carries the NATIVE schema {"headers": [...], "rows": [[...]]}
			// in `content`, encoding='json'. It is the only element_type whose content is
			// JSON.
			//
			// This too was a HELD deferral, and its comment stated the condition: the cells
			// came through as bare `plain` runs -- "honestly wrong about shape, honestly
			// right about content" -- with "when a structural table lands upstream this
			// becomes a real mapping". It landed. Before that it was worse still: <table>
			// sat in the SKIPPED set beside <script>, discarding every cell's text, which
			// contradicted this reader's own rule that losing structure is a gap and losing
			// text is a bug.
			//
			// THE PROJECTION IS LOSSY AND panduck HAS NOWHERE TO PUT WHAT IT DROPS. colspan,
			// rowspan, alignment and multiple bodies are flattened away. duck_block's answer
			// is attributes['pandoc_ast'], which preserves the verbatim Pandoc tuple -- but
			// panduck never HAS a tuple. It reads XHTML, not Pandoc JSON, so there is
			// nothing authentic to preserve and synthesising one would put a fabricated
			// artifact in the slot reserved for the faithful one. So the attribute is
			// omitted and the loss is real. Documented rather than hidden.
			std::vector<pugi::xml_node> rows;
			CollectRows(child, rows);

			std::vector<std::string> headers;
			size_t first_body = 0;
			auto thead = child.child("thead");
			if (thead) {
				std::vector<pugi::xml_node> head_rows;
				CollectRows(thead, head_rows);
				if (!head_rows.empty()) {
					for (auto cell : head_rows[0].children()) {
						std::string ct = cell.name();
						if (ct == "th" || ct == "td") {
							headers.push_back(CellText(cell, ctx));
						}
					}
					first_body = head_rows.size();
				}
			} else if (!rows.empty() && IsHeaderRow(rows[0])) {
				// No <thead>, but a leading all-<th> row means the same thing.
				for (auto cell : rows[0].children()) {
					std::string ct = cell.name();
					if (ct == "th" || ct == "td") {
						headers.push_back(CellText(cell, ctx));
					}
				}
				first_body = 1;
			}

			std::vector<std::vector<std::string>> body;
			for (size_t r = first_body; r < rows.size(); r++) {
				std::vector<std::string> cells;
				for (auto cell : rows[r].children()) {
					std::string ct = cell.name();
					if (ct == "th" || ct == "td") {
						cells.push_back(CellText(cell, ctx));
					}
				}
				if (cells.empty()) {
					continue; // a <tr> with no cells contributes no row rather than an empty one
				}
				body.push_back(std::move(cells));
			}
			bool any_row = !body.empty();
			std::string json = BuildTableJson(headers, body);

			if (!headers.empty() || any_row) {
				// An empty table emits nothing, matching the empty-div rule above: a table
				// with no cells is scaffolding, and a row of `{"headers":[],"rows":[]}` says
				// a table was there while carrying nothing a consumer can use.
				EpubBlock block;
				block.element_type = DuckBlockTypes::TYPE_TABLE;
				block.content = std::move(json);
				block.encoding = DuckBlockTypes::ENCODING_JSON;
				block.level = depth;
				out.push_back(std::move(block));

				// <caption> is a real block and has no home in the projection, so it stays a
				// child of the table rather than being swallowed with the cells.
				auto caption = child.child("caption");
				if (caption) {
					EmitBlock(caption, DuckBlockTypes::TYPE_CAPTION, 0, ctx, out, depth + 1);
				}
			}
		} else if (BLOCK_LEAF.count(tag)) {
			auto type = (tag == "li" || tag == "dt" || tag == "dd") ? DuckBlockTypes::TYPE_LIST_ITEM
			            : tag == "figcaption" || tag == "caption"   ? DuckBlockTypes::TYPE_CAPTION
			                                                        : DuckBlockTypes::TYPE_PARAGRAPH;
			// spec 5.0: role distinguishes the two halves of a definition list, the same
			// discriminator `section` uses for its seven sectioning kinds.
			std::string item_role = (tag == "dt")   ? DuckBlockTypes::ROLE_TERM
			                        : (tag == "dd") ? DuckBlockTypes::ROLE_DEFINITION
			                                        : std::string();
			if (HasBlockChildren(child)) {
				// <li><p>..</p></li>: the LOOSE form. The item is a container and its text
				// is read by the blocks inside it -- a real `paragraph`, because the source
				// wrote one.
				EpubBlock block;
				block.element_type = type;
				block.container = true;
				block.level = depth;
				block.role = item_role;
				out.push_back(std::move(block));
				WalkBlocks(child, ctx, out, depth + 1);
			} else {
				// <li>text</li>: the TIGHT form, and its run belongs on the ITEM.
				//
				// duck_block 6.0: `content` is populated IF AND ONLY IF the container's
				// only child is a plain text run, and `plain` is for a run with NOWHERE
				// ELSE to live -- beside a block sibling, or at the document root. A lone
				// run inside a list item has somewhere, so it is content and no child is
				// emitted at all. EmitBlock already implements exactly that rule.
				//
				// This briefly emitted `list_item(NULL) > plain(text)` instead. The
				// tight/loose distinction it was reaching for is real -- the two forms
				// render with different spacing, and HTML, Pandoc and duck_block all carry
				// it -- but 6.0 carries it through the CONTENT RULE rather than the type:
				// tight puts the text on the item, loose grows a paragraph. Emitting
				// `plain` there said "this run had nowhere to go" about a run that did.
				EmitBlock(child, type, 0, ctx, out, depth, item_role);
			}
		} else {
			// Unknown or transparent element: recurse. Never drop, for the same reason as
			// the bare-text case above.
			WalkBlocks(child, ctx, out, depth);
		}
	}
}

pugi::xml_document ParseXml(const std::string &xml, const std::string &path, const std::string &member) {
	pugi::xml_document doc;
	// parse_ws_pcdata for the same reason DOCX and ODT need it: a whitespace-only text
	// node between two spans is real inter-word spacing, and the default flags discard it.
	auto parsed = doc.load_buffer(xml.data(), xml.size(), pugi::parse_default | pugi::parse_ws_pcdata);
	if (!parsed) {
		throw InvalidInputException("read_epub_blocks: %s is not well-formed XML in %s: %s", member, path,
		                            parsed.description());
	}
	return doc;
}

} // namespace

//! The OPF's Dublin Core, as duck_block `value` elements.
//!
//! Every EPUB carried this and panduck dropped all of it -- a DISCARD, not a fidelity
//! gap, and the only unrecoverable one this reader had. The OPF was already parsed for
//! the manifest and spine; the metadata sat in the same tree, unread.
//!
//! KEYS ARE PANDOC'S, NOT DUBLIN CORE'S. dc:creator -> author, dc:title -> title.
//! Measured against pandoc 3.1.3 rather than assumed: preserving the `dc:` prefixes is
//! the obvious reading and nothing in the vocabulary would have objected to it, so a
//! consumer would have got `dc:title` from an EPUB and `title` from everything else.
//!
//! SHAPE IS MetaInlines, which is what pandoc emits for every one of these -- so
//! `value`/`inlines` with the text as an inline CHILD, not `value`/`string` with the text
//! in content. A title read from a panduck EPUB and one read through
//! pandoc_ast_to_blocks have to be the same shape or the vocabulary buys nothing.
//!
//! ORDER IS ALPHABETICAL BY KEY. Pandoc's Meta is a map and serialises sorted, so this
//! matches what the converter produces for the same document. OPF document order would
//! be more faithful to the file and less comparable to the reference; the reference wins,
//! because disagreeing with it is the expensive kind of difference.
void CollectMetadata(const pugi::xml_node &package, std::vector<EpubBlock> &out) {
	// Only the fields pandoc actually extracts. dcterms:modified and the rest of the OPF's
	// <meta> soup are deliberately absent: emitting fields pandoc does not would put
	// panduck ahead of the reference in a direction nobody has asked for, and the gap is
	// recorded rather than silently filled.
	static const std::pair<const char *, const char *> DC_TO_PANDOC[] = {
	    {"dc:creator", "author"},   {"dc:date", "date"},   {"dc:identifier", "identifier"},
	    {"dc:language", "language"}, {"dc:title", "title"},
	};
	auto meta = package.child("metadata");
	if (!meta) {
		return;
	}
	std::map<std::string, std::string> found; // sorted by pandoc key, which is the emission order
	for (auto &pair : DC_TO_PANDOC) {
		auto node = meta.child(pair.first);
		if (!node) {
			continue;
		}
		// AN EMPTY FIELD IS STILL A FIELD. <dc:title/> is emitted, with no inline child.
		//
		// I had this the other way -- skipping empties on the reasoning that a blank value
		// asserts nothing. duck_block_utils measured pandoc and ruled against it: pandoc
		// emits the KEY with an empty value in every format that has one. Present-and-empty
		// is not absent, and a consumer cannot recover "the document declared a title and
		// left it blank" from silence. Dropping it discards a fact the reference preserved.
		found[pair.second] = Trim(node.child_value());
	}
	for (auto &kv : found) {
		EpubBlock block;
		block.kind = DuckBlockTypes::KIND_VALUE;
		block.element_type = DuckBlockTypes::VALUE_INLINES;
		block.key = kv.first;
		block.level = 1;
		if (!kv.second.empty()) {
			// No child for an empty field: `value`/`inlines` with no inline children IS the
			// empty spelling, matching pandoc's MetaInlines []. A child carrying "" would
			// be a run of no text, which is a different and meaningless claim.
			EpubInline text;
			text.element_type = DuckBlockTypes::INLINE_TEXT;
			text.content = kv.second;
			block.inlines.push_back(std::move(text));
		}
		out.push_back(std::move(block));
	}
}

std::vector<EpubBlock> ParseEpubFile(const std::string &path) {
	ZipContainer zip(path, "read_epub_blocks");

	// META-INF/container.xml is the ONLY path an EPUB fixes. Everything else is found by
	// following it, which is why a missing one means "a ZIP, but not an EPUB".
	auto container_xml = zip.ReadRequired("META-INF/container.xml");
	auto container = ParseXml(container_xml, path, "META-INF/container.xml");
	std::string opf_path =
	    container.child("container").child("rootfiles").child("rootfile").attribute("full-path").value();
	if (opf_path.empty()) {
		throw InvalidInputException("read_epub_blocks: %s has a container.xml with no rootfile full-path", path);
	}
	opf_path = ResolvePath(std::string(), opf_path);

	std::string opf_xml;
	if (!zip.Read(opf_path.c_str(), opf_xml)) {
		throw InvalidInputException("read_epub_blocks: %s names a package document '%s' that is not in the archive",
		                            path, opf_path);
	}
	auto opf = ParseXml(opf_xml, path, opf_path.c_str());
	auto opf_dir = DirOf(opf_path);

	auto package = opf.child("package");
	std::map<std::string, std::string> manifest; // id -> archive member
	std::map<std::string, std::string> media;    // id -> media type
	for (auto item : package.child("manifest").children("item")) {
		std::string id = item.attribute("id").value();
		if (id.empty()) {
			continue;
		}
		manifest[id] = ResolvePath(opf_dir, item.attribute("href").value());
		media[id] = item.attribute("media-type").value();
	}

	// THE SPINE IS THE READING ORDER. ZIP member order is arbitrary and the manifest is a
	// set, so this is the only statement of what order a human reads the book in.
	std::vector<std::string> documents;
	for (auto itemref : package.child("spine").children("itemref")) {
		std::string idref = itemref.attribute("idref").value();
		auto it = manifest.find(idref);
		if (it == manifest.end() || it->second.empty()) {
			continue;
		}
		auto type = media[idref];
		if (type.empty() || type.find("xhtml") != std::string::npos || type.find("html") != std::string::npos) {
			documents.push_back(it->second);
		}
	}

	std::map<std::string, CssRules> stylesheets; // member -> rules, shared across chapters
	std::vector<EpubBlock> blocks;
	for (auto &doc_path : documents) {
		std::string doc_xml;
		if (!zip.Read(doc_path.c_str(), doc_xml)) {
			continue; // a spine entry naming a missing member is the book's bug, not ours
		}
		auto content = ParseXml(doc_xml, path, doc_path.c_str());
		auto html = content.child("html");

		DocContext ctx;
		ctx.base_dir = DirOf(doc_path);
		for (auto link : html.child("head").children("link")) {
			std::string rel = Lower(link.attribute("rel").value());
			if (rel.find("stylesheet") == std::string::npos) {
				continue;
			}
			auto css_path = ResolvePath(ctx.base_dir, link.attribute("href").value());
			auto cached = stylesheets.find(css_path);
			if (cached == stylesheets.end()) {
				CssRules rules;
				std::string css;
				if (zip.Read(css_path.c_str(), css)) {
					ParseCss(css, rules);
				}
				cached = stylesheets.emplace(css_path, std::move(rules)).first;
			}
			for (auto &rule : cached->second) {
				ctx.rules[rule.first] = rule.second;
			}
		}
		// An inline <style> in the head is resolved the same way as a linked sheet.
		for (auto style : html.child("head").children("style")) {
			ParseCss(style.child_value(), ctx.rules);
		}

		WalkBlocks(html.child("body"), ctx, blocks, 1);
	}
	// AFTER the blocks, which spec 6.2 makes a contract rather than a convenience.
	CollectMetadata(package, blocks);
	return blocks;
}

} // namespace epub

namespace {

struct EpubRow {
	std::string kind, element_type, content;
	//! Defaults to `text`, which every element except `table` uses. Column 4 emitted this
	//! as a hardcoded constant until spec 5.0 gave `table` a JSON content schema -- so the
	//! one element_type that needs a different encoding was the one the emitter could not
	//! express.
	std::string encoding = DuckBlockTypes::ENCODING_TEXT;
	bool has_level = false;
	int32_t level = 0;
	std::map<std::string, std::string> attributes;
	int32_t element_order = 0;
};

struct EpubBindData : public TableFunctionData {
	std::vector<EpubRow> rows;
};

struct EpubGlobalState : public GlobalTableFunctionState {
	idx_t offset = 0;
	static unique_ptr<GlobalTableFunctionState> Init(ClientContext &, TableFunctionInitInput &) {
		return make_uniq<EpubGlobalState>();
	}
};

unique_ptr<FunctionData> EpubBind(ClientContext &, TableFunctionBindInput &input, vector<LogicalType> &return_types,
                                  vector<string> &names) {
	names = {"kind", "element_type", "content", "level", "encoding", "attributes", "element_order"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::INTEGER,
	                LogicalType::VARCHAR, LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR),
	                LogicalType::INTEGER};

	auto result = make_uniq<EpubBindData>();
	int32_t order = 0;
	for (auto &block : epub::ParseEpubFile(input.inputs[0].GetValue<string>())) {
		EpubRow row;
		row.kind = block.kind.empty() ? DuckBlockTypes::KIND_BLOCK : block.kind;
		row.element_type = block.element_type;
		row.content = block.content;
		if (!block.encoding.empty()) {
			row.encoding = block.encoding;
		}
		row.element_order = order++;
		if (block.heading_level > 0) {
			row.attributes[DuckBlockTypes::ATTR_HEADING_LEVEL] = std::to_string(block.heading_level);
		}
		if (!block.role.empty()) {
			row.attributes[DuckBlockTypes::ATTR_ROLE] = block.role;
		}
		if (!block.key.empty()) {
			row.attributes[DuckBlockTypes::ATTR_KEY] = block.key;
		}
		if (!block.page_number.empty()) {
			// The name duck_blocks_page_rows reads, so a page marker is queryable rather
			// than merely present.
			row.attributes["page_number"] = block.page_number;
		}
		if (!block.list_type.empty()) {
			// BOTH SPELLINGS, deliberately. attributes['ordered'] is the CANONICAL name --
			// duck_block spec v1.0 documented it -- and 'list_type' is an alias that
			// arrived later with duck_block_utils' Pandoc reader. That reader emitted only
			// the alias for a while, so a consumer written against the published v1 spec
			// read nothing at all from a Pandoc-produced list. Emitting both is what both
			// upstream producers now do; prefer `ordered` when writing new code against
			// this output, tolerate either when reading it.
			row.attributes[DuckBlockTypes::ATTR_ORDERED_LEGACY] =
			    block.list_type == DuckBlockTypes::LIST_TYPE_ORDERED ? "true" : "false";
			row.attributes[DuckBlockTypes::ATTR_LIST_TYPE] = block.list_type;
			if (!block.list_start.empty()) {
				row.attributes["start"] = block.list_start;
				row.attributes["number_style"] = block.number_style;
				row.attributes["number_delim"] = block.number_delim;
			}
		}
		// Every element carries a structural level; an inline is a CHILD of its block.
		const int32_t block_level = block.level > 0 ? block.level : 1;
		row.has_level = true;
		row.level = block_level;
		result->rows.push_back(std::move(row));

		for (auto &inl : block.inlines) {
			EpubRow child;
			child.kind = DuckBlockTypes::KIND_INLINE;
			child.element_type = inl.element_type;
			child.content = inl.content;
			child.has_level = true;
			child.level = block_level + 1;
			child.element_order = order++;
			if (!inl.href.empty()) {
				child.attributes["href"] = inl.href;
			}
			if (!inl.src.empty()) {
				child.attributes["src"] = inl.src;
			}
			result->rows.push_back(std::move(child));
		}
	}
	return std::move(result);
}

void EpubScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<EpubBindData>();
	auto &state = input.global_state->Cast<EpubGlobalState>();
	idx_t count = 0;
	while (state.offset < data.rows.size() && count < STANDARD_VECTOR_SIZE) {
		const auto &row = data.rows[state.offset];
		output.SetValue(0, count, Value(row.kind));
		output.SetValue(1, count, Value(row.element_type));
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

void RegisterEpubReaderFunction(ExtensionLoader &loader) {
	TableFunction fn("read_epub_blocks", {LogicalType::VARCHAR}, EpubScan, EpubBind, EpubGlobalState::Init);
	loader.RegisterFunction(fn);
}

} // namespace duckdb
