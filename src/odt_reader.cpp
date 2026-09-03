#include "doc_metadata.hpp"
#include "odt_reader.hpp"

#include "block_json.hpp"

#include "duck_block_types.hpp"
#include "zip_container.hpp"

#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <functional>
#include <map>
#include <pugixml.hpp>

namespace duckdb {
namespace odt {

namespace {

struct RunFormat {
	bool bold = false, italic = false, underline = false, strike = false;
	bool code = false, superscript = false, subscript = false;

	bool Plain() const {
		return !bold && !italic && !underline && !strike && !code && !superscript && !subscript;
	}
	//! duck_block's inline vocabulary is flat, so a run carrying several attributes is
	//! reported by its strongest. Same documented limitation as the RTF and DOCX readers.
	std::string ElementType() const {
		// Code outranks the character toggles: pandoc's ODT writer gives a verbatim run a
		// monospace FONT, and some themes make that font italic. Reporting such a run as
		// italic would lose the one property that actually carries meaning.
		if (code) {
			return DuckBlockTypes::INLINE_CODE;
		}
		if (superscript) {
			return DuckBlockTypes::INLINE_SUPERSCRIPT;
		}
		if (subscript) {
			return DuckBlockTypes::INLINE_SUBSCRIPT;
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

//! ODT resolves inline formatting indirectly: a run names a style
//! (<text:span text:style-name="T1">) and the style carries the properties. This is the
//! ODF analogue of DOCX's styles.xml, except the styles that matter live in content.xml's
//! own <office:automatic-styles>, generated per document.
std::map<std::string, RunFormat> ParseAutomaticStyles(const pugi::xml_node &root) {
	std::map<std::string, RunFormat> styles;
	for (auto style : root.child("office:automatic-styles").children("style:style")) {
		std::string name = style.attribute("style:name").value();
		if (name.empty()) {
			continue;
		}
		auto props = style.child("style:text-properties");
		if (!props) {
			continue;
		}
		RunFormat fmt;
		fmt.bold = std::string(props.attribute("fo:font-weight").value()) == "bold";
		fmt.italic = std::string(props.attribute("fo:font-style").value()) == "italic";
		// A line-through or underline is present unless it says "none" -- ODF spells the
		// style rather than toggling a flag, so absence and "none" both mean off.
		std::string strike_style = props.attribute("style:text-line-through-style").value();
		fmt.strike = !strike_style.empty() && strike_style != "none";
		std::string underline_style = props.attribute("style:text-underline-style").value();
		fmt.underline = !underline_style.empty() && underline_style != "none";
		// SUB/SUPERSCRIPT is a POSITION in ODF, written as `super 58%` or `sub 58%` -- a
		// keyword and a size, so the keyword is matched by prefix rather than by equality.
		std::string position = props.attribute("style:text-position").value();
		fmt.superscript = position.rfind("super", 0) == 0;
		fmt.subscript = position.rfind("sub", 0) == 0;
		styles[name] = fmt;
	}
	return styles;
}

struct Run {
	RunFormat fmt;
	std::string text;
	//! Non-empty when this run is not a formatted text span -- an image or a footnote,
	//! which carry an element_type of their own.
	std::string special_type;
	std::map<std::string, std::string> attrs;
};

//! Walk the mixed content of a text:p / text:h in document order. ODT interleaves raw
//! text with elements, so the order of children is the order of the sentence.
void CollectRuns(const pugi::xml_node &node, const RunFormat &inherited, const std::map<std::string, RunFormat> &styles,
                 std::vector<Run> &runs) {
	for (auto child : node.children()) {
		if (child.type() == pugi::node_pcdata) {
			std::string text = child.value();
			if (text.empty()) {
				continue;
			}
			// The special_type guard matters as soon as a run carries its own element_type:
			// without it the text FOLLOWING a link merges into the anchor, because a link
			// run's `fmt` is default-constructed and so compares equal to plain text. The
			// sentence "... and a [link](url)." read back with the anchor as "link.".
			if (!runs.empty() && runs.back().special_type.empty() &&
			    runs.back().fmt.ElementType() == inherited.ElementType()) {
				runs.back().text += text;
			} else {
				runs.push_back(Run {inherited, text});
			}
			continue;
		}
		std::string tag = child.name();
		if (tag == "text:span") {
			auto it = styles.find(child.attribute("text:style-name").value());
			CollectRuns(child, it != styles.end() ? it->second : inherited, styles, runs);
		} else if (tag == "text:a") {
			// A LINK. The anchor's own children are collected first -- it can hold spans,
			// so its text is not simply child.text() -- and then flattened into one link
			// run carrying the href. The text was never lost here (unlike DOCX, where the
			// wrapped runs were skipped outright); what was missing was the URL.
			std::vector<Run> anchor;
			CollectRuns(child, inherited, styles, anchor);
			std::string text;
			for (auto &r : anchor) {
				text += r.text;
			}
			std::string href = child.attribute("xlink:href").value();
			if (!text.empty() && !href.empty()) {
				Run link;
				link.special_type = DuckBlockTypes::INLINE_LINK;
				link.attrs["href"] = href;
				link.text = text;
				runs.push_back(std::move(link));
			} else {
				// No href to record: keep the words rather than inventing a link.
				for (auto &r : anchor) {
					runs.push_back(std::move(r));
				}
			}
		} else if (tag == "text:s") {
			// <text:s text:c="3"/> is a run of spaces; ODF collapses literal ones.
			int count = child.attribute("text:c").as_int(1);
			std::string spaces(count < 1 ? 1 : count, ' ');
			// Same guard as the text branch above: spaces after a link belong to the
			// sentence, not to the anchor.
			if (!runs.empty() && runs.back().special_type.empty() &&
			    runs.back().fmt.ElementType() == inherited.ElementType()) {
				runs.back().text += spaces;
			} else {
				runs.push_back(Run {inherited, spaces});
			}
		} else if (tag == "text:tab") {
			runs.push_back(Run {inherited, "\t"});
		} else if (tag == "text:line-break") {
			runs.push_back(Run {inherited, " "});
		} else if (tag == "draw:frame" || tag == "draw:image") {
			// AN IMAGE. ODF points straight at the file with xlink:href -- no relationship
			// indirection, unlike OOXML -- so the href IS the src.
			auto img = tag == "draw:image" ? child : child.child("draw:image");
			if (img) {
				Run r;
				r.special_type = DuckBlockTypes::INLINE_IMAGE;
				r.attrs["src"] = img.attribute("xlink:href").value();
				runs.push_back(std::move(r));
			}
		} else if (tag == "text:note") {
			// A FOOTNOTE. text:note-citation is the RENDERED marker -- the number a reader
			// sees -- and text:note-body is the note itself. Collecting the whole element
			// would put the citation digit into the note's text.
			std::string body;
			for (auto p : child.child("text:note-body").children("text:p")) {
				if (!body.empty()) {
					body += " ";
				}
				body += p.text().get();
			}
			Run r;
			r.special_type = DuckBlockTypes::INLINE_NOTE;
			r.text = body;
			runs.push_back(std::move(r));
		} else if (tag == "text:meta" || tag == "text:meta-field" || tag == "text:ruby-base" ||
		           tag == "text:bookmark-ref" || tag == "text:reference-mark-start") {
			// TRANSPARENT WRAPPERS. They annotate a span of text -- RDF metadata, a
			// reference -- and carry no formatting of their own, so their CONTENT is part of
			// the sentence. Probed: "meta <text:meta>METATEXT</text:meta> tail" read back as
			// "meta  tail", with the wrapped word gone. Same class as DOCX's <w:hyperlink>.
			CollectRuns(child, inherited, styles, runs);
		} else if (tag == "text:ruby") {
			// RUBY is a base plus a pronunciation gloss. Only the BASE is the sentence's
			// text; descending into the whole element would splice the gloss into the prose.
			CollectRuns(child.child("text:ruby-base"), inherited, styles, runs);
		}
		// DELIBERATELY NOT DESCENDED: office:annotation is a COMMENT, and text:ruby-text is
		// a pronunciation gloss. Both are about the document rather than part of it -- the
		// same reason DOCX does not descend into <w:del>. Unknown elements are ignored
		// rather than flattened, so a wrapper that does carry text has to be added here
		// deliberately; that is what hid text:meta and text:ruby until they were probed.
	}
}

} // namespace

//! meta.xml as `value` elements, appended after the blocks.
//!
//! Every field EXCEEDS pandoc, which extracts nothing from ODT -- see doc_metadata.hpp --
//! so each carries attributes['source_type'] with its original spelling, marking it as
//! format-derived rather than pandoc-derived.
void CollectOdtMetadata(const std::string &meta_xml, std::vector<OdtBlock> &out) {
	if (meta_xml.empty()) {
		return;
	}
	pugi::xml_document doc;
	if (!doc.load_buffer(meta_xml.data(), meta_xml.size())) {
		// A malformed metadata part must not fail the document -- the body is what the
		// caller asked for.
		return;
	}
	auto meta = doc.child("office:document-meta").child("office:meta");
	if (!meta) {
		return;
	}
	std::map<std::string, std::pair<std::string, std::string>> found; // key -> {text, source}
	for (auto &field : ODT_META_FIELDS) {
		auto node = meta.child(field.source);
		if (!node) {
			continue;
		}
		auto text = TrimMetaText(node.child_value());
		if (text.empty()) {
			// EMPTY FIELDS ARE SKIPPED HERE, and this is the one place that differs from
			// the EPUB/LaTeX/RTF readers, deliberately.
			//
			// The ruling is "an empty field is still a field", and its stated ground is
			// that "emitting nothing would discard a fact PANDOC PRESERVED" -- pandoc emits
			// the key with an empty value, so a reader mirrors it. That reasoning is about
			// MIRRORING, and pandoc extracts nothing at all from DOCX and ODT, so there is
			// no empty field of pandoc's to mirror and nothing is being discarded.
			//
			// THAT GROUND WAS INCOMPLETE, and the ruling's second one has to be answered
			// on its own terms: a consumer cannot recover "the author declared a title and
			// left it blank" from silence. That argument never mentions pandoc, so pandoc's
			// absence does not dispose of it.
			//
			// Answered directly it points the same way and gives a better rule than "docx
			// and odt are different": PRESENT-AND-EMPTY CARRIES INFORMATION ONLY WHEN
			// PRESENCE IS A CHOICE. Emit an empty field when its presence is authorial;
			// skip it when the format's writer emits the element unconditionally. That
			// yields this behaviour here and the opposite for YAML and Org, from one
			// principle rather than an exception.
			//
			// MEASURED ACROSS TWO INDEPENDENT PRODUCERS rather than assumed from one:
			//
			//     LibreOffice  dc:title, dc:creator, dc:subject, dc:description  all EMPTY
			//     Pandoc       dc:title, dc:creator EMPTY; subject/description ABSENT
			//
			// Both write an empty title and creator into every file, so presence there is a
			// constant rather than an authorial act. Emitting them would put four empty
			// rows in every document and invite exactly the misreading the original rule
			// protects against -- a deliberate blanking that never happened.
			//
			// BOUNDED DELIBERATELY: Word and Pages are UNMEASURED. If some producer writes
			// <dc:title/> only when a title was set and then cleared, the element IS
			// authorial in those files and this skip is wrong for them. A reader cannot see
			// which producer wrote a file, so per-format is the implementable
			// approximation. Recorded as a measurement about named producers rather than a
			// law, so it can be revisited instead of inherited.
			continue;
		}
		found[field.key] = {text, field.source};
	}
	if (!found.count("date")) {
		// meta:creation-date ONLY when dc:date is absent. Both carry the same instant in
		// every fixture measured, and two sources feeding one key is how the two drift
		// apart -- so the Dublin Core spelling wins and this is the fallback, not a peer.
		auto created = meta.child("meta:creation-date");
		auto created_text = created ? TrimMetaText(created.child_value()) : std::string();
		if (!created_text.empty()) {
			found["date"] = {created_text, "meta:creation-date"};
		}
	}
	for (auto &kv : found) {
		OdtBlock block;
		block.kind = DuckBlockTypes::KIND_VALUE;
		block.element_type = DuckBlockTypes::VALUE_INLINES;
		block.key = kv.first;
		block.source_type = kv.second.second;
		if (!kv.second.first.empty()) { // always true here; see the skip above
			OdtInline run;
			run.element_type = DuckBlockTypes::INLINE_TEXT;
			run.content = kv.second.first;
			block.inlines.push_back(std::move(run));
		}
		out.push_back(std::move(block));
	}
}

//! Per-level orderedness for every `text:list-style`, keyed by style name.
//!
//! ODF defines ALL TEN levels of a list style up front, and a single style routinely mixes
//! them -- the fixture's WWNum1001 declares both `bullet` and `number` levels. So "is this
//! list ordered" is a question about a LEVEL, not about a style, and reading the style as a
//! whole gets it wrong on any document whose nested levels differ from its first.
std::map<std::string, std::map<int, bool>> ParseListStyles(const pugi::xml_node &root) {
	std::map<std::string, std::map<int, bool>> out;
	std::function<void(const pugi::xml_node &)> walk = [&](const pugi::xml_node &node) {
		for (auto child : node.children()) {
			if (std::string(child.name()) == "text:list-style") {
				std::string name = child.attribute("style:name").value();
				if (name.empty()) {
					name = child.attribute("text:name").value();
				}
				for (auto lvl : child.children()) {
					std::string tag = lvl.name();
					if (tag.rfind("text:list-level-style-", 0) != 0) {
						continue;
					}
					int level = lvl.attribute("text:level").as_int(1);
					out[name][level] = (tag == "text:list-level-style-number");
				}
			}
			walk(child);
		}
	};
	walk(root);
	return out;
}

//! Paragraph style names reachable from `seeds` through parent-style-name.
//!
//! A document rarely names a semantic style directly. pandoc's ODT writer emits automatic
//! styles -- `P8`, `P10` -- whose parent is the meaningful one (`Quotations`,
//! `Preformatted_20_Text`), so matching the name on the paragraph finds nothing and the
//! chain has to be followed. Used for blockquotes and for code blocks, which differ only
//! in their seed names.
std::set<std::string> ParseStyleClosure(const pugi::xml_node &content_root, const pugi::xml_node &styles_root,
                                        std::set<std::string> quote, const char *family = "paragraph") {
	std::map<std::string, std::string> parent;
	std::function<void(const pugi::xml_node &)> walk = [&](const pugi::xml_node &node) {
		for (auto child : node.children()) {
			if (std::string(child.name()) == "style:style" &&
			    std::string(child.attribute("style:family").value()) == family) {
				std::string name = child.attribute("style:name").value();
				std::string par = child.attribute("style:parent-style-name").value();
				if (!name.empty() && !par.empty()) {
					parent[name] = par;
				}
			}
			walk(child);
		}
	};
	walk(content_root);
	if (styles_root) {
		walk(styles_root);
	}
	// Close over the parent chain. Bounded, because a cycle in a malformed document must
	// not hang the reader.
	for (int pass = 0; pass < 8; pass++) {
		size_t before = quote.size();
		for (auto &kv : parent) {
			if (quote.count(kv.second)) {
				quote.insert(kv.first);
			}
		}
		if (quote.size() == before) {
			break;
		}
	}
	return quote;
}

//! BLOCKQUOTE styles. LibreOffice writes `Block_20_Text` (display name "Block Text");
//! pandoc's own ODT writer uses `Quotations`.
std::set<std::string> ParseBlockquoteStyles(const pugi::xml_node &content_root, const pugi::xml_node &styles_root) {
	return ParseStyleClosure(content_root, styles_root, {"Block_20_Text", "Quotations"});
}

//! CODE-BLOCK styles. Measured on a pandoc-written ODT: a fenced block and an indented
//! block both become paragraphs whose style parent is `Preformatted_20_Text`. Without
//! this they read as ordinary paragraphs -- the text survived, but a code listing was
//! indistinguishable from prose, and its line breaks had already been flattened to spaces.
std::set<std::string> ParseCodeStyles(const pugi::xml_node &content_root, const pugi::xml_node &styles_root) {
	return ParseStyleClosure(content_root, styles_root, {"Preformatted_20_Text", "Source_20_Code", "Source_20_Text"});
}

//! INLINE-CODE text styles. pandoc names the run's style `Source_Text` and defines it in
//! styles.xml -- NOT in content.xml's automatic styles -- so ParseAutomaticStyles never
//! sees it and a verbatim run read back as ordinary text. Matched by name through the text
//! style chain rather than by sniffing for a monospace font: the font list would be a
//! guess, and these names are what the two writers that matter actually emit.
std::set<std::string> ParseCodeTextStyles(const pugi::xml_node &content_root, const pugi::xml_node &styles_root) {
	return ParseStyleClosure(content_root, styles_root,
	                         {"Source_Text", "Source_20_Text", "Teletype", "Preformatted_20_Text"}, "text");
}

//! DEFINITION-LIST styles. pandoc writes `Definition_20_Term` / `Definition_20_Definition`
//! (with a `_20_Tight` variant for a compact list), which read back as two unrelated
//! paragraphs -- the words survived, the pairing did not.
std::set<std::string> ParseDefTermStyles(const pugi::xml_node &content_root, const pugi::xml_node &styles_root) {
	return ParseStyleClosure(content_root, styles_root, {"Definition_20_Term", "Definition_20_Term_20_Tight"});
}

std::set<std::string> ParseDefBodyStyles(const pugi::xml_node &content_root, const pugi::xml_node &styles_root) {
	return ParseStyleClosure(content_root, styles_root,
	                         {"Definition_20_Definition", "Definition_20_Definition_20_Tight"});
}

std::vector<OdtBlock> ParseOdtFile(const std::string &path) {
	ZipContainer zip(path, "read_odt_blocks");
	auto content_xml = zip.ReadRequired("content.xml");
	// OPTIONAL: a missing meta.xml is a document that declared no metadata, not an error.
	std::string meta_xml;
	zip.Read("meta.xml", meta_xml);
	// OPTIONAL like meta.xml: list styles and the blockquote style live here, and a document
	// without it simply has neither.
	std::string styles_xml;
	zip.Read("styles.xml", styles_xml);

	pugi::xml_document doc;
	// parse_ws_pcdata for the same reason DOCX needs it: a whitespace-only text node
	// between two spans is real inter-word spacing, and the default flags discard it.
	auto parsed = doc.load_buffer(content_xml.data(), content_xml.size(), pugi::parse_default | pugi::parse_ws_pcdata);
	if (!parsed) {
		throw InvalidInputException("read_odt_blocks: content.xml is not well-formed XML in %s: %s", path,
		                            parsed.description());
	}

	auto root = doc.child("office:document-content");
	auto styles = ParseAutomaticStyles(root);
	auto body = root.child("office:body").child("office:text");

	pugi::xml_document styles_doc;
	pugi::xml_node styles_root;
	if (!styles_xml.empty() &&
	    styles_doc.load_buffer(styles_xml.data(), styles_xml.size(), pugi::parse_default | pugi::parse_ws_pcdata)) {
		styles_root = styles_doc.child("office:document-styles");
	}
	auto list_styles = ParseListStyles(root);
	for (auto &kv : ParseListStyles(styles_root)) {
		list_styles[kv.first] = kv.second;
	}
	auto quote_styles = ParseBlockquoteStyles(root, styles_root);
	auto code_styles = ParseCodeStyles(root, styles_root);
	auto def_term_styles = ParseDefTermStyles(root, styles_root);
	auto def_body_styles = ParseDefBodyStyles(root, styles_root);
	// A verbatim run names a style defined in styles.xml, which ParseAutomaticStyles (which
	// reads content.xml's automatic styles) cannot see. Rather than thread another set
	// through CollectRuns, mark those names in the style map the collector already
	// consults -- the lookup is by name, so the two sources merge cleanly.
	for (auto &name : ParseCodeTextStyles(root, styles_root)) {
		styles[name].code = true;
	}

	// ODF nests list content: text:list > text:list-item > text:p, and this reader used to
	// FLATTEN it -- every list paragraph came out as a top-level paragraph, so the words
	// survived and the list did not. That was recorded as a declared gap rather than a
	// defect, on the correct grounds that losing structure beats losing text.
	//
	// It is structure now. The depth is the nesting of text:list elements, and the list
	// style travels down with it so each level can ask whether IT is ordered -- ODF defines
	// all ten levels of a style up front and routinely mixes bullet and number among them.
	struct Entry {
		pugi::xml_node node;
		int depth = 0;
		std::string list_style;
		bool is_table = false; //!< a table:table, which is a sibling of text:p, not a container of it
	};
	std::vector<Entry> entries;
	std::function<void(const pugi::xml_node &, int, const std::string &)> collect =
	    [&](const pugi::xml_node &parent, int depth, const std::string &list_style) {
		    for (auto node : parent.children()) {
			    std::string tag = node.name();
			    if (tag == "text:h" || tag == "text:p") {
				    entries.push_back(Entry {node, depth, list_style});
			    } else if (tag == "text:list") {
				    // A nested text:list may restate the style or inherit the enclosing one.
				    std::string style = node.attribute("text:style-name").value();
				    collect(node, depth + 1, style.empty() ? list_style : style);
			    } else if (tag == "text:list-item" || tag == "text:list-header") {
				    collect(node, depth, list_style);
			    } else if (tag == "text:section" || tag == "office:text") {
				    // A SECTION WRAPS block content. Probed: a paragraph inside
				    // <text:section> never reached the output at all -- the collector only
				    // recognised text:p, text:h, lists and tables, and silently dropped
				    // everything else. Sections are how ODF marks linked or protected
				    // regions, and a table of contents lives in one.
				    collect(node, depth, list_style);
			    } else if (tag == "table:table") {
				    // A table:table is a SIBLING of text:p and was never collected, so every
				    // cell's text vanished -- content loss, not a structure gap. pandoc
				    // reports Table for the same file.
				    entries.push_back(Entry {node, 0, {}, true});
			    }
			    // tables, sequence declarations and drawing frames are not read yet.
		    }
	    };
	collect(body, 0, "");

	std::vector<OdtBlock> blocks;
	// See the DOCX reader: the TYPE of each open list, not just the count. A bullet list
	// after an ordered one at the same depth must close and reopen, or the second is
	// swallowed into the first.
	std::vector<bool> open_ordered;
	for (auto &entry : entries) {
		auto node = entry.node;
		if (entry.is_table) {
			open_ordered.clear();
			std::vector<std::string> headers;
			std::vector<std::vector<std::string>> rows;
			std::function<void(const pugi::xml_node &, bool)> read_rows = [&](const pugi::xml_node &parent,
			                                                                  bool header) {
				for (auto child : parent.children()) {
					std::string ctag = child.name();
					if (ctag == "table:table-header-rows") {
						// ODF marks header rows STRUCTURALLY, in their own element, rather than
						// with an attribute on the row. A table without one has no header, and
						// promoting its first row would invent one.
						read_rows(child, true);
					} else if (ctag == "table:table-row") {
						std::vector<std::string> cells;
						for (auto cell : child.children("table:table-cell")) {
							std::string text;
							for (auto p : cell.children("text:p")) {
								if (!text.empty()) {
									text += " ";
								}
								text += p.text().get();
							}
							cells.push_back(text);
						}
						if (header && headers.empty()) {
							headers = cells;
						} else {
							rows.push_back(cells);
						}
					} else if (ctag == "table:table-rows" || ctag == "table:table-column-group") {
						read_rows(child, header);
					}
				}
			};
			read_rows(node, false);
			OdtBlock t;
			t.element_type = DuckBlockTypes::TYPE_TABLE;
			t.level = 1;
			t.encoding = DuckBlockTypes::ENCODING_JSON;
			t.content = BuildTableJson(headers, rows);
			blocks.push_back(std::move(t));
			continue;
		}
		std::string tag = node.name();
		bool is_heading = (tag == "text:h");

		// OPEN AND CLOSE LISTS around the entries, so `list` wraps its `list_item`s the way
		// every other panduck reader emits them. A heading inside a list closes it: ODF
		// permits the nesting and no consumer expects a heading as a list item.
		int want = is_heading ? 0 : entry.depth;
		auto ordered_at = [&](int depth) {
			auto sit = list_styles.find(entry.list_style);
			if (sit == list_styles.end()) {
				return false;
			}
			auto lit = sit->second.find(depth);
			return lit != sit->second.end() && lit->second;
		};
		if (want > 0 && open_ordered.size() == static_cast<size_t>(want) && open_ordered.back() != ordered_at(want)) {
			open_ordered.pop_back();
		}
		while (open_ordered.size() > static_cast<size_t>(want)) {
			open_ordered.pop_back();
		}
		while (open_ordered.size() < static_cast<size_t>(want)) {
			int depth = static_cast<int>(open_ordered.size()) + 1;
			bool ordered = ordered_at(depth);
			OdtBlock l;
			l.element_type = DuckBlockTypes::TYPE_LIST;
			l.level = 2 * (depth - 1) + 1;
			l.list_type = ordered ? DuckBlockTypes::LIST_TYPE_ORDERED : DuckBlockTypes::LIST_TYPE_BULLET;
			blocks.push_back(std::move(l));
			open_ordered.push_back(ordered);
		}

		std::vector<Run> runs;
		CollectRuns(node, RunFormat(), styles, runs);

		std::string all;
		bool any_format = false;
		for (auto &r : runs) {
			all += r.text;
			if (!r.fmt.Plain() || !r.special_type.empty()) {
				// An image or note forces the paragraph to emit INLINES rather than flatten,
				// or it is dropped in favour of the surrounding text.
				any_format = true;
			}
		}
		auto begin = all.find_first_not_of(" \t\n");
		std::string trimmed;
		if (begin == std::string::npos) {
			// A paragraph whose ONLY content is an image has no text; skipping it as
			// whitespace-only dropped the image.
			bool has_special = false;
			for (auto &r : runs) {
				if (!r.special_type.empty()) {
					has_special = true;
				}
			}
			if (!has_special) {
				continue;
			}
		} else {
			auto end = all.find_last_not_of(" \t\n");
			trimmed = all.substr(begin, end - begin + 1);
		}

		std::string style_name = node.attribute("text:style-name").value();
		bool top = !is_heading && entry.depth == 0;
		bool is_quote = top && quote_styles.count(style_name) > 0;
		bool is_code = top && code_styles.count(style_name) > 0;
		bool is_def_term = top && def_term_styles.count(style_name) > 0;
		bool is_def_body = top && def_body_styles.count(style_name) > 0;

		// A CODE BLOCK IS WRITTEN AS ONE PARAGRAPH PER LINE. pandoc's ODT writer splits
		// `def hello():\n    return 1` across two <text:p>, so emitting a block each would
		// give two code listings where the document has one. Consecutive code paragraphs
		// join with the newline that separated them.
		//
		// `all` is used rather than `trimmed`: the leading spaces ARE the indentation, and
		// trimming them turns "    return 1" into "return 1".
		if (is_code) {
			if (!blocks.empty() && blocks.back().element_type == DuckBlockTypes::TYPE_CODE) {
				blocks.back().content += "\n" + all;
			} else {
				OdtBlock code;
				code.element_type = DuckBlockTypes::TYPE_CODE;
				code.content = all;
				blocks.push_back(std::move(code));
			}
			continue;
		}

		// A DEFINITION LIST is a term paragraph followed by a body paragraph. Emitted as
		// the list/list_item shape with a `role`, which is what the org, rst, epub,
		// mediawiki and textile readers already produce for the same construct.
		if (is_def_term || is_def_body) {
			bool open = !blocks.empty() && blocks.back().element_type == DuckBlockTypes::TYPE_LIST_ITEM &&
			            blocks.back().attributes.count("role") > 0;
			if (!open) {
				OdtBlock l;
				l.element_type = DuckBlockTypes::TYPE_LIST;
				l.level = 1;
				blocks.push_back(std::move(l));
			}
			OdtBlock item;
			item.element_type = DuckBlockTypes::TYPE_LIST_ITEM;
			item.level = 2;
			item.content = trimmed;
			item.attributes["role"] = is_def_term ? "term" : "definition";
			blocks.push_back(std::move(item));
			continue;
		}

		OdtBlock block;
		if (is_heading) {
			int level = node.attribute("text:outline-level").as_int(1);
			block.element_type = DuckBlockTypes::TYPE_HEADING;
			block.heading_level = (level >= 1 && level <= 6) ? level : 1;
		} else if (entry.depth > 0) {
			block.element_type = DuckBlockTypes::TYPE_LIST_ITEM;
			block.level = 2 * entry.depth;
		} else if (is_quote) {
			// A BLOCKQUOTE WRAPS A PARAGRAPH, which is what pandoc emits: BlockQuote [Para].
			OdtBlock q;
			q.element_type = DuckBlockTypes::TYPE_BLOCKQUOTE;
			q.level = 1;
			blocks.push_back(std::move(q));
			block.element_type = DuckBlockTypes::TYPE_PARAGRAPH;
			block.level = 2;
		} else {
			block.element_type = DuckBlockTypes::TYPE_PARAGRAPH;
		}

		// Headings always flatten -- see the RTF and DOCX readers for why.
		if (!any_format || is_heading) {
			block.content = trimmed;
		} else {
			for (auto &r : runs) {
				block.inlines.push_back(
				    OdtInline {r.special_type.empty() ? r.fmt.ElementType() : r.special_type, r.text, r.attrs});
			}
		}
		blocks.push_back(std::move(block));
	}
	// AFTER the blocks -- spec 6.2 makes body-then-metadata a contract.
	CollectOdtMetadata(meta_xml, blocks);
	return blocks;
}

} // namespace odt

namespace {

struct OdtRow {
	std::string kind, element_type, content;
	std::string encoding = DuckBlockTypes::ENCODING_TEXT;
	bool has_level = false;
	int32_t level = 0;
	std::map<std::string, std::string> attributes;
	int32_t element_order = 0;
};

struct OdtBindData : public TableFunctionData {
	std::vector<OdtRow> rows;
};

struct OdtGlobalState : public GlobalTableFunctionState {
	idx_t offset = 0;
	static unique_ptr<GlobalTableFunctionState> Init(ClientContext &, TableFunctionInitInput &) {
		return make_uniq<OdtGlobalState>();
	}
};

unique_ptr<FunctionData> OdtBind(ClientContext &, TableFunctionBindInput &input, vector<LogicalType> &return_types,
                                 vector<string> &names) {
	names = {"kind", "element_type", "content", "level", "encoding", "attributes", "element_order"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::INTEGER,
	                LogicalType::VARCHAR, LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR),
	                LogicalType::INTEGER};

