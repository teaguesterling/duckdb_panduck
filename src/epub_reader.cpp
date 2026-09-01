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
const std::set<std::string> TRANSPARENT = {"body", "article", "aside",  "header", "footer", "main",
                                           "nav",  "figure",  "hgroup", "ul",     "ol",     "dl"};

//! Not read. `table` is skipped in every panduck reader alike -- modelling tables needs
//! table/row/cell blocks, and doing it in one reader only would make the vocabulary mean
//! different things per format.
const std::set<std::string> SKIPPED = {"head", "script", "style", "template", "svg", "math", "table"};

bool IsBlockTag(const std::string &tag) {
	return BLOCK_LEAF.count(tag) || TRANSPARENT.count(tag) || SKIPPED.count(tag) || tag == "div" || tag == "section" ||
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

void EmitBlock(const pugi::xml_node &node, const std::string &element_type, int heading_level, const DocContext &ctx,
               std::vector<EpubBlock> &out, int depth) {
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
			// text is a bug. It becomes a paragraph of its own so document order holds.
			std::string text = Trim(child.value());
			if (!text.empty()) {
				EpubBlock block;
				block.element_type = DuckBlockTypes::TYPE_PARAGRAPH;
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
			EpubBlock block;
			block.element_type = DuckBlockTypes::TYPE_LIST;
			block.container = true;
			block.level = depth;
			block.list_type = (tag == "ol") ? "ordered" : "bullet";
			if (tag == "ol") {
				std::string start = child.attribute("start").value();
				block.list_start = start.empty() ? "1" : start;
				block.number_style = "Decimal";
				block.number_delim = "Period";
			}
			out.push_back(std::move(block));
			WalkBlocks(child, ctx, out, depth + 1);
		} else if (tag == "blockquote" || tag == "div" || tag == "section") {
			bool structural = tag != "blockquote";
			EpubBlock block;
			block.element_type = tag == "blockquote" ? DuckBlockTypes::TYPE_BLOCKQUOTE : DuckBlockTypes::TYPE_DIV;
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
		} else if (BLOCK_LEAF.count(tag)) {
			auto type = (tag == "li" || tag == "dt" || tag == "dd") ? DuckBlockTypes::TYPE_LIST_ITEM
			            : tag == "figcaption" || tag == "caption"   ? DuckBlockTypes::TYPE_CAPTION
			                                                        : DuckBlockTypes::TYPE_PARAGRAPH;
			if (HasBlockChildren(child)) {
				// <li><p>..</p></li>: the item is a container and its text is read by the
				// blocks inside it.
				EpubBlock block;
				block.element_type = type;
				block.container = true;
				block.level = depth;
				out.push_back(std::move(block));
				WalkBlocks(child, ctx, out, depth + 1);
			} else if (type == DuckBlockTypes::TYPE_LIST_ITEM) {
				// <li>text</li>: the item owns no content of its own under spec 2.0, so
				// its words become a paragraph child rather than sitting on the item.
				EpubBlock item;
				item.element_type = type;
				item.container = true;
				item.level = depth;
				out.push_back(std::move(item));
				EmitBlock(child, DuckBlockTypes::TYPE_PARAGRAPH, 0, ctx, out, depth + 1);
			} else {
				EmitBlock(child, type, 0, ctx, out, depth);
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
	return blocks;
}

} // namespace epub

namespace {

struct EpubRow {
	std::string kind, element_type, content;
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
		row.kind = DuckBlockTypes::KIND_BLOCK;
		row.element_type = block.element_type;
		row.content = block.content;
		row.element_order = order++;
		if (block.heading_level > 0) {
			row.attributes[DuckBlockTypes::ATTR_HEADING_LEVEL] = std::to_string(block.heading_level);
		}
		if (!block.list_type.empty()) {
			// BOTH SPELLINGS, deliberately. attributes['ordered'] is the CANONICAL name --
			// duck_block spec v1.0 documented it -- and 'list_type' is an alias that
			// arrived later with duck_block_utils' Pandoc reader. That reader emitted only
			// the alias for a while, so a consumer written against the published v1 spec
			// read nothing at all from a Pandoc-produced list. Emitting both is what both
			// upstream producers now do; prefer `ordered` when writing new code against
			// this output, tolerate either when reading it.
			row.attributes["ordered"] = block.list_type == "ordered" ? "true" : "false";
			row.attributes["list_type"] = block.list_type;
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
		output.SetValue(4, count, Value(DuckBlockTypes::ENCODING_TEXT));
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
