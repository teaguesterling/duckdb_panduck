#include "panduck/mediawiki.hpp"

#include "panduck/block_json.hpp"
#include "panduck/mediawiki_scanner.hpp"
#include "panduck/slugify.hpp"
#include "panduck/vocabulary.hpp"

// INCLUDES NAMED RATHER THAN INHERITED. src/mediawiki_reader.cpp named
// <algorithm>, <cctype> and <fstream>; of those the core uses only tolower, and
// <fstream> belonged to the tail. <cstddef> (size_t at 38 sites), <map>,
// <utility> (std::move) and <vector> were never named and arrived through
// duckdb.hpp and the reader header.
//
// MEASURED after the move, as for org and rst: the standalone build still
// compiles with these deleted, because libstdc++ supplies them behind <string>
// and panduck/mediawiki.hpp pulls <map> and <vector>. So unlike textile's
// <cstring> none of them was a break -- they were undeclared dependencies, named
// here so what the file uses and what its includes promise are the same set.
#include <cctype>
#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

// The parse core and the row flattening, moved out of src/mediawiki_reader.cpp
// (issue #104, L2). Behaviour unchanged: the only edits are the namespace
// (duckdb::mediawiki -> panduck::mediawiki), the DuckBlockTypes:: ->
// DuckBlockVocabulary:: rename (the same constants; DuckBlockTypes merely adds
// the LogicalType/Value helpers that cannot cross the seam), and MwRow ->
// panduck::Block.

