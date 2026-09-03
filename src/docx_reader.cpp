#include "doc_metadata.hpp"
#include "docx_reader.hpp"

#include "block_json.hpp"

#include "duck_block_types.hpp"
#include "zip_container.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <cctype>
#include <functional>
#include <map>
#include <pugixml.hpp>

namespace duckdb {
namespace docx {

namespace {

//! styleId -> style name, from word/styles.xml. Word writes localized names ("Overskrift
//! 1"), so the id is checked too; between them the common writers are covered.
std::map<std::string, std::string> ParseStyleNames(const std::string &xml) {
	std::map<std::string, std::string> names;
	pugi::xml_document doc;
	if (!doc.load_buffer(xml.data(), xml.size())) {
		return names;
	}
	for (auto style : doc.child("w:styles").children("w:style")) {
		std::string id = style.attribute("w:styleId").value();
		std::string name = style.child("w:name").attribute("w:val").value();
		if (!id.empty()) {
			names[id] = name;
		}
	}
	return names;
}

//! Heading level from a "Heading 3"-ish string, or 0. Accepts "heading 3", "Heading3"
//! and "heading_20_3" -- the spellings the common writers actually emit.
int HeadingLevelFromName(const std::string &raw) {
	std::string s;
	for (char c : raw) {
		s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
	}
	auto pos = s.find(DuckBlockTypes::TYPE_HEADING);
	if (pos == std::string::npos) {
		return 0;
	}
	for (size_t i = pos + 7; i < s.size(); i++) {
		if (std::isdigit(static_cast<unsigned char>(s[i]))) {
			int level = s[i] - '0';
			return (level >= 1 && level <= 6) ? level : 0;
		}
		if (s[i] != ' ' && s[i] != '_' && !std::isdigit(static_cast<unsigned char>(s[i]))) {
			// "Heading" with no number, or "HeadingChar" -- not a numbered heading.
			if (!std::isalpha(static_cast<unsigned char>(s[i]))) {
				continue;
			}
			return 0;
		}
	}
	return 0;
}

struct RunFormat {
	bool bold = false, italic = false, underline = false, strike = false;
	bool code = false, superscript = false, subscript = false;

