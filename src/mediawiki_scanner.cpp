#include "mediawiki_scanner.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace duckdb {
namespace mediawiki {

namespace {

std::string Trim(const std::string &s) {
	size_t b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos) {
		return {};
	}
	size_t e = s.find_last_not_of(" \t\r\n");
	return s.substr(b, e - b + 1);
}

std::string TrimRight(const std::string &s) {
	size_t e = s.find_last_not_of(" \t\r\n");
	return e == std::string::npos ? std::string() : s.substr(0, e + 1);
}

bool StartsWith(const std::string &s, const char *p) {
	return s.compare(0, strlen(p), p) == 0;
}

//! Split source into lines, dropping a trailing CR so CRLF files scan identically.
std::vector<std::string> SplitLines(const std::string &src) {
	std::vector<std::string> out;
	size_t start = 0;
	while (start <= src.size()) {
		size_t nl = src.find('\n', start);
		if (nl == std::string::npos) {
			out.push_back(src.substr(start));
			break;
		}
		out.push_back(src.substr(start, nl - start));
		start = nl + 1;
	}
	for (auto &l : out) {
		if (!l.empty() && l.back() == '\r') {
			l.pop_back();
		}
	}
	return out;
}

//! Net change in `{{` nesting across one line. Counts pairs, so `}}}}` closes two.
int BraceDelta(const std::string &s) {
	int delta = 0;
	for (size_t i = 0; i + 1 < s.size(); i++) {
		if (s[i] == '{' && s[i + 1] == '{') {
			delta++;
			i++;
		} else if (s[i] == '}' && s[i + 1] == '}') {
			delta--;
			i++;
		}
	}
	return delta;
}

//! A template's name is everything up to the first `|` or the closing braces, trimmed.
//! `{{Infobox person\n| name = X}}` names "Infobox person".
std::string TemplateName(const std::string &raw) {
	if (raw.size() < 3) {
		return {};
	}
	size_t start = 2; // past "{{"
	size_t end = raw.size();
	for (size_t i = start; i + 1 < raw.size(); i++) {
		if (raw[i] == '|' || (raw[i] == '}' && raw[i + 1] == '}')) {
			end = i;
			break;
		}
	}
	return Trim(raw.substr(start, end - start));
}

Line Make(LineKind kind, std::string text = {}) {
	Line ln;
	ln.kind = kind;
	ln.text = std::move(text);
	return ln;
}

//! Block-level HTML wikitext allows. Deliberately a CLOSED LIST rather than "anything in
//! angle brackets": `<` is common in prose, and treating every tag-looking line as a block
//! would swallow text. Anything not here stays in the paragraph and reaches the inline pass.
const char *const kHtmlBlocks[] = {"blockquote", "syntaxhighlight", "source", "pre",  "references",
                                   "div",        "center",          "poem",   nullptr};

//! `__TOC__` and friends: a run of A-Z and _ bracketed by double underscores.
bool IsBehaviorSwitch(const std::string &t) {
	if (t.size() < 5 || !StartsWith(t, "__") || t.compare(t.size() - 2, 2, "__") != 0) {
		return false;
	}
	for (size_t i = 2; i + 2 < t.size(); i++) {
		if (!std::isupper(static_cast<unsigned char>(t[i])) && t[i] != '_') {
			return false;
		}
	}
	return true;
}

} // namespace

std::vector<std::string> SplitCells(const std::string &s, char sep) {
	std::vector<std::string> out;
	size_t start = 0;
	for (size_t i = 0; i + 1 < s.size(); i++) {
		if (s[i] == sep && s[i + 1] == sep) {
			out.push_back(Trim(s.substr(start, i - start)));
			i++;
			start = i + 1;
		}
	}
	out.push_back(Trim(s.substr(start)));
	return out;
}

