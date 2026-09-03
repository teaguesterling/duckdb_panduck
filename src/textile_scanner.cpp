#include "textile_scanner.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace duckdb {
namespace textile {

namespace {

std::string Trim(const std::string &s) {
	size_t b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos) {
		return {};
	}
	size_t e = s.find_last_not_of(" \t\r\n");
	return s.substr(b, e - b + 1);
}

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

//! Parse a block marker head: a name, optional attribute groups, then a `.`.
//!
//! `p.`, `h2.`, `bq.`, `bc.`, `notextile.` -- and each may carry `{style}`, `(class)` or
//! `[lang]` between the name and the dot, in any order. This is the one place textile is
//! not a fixed-prefix format, which is why the marker is parsed rather than matched.
//!
//! Returns false when the line does not open with a marker. `body_start` is then untouched.
bool ParseMarker(const std::string &t, std::string &name, std::string &style, std::string &css_class, std::string &id,
                 size_t &body_start) {
	size_t i = 0;
	while (i < t.size() && (std::isalnum(static_cast<unsigned char>(t[i])))) {
		i++;
	}
	if (i == 0) {
		return false;
	}
	name = t.substr(0, i);

	while (i < t.size() && (t[i] == '{' || t[i] == '(' || t[i] == '[')) {
		char close = t[i] == '{' ? '}' : (t[i] == '(' ? ')' : ']');
		size_t end = t.find(close, i + 1);
		if (end == std::string::npos) {
			return false;
		}
		std::string value = t.substr(i + 1, end - i - 1);
		if (t[i] == '{') {
			style = value;
		} else if (t[i] == '(') {
			// `(...)` HOLDS A CLASS, AN #ID, OR BOTH -- `(cls)`, `(#id)`, `(cls#id)`.
			//
			// Found by the pandoc-generated fixture, which writes `h1(#guide-title).` for
			// every heading. Treating the whole group as a class put the string "#guide-title"
			// into attributes['class'] -- and the id survived only because the slugifier
			// happened to derive the same value from the heading text. An author's explicit
			// id, differing from its slug, was silently lost.
			size_t hash = value.find('#');
			if (hash == std::string::npos) {
				css_class = value;
			} else {
				css_class = value.substr(0, hash);
				id = value.substr(hash + 1);
			}
		}
		i = end + 1;
	}
	// Alignment modifiers sit here in real textile (`p<.`, `p>.`, `p=.`, `p<>.`). Consumed
	// so they do not defeat the marker; they are out of scope rather than dropped silently.
	while (i < t.size() && (t[i] == '<' || t[i] == '>' || t[i] == '=')) {
		i++;
	}
	if (i >= t.size() || t[i] != '.') {
		return false;
	}
	i++;
	// `bc..` and friends are EXTENDED blocks, running until the next marker rather than the
	// next blank line. Consuming the second dot keeps the body correct; the extended
	// semantics are out of scope and recorded as such.
	if (i < t.size() && t[i] == '.') {
		i++;
	}
	if (i < t.size() && t[i] != ' ') {
		return false; // `p.x` is not a marker -- a marker's dot is followed by space or EOL
	}
	body_start = i < t.size() ? i + 1 : i;
	return true;
}

} // namespace

std::vector<std::string> SplitRow(const std::string &s) {
	std::vector<std::string> out;
	std::string body = Trim(s);
	if (!body.empty() && body.front() == '|') {
		body.erase(0, 1);
	}
	if (!body.empty() && body.back() == '|') {
		body.pop_back();
	}
	size_t start = 0;
	for (size_t i = 0; i <= body.size(); i++) {
		if (i == body.size() || body[i] == '|') {
			out.push_back(Trim(body.substr(start, i - start)));
			start = i + 1;
		}
	}
	return out;
}