	bool Plain() const {
		return !bold && !italic && !underline && !strike && !code && !superscript && !subscript;
	}
	//! duck_block's inline vocabulary is flat, so a run carrying several attributes is
	//! reported by its strongest. Documented limitation, matching the RTF reader.
	std::string ElementType() const {
		// Code outranks the toggles: pandoc marks a verbatim run with the VerbatimChar
		// character style, and the theme behind it may also set a face. The verbatim-ness
		// is the property that carries meaning.
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

//! A w:rPr toggle is on unless it explicitly says otherwise: <w:b/> means bold, and
//! <w:b w:val="0"/> means not. Treating presence alone as true would make every
//! explicitly-disabled run bold.
bool ToggleOn(const pugi::xml_node &rpr, const char *tag) {
	auto node = rpr.child(tag);
	if (!node) {
		return false;
	}
	std::string val = node.attribute("w:val").value();
	return val.empty() || (val != "0" && val != "false" && val != "none");
}

int ParagraphHeadingLevel(const pugi::xml_node &ppr, const std::map<std::string, std::string> &styles) {
	if (!ppr) {
		return 0;
	}
	// Mechanism 1: an explicit outline level. LibreOffice writes this and no pStyle.
	auto outline = ppr.child("w:outlineLvl");
	if (outline) {
		std::string val = outline.attribute("w:val").value();
		if (!val.empty()) {
			int level = std::atoi(val.c_str()) + 1;
			if (level >= 1 && level <= 6) {
				return level;
			}
		}
	}
	// Mechanism 2: a paragraph style named "Heading N". pandoc writes this and no
	// outlineLvl. Resolve through styles.xml, falling back to the id itself.
	auto pstyle = ppr.child("w:pStyle");
	if (pstyle) {
		std::string id = pstyle.attribute("w:val").value();
		auto it = styles.find(id);
		if (it != styles.end()) {
			int level = HeadingLevelFromName(it->second);
			if (level > 0) {
				return level;
			}
		}
		return HeadingLevelFromName(id);
	}
	return 0;
}

} // namespace

//! docProps/core.xml as `value` elements, appended after the blocks.
//!
//! Every field here EXCEEDS pandoc, which extracts nothing from DOCX -- see
//! doc_metadata.hpp. Each carries attributes['source_type'] with its original spelling so
//! a consumer can tell format-derived metadata from pandoc-derived, which is the condition
//! the approval to exceed came with.
void CollectDocxMetadata(const std::string &core_xml, std::vector<DocxBlock> &out) {
	if (core_xml.empty()) {
		return;
	}
	pugi::xml_document doc;
	if (!doc.load_buffer(core_xml.data(), core_xml.size())) {
		// A malformed metadata part must not fail the document. The body is what the
		// caller asked for; metadata is enrichment, and losing it is a smaller harm than
		// refusing a file whose prose reads perfectly.
		return;
	}
	auto props = doc.child("cp:coreProperties");
	if (!props) {
		return;
	}
	std::map<std::string, std::pair<std::string, std::string>> found; // key -> {text, source}
	for (auto &field : DOCX_CORE_FIELDS) {
		auto node = props.child(field.source);
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
	for (auto &kv : found) {
		DocxBlock block;
		block.kind = DuckBlockTypes::KIND_VALUE;
		block.element_type = DuckBlockTypes::VALUE_INLINES;
		block.key = kv.first;
		block.source_type = kv.second.second;
		if (!kv.second.first.empty()) { // always true here; see the skip above
			DocxInline run;
			run.element_type = DuckBlockTypes::INLINE_TEXT;
			run.content = kv.second.first;
			block.inlines.push_back(std::move(run));
		}
		out.push_back(std::move(block));
	}
}

std::vector<DocxBlock> ParseDocxFile(const std::string &path) {
	ZipContainer zip(path, "read_docx_blocks");
	auto document_xml = zip.ReadRequired("word/document.xml");
	// OPTIONAL. A document with no lists has no numbering part, and -- measured on
	// test/fixtures/libreoffice_outlinelvl.docx -- a document CAN carry w:numPr and still
	// have none, because numId 0 means "no numbering" rather than "numbering zero". Pandoc
	// makes those paragraphs, and so does this reader.
	std::string numbering_xml;
	zip.Read("word/numbering.xml", numbering_xml);
	// An image's <a:blip r:embed="rIdN"> names a RELATIONSHIP, not a file; the target lives
	// in the rels part. A footnote's body lives in its own part too, keyed by w:id.
	std::string rels_xml, footnotes_xml;
	zip.Read("word/_rels/document.xml.rels", rels_xml);
	zip.Read("word/footnotes.xml", footnotes_xml);
	std::string styles_xml;
	zip.Read("word/styles.xml", styles_xml); // optional: a minimal DOCX may omit it

	auto styles = ParseStyleNames(styles_xml);

	// OPTIONAL: a DOCX need not carry docProps/core.xml, and a missing part is not an
	// error -- it is a document that declared no metadata.
	std::string core_xml;
	zip.Read("docProps/core.xml", core_xml);

	pugi::xml_document doc;
	// parse_ws_pcdata is REQUIRED, not a tuning knob. pandoc emits inter-word spacing as
	// separate runs whose only content is a space: <w:t xml:space="preserve"> </w:t>.
	// pugixml's default flags discard whitespace-only text nodes, so those runs came back
	// empty and were skipped, welding "with" and "bold" into "withbold". The document says
	// xml:space="preserve"; honouring that is the reader's job.
	auto parsed =
	    doc.load_buffer(document_xml.data(), document_xml.size(), pugi::parse_default | pugi::parse_ws_pcdata);
	if (!parsed) {
		throw InvalidInputException("read_docx_blocks: word/document.xml is not well-formed XML in %s: %s", path,
		                            parsed.description());
	}

	// numId -> per-ilvl orderedness, resolved through abstractNumId. A numId that does not
	// resolve here is NOT a list -- see the numbering_xml comment above.
	std::map<int, std::map<int, bool>> num_ordered;
	pugi::xml_document num_doc;
	if (!numbering_xml.empty() &&
	    num_doc.load_buffer(numbering_xml.data(), numbering_xml.size(), pugi::parse_default)) {
		auto num_root = num_doc.child("w:numbering");
		std::map<int, std::map<int, bool>> abstract;
		for (auto an : num_root.children("w:abstractNum")) {
			int aid = an.attribute("w:abstractNumId").as_int(-1);
			for (auto lvl : an.children("w:lvl")) {
				int ilvl = lvl.attribute("w:ilvl").as_int(0);
				std::string fmt = lvl.child("w:numFmt").attribute("w:val").value();
				// Everything that is not a bullet is a numbering scheme -- decimal, lowerRoman,
				// upperLetter and the rest -- so the test is against `bullet` rather than for a
				// list of ordered spellings that would need extending per format.
				abstract[aid][ilvl] = (fmt != "bullet" && !fmt.empty());
			}
		}
		for (auto n : num_root.children("w:num")) {
			int nid = n.attribute("w:numId").as_int(-1);
			int aid = n.child("w:abstractNumId").attribute("w:val").as_int(-1);
			auto it = abstract.find(aid);
			if (nid > 0 && it != abstract.end()) {
				num_ordered[nid] = it->second;
			}
		}
	}

	// A w:tbl is a SIBLING of w:p, and this loop used to iterate `body.children("w:p")` --
	// so a table was not flattened, it was NEVER SEEN. Every cell's text vanished with it,
	// which is content loss rather than a structure gap. Measured against pandoc, which
	// reports Table for the same file.
	auto para_text = [](const pugi::xml_node &p) {
		std::string out;
		for (auto t : p.select_nodes(".//w:t")) {
			out += t.node().text().get();
		}
		return out;
	};

	// rId -> target path, so an image can report the file it actually points at rather than
	// an opaque relationship id no consumer can resolve.
	std::map<std::string, std::string> rels;
	pugi::xml_document rels_doc;
	if (!rels_xml.empty() && rels_doc.load_buffer(rels_xml.data(), rels_xml.size())) {
		for (auto rel : rels_doc.child("Relationships").children("Relationship")) {
			rels[rel.attribute("Id").value()] = rel.attribute("Target").value();
		}
	}

	// footnote id -> flattened text.
	std::map<std::string, std::string> footnotes;
	pugi::xml_document fn_doc;
	if (!footnotes_xml.empty() &&
	    fn_doc.load_buffer(footnotes_xml.data(), footnotes_xml.size(), pugi::parse_default | pugi::parse_ws_pcdata)) {
		for (auto fn : fn_doc.child("w:footnotes").children("w:footnote")) {
			// SEPARATOR AND CONTINUATION footnotes are not content: Word stores the rule drawn
			// above a footnote block as footnotes with w:type. Reading them would put a stray
			// empty note in every document that has any footnote at all.
			std::string type = fn.attribute("w:type").value();
			if (!type.empty() && type != "normal") {
				continue;
			}
			std::string text;
			for (auto t : fn.select_nodes(".//w:t")) {
				text += t.node().text().get();
			}
			size_t b = text.find_first_not_of(" \t\n");
			if (b != std::string::npos) {
				size_t e = text.find_last_not_of(" \t\n");
				footnotes[fn.attribute("w:id").value()] = text.substr(b, e - b + 1);
			}
		}
	}

	std::vector<DocxBlock> blocks;
	auto body = doc.child("w:document").child("w:body");
	// The TYPE of each currently-open list, not merely how many are open. A bullet list
	// following an ordered one at the SAME depth is a different list, and comparing depth
	// alone silently swallows the second into the first -- measured on a generated fixture,
	// where `- plain bullet` after `1. 2. 3.` became a fourth ordered item.
	std::vector<bool> open_ordered;

	// <w:sdt> IS A CONTENT CONTROL, and it WRAPS block content in <w:sdtContent>. Word
	// emits them for form fields, citations, tables of contents and any structured region,
	// so they are ordinary in real documents rather than exotic. A paragraph inside one is
	// not a child of the body, and this loop skipped it outright: the whole paragraph, text
	// and all, never reached the output.
	//
	// Same class as the <w:hyperlink> bug -- a wrapper between the container and the thing
	// being looked for. Flattened here so every later stage sees a plain block sequence.
	std::vector<pugi::xml_node> body_nodes;
	std::function<void(pugi::xml_node)> flatten_blocks = [&](pugi::xml_node parent) {
		for (auto n : parent.children()) {
			if (std::string(n.name()) == "w:sdt") {
				flatten_blocks(n.child("w:sdtContent"));
			} else {
				body_nodes.push_back(n);
			}
		}
	};
	flatten_blocks(body);

	for (auto node : body_nodes) {
		std::string node_name = node.name();
		if (node_name == "w:tbl") {
			// A table ends any open list -- OOXML permits the nesting and no consumer wants
			// a table as a list item.
			open_ordered.clear();
			std::vector<std::string> headers;
			std::vector<std::vector<std::string>> rows;
			for (auto tr : node.children("w:tr")) {
				std::vector<std::string> cells;
				for (auto tc : tr.children("w:tc")) {
					std::string cell;
					for (auto p : tc.children("w:p")) {
						if (!cell.empty()) {
							cell += " ";
						}
						cell += para_text(p);
					}
					cells.push_back(cell);
				}
				// THE HEADER ROW is the one marked w:tblHeader, and only that. Treating the
				// first row as a header unconditionally invents one for every headerless
				// table -- pandoc reports an empty header for those, and so does this.
				bool is_header = tr.child("w:trPr").child("w:tblHeader") || tr.select_node(".//w:tblHeader").node();
				if (is_header && headers.empty()) {
					headers = cells;
				} else {
					rows.push_back(cells);
				}
			}
			DocxBlock t;
			t.element_type = DuckBlockTypes::TYPE_TABLE;
			t.level = 1;
			t.encoding = DuckBlockTypes::ENCODING_JSON;
			t.content = BuildTableJson(headers, rows);
			blocks.push_back(std::move(t));
			continue;
		}
		if (node_name != "w:p") {
			continue;
		}
		auto para = node;
		auto ppr = para.child("w:pPr");
		int level = ParagraphHeadingLevel(ppr, styles);

		// LIST MEMBERSHIP. A w:numPr whose numId resolves in numbering.xml; ilvl gives depth.
		int list_depth = 0;
		bool list_ordered = false;
		auto numpr = ppr.child("w:numPr");
		if (numpr && level == 0) {
			int nid = numpr.child("w:numId").attribute("w:val").as_int(0);
			int ilvl = numpr.child("w:ilvl").attribute("w:val").as_int(0);
			auto it = num_ordered.find(nid);
			if (nid > 0 && it != num_ordered.end()) {
				list_depth = ilvl + 1;
				auto lit = it->second.find(ilvl);
				list_ordered = lit != it->second.end() && lit->second;
			}
		}

		// BLOCKQUOTE. Either a quote paragraph style, or indentation with no numbering --
		// which is how LibreOffice marks one, measured: w:ind w:left="720" and pStyle
		// "Normal". 720 twentieths of a point is half an inch, Word's default quote indent.
		std::string pstyle_name = ppr.child("w:pStyle").attribute("w:val").value();
		bool quote_style = pstyle_name == "BlockText" || pstyle_name == "Quote" || pstyle_name == "IntenseQuote" ||
		                   pstyle_name == "BlockQuote";
		int ind_left = ppr.child("w:ind").attribute("w:left").as_int(0);
		bool is_quote = level == 0 && list_depth == 0 && (quote_style || ind_left >= 720);

		struct Run {
			RunFormat fmt;
			std::string text;
			//! Non-empty when this run is NOT a formatted text span -- an image or a
			//! footnote reference, which carry an element_type of their own rather than one
			//! derived from character formatting.
			std::string special_type;
			std::map<std::string, std::string> attrs;
		};
		std::vector<Run> runs;
		bool any_format = false;

		// A RUN IS NOT ALWAYS A DIRECT CHILD OF THE PARAGRAPH. <w:hyperlink> WRAPS its runs,
		// so iterating children("w:r") never visits them: measured on a pandoc-written DOCX,
		// "and a [link](https://example.com)." read back as "and a ." -- the anchor text gone
		// from the output entirely, not merely unmarked. That is DATA LOSS, which is a
		// different failure from ODT's, where the text survives and only the href is dropped.
		// pandoc marks a code listing with the SourceCode paragraph style. Captured before
		// the run walk because <w:br/> inside such a paragraph is a real newline rather
		// than the space it means in prose -- flattening it would run the listing's lines
		// together.
		bool is_code_para = pstyle_name == "SourceCode" || pstyle_name == "PreformattedText";

		auto process_run = [&](pugi::xml_node run, const std::string &link_href) {
			RunFormat fmt;
			auto rpr = run.child("w:rPr");
			if (rpr) {
				fmt.bold = ToggleOn(rpr, "w:b");
				fmt.italic = ToggleOn(rpr, "w:i");
				fmt.underline = static_cast<bool>(rpr.child("w:u"));
				fmt.strike = ToggleOn(rpr, "w:strike");
				// INLINE CODE is a character STYLE, not a toggle: pandoc writes
				// <w:rStyle w:val="VerbatimChar"/>. Without this a `verbatim` run read back
				// as ordinary text, indistinguishable from the prose around it.
				std::string rstyle = rpr.child("w:rStyle").attribute("w:val").value();
				fmt.code = rstyle == "VerbatimChar" || rstyle == "SourceText" || rstyle == "Code";
				// SUB/SUPERSCRIPT is a vertical alignment. H~2~O flattened to "H2O", which
				// is not wrong so much as no longer chemistry.
				std::string valign = rpr.child("w:vertAlign").attribute("w:val").value();
				fmt.superscript = valign == "superscript";
				fmt.subscript = valign == "subscript";
			}
			std::string text;
			for (auto child : run.children()) {
				std::string tag = child.name();
				if (tag == "w:t") {
					text += child.text().get();
				} else if (tag == "w:tab") {
					text += "\t";
				} else if (tag == "w:br" || tag == "w:cr") {
					text += is_code_para ? "\n" : " ";
				}
			}

			// AN IMAGE. <w:drawing> ... <a:blip r:embed="rIdN">, and rIdN is a RELATIONSHIP
			// rather than a path -- resolved through document.xml.rels so the emitted src is
			// a file a consumer can find, not an id only Word understands.
			auto blip = run.select_node(".//a:blip").node();
			if (blip) {
				std::string rid = blip.attribute("r:embed").value();
				if (rid.empty()) {
					rid = blip.attribute("r:link").value();
				}
				auto rit = rels.find(rid);
				Run img;
				img.special_type = DuckBlockTypes::INLINE_IMAGE;
				if (rit != rels.end()) {
					img.attrs["src"] = rit->second;
				}
				// An image forces the paragraph to emit INLINES rather than flatten, or the
				// image is dropped in favour of the surrounding text.
				any_format = true;
				runs.push_back(std::move(img));
			}

			// A FOOTNOTE REFERENCE. The body lives in footnotes.xml, keyed by w:id, so the
			// note carries its TEXT rather than a number the reader would have to resolve.
			auto fnref = run.child("w:footnoteReference");
			if (fnref) {
				std::string id = fnref.attribute("w:id").value();
				auto fit = footnotes.find(id);
				Run note;
				note.special_type = DuckBlockTypes::INLINE_NOTE;
				if (fit != footnotes.end()) {
					note.text = fit->second;
				}
				any_format = true;
				runs.push_back(std::move(note));
			}

			if (text.empty()) {
				return;
			}

			// A LINK carries its own element_type and href rather than being folded into the
			// surrounding text, so the URL survives into attributes where a consumer can use it.
			if (!link_href.empty()) {
				Run link;
				link.special_type = DuckBlockTypes::INLINE_LINK;
				link.attrs["href"] = link_href;
				link.text = text;
				any_format = true;
				runs.push_back(std::move(link));
				return;
			}
			if (!fmt.Plain()) {
				any_format = true;
			}
			if (!runs.empty() && runs.back().special_type.empty() &&
			    runs.back().fmt.ElementType() == fmt.ElementType()) {
				runs.back().text += text;
			} else {
				runs.push_back(Run {fmt, text});
			}
		};

		// A RUN IS REACHED THROUGH ANY NUMBER OF WRAPPERS, so this descends rather than
		// listing direct children. Probed against a hand-built document: <w:ins> lost the
		// inserted words ("before INSERTED after" read back as "before  after"),
		// <w:smartTag> and <w:fldSimple> lost theirs entirely. Tracked changes and fields
		// are not edge cases -- a reviewed document is full of the first and any
		// cross-reference or page number is the second.
		//
		// <w:del> is deliberately NOT transparent: it holds <w:delText>, text the author
		// REMOVED. Descending into it would resurrect deleted content into the document,
		// which is a worse failure than dropping it.
		std::function<void(pugi::xml_node, const std::string &)> walk_inline = [&](pugi::xml_node parent,
		                                                                           const std::string &href) {
			for (auto child : parent.children()) {
				std::string ctag = child.name();
				if (ctag == "w:r") {
					process_run(child, href);
				} else if (ctag == "w:hyperlink") {
					// r:id resolves through document.xml.rels, as an image's r:embed does.
					std::string link;
					auto hit = rels.find(child.attribute("r:id").value());
					if (hit != rels.end()) {
						link = hit->second;
					}
					if (link.empty()) {
						// w:anchor is an internal bookmark rather than a URL -- the
						// same-document target a cross-reference or TOC entry uses.
						std::string anchor = child.attribute("w:anchor").value();
						if (!anchor.empty()) {
							link = "#" + anchor;
						}
					}
					walk_inline(child, link);
				} else if (ctag == "w:ins" || ctag == "w:smartTag" || ctag == "w:fldSimple" || ctag == "w:sdt" ||
				           ctag == "w:sdtContent" || ctag == "w:bdo" || ctag == "w:dir") {
					// Transparent: these carry no text of their own, only runs.
					walk_inline(child, href);
				}
			}
		};
		walk_inline(para, "");

		std::string all;
		for (auto &r : runs) {
			all += r.text;
		}
		auto begin = all.find_first_not_of(" \t\n");
		if (begin == std::string::npos) {
			// A paragraph whose ONLY content is an image has no text at all, and skipping
			// whitespace-only paragraphs used to drop it. An image is content.
			bool has_special = false;
			for (auto &r : runs) {
				if (!r.special_type.empty()) {
					has_special = true;
				}
			}
			if (!has_special) {
				continue;
			}
			all.clear();
		}
		std::string trimmed;
		if (begin != std::string::npos) {
			auto end = all.find_last_not_of(" \t\n");
			trimmed = all.substr(begin, end - begin + 1);
		}

		// Open and close lists around the run of items, so `list` wraps its `list_item`s the
		// way every other panduck reader emits them.
		// A type change at the innermost depth closes that list so a new one opens.
		if (list_depth > 0 && open_ordered.size() == static_cast<size_t>(list_depth) &&
		    open_ordered.back() != list_ordered) {
			open_ordered.pop_back();
		}
		while (open_ordered.size() > static_cast<size_t>(list_depth)) {
			open_ordered.pop_back();
		}
		while (open_ordered.size() < static_cast<size_t>(list_depth)) {
			DocxBlock l;
			l.element_type = DuckBlockTypes::TYPE_LIST;
			l.level = 2 * static_cast<int>(open_ordered.size()) + 1;
			l.list_type = list_ordered ? DuckBlockTypes::LIST_TYPE_ORDERED : DuckBlockTypes::LIST_TYPE_BULLET;
			blocks.push_back(std::move(l));
			open_ordered.push_back(list_ordered);
		}

		// A CODE BLOCK. Consecutive SourceCode paragraphs are one listing: pandoc's DOCX
		// writer splits a fenced block across paragraphs, so emitting one block each would
		// report several listings where the document has one. `all` rather than `trimmed`,
		// because the leading spaces are the indentation.
		if (is_code_para && list_depth == 0) {
			if (!blocks.empty() && blocks.back().element_type == DuckBlockTypes::TYPE_CODE) {
				blocks.back().content += "\n" + all;
			} else {
				DocxBlock code;
				code.element_type = DuckBlockTypes::TYPE_CODE;
				code.content = all;
				blocks.push_back(std::move(code));
			}
			continue;
		}

		// A DEFINITION LIST. pandoc writes DefinitionTerm / Definition paragraph styles,
		// which read back as two unrelated paragraphs -- the words survived, the pairing
		// did not. Emitted in the list/list_item + `role` shape the other readers use.
		bool is_def_term = pstyle_name == "DefinitionTerm";
		bool is_def_body = pstyle_name == "Definition";
		if ((is_def_term || is_def_body) && list_depth == 0 && level == 0) {
			bool open = !blocks.empty() && blocks.back().element_type == DuckBlockTypes::TYPE_LIST_ITEM &&
			            blocks.back().attributes.count("role") > 0;
			if (!open) {
				DocxBlock l;
				l.element_type = DuckBlockTypes::TYPE_LIST;
				l.level = 1;
				blocks.push_back(std::move(l));
			}
			DocxBlock item;
			item.element_type = DuckBlockTypes::TYPE_LIST_ITEM;
			item.level = 2;
			item.content = trimmed;
			item.attributes["role"] = is_def_term ? "term" : "definition";
			blocks.push_back(std::move(item));
			continue;
		}

		DocxBlock block;
		if (level > 0) {
			block.element_type = DuckBlockTypes::TYPE_HEADING;
			block.heading_level = level;
		} else if (list_depth > 0) {
			block.element_type = DuckBlockTypes::TYPE_LIST_ITEM;
			block.level = 2 * list_depth;
		} else if (is_quote) {
			DocxBlock q;
			q.element_type = DuckBlockTypes::TYPE_BLOCKQUOTE;
			q.level = 1;
			blocks.push_back(std::move(q));
			block.element_type = DuckBlockTypes::TYPE_PARAGRAPH;
			block.level = 2;
		} else {
			block.element_type = DuckBlockTypes::TYPE_PARAGRAPH;
		}

		// Character formatting inside a heading is presentational -- writers bold heading
		// text as part of the heading style -- so headings always flatten. Leaving content
		// NULL would make a table of contents built from `content` come back empty.
		if (!any_format || level > 0) {
			block.content = trimmed;
		} else {
			for (auto &r : runs) {
				block.inlines.push_back(
				    DocxInline {r.special_type.empty() ? r.fmt.ElementType() : r.special_type, r.text, r.attrs});
			}
		}
		blocks.push_back(std::move(block));
	}
	// AFTER the blocks -- spec 6.2 makes body-then-metadata a contract.
	CollectDocxMetadata(core_xml, blocks);
	return blocks;
}

} // namespace docx

namespace {

struct DocxRow {
	std::string kind, element_type, content;
	std::string encoding = DuckBlockTypes::ENCODING_TEXT;
	bool has_level = false;
	int32_t level = 0;
	std::map<std::string, std::string> attributes;
	int32_t element_order = 0;
};

struct DocxBindData : public TableFunctionData {
	std::vector<DocxRow> rows;
};

struct DocxGlobalState : public GlobalTableFunctionState {
	idx_t offset = 0;
	static unique_ptr<GlobalTableFunctionState> Init(ClientContext &, TableFunctionInitInput &) {
		return make_uniq<DocxGlobalState>();
	}
};

unique_ptr<FunctionData> DocxBind(ClientContext &, TableFunctionBindInput &input, vector<LogicalType> &return_types,
                                  vector<string> &names) {
	// Column order mirrors the duck_block struct, so a row casts straight to duck_block
	// and read_panduck_doc's flat branch can SELECT * it through.
	names = {"kind", "element_type", "content", "level", "encoding", "attributes", "element_order"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::INTEGER,
	                LogicalType::VARCHAR, LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR),
	                LogicalType::INTEGER};

