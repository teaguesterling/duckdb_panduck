#include "rst_scanner.hpp"

#include <cctype>
#include <cstdlib>

namespace duckdb {
namespace rst {
namespace {

//! The characters docutils accepts as section adornment. Any ASCII punctuation, in
//! practice -- the set is open, so this tests the CLASS rather than enumerating members.
bool IsAdornmentChar(char c) {
	return std::ispunct(static_cast<unsigned char>(c)) && c != ',' && c != ';';
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

int Indent(const std::string &s) {
	int n = 0;
	while (n < (int)s.size() && (s[n] == ' ' || s[n] == '\t')) {
		n++;
	}
	return n;
}

//! A run of ONE punctuation character, two or more long. Two is docutils' minimum for an
//! underline, and `--` is a legal (if unusual) one.
bool IsAdornment(const std::string &t, char &c) {
	if (t.size() < 2 || !IsAdornmentChar(t[0])) {
		return false;
	}
	for (char ch : t) {
		if (ch != t[0]) {
			return false;
		}
	}
	c = t[0];
	return true;
}

//! `+-----+-----+` or `+=====+=====+`. The `=` spelling marks the header boundary.
bool IsGridSep(const std::string &t, bool &header) {
	if (t.size() < 3 || t[0] != '+' || t.back() != '+') {
		return false;
	}
	header = false;
	for (char c : t) {
		if (c == '=') {
			header = true;
		} else if (c != '+' && c != '-') {
			return false;
		}
	}
	return true;
}

//! `=====  =====` -- runs of `=` separated by spaces. The run LENGTHS are the column
//! widths, which is the only place RST cell extraction is positional rather than
//! delimited.
bool IsSimpleSep(const std::string &t, std::vector<int> &spans, std::vector<int> &starts) {
	if (t.size() < 2 || t[0] != '=') {
		return false;
	}
	spans.clear();
	starts.clear();
	size_t i = 0;
	while (i < t.size()) {
		if (t[i] == '=') {
			size_t j = i;
			while (j < t.size() && t[j] == '=') {
				j++;
			}
			spans.push_back(static_cast<int>(j - i));
			starts.push_back(static_cast<int>(i));
			i = j;
		} else if (t[i] == ' ') {
			i++;
		} else {
			return false;
		}
	}
	// TWO RUNS MINIMUM. `=====` alone is a HEADING ADORNMENT, not a one-column table, and
	// testing simple-tables first without this turned every `Title\n=====` in the corpus
	// into a paragraph -- the most conventional heading spelling there is. A simple table
	// needs at least two columns to be a table at all.
	return spans.size() >= 2;
}

} // namespace

std::vector<Line> ScanRst(const std::string &src) {
	std::vector<Line> out;
	size_t pos = 0;
	while (pos <= src.size()) {
		size_t nl = src.find('\n', pos);
		std::string raw = TrimRight(src.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
		pos = nl == std::string::npos ? src.size() + 1 : nl + 1;

		Line line;
		line.indent = Indent(raw);
		auto t = TrimBoth(raw);
		if (t.empty()) {
			line.kind = LineKind::BLANK;
			out.push_back(std::move(line));
			continue;
		}

		// `..` BEFORE ADORNMENT: `..` is itself two punctuation characters and would
		// otherwise read as a two-character adornment run.
		if (t.rfind("..", 0) == 0) {
			auto rest = TrimBoth(t.substr(2));
			auto colons = rest.find("::");
			if (colons != std::string::npos && colons > 0) {
				line.kind = LineKind::DIRECTIVE;
				line.name = TrimBoth(rest.substr(0, colons));
				line.text = TrimBoth(rest.substr(colons + 2));
			} else {
				// A COMMENT, and it produces nothing. Not dropping it silently would put
				// docutils bookkeeping into the document as prose.
				line.kind = LineKind::COMMENT;
				line.text = rest;
			}
			out.push_back(std::move(line));
			continue;
		}

		bool header = false;
		if (IsGridSep(t, header)) {
			line.kind = LineKind::GRID_SEP;
			line.header_sep = header;
			out.push_back(std::move(line));
			continue;
		}
		if (t[0] == '|' && t.size() > 1) {
			line.kind = LineKind::TABLE_ROW;
			line.text = t;
			out.push_back(std::move(line));
			continue;
		}
		if (IsSimpleSep(t, line.spans, line.span_starts)) {
			line.kind = LineKind::SIMPLE_SEP;
			out.push_back(std::move(line));
			continue;
		}
		if (IsAdornment(t, line.adornment)) {
			// AMBIGUOUS BY DESIGN. Heading underline or transition -- only the previous
			// line can say, and the reader has it.
			line.kind = LineKind::ADORNMENT;
			out.push_back(std::move(line));
			continue;
		}
		// `:Name: value` -- a FIELD, and NOT metadata. Pandoc emits a DefinitionList for a
		// field list and an EMPTY meta, measured. The syntax exists to record document
		// fields and docutils itself promotes them, which is exactly why a reader written
		// on the obvious assumption emits `kind='value'` rows pandoc does not have -- and
		// nothing in the vocabulary would object, because the shape is valid and the keys
		// are plausible.
		if (t[0] == ':') {
			auto close = t.find(':', 1);
			if (close != std::string::npos && close > 1) {
				line.kind = LineKind::FIELD;
				line.name = t.substr(1, close - 1);
				line.text = TrimBoth(t.substr(close + 1));
				out.push_back(std::move(line));
				continue;
			}
		}
		if ((t[0] == '-' || t[0] == '*' || t[0] == '+') && t.size() > 1 && t[1] == ' ') {
			line.kind = LineKind::BULLET;
			line.text = TrimBoth(t.substr(2));
			out.push_back(std::move(line));
			continue;
		}
		{
			size_t d = 0;
			while (d < t.size() && std::isdigit(static_cast<unsigned char>(t[d]))) {
				d++;
			}
			bool auto_num = t[0] == '#';
			if (auto_num) {
				d = 1;
			}
			if (d > 0 && d < t.size() && (t[d] == '.' || t[d] == ')') && d + 1 < t.size() && t[d + 1] == ' ') {
				line.kind = LineKind::ENUM;
				line.ordered = true;
				line.start = auto_num ? 1 : std::atoi(t.substr(0, d).c_str());
				line.text = TrimBoth(t.substr(d + 2));
				out.push_back(std::move(line));
				continue;
			}
		}
		line.kind = LineKind::TEXT;
		line.text = t;
		out.push_back(std::move(line));
	}
	if (!out.empty() && out.back().kind == LineKind::BLANK) {
		out.pop_back();
	}
	return out;
}

} // namespace rst
} // namespace duckdb