	auto result = make_uniq<OdtBindData>();
	int32_t order = 0;
	for (auto &block : odt::ParseOdtFile(input.inputs[0].GetValue<string>())) {
		OdtRow row;
		row.kind = block.kind.empty() ? DuckBlockTypes::KIND_BLOCK : block.kind;
		if (!block.key.empty()) {
			row.attributes[DuckBlockTypes::ATTR_KEY] = block.key;
		}
		if (!block.source_type.empty()) {
			row.attributes[DuckBlockTypes::ATTR_SOURCE_TYPE] = block.source_type;
		}
		row.element_type = block.element_type;
		row.content = block.content;
		row.element_order = order++;
		if (block.heading_level > 0) {
			row.attributes[DuckBlockTypes::ATTR_HEADING_LEVEL] = std::to_string(block.heading_level);
		}
		if (!block.encoding.empty()) {
			row.encoding = block.encoding;
		}
		if (!block.list_type.empty()) {
			// BOTH SPELLINGS, as every panduck reader emits -- `ordered` is the v1 name and
			// `list_type` the later alias, and a consumer written against either reads this.
			row.attributes[DuckBlockTypes::ATTR_LIST_TYPE] = block.list_type;
			row.attributes[DuckBlockTypes::ATTR_ORDERED_LEGACY] =
			    block.list_type == DuckBlockTypes::LIST_TYPE_ORDERED ? "true" : "false";
		}
		// Reader-specific keys LAST, and only where they do not already exist, so a stray
		// key cannot displace a derived one.
		for (auto &kv : block.attributes) {
			row.attributes.emplace(kv.first, kv.second);
		}
		// EVERY ELEMENT CARRIES A STRUCTURAL LEVEL. Top level is 1; an inline is a CHILD of
		// its block, so it is one deeper. This reader emits CONTAINERS now -- lists and
		// blockquotes -- so the level comes from the block rather than being fixed at 1.
		const int32_t block_level = block.level > 0 ? block.level : 1;
		row.has_level = true;
		row.level = block_level;
		result->rows.push_back(std::move(row));

		for (auto &inl : block.inlines) {
			OdtRow child;
			child.kind = DuckBlockTypes::KIND_INLINE;
			child.element_type = inl.element_type;
			child.content = inl.content;
			child.attributes = inl.attributes;
			child.has_level = true;
			child.level = block_level + 1;
			child.element_order = order++;
			result->rows.push_back(std::move(child));
		}
	}
	return std::move(result);
}

void OdtScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<OdtBindData>();
	auto &state = input.global_state->Cast<OdtGlobalState>();
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

void RegisterOdtReaderFunction(ExtensionLoader &loader) {
	TableFunction fn("read_odt_blocks", {LogicalType::VARCHAR}, OdtScan, OdtBind, OdtGlobalState::Init);
	loader.RegisterFunction(fn);
}

} // namespace duckdb