namespace panduck {
namespace mediawiki {

namespace {

//! The pandoc RawBlock format name for held-raw wikitext.
//!
//! THIS IS AN ATTRIBUTE, NOT AN `encoding`, and the distinction was a real
//! mistake on the way here. `encoding='mediawiki'` looks obviously right and is
//! NOT CONFORMANT: duck_block validates `encoding` against a closed set --
//! text, json, yaml, html, xml, latex, markdown, toml -- and `mediawiki` is not
//! in it. make check-conformance rejects it, which is the vocabulary defending
//! a spec-owned enumeration that panduck does not get to extend from the
//! outside.
//!
//! `attributes['format']` is where it belongs, and not by invention: the
//! converter in pandoc_block_convert.cpp ALREADY reads exactly that attribute
//! to build RawBlock [<format>, ...], defaulting to "html". So the export path
//! was looking for this all along while the reader was writing it somewhere the
//! exporter never read.
//!
//! Asking duck_block_utils to add `mediawiki` to the encoding set is still the
//! right follow-up -- pandoc uses that word, and rst and org have the same gap
//! -- but it is their ruling to make, and holding the format in an attribute is
//! correct today rather than blocked on it.
constexpr const char *RAW_FORMAT_MEDIAWIKI = "mediawiki";

std::string Trim(const std::string &s) {
	size_t b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos) {
		return {};
	}
	size_t e = s.find_last_not_of(" \t\r\n");
	return s.substr(b, e - b + 1);
}

// The heading anchor moved to slugify.hpp, shared with rst and textile (#85). What stays
// HERE is the argument: this reader slugifies PlainText(...) rather than the raw line, so
// the quote marks of '''bold''' and the brackets of [[link]] contribute no characters and
// the anchor matches pandoc's. The separator -- an underscore, where textile and rst use a
// hyphen -- is this format's convention and is passed to the shared function.

//! Strip wiki markup down to its text, for slugs and for table cells. Not a
//! parser: it removes the delimiters that carry no text of their own.
std::string PlainText(const std::string &s) {
	std::string out;
	for (size_t i = 0; i < s.size();) {
		if (s.compare(i, 2, "''") == 0) {
			while (i < s.size() && s[i] == '\'') {
				i++;
			}
			continue;
		}
		if (s.compare(i, 2, "[[") == 0) {
			size_t close = s.find("]]", i);
			if (close != std::string::npos) {
				std::string inner = s.substr(i + 2, close - i - 2);
				size_t bar = inner.rfind('|');
				out += bar == std::string::npos ? inner : inner.substr(bar + 1);
				i = close + 2;
				continue;
			}
		}
		out += s[i];
		i++;
	}
	return out;
}

void PushText(std::vector<MwInline> &out, const std::string &text, int level) {
	if (text.empty()) {
		return;
	}
	if (!out.empty() && out.back().element_type == DuckBlockVocabulary::INLINE_TEXT && out.back().attributes.empty()) {
		out.back().content += text;
		return;
	}
	MwInline in;
	in.element_type = DuckBlockVocabulary::INLINE_TEXT;
	in.content = text;
	in.level = level;
	out.push_back(in);
}

//! Net `{{` nesting change across a substring, so an inline template can be
//! balanced the same way the scanner balances a block one.
int BraceDelta(const std::string &s, size_t from, size_t to) {
	int d = 0;
	for (size_t i = from; i + 1 < to; i++) {
		if (s[i] == '{' && s[i + 1] == '{') {
			d++;
			i++;
		} else if (s[i] == '}' && s[i + 1] == '}') {
			d--;
			i++;
		}
	}
	return d;
}

std::string TemplateNameOf(const std::string &raw) {
	size_t end = raw.size();
	for (size_t i = 2; i + 1 < raw.size(); i++) {
		if (raw[i] == '|' || (raw[i] == '}' && raw[i + 1] == '}')) {
			end = i;
			break;
		}
	}
	return Trim(raw.substr(2, end - 2));
}

void ParseInlines(const std::string &s, int level, std::vector<MwInline> &out);

//! Emit one delimited run as an element with its own inline children one level
//! deeper.
void PushWrapped(std::vector<MwInline> &out, const char *type, const std::string &inner, int level) {
	MwInline node;
	node.element_type = type;
	node.level = level;
	std::vector<MwInline> children;
	ParseInlines(inner, level + 1, children);
	// A run whose only content is plain text collapses into the wrapper's own
	// content, which is spec 6.0's content rule and what every other panduck
	// reader emits.
	if (children.size() == 1 && children[0].element_type == DuckBlockVocabulary::INLINE_TEXT &&
	    children[0].attributes.empty()) {
		node.content = children[0].content;
		out.push_back(node);
		return;
	}
	out.push_back(node);
	for (auto &c : children) {
		out.push_back(c);
	}
}

void ParseInlines(const std::string &s, int level, std::vector<MwInline> &out) {
	std::string pending;
	size_t i = 0;
	while (i < s.size()) {
		// AN INLINE TEMPLATE, balanced rather than matched -- {{a|{{b|x}}|y}} is
		// one call.
		if (s.compare(i, 2, "{{") == 0) {
			size_t j = i + 2;
			int depth = 1;
			while (j + 1 < s.size() && depth > 0) {
				if (s.compare(j, 2, "{{") == 0) {
					depth++;
					j += 2;
				} else if (s.compare(j, 2, "}}") == 0) {
					depth--;
					j += 2;
				} else {
					j++;
				}
			}
			if (depth == 0) {
				PushText(out, pending, level);
				pending.clear();
				std::string raw = s.substr(i, j - i);
				MwInline node;
				node.element_type = DuckBlockVocabulary::INLINE_RAW;
				node.content = raw;
				node.level = level;
				node.attributes["format"] = RAW_FORMAT_MEDIAWIKI;
				node.attributes[DuckBlockVocabulary::ATTR_SOURCE_TYPE] = "template";
				auto name = TemplateNameOf(raw);
				if (!name.empty()) {
					node.attributes["template_name"] = name;
				}
				out.push_back(node);
				i = j;
				continue;
			}
		}

		// `<nowiki>` SUPPRESSES MARKUP, it does not mark content -- measured:
		// pandoc yields a bare Str. So its body becomes text and never re-enters
		// this parser.
		if (s.compare(i, 8, "<nowiki>") == 0) {
			size_t close = s.find("</nowiki>", i);
			if (close != std::string::npos) {
				pending += s.substr(i + 8, close - i - 8);
				i = close + 9;
				continue;
			}
		}

		if (s.compare(i, 6, "<code>") == 0) {
			size_t close = s.find("</code>", i);
			if (close != std::string::npos) {
				PushText(out, pending, level);
				pending.clear();
				MwInline node;
				node.element_type = DuckBlockVocabulary::INLINE_CODE;
				node.content = s.substr(i + 6, close - i - 6);
				node.level = level;
				out.push_back(node);
				i = close + 7;
				continue;
			}
		}

		if (s.compare(i, 4, "<ref") == 0) {
			size_t gt = s.find('>', i);
			if (gt != std::string::npos) {
				std::string tag = s.substr(i, gt - i + 1);
				std::string name;
				size_t np = tag.find("name=");
				if (np != std::string::npos) {
					size_t q = tag.find_first_of("\"'", np);
					if (q != std::string::npos) {
						size_t q2 = tag.find(tag[q], q + 1);
						if (q2 != std::string::npos) {
							name = tag.substr(q + 1, q2 - q - 1);
						}
					}
				}
				PushText(out, pending, level);
				pending.clear();
				MwInline node;
				node.element_type = DuckBlockVocabulary::INLINE_NOTE;
				node.level = level;
				// THE NAME IS KEPT ON BOTH FORMS. pandoc discards it -- a `<ref
				// name="a"/>` reuse becomes an EMPTY Note -- so a consumer cannot join
				// a reuse back to its definition. The attribute costs nothing and
				// pandoc's own shape is unchanged, so this is additive rather than a
				// divergence.
				if (!name.empty()) {
					node.attributes["name"] = name;
				}
				if (tag.size() >= 2 && tag[tag.size() - 2] == '/') {
					out.push_back(node); // self-closing reuse: no body
					i = gt + 1;
					continue;
				}
				size_t close = s.find("</ref>", gt);
				std::string body = close == std::string::npos ? s.substr(gt + 1) : s.substr(gt + 1, close - gt - 1);
				node.content = PlainText(body);
				out.push_back(node);
				i = close == std::string::npos ? s.size() : close + 6;
				continue;
			}
		}

		// `[[Article]]` and `[[Article|label]]`.
		if (s.compare(i, 2, "[[") == 0) {
			size_t close = s.find("]]", i);
			if (close != std::string::npos) {
				std::string inner = s.substr(i + 2, close - i - 2);
				size_t bar = inner.find('|');
				std::string target = bar == std::string::npos ? inner : inner.substr(0, bar);
				std::string label = bar == std::string::npos ? inner : inner.substr(bar + 1);
				PushText(out, pending, level);
				pending.clear();
				MwInline node;
				node.element_type = DuckBlockVocabulary::INLINE_LINK;
				node.content = label;
				node.level = level;
				node.attributes["href"] = target;
				// PANDOC MARKS AN INTERNAL LINK IN THE TITLE FIELD -- Link [..] ["A",
				// "wikilink"] -- overloading the link title as a type marker. Copying
				// that field into `title` would produce a document full of links titled
				// "wikilink", so it is read as what it means.
				node.attributes["link_type"] = "wikilink";
				out.push_back(node);
				i = close + 2;
				continue;
			}
		}

		// `[http://x label]` and the bare `[http://x]`.
		if (s[i] == '[' && s.compare(i, 2, "[[") != 0) {
			size_t close = s.find(']', i);
			if (close != std::string::npos) {
				std::string inner = s.substr(i + 1, close - i - 1);
				size_t sp = inner.find(' ');
				std::string target = sp == std::string::npos ? inner : inner.substr(0, sp);
				if (target.compare(0, 4, "http") == 0 || target.compare(0, 2, "//") == 0) {
					PushText(out, pending, level);
					pending.clear();
					MwInline node;
					node.element_type = DuckBlockVocabulary::INLINE_LINK;
					node.content = sp == std::string::npos ? target : Trim(inner.substr(sp + 1));
					node.level = level;
					node.attributes["href"] = target;
					out.push_back(node);
					i = close + 1;
					continue;
				}
			}
		}

		// The quote family. FIVE first: `'''''both'''''` nests Strong around Emph,
		// measured, and testing three-before-five would consume the wrong
		// delimiter.
		if (s.compare(i, 5, "'''''") == 0) {
			size_t close = s.find("'''''", i + 5);
			if (close != std::string::npos) {
				PushText(out, pending, level);
				pending.clear();
				MwInline bold;
				bold.element_type = DuckBlockVocabulary::INLINE_BOLD;
				bold.level = level;
				out.push_back(bold);
				PushWrapped(out, DuckBlockVocabulary::INLINE_ITALIC, s.substr(i + 5, close - i - 5), level + 1);
				i = close + 5;
				continue;
			}
		}
		if (s.compare(i, 3, "'''") == 0) {
			size_t close = s.find("'''", i + 3);
			if (close != std::string::npos) {
				PushText(out, pending, level);
				pending.clear();
				PushWrapped(out, DuckBlockVocabulary::INLINE_BOLD, s.substr(i + 3, close - i - 3), level);
				i = close + 3;
				continue;
			}
		}
		if (s.compare(i, 2, "''") == 0) {
			size_t close = s.find("''", i + 2);
			if (close != std::string::npos) {
				PushText(out, pending, level);
				pending.clear();
				PushWrapped(out, DuckBlockVocabulary::INLINE_ITALIC, s.substr(i + 2, close - i - 2), level);
				i = close + 2;
				continue;
			}
		}

		pending += s[i];
		i++;
	}
	PushText(out, pending, level);
}

//! Attach a line's inline runs to a block, collapsing the text-only case into
//! `content`.
void AttachInlines(MwBlock &block, const std::string &text) {
	std::vector<MwInline> inl;
	ParseInlines(text, block.level + 1, inl);
	if (inl.size() == 1 && inl[0].element_type == DuckBlockVocabulary::INLINE_TEXT && inl[0].attributes.empty()) {
		block.content = inl[0].content;
		return;
	}
	block.inlines = std::move(inl);
}

//! `[[File:name|thumb|caption]]` alone on a line. Pandoc makes this a Figure
//! wrapping an Image, with the caption as the figure's own caption, and
//! duck_block has all three types.
bool FileLink(const std::string &t, std::vector<MwBlock> &out) {
	if (t.compare(0, 2, "[[") != 0 || t.size() < 4 || t.compare(t.size() - 2, 2, "]]") != 0) {
		return false;
	}
	std::string inner = t.substr(2, t.size() - 4);
	std::string lower;
	for (char c : inner.substr(0, 6)) {
		lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	if (lower.compare(0, 5, "file:") != 0 && lower.compare(0, 6, "image:") != 0) {
		return false;
	}
	std::vector<std::string> parts;
	size_t start = 0;
	for (size_t i = 0; i <= inner.size(); i++) {
		if (i == inner.size() || inner[i] == '|') {
			parts.push_back(inner.substr(start, i - start));
			start = i + 1;
		}
	}
	std::string src = parts[0].substr(parts[0].find(':') + 1);
	// The caption is the LAST parameter that is not a known display option --
	// MediaWiki's own rule, and the reason `thumb` does not become the caption.
	static const char *kOptions[] = {"thumb", "thumbnail", "frame", "frameless", "border",
	                                 "right", "left",      "none",  "center",    nullptr};
	std::string caption;
	for (size_t p = 1; p < parts.size(); p++) {
		std::string v = Trim(parts[p]);
		bool is_option = v.empty() || v.find("px") != std::string::npos;
		for (const char **o = kOptions; *o && !is_option; o++) {
			if (v == *o) {
				is_option = true;
			}
		}
		if (!is_option) {
			caption = v;
		}
	}

	MwBlock fig;
	fig.element_type = DuckBlockVocabulary::TYPE_FIGURE;
	fig.level = 1;
	out.push_back(fig);
	if (!caption.empty()) {
		MwBlock cap;
		cap.element_type = DuckBlockVocabulary::TYPE_CAPTION;
		cap.level = 2;
		AttachInlines(cap, caption);
		out.push_back(cap);
	}
	MwBlock img;
	img.element_type = DuckBlockVocabulary::TYPE_IMAGE;
	img.level = 2;
	img.attributes["src"] = src;
	if (!caption.empty()) {
		img.attributes["alt"] = caption;
	}
	out.push_back(img);
	return true;
}

class Builder {
public:
	std::vector<MwBlock> Build(const std::string &src) {
		auto lines = ScanMediaWiki(src);
		for (size_t i = 0; i < lines.size(); i++) {
			const auto &ln = lines[i];
			switch (ln.kind) {
			case LineKind::BLANK:
				Flush();
				break;
			case LineKind::COMMENT:
				// A comment is not content. Pandoc emits nothing for it either.
				Flush();
				break;
			case LineKind::HEADING: {
				Flush();
				CloseLists();
				MwBlock b;
				b.element_type = DuckBlockVocabulary::TYPE_HEADING;
				b.level = 1;
				b.attributes[DuckBlockVocabulary::ATTR_HEADING_LEVEL] = std::to_string(ln.level);
				auto id = HeadingSlug(PlainText(ln.text), '_');
				if (!id.empty()) {
					b.attributes["id"] = id;
				}
				AttachInlines(b, ln.text);
				blocks_.push_back(std::move(b));
				break;
			}
			case LineKind::HRULE: {
				Flush();
				CloseLists();
				MwBlock b;
				b.element_type = DuckBlockVocabulary::TYPE_HR;
				b.level = 1;
				blocks_.push_back(std::move(b));
				break;
			}
			case LineKind::TEMPLATE: {
				Flush();
				CloseLists();
				MwBlock b;
				b.element_type = DuckBlockVocabulary::TYPE_RAW;
				b.level = 1;
				b.content = ln.text;
				b.attributes["format"] = RAW_FORMAT_MEDIAWIKI;
				b.attributes[DuckBlockVocabulary::ATTR_SOURCE_TYPE] = "template";
				if (!ln.name.empty()) {
					b.attributes["template_name"] = ln.name;
				}
				blocks_.push_back(std::move(b));
				break;
			}
			case LineKind::BEHAVIOR: {
				Flush();
				CloseLists();
				// HELD RAW, NOT DROPPED, and not leaked as prose either.
				//
				// Measured against MediaWiki's own parser 2026-09-02: __TOC__ is
				// CONSUMED and never reaches the reader, so pandoc's `Str "__TOC__"`
				// puts a token in the document that no reader of the wiki would ever
				// see. But it is a render-time instruction exactly as a template is --
				// unresolvable without the wiki, and really there in the source -- so
				// discarding it would lose something real.
				MwBlock b;
				b.element_type = DuckBlockVocabulary::TYPE_RAW;
				b.level = 1;
				b.content = ln.text;
				b.attributes["format"] = RAW_FORMAT_MEDIAWIKI;
				b.attributes[DuckBlockVocabulary::ATTR_SOURCE_TYPE] = "behavior_switch";
				blocks_.push_back(std::move(b));
				break;
			}
			case LineKind::HTML_BLOCK: {
				Flush();
				CloseLists();
				HtmlBlock(ln);
				break;
			}
			case LineKind::PREFORMATTED: {
				if (!pre_.empty()) {
					pre_ += "\n";
				}
				pre_ += ln.text;
				break;
			}
			case LineKind::LIST_ITEM:
				FlushParagraph();
				FlushPre();
				ListItem(ln);
				break;
			case LineKind::TABLE_START:
				Flush();
				CloseLists();
				i = Table(lines, i);
				break;
			case LineKind::TEXT:
				FlushPre();
				CloseLists();
				if (para_.empty() && FileLink(Trim(ln.text), blocks_)) {
					break;
				}
				if (!para_.empty()) {
					para_ += " ";
				}
				para_ += ln.text;
				break;
			default:
				// TABLE_* outside a table: the scanner only emits these between START
				// and END, so reaching here means a stray `|` line. Treat it as prose
				// rather than dropping it.
				FlushPre();
				if (!para_.empty()) {
					para_ += " ";
				}
				para_ += ln.text;
				break;
			}
		}
		Flush();
		CloseLists();
		return std::move(blocks_);
	}

private:
	void FlushParagraph() {
		if (para_.empty()) {
			return;
		}
		MwBlock b;
		b.element_type = DuckBlockVocabulary::TYPE_PARAGRAPH;
		b.level = 1;
		AttachInlines(b, para_);
		blocks_.push_back(std::move(b));
		para_.clear();
	}

	void FlushPre() {
		if (pre_.empty()) {
			return;
		}
		// A `code` BLOCK, where pandoc produces a paragraph of inline Code joined
		// by LineBreaks with spaces replaced by U+00A0. MediaWiki's own parser
		// renders a leading space as <pre> -- measured 2026-09-02 -- so the block
		// type is the faithful reading and pandoc's is an approximation of it
		// inside a paragraph.
		MwBlock b;
		b.element_type = DuckBlockVocabulary::TYPE_CODE;
		b.level = 1;
		b.content = pre_;
		blocks_.push_back(std::move(b));
		pre_.clear();
	}

	void Flush() {
		FlushParagraph();
		FlushPre();
	}

	//! Block-level HTML that wikitext allows. Four of these have a real
	//! duck_block type and the rest are held raw as HTML -- which is what pandoc
	//! does too, and one of the few places this reader deliberately MIRRORS
	//! pandoc rather than improving on it.
	//!
	//! `<references/>` is the case worth naming: it is a placeholder for
	//! generated content, exactly like `__TOC__`, and it would be consistent to
	//! treat it as a behavior switch. It is not, because pandoc emits RawBlock
	//! ["html", ...] for it DELIBERATELY, and the rule this reader follows is to
	//! mirror pandoc's considered choices and diverge only where pandoc leaked
	//! non-prose into prose. This is a choice, not a leak.
	void HtmlBlock(const Line &ln) {
		if (ln.verbatim) {
			// A line that is only tags -- `<span id="x"></span>` and the like. Held
			// as raw HTML, which is what pandoc does with it, rather than allowed to
			// reach a paragraph as literal text.
			MwBlock r;
			r.element_type = DuckBlockVocabulary::TYPE_RAW;
			r.level = 1;
			// THE FORMAT NEVER LIVES IN `encoding`. duck_block_utils measured this
			// across four formats and made it a FLAT rule at 61da561: a raw block's
			// encoding is `text` even for html and latex, which ARE declared
			// encodings. Stated as a fallback -- "use the attribute when the encoding
			// set lacks your format" -- it invites encoding='html' whenever it
			// happens to fit, and produces two shapes for one element_type.
			r.attributes["format"] = "html";
			r.content = ln.text;
			if (!ln.name.empty()) {
				r.attributes[DuckBlockVocabulary::ATTR_SOURCE_TYPE] = ln.name;
			}
			blocks_.push_back(std::move(r));
			return;
		}
		if (ln.name == "blockquote") {
			MwBlock q;
			q.element_type = DuckBlockVocabulary::TYPE_BLOCKQUOTE;
			q.level = 1;
			blocks_.push_back(std::move(q));
			MwBlock p;
			p.element_type = DuckBlockVocabulary::TYPE_PARAGRAPH;
			p.level = 2;
			AttachInlines(p, ln.text);
			blocks_.push_back(std::move(p));
			return;
		}
		if (ln.name == "syntaxhighlight" || ln.name == "source" || ln.name == "pre") {
			MwBlock c;
			c.element_type = DuckBlockVocabulary::TYPE_CODE;
			c.level = 1;
			c.content = ln.text;
			// `lang="python"` on syntaxhighlight; `<pre>` never carries one.
			size_t lp = ln.attrs.find("lang");
			if (lp != std::string::npos) {
				size_t q = ln.attrs.find_first_of("\"'", lp);
				if (q != std::string::npos) {
					size_t q2 = ln.attrs.find(ln.attrs[q], q + 1);
					if (q2 != std::string::npos) {
						c.attributes["language"] = ln.attrs.substr(q + 1, q2 - q - 1);
					}
				}
			}
			blocks_.push_back(std::move(c));
			return;
		}
		MwBlock r;
		r.element_type = DuckBlockVocabulary::TYPE_RAW;
		r.level = 1;
		// The format never lives in `encoding` -- see the verbatim arm above.
		r.attributes["format"] = "html";
		r.content = ln.text.empty() ? "<" + ln.name + (ln.attrs.empty() ? "" : " " + ln.attrs) + "/>"
		                            : "<" + ln.name + ">" + ln.text + "</" + ln.name + ">";
		r.attributes[DuckBlockVocabulary::ATTR_SOURCE_TYPE] = ln.name;
		blocks_.push_back(std::move(r));
	}

	//! `*` bullet, `#` ordered, `;` term, `:` definition. Nesting depth is the
	//! LENGTH of the marker run, not indentation, so `#*` is a bullet list inside
	//! an ordered one.
	void ListItem(const Line &ln) {
		const std::string &m = ln.markers;
		// `;` AND `:` ARE ONE LIST, not two. A term line and its definition line
		// carry different marker characters but belong to the same definition list,
		// so nesting identity is compared on a NORMALISED marker -- otherwise `;
		// term` / `: definition` closes a list and opens another, and the pair that
		// defines a definition list ends up in two sibling lists that share
		// nothing.
		auto norm = [](char c) {
			return c == ':' ? ';' : c;
		};
		size_t common = 0;
		while (common < m.size() && common < open_.size() && norm(m[common]) == open_[common]) {
			common++;
		}
		while (open_.size() > common) {
			open_.pop_back();
		}
		for (size_t k = common; k < m.size(); k++) {
			MwBlock b;
			b.element_type = DuckBlockVocabulary::TYPE_LIST;
			b.level = static_cast<int>(2 * k + 1);
			const char *lt = m[k] == '#'                    ? DuckBlockVocabulary::LIST_TYPE_ORDERED
			                 : (m[k] == ';' || m[k] == ':') ? DuckBlockVocabulary::LIST_TYPE_DEFINITION
			                                                : DuckBlockVocabulary::LIST_TYPE_BULLET;
			b.attributes[DuckBlockVocabulary::ATTR_LIST_TYPE] = lt;
			b.attributes[DuckBlockVocabulary::ATTR_ORDERED_LEGACY] =
			    lt == std::string(DuckBlockVocabulary::LIST_TYPE_ORDERED) ? "true" : "false";
			if (lt == std::string(DuckBlockVocabulary::LIST_TYPE_ORDERED)) {
				b.attributes["start"] = "1";
				b.attributes["number_style"] = "Decimal";
				b.attributes["number_delim"] = "Period";
			}
			blocks_.push_back(std::move(b));
			open_.push_back(norm(m[k]));
		}
		MwBlock item;
		item.element_type = DuckBlockVocabulary::TYPE_LIST_ITEM;
		item.level = static_cast<int>(2 * m.size());
		char last = m.empty() ? '*' : m.back();
		if (last == ';') {
			item.attributes[DuckBlockVocabulary::ATTR_ROLE] = DuckBlockVocabulary::ROLE_TERM;
		} else if (last == ':') {
			item.attributes[DuckBlockVocabulary::ATTR_ROLE] = DuckBlockVocabulary::ROLE_DEFINITION;
		}
		AttachInlines(item, ln.text);
		blocks_.push_back(std::move(item));
	}

	void CloseLists() {
		open_.clear();
	}

	//! Consume `{| ... |}` into one native table. Returns the index of the
	//! closing line.
	size_t Table(const std::vector<Line> &lines, size_t start) {
		std::vector<std::string> headers;
		std::vector<std::vector<std::string>> rows;
		std::vector<std::string> current;
		bool started = false;
		size_t i = start + 1;
		for (; i < lines.size() && lines[i].kind != LineKind::TABLE_END; i++) {
			const auto &ln = lines[i];
			if (ln.kind == LineKind::TABLE_HEADER) {
				for (auto &c : SplitCells(ln.text, '!')) {
					headers.push_back(PlainText(c));
				}
			} else if (ln.kind == LineKind::TABLE_ROW) {
				if (started) {
					rows.push_back(current);
				}
				current.clear();
				started = true;
			} else if (ln.kind == LineKind::TABLE_CELL) {
				for (auto &c : SplitCells(ln.text, '|')) {
					current.push_back(PlainText(c));
				}
				started = true;
			}
		}
		if (!current.empty()) {
			rows.push_back(current);
		}
		MwBlock b;
		b.element_type = DuckBlockVocabulary::TYPE_TABLE;
		b.level = 1;
		b.encoding = DuckBlockVocabulary::ENCODING_JSON;
		b.content = BuildTableJson(headers, rows);
		blocks_.push_back(std::move(b));
		return i;
	}

	std::vector<MwBlock> blocks_;
	std::string para_;
	std::string pre_;
	std::string open_; //!< the marker chars of currently open lists, outermost first
};

} // namespace

std::vector<MwBlock> ParseMediaWikiString(const std::string &src) {
	Builder builder;
	return builder.Build(src);
}

std::vector<Block> ReadMediaWiki(const std::string &src) {
	std::vector<Block> rows;
	int32_t order = 0;
	for (auto &block : ParseMediaWikiString(src)) {
		Block row;
		row.kind = block.kind.empty() ? DuckBlockVocabulary::KIND_BLOCK : block.kind;
		row.element_type = block.element_type;
		row.content = block.content;
		// ENCODING SET EXPLICITLY, not defaulted. MwRow defaulted this field to
		// ENCODING_TEXT and the loop only overrode it when the block carried one
		// (`if (!block.encoding.empty())`); panduck::Block defaults it to empty,
		// because a shared vocabulary type should not carry one format's default.
		row.encoding = block.encoding.empty() ? DuckBlockVocabulary::ENCODING_TEXT : block.encoding;
		row.attributes = block.attributes;
		row.level = block.level > 0 ? block.level : 1;
		row.element_order = order++;
		const int32_t block_level = row.level;
		rows.push_back(std::move(row));

		for (auto &inl : block.inlines) {
			Block child;
			// NOT the unconditional ENCODING_TEXT org, rst and textile children get.
			// MwInline HAS an `encoding` field -- 'mediawiki' for an inline raw
			// template -- and the old loop overrode the struct default under
			// `if (!inl.encoding.empty())`. So the fallback is conditional here, and
			// copying a sibling reader's child line would have silently rewritten
			// every inline raw template's encoding to 'text'.
			child.encoding = inl.encoding.empty() ? DuckBlockVocabulary::ENCODING_TEXT : inl.encoding;
			child.kind = DuckBlockVocabulary::KIND_INLINE;
			child.element_type = inl.element_type;
			child.content = inl.content;
			child.attributes = inl.attributes;
			child.level = inl.level > 0 ? inl.level : block_level + 1;
			child.element_order = order++;
			rows.push_back(std::move(child));
		}
	}
	return rows;
}

} // namespace mediawiki
} // namespace panduck
