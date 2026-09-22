#pragma once

#include <cctype>
#include <string>

namespace duckdb {

//! Pandoc's heading identifier, shared by every reader that derives an anchor.
//!
//! WHY THIS IS ONE FUNCTION. textile and mediawiki each held a private copy that differed
//! only in separator, and the comments on both insisted the separator was a FORMAT
//! CONVENTION rather than a choice -- which was true, and which hid that both copies were
//! also wrong in the same three ways. Measured against pandoc 3.1.3, the copies kept
//! leading digits, trimmed a trailing connector, and had no fallback for a title with no
//! letters in it. One wrong function in two places reads as two independent decisions.
//!
//! THE RULE, measured rather than assumed -- every line below was probed against a real
//! pandoc, and the probes that turned out to measure a format's INLINE PARSER instead of
//! this rule were discarded rather than encoded:
//!
//!   alphanumeric     lowercased
//!   a RUN of spaces  collapses to ONE separator
//!   `-`              becomes the separator: `-` for rst/textile, `_` for mediawiki
//!   `_`              always literal `_`, whatever the separator is
//!   anything else    DROPPED, and never becomes a separator, so `Trailing bang!` is
//!                    `trailing-bang` with no dangling connector
//!   leading run      skipped up to the FIRST LETTER, so `2019 Annual Report` is
//!                    `annual-report` and `1. Real Title` is `real-title`
//!   nothing left     the literal `section`
//!
//! Runs of `-` and `_` are NOT collapsed, though runs of spaces are: `Mix-_-Mix` is
//! `mix-_-mix` with a hyphen separator and `mix___mix` with an underscore one. A trailing
//! connector SURVIVES -- `End With Hyphen-` is `end-with-hyphen-` -- which is the opposite
//! of what both previous copies did.
//!
//! THE INPUT IS RENDERED TEXT, not source. `**Bold** Title` must arrive as `Bold Title`, so
//! the delimiters contribute nothing to the anchor. Callers pass the flattened title.
//!
//! DECLARED DIVERGENCE -- NON-ASCII. pandoc lowercases by Unicode, so `Über Title` anchors
//! as `uber-title` with a real `ü`. Doing that here means carrying a Unicode casing table,
//! which is far more than an anchor is worth. Bytes at or above 0x80 are PASSED THROUGH
//! instead: `Über Title` becomes `Über-title`. That is one wrong CASE against pandoc, where
//! the previous copies silently DELETED the character and produced `ber-title` -- one
//! missing letter, and a leading one, which the strip-to-first-letter rule then compounded.
//! Preserving the byte is closer to pandoc and loses nothing; the casing gap is the
//! declared residue.
inline std::string HeadingSlug(const std::string &text, char sep) {
	std::string out;
	bool started = false;       // have we reached the first letter yet?
	bool pending_space = false; // a space run is only worth a separator if something follows
	for (unsigned char c : text) {
		// A byte at or above 0x80 is part of a multi-byte character. Treat it as a letter:
		// it can START an identifier, which is the whole point of not deleting it.
		const bool non_ascii = c >= 0x80;
		if (!started) {
			if (!non_ascii && !std::isalpha(c)) {
				continue;
			}
			started = true;
		}
		if (non_ascii || std::isalnum(c)) {
			if (pending_space) {
				out += sep;
				pending_space = false;
			}
			out += non_ascii ? static_cast<char>(c) : static_cast<char>(std::tolower(c));
		} else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
			if (!out.empty()) {
				pending_space = true;
			}
		} else if (c == '-') {
			if (pending_space) {
				out += sep;
				pending_space = false;
			}
			out += sep;
		} else if (c == '_') {
			if (pending_space) {
				out += sep;
				pending_space = false;
			}
			out += '_';
		}
		// Everything else is dropped WITHOUT setting pending_space, so punctuation never
		// leaves a connector behind it.
	}
	// pandoc's fallback when the title contributes no identifier at all -- `123`, `!!! ???`.
	// Without this the anchor is the empty string, which no consumer can match on.
	return out.empty() ? std::string("section") : out;
}

} // namespace duckdb