std::vector<Line> ScanTextile(const std::string &src) {
	std::vector<Line> out;
	auto lines = SplitLines(src);
	for (size_t li = 0; li < lines.size(); li++) {
		const std::string &raw = lines[li];
		std::string t = Trim(raw);
		Line ln;

		if (t.empty()) {
			ln.kind = LineKind::BLANK;
			out.push_back(ln);
			continue;
		}

		// A TABLE ROW is checked before the marker parse: `|` is not alphanumeric, so
		// ParseMarker would reject it anyway, but stating the order keeps the intent clear.
		if (t.front() == '|') {
			ln.kind = LineKind::TABLE_ROW;
			ln.text = t;
			out.push_back(ln);
			continue;
		}

		// `###.` is a comment. Checked before the marker parse because `#` also opens an
		// ordered list item, and a comment marker is not alphanumeric so ParseMarker cannot
		// see it.
		if (t.rfind("###.", 0) == 0) {
			ln.kind = LineKind::COMMENT;
			out.push_back(ln);
			continue;
		}

		if (t.front() == '*' || t.front() == '#') {
			size_t n = 0;
			while (n < t.size() && (t[n] == '*' || t[n] == '#')) {
				n++;
			}
			// A run must be followed by a space to be a list. `*strong*` at line start is
			// emphasis, and this is the same column-versus-content distinction Org's leading
			// `*` forced -- one character, two meanings, told apart by what follows it.
			if (n < t.size() && t[n] == ' ') {
				ln.kind = LineKind::LIST_ITEM;
				ln.markers = t.substr(0, n);
				ln.level = static_cast<int>(n);
				ln.text = Trim(t.substr(n));
				out.push_back(ln);
				continue;
			}
		}

		// `- term := definition`
		if (t.rfind("- ", 0) == 0) {
			size_t sep = t.find(" := ");
			if (sep != std::string::npos) {
				ln.kind = LineKind::LIST_ITEM;
				ln.markers = "-";
				ln.level = 1;
				ln.definition = true;
				ln.term = Trim(t.substr(2, sep - 2));
				ln.text = Trim(t.substr(sep + 4));
				out.push_back(ln);
				continue;
			}
		}

		// BLOCK-LEVEL HTML. pandoc's textile WRITER emits `<dl>` for a definition list,
		// because textile's `- term := def` is not in its writer -- so a pandoc-generated
		// document contains raw HTML that a hand-written one never does. Without this the
		// tags arrive as a paragraph whose text is the literal markup, which is the
		// leaked-as-prose failure three earlier readers each paid for.
		if (t.front() == '<' && t.size() > 2) {
			size_t np = 1;
			while (np < t.size() && std::isalnum(static_cast<unsigned char>(t[np]))) {
				np++;
			}
			std::string tag;
			for (size_t k = 1; k < np; k++) {
				tag += static_cast<char>(std::tolower(static_cast<unsigned char>(t[k])));
			}
			static const char *const kBlocks[] = {"dl", "table", "div", "ul", "ol", "blockquote", "pre", "p", nullptr};
			bool known = false;
			for (const char *const *b = kBlocks; *b && !known; b++) {
				known = (tag == *b);
			}
			if (known) {
				// CONSUMED WHOLE, to the matching closing tag. Matching only the OPENING line
				// left `<dt>term</dt>` and `</dl>` behind as ordinary text -- so the element's
				// first line became raw HTML and its body still leaked into a paragraph, which
				// is worse than not handling it at all: half the markup hidden and half of it
				// on show.
				std::string acc = t;
				const std::string close = "</" + tag + ">";
				while (acc.find(close) == std::string::npos && li + 1 < lines.size()) {
					li++;
					acc += "\n" + Trim(lines[li]);
				}
				ln.kind = LineKind::HTML_BLOCK;
				ln.text = acc;
				ln.markers = tag;
				out.push_back(ln);
				continue;
			}
		}

		std::string name, style, css_class, id;
		size_t body = 0;
		if (ParseMarker(t, name, style, css_class, id, body)) {
			ln.style = style;
			ln.css_class = css_class;
			ln.id = id;
			ln.text = body <= t.size() ? Trim(t.substr(body)) : std::string();
			if (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6') {
				ln.kind = LineKind::HEADING;
				ln.level = name[1] - '0';
				out.push_back(ln);
				continue;
			}
			if (name == "p") {
				ln.kind = LineKind::PARA;
				out.push_back(ln);
				continue;
			}
			if (name == "bq") {
				ln.kind = LineKind::BLOCKQUOTE;
				out.push_back(ln);
				continue;
			}
			if (name == "pre" || name == "bc") {
				ln.kind = LineKind::CODE;
				out.push_back(ln);
				continue;
			}
			if (name == "notextile") {
				ln.kind = LineKind::NOTEXTILE;
				out.push_back(ln);
				continue;
			}
			// A marker-shaped head that names no block -- `foo.` -- is prose. Falling
			// through rather than inventing a block type is what keeps an unknown marker
			// from becoming a silent container.
		}

		ln.kind = LineKind::TEXT;
		ln.text = t;
		out.push_back(ln);
	}
	return out;
}

} // namespace textile
} // namespace duckdb