	auto result = make_uniq<DocxBindData>();
	int32_t order = 0;
	for (auto &block : docx::ParseDocxFile(input.inputs[0].GetValue<string>())) {
		DocxRow row;
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
			// Both spellings, as every panduck reader emits.
			row.attributes[DuckBlockTypes::ATTR_LIST_TYPE] = block.list_type;
			row.attributes[DuckBlockTypes::ATTR_ORDERED_LEGACY] =
			    block.list_type == DuckBlockTypes::LIST_TYPE_ORDERED ? "true" : "false";
		}
		// Reader-specific keys LAST, and only where absent, so one cannot displace a
		// derived entry.
		for (auto &kv : block.attributes) {
			row.attributes.emplace(kv.first, kv.second);
		}
		// EVERY ELEMENT CARRIES A STRUCTURAL LEVEL. Top level is 1; an inline is a CHILD
		// of its block, so it is one deeper. This reader emits no containers, so every
		// block sits at 1 and every inline at 2.
		const int32_t block_level = block.level > 0 ? block.level : 1;
		row.has_level = true;
		row.level = block_level;
		result->rows.push_back(std::move(row));

		for (auto &inl : block.inlines) {
			DocxRow child;
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

void DocxScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<DocxBindData>();
	auto &state = input.global_state->Cast<DocxGlobalState>();
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

void RegisterDocxReaderFunction(ExtensionLoader &loader) {
	TableFunction fn("read_docx_blocks", {LogicalType::VARCHAR}, DocxScan, DocxBind, DocxGlobalState::Init);
	loader.RegisterFunction(fn);
}

} // namespace duckdb
