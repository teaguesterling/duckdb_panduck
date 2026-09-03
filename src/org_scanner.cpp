#include "org_scanner.hpp"

#include <cctype>

namespace duckdb {
namespace org {
namespace {

bool IsSpace(char c) {
	return c == ' ' || c == '\t';
}

std::string TrimRight(const std::string &s) {
	size_t e = s.find_last_not_of(" \t\r");
	return e == std::string::npos ? std::string() : s.substr(0, e + 1);
}

std::string TrimBoth(const std::string &s) {
	size_t b = s.find_first_not_of(" \t\r");
	if (b == std::string::npos) {
		return std::string();
	}
	size_t e = s.find_last_not_of(" \t\r");
	return s.substr(b, e - b + 1);
}

std::string Upper(std::string s) {
	for (auto &c : s) {
		c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	}
	return s;
}

//! `-----` or longer, alone on a line. Five is Org's threshold, not four.
bool IsHorizontalRule(const std::string &t) {
	if (t.size() < 5) {
		return false;
	}
	for (char c : t) {
		if (c != '-') {
			return false;
		}
	}
	return true;
}

//! `|---+---|` and friends: a row whose cells contain only dashes and pluses. This is what
//! promotes the row ABOVE it to a header, so it must not be confused with a data row whose
//! first cell happens to be a dash.
bool IsTableRule(const std::string &t) {
	if (t.size() < 2 || t[0] != '|') {
		return false;
	}
	bool any_dash = false;
	for (char c : t) {
		if (c == '-') {
			any_dash = true;
		} else if (c != '|' && c != '+' && !IsSpace(c)) {
			return false;
		}
	}
	return any_dash;
}

//! A LEADING `*` IS A HEADING ONLY AT COLUMN 0, and only when followed by a space. All
//! three of `* text`, `  * text` and `*bold*` begin with the same character, and column
//! position plus the trailing space is what separates them. This is the reader's one real
//! parsing hazard, so it is decided in one place rather than at each use.
bool ParseHeading(const std::string &line, int &level, std::string &text) {
	size_t stars = 0;
	while (stars < line.size() && line[stars] == '*') {
		stars++;
	}
	if (stars == 0 || stars >= line.size() || !IsSpace(line[stars])) {
		return false;
	}
	// Capped at 6, as everywhere else in panduck: duck_block's heading_level is 1..6 and a
	// seventh star is still a heading, just not a deeper one.
	level = static_cast<int>(stars > 6 ? 6 : stars);
	text = TrimBoth(line.substr(stars));
	return true;
}

//! `- x`, `+ x`, `1. x`, `1) x`. Returns the indent, or -1 when the line is not an item.
//! A bullet `*` is accepted only when INDENTED -- at column 0 it is a heading.
int ParseListItem(const std::string &line, bool &ordered, int &start, std::string &rest) {
	size_t i = 0;
	while (i < line.size() && IsSpace(line[i])) {
		i++;
	}
	if (i >= line.size()) {
		return -1;
	}
	const int indent = static_cast<int>(i);
	if (line[i] == '-' || line[i] == '+' || (line[i] == '*' && indent > 0)) {
		if (i + 1 >= line.size() || !IsSpace(line[i + 1])) {
			return -1; // `--` or `-x` is not an item; a bullet needs its space
		}
		ordered = false;
		rest = TrimBoth(line.substr(i + 2));
		return indent;
	}
	size_t d = i;
	while (d < line.size() && std::isdigit(static_cast<unsigned char>(line[d]))) {
		d++;
	}
	if (d == i || d >= line.size() || (line[d] != '.' && line[d] != ')')) {
		return -1;
	}
	if (d + 1 >= line.size() || !IsSpace(line[d + 1])) {
		return -1;
	}
	ordered = true;
	start = std::atoi(line.substr(i, d - i).c_str());
	rest = TrimBoth(line.substr(d + 2));
	return indent;
}

} // namespace

std::vector<Line> ScanOrg(const std::string &src) {
	std::vector<Line> out;
	size_t pos = 0;
	while (pos <= src.size()) {
		size_t nl = src.find('\n', pos);
		std::string raw = TrimRight(src.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
		pos = nl == std::string::npos ? src.size() + 1 : nl + 1;

		Line line;
		auto trimmed = TrimBoth(raw);
		if (trimmed.empty()) {
			line.kind = LineKind::BLANK;
			out.push_back(std::move(line));
			continue;
		}

		// `#+` BEFORE `#`: a keyword and a comment share a first character, and testing the
		// comment first would swallow every `#+TITLE:` in the document.
		if (trimmed.rfind("#+", 0) == 0) {
			auto body = trimmed.substr(2);
			auto up = Upper(body);
			if (up.rfind("BEGIN_", 0) == 0) {
				line.kind = LineKind::BLOCK_BEGIN;
				auto rest = body.substr(6);
				auto sp = rest.find_first_of(" \t");
				line.key = Upper(sp == std::string::npos ? rest : rest.substr(0, sp));
				line.text = sp == std::string::npos ? std::string() : TrimBoth(rest.substr(sp));
				out.push_back(std::move(line));
				continue;
			}
			if (up.rfind("END_", 0) == 0) {
				line.kind = LineKind::BLOCK_END;
				line.key = Upper(TrimBoth(body.substr(4)));
				out.push_back(std::move(line));
				continue;
			}
			auto colon = body.find(':');
			if (colon != std::string::npos) {
				line.kind = LineKind::KEYWORD;
				line.key = Upper(TrimBoth(body.substr(0, colon)));
				line.text = TrimBoth(body.substr(colon + 1));
				// The line VERBATIM, because an unrecognised keyword is held as `raw` and raw
				// content that has been case-folded is not verbatim.
				line.raw = TrimRight(raw);
				out.push_back(std::move(line));
				continue;
			}
			// `#+` with no colon and no BEGIN/END is not a keyword. Falling through to TEXT
			// keeps its words rather than dropping a line nobody modelled.
		} else if (trimmed[0] == '#' && (trimmed.size() == 1 || IsSpace(trimmed[1]))) {
			line.kind = LineKind::COMMENT;
			line.text = trimmed.size() > 1 ? TrimBoth(trimmed.substr(1)) : std::string();
			out.push_back(std::move(line));
			continue;
		}

		// DRAWERS ARE NOT PROSE. `:PROPERTIES:` .. `:END:` carries Org's per-heading
		// bookkeeping, and pandoc emits nothing for it. Scoping drawers OUT of this reader
		// has to mean DROPPED, not LEAKED: without this the drawer's lines fall through to
		// TEXT and join the following paragraph, so pandoc's own Org output -- which writes
		// a :PROPERTIES: block under every heading by default -- came back reading
		// ":PROPERTIES: :CUSTOM_ID: heading-one :END: Body text...".
		//
		// Losing structure is a gap and emitting non-content as prose is a bug; this reader
		// says so elsewhere and the rule applies to its own omissions.
		if (trimmed.size() >= 2 && trimmed.front() == ':' && trimmed.back() == ':' &&
		    trimmed.find(' ') == std::string::npos) {
			auto name = Upper(trimmed.substr(1, trimmed.size() - 2));
			line.kind = name == "END" ? LineKind::DRAWER_END : LineKind::DRAWER_BEGIN;
			line.key = name;
			out.push_back(std::move(line));
			continue;
		}

		if (IsHorizontalRule(trimmed)) {
			line.kind = LineKind::HRULE;
			out.push_back(std::move(line));
			continue;
		}
		if (IsTableRule(trimmed)) {
			line.kind = LineKind::TABLE_RULE;
			out.push_back(std::move(line));
			continue;
		}
		if (trimmed[0] == '|') {
			line.kind = LineKind::TABLE_ROW;
			line.text = trimmed;
			out.push_back(std::move(line));
			continue;
		}
		if (ParseHeading(raw, line.level, line.text)) {
			line.kind = LineKind::HEADING;
			out.push_back(std::move(line));
			continue;
		}
		{
			bool ordered = false;
			int start = 1;
			std::string rest;
			int indent = ParseListItem(raw, ordered, start, rest);
			if (indent >= 0) {
				line.kind = LineKind::LIST_ITEM;
				line.level = indent;
				line.ordered = ordered;
				line.start = start;
				// ` :: ` SPLITS A DESCRIPTION ITEM. Org spells a definition list as an
				// ordinary bullet with a separator, so the shape is decided here rather
				// than by a different marker -- `- term :: definition`.
				auto sep = rest.find(" :: ");
				if (sep != std::string::npos) {
					line.definition = true;
					line.term = TrimBoth(rest.substr(0, sep));
					line.text = TrimBoth(rest.substr(sep + 4));
				} else {
					line.text = rest;
				}
				out.push_back(std::move(line));
				continue;
			}
		}
		line.kind = LineKind::TEXT;
		line.text = trimmed;
		out.push_back(std::move(line));
	}
	// The loop runs one past the end so a trailing newline yields its final empty line;
	// drop it so a document does not gain a blank the source never had.
	if (!out.empty() && out.back().kind == LineKind::BLANK) {
		out.pop_back();
	}
	return out;
}

} // namespace org
} // namespace duckdb