std::vector<Line> ScanMediaWiki(const std::string &src) {
	std::vector<Line> out;
	auto lines = SplitLines(src);
	bool in_table = false;

	for (size_t i = 0; i < lines.size(); i++) {
		const std::string &raw = lines[i];
		std::string t = Trim(raw);

		if (t.empty()) {
			out.push_back(Make(LineKind::BLANK));
			continue;
		}

		// A TEMPLATE CALL OPENING A LINE is consumed whole, across as many source lines as
		// its braces need. Doing this here rather than in the reader is what keeps a `|`
		// inside a template from ever reaching the table branches below.
		if (StartsWith(t, "{{")) {
			std::string acc = raw;
			int depth = BraceDelta(raw);
			while (depth > 0 && i + 1 < lines.size()) {
				i++;
				acc += "\n" + lines[i];
				depth += BraceDelta(lines[i]);
			}
			// An UNTERMINATED template runs to end of input and is still emitted. Wikitext
			// has no error state -- MediaWiki renders what it can -- and dropping the text
			// would lose content that is plainly in the file.
			Line ln;
			ln.kind = LineKind::TEMPLATE;
			ln.text = TrimRight(acc);
			ln.name = TemplateName(Trim(acc));
			out.push_back(ln);
			continue;
		}

		if (in_table) {
			if (StartsWith(t, "|}")) {
				in_table = false;
				out.push_back(Make(LineKind::TABLE_END));
				continue;
			}
			if (StartsWith(t, "|-")) {
				out.push_back(Make(LineKind::TABLE_ROW));
				continue;
			}
			if (!t.empty() && t[0] == '!') {
				out.push_back(Make(LineKind::TABLE_HEADER, Trim(t.substr(1))));
				continue;
			}
			if (!t.empty() && t[0] == '|') {
				out.push_back(Make(LineKind::TABLE_CELL, Trim(t.substr(1))));
				continue;
			}
			// A bare line inside a table is a continuation of the cell above it. Emitting it
			// as TEXT would break it out of the table entirely.
			out.push_back(Make(LineKind::TEXT, t));
			continue;
		}

		if (StartsWith(t, "{|")) {
			in_table = true;
			out.push_back(Make(LineKind::TABLE_START));
			continue;
		}

		// `== Heading ==`. The trailing run need not match the leading one -- MediaWiki
		// takes the SHORTER of the two, and so does pandoc.
		if (t[0] == '=' && t.size() >= 3 && t.back() == '=') {
			size_t lead = 0;
			while (lead < t.size() && t[lead] == '=') {
				lead++;
			}
			size_t trail = 0;
			while (trail < t.size() - lead && t[t.size() - 1 - trail] == '=') {
				trail++;
			}
			size_t n = std::min(lead, trail);
			if (n >= 1 && t.size() > 2 * n) {
				Line ln;
				ln.kind = LineKind::HEADING;
				ln.level = static_cast<int>(std::min<size_t>(n, 6));
				ln.text = Trim(t.substr(n, t.size() - 2 * n));
				out.push_back(ln);
				continue;
			}
		}

		if (t[0] == '*' || t[0] == '#' || t[0] == ';' || t[0] == ':') {
			size_t n = 0;
			while (n < t.size() && (t[n] == '*' || t[n] == '#' || t[n] == ';' || t[n] == ':')) {
				n++;
			}
			Line ln;
			ln.kind = LineKind::LIST_ITEM;
			ln.markers = t.substr(0, n);
			ln.level = static_cast<int>(n);
			ln.text = Trim(t.substr(n));
			out.push_back(ln);
			continue;
		}

		// FOUR or more dashes, per MediaWiki -- not three. A three-dash line is text.
		if (StartsWith(t, "----") && t.find_first_not_of('-') == std::string::npos) {
			out.push_back(Make(LineKind::HRULE));
			continue;
		}

		if (IsBehaviorSwitch(t)) {
			Line bh = Make(LineKind::BEHAVIOR, t);
			bh.name = t;
			out.push_back(bh);
			continue;
		}

		if (StartsWith(t, "<!--") && t.find("-->") != std::string::npos) {
			out.push_back(Make(LineKind::COMMENT));
			continue;
		}

		// BLOCK-LEVEL HTML, consumed whole across as many source lines as its closing tag
		// needs. Without this the tags leak into a paragraph as literal text -- which is the
		// "emitting non-content as prose" failure the Org reader already paid for once.
		if (t[0] == '<' && t.size() > 2) {
			size_t np = 1;
			while (np < t.size() && (std::isalnum(static_cast<unsigned char>(t[np])) || t[np] == '_')) {
				np++;
			}
			std::string tag;
			for (size_t k = 1; k < np; k++) {
				tag += static_cast<char>(std::tolower(static_cast<unsigned char>(t[k])));
			}
			bool known = false;
			for (const char *const *b = kHtmlBlocks; *b && !known; b++) {
				known = (tag == *b);
			}
			if (known) {
				size_t gt = t.find('>');
				if (gt != std::string::npos) {
					Line ln = Make(LineKind::HTML_BLOCK);
					ln.name = tag;
					ln.attrs = Trim(t.substr(np, gt - np));
					if (!ln.attrs.empty() && ln.attrs.back() == '/') {
						ln.attrs.pop_back(); // `<references/>`
					}
					const std::string close = "</" + tag + ">";
					// Self-closing, or a void tag with no body in this document.
					if (t[gt - 1] == '/') {
						out.push_back(ln);
						continue;
					}
					std::string acc = t.substr(gt + 1);
					size_t cpos = acc.find(close);
					while (cpos == std::string::npos && i + 1 < lines.size()) {
						i++;
						acc += "\n" + lines[i];
						cpos = acc.find(close);
					}
					ln.text = cpos == std::string::npos ? acc : acc.substr(0, cpos);
					// Leading and trailing newlines are the tag's own layout, not content.
					while (!ln.text.empty() && (ln.text.front() == '\n' || ln.text.front() == '\r')) {
						ln.text.erase(0, 1);
					}
					ln.text = TrimRight(ln.text);
					out.push_back(ln);
					continue;
				}
			}
		}

		// A LEADING SPACE IS PREFORMATTED, and MediaWiki's own parser renders it <pre> --
		// measured 2026-09-02 against maintenance/parse.php, not inferred. Tested on the RAW
		// line, since trimming is exactly what destroys the signal.
		if (!raw.empty() && (raw[0] == ' ' || raw[0] == '\t')) {
			out.push_back(Make(LineKind::PREFORMATTED, TrimRight(raw).substr(1)));
			continue;
		}

		// A LINE THAT IS NOTHING BUT HTML TAGS is raw HTML, not prose.
		//
		// The pandoc-generated fixture is what forced this: `pandoc -t mediawiki` writes
		// `<span id="introduction"></span>` before every heading, and without this the tags
		// arrived as a paragraph whose text was the literal markup. A hand-written fixture
		// contains no such line, so only the second witness could have found it.
		//
		// The rule is deliberately about SHAPE rather than a tag list: any line whose only
		// content is tags. Adding `span` to the block-element list would be wrong -- span is
		// an inline element, and it is a block here only because it is alone on a line.
		if (t[0] == '<' && t.back() == '>') {
			std::string outside;
			bool in_tag = false;
			for (char c : t) {
				if (c == '<') {
					in_tag = true;
				} else if (c == '>') {
					in_tag = false;
				} else if (!in_tag && !std::isspace(static_cast<unsigned char>(c))) {
					outside += c;
				}
			}
			if (outside.empty()) {
				size_t np = 1;
				while (np < t.size() && (std::isalnum(static_cast<unsigned char>(t[np])) || t[np] == '_')) {
					np++;
				}
				std::string tag;
				for (size_t k = 1; k < np; k++) {
					tag += static_cast<char>(std::tolower(static_cast<unsigned char>(t[k])));
				}
				// TAGS THE INLINE PASS OWNS ARE NOT BLOCKS, even alone on a line. `<ref
				// name="a"/>` is a self-closing reference reuse and therefore a line of pure
				// markup by this rule's test -- but it is a footnote, and pandoc keeps it as
				// an inline Note inside a paragraph. Without this the shape rule silently
				// converted every bare reference reuse into raw HTML.
				bool inline_owned = tag == "ref" || tag == "nowiki" || tag == "code" || tag == "math";
				if (!inline_owned) {
					Line ln = Make(LineKind::HTML_BLOCK, t);
					ln.verbatim = true;
					ln.name = tag;
					out.push_back(ln);
					continue;
				}
			}
		}

		// A line carrying an INLINE template that does not close on this line takes its
		// continuations with it, so the paragraph stays whole and the inline pass sees a
		// balanced call.
		std::string acc = t;
		int depth = BraceDelta(t);
		while (depth > 0 && i + 1 < lines.size()) {
			i++;
			acc += "\n" + Trim(lines[i]);
			depth += BraceDelta(lines[i]);
		}
		out.push_back(Make(LineKind::TEXT, acc));
	}
	return out;
}

} // namespace mediawiki
} // namespace duckdb
