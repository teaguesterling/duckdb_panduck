#pragma once

#include "duck_block_types.hpp"
#include <vector>
#include <set>

#include <string>
#include <utility>

namespace duckdb {

//! Office-document metadata field names, mapped to PANDOC's key namespace.
//!
//! DOCX carries these in `docProps/core.xml` and ODT in `meta.xml`. The two formats use
//! overlapping but not identical spellings for the same fields, so the shared part lives
//! here rather than as two copies that cannot detect their own disagreement -- the reason
//! the JSON table helper is shared too.
//!
//! PANDOC EXTRACTS NOTHING FROM EITHER FORMAT. Measured against 3.1.3: `pandoc file.docx
//! -t json` and `pandoc file.odt -t json` both return an empty `meta`, even though the
//! files plainly carry dcterms:created, dc:language and meta:generator. Its DOCX and ODT
//! readers simply do not populate Meta.
//!
//! So every field these two readers emit EXCEEDS the reference, which is a deliberate
//! exception rather than a reader being more thorough by accident. Teague approved
//! exceeding pandoc on the grounds that recovering fields the user's file plainly contains
//! beats matching pandoc's blind spot -- with one condition, which
//! `attributes['source_type']` discharges: a format-derived field must be
//! DISTINGUISHABLE from a pandoc-derived one.
//!
//! Without that marker panduck's output stops being reproducible from `pandoc -t json`,
//! and the next person to diff the two reads recovered data as a bug. It is also the
//! narrow exception to the rule the multi_author fixtures enforce -- a reader MIRRORS
//! pandoc's shape rather than normalising it -- so it should be visible as one.
//!
//! source_type carries the ORIGINAL spelling (`dcterms:created`, `meta:generator`), which
//! serves both purposes at once: it marks the field as format-derived AND records what it
//! was called before the pandoc key namespace flattened it.
//! Trim ASCII whitespace. Metadata text arrives padded in every format that carries it --
//! XML indentation around a <dc:title>, and RTF's space between a control word and its
//! argument -- and three readers now need the same treatment. Kept beside the field table
//! rather than copied per reader, and dependency-free so the parse layer stays extractable.
inline std::string TrimMetaText(const std::string &s) {
	size_t b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos) {
		return std::string();
	}
	size_t e = s.find_last_not_of(" \t\r\n");
	return s.substr(b, e - b + 1);
}

struct MetadataField {
	const char *source; //!< the element name in the source document
	const char *key;    //!< pandoc's key, which is what attributes['key'] carries
};

//! DOCX `docProps/core.xml`. dcterms:modified is deliberately absent: pandoc has no key
//! for it in any format, and minting one is what the spec tells producers not to do.
//! cp:revision and cp:lastModifiedBy are absent for the same reason.
static constexpr MetadataField DOCX_CORE_FIELDS[] = {
    {"dc:title", "title"},       {"dc:creator", "author"},
    {"dc:subject", "subject"},   {"dc:description", "description"},
    {"dc:language", "language"}, {"dcterms:created", "date"},
};

//! ODT `meta.xml`. `dc:date` is preferred over `meta:creation-date` when both are present
//! -- they carry the same instant in every fixture measured, and two sources for one key
//! is how the two drift apart. meta:editing-duration and meta:user-defined have no pandoc
//! key and are left as a recorded gap rather than given an invented one.
static constexpr MetadataField ODT_META_FIELDS[] = {
    {"dc:title", "title"},           {"dc:creator", "author"},
    {"dc:subject", "subject"},       {"dc:description", "description"},
    {"dc:language", "language"},     {"dc:date", "date"},
    {"meta:generator", "generator"},
};

//! Drop body paragraphs that merely restate the document's metadata.
//!
//! DOCX and ODT both style the title, author and date as ordinary paragraphs at the top of
//! the body, AND record them in a metadata part. A reader that walks the body and reads the
//! metadata therefore emits each of them twice -- once as prose, once as a `value`. pandoc's
//! body for the same file starts at the first real heading.
//!
//! Duplication is invisible to every other check in this repo: the conformance and
//! write-back checks see a perfectly valid paragraph, and the word-loss check looks only for
//! ABSENCE. It took comparing one logical document across nine readers to see it.
//!
//! A candidate is dropped ONLY when its text actually appears in the metadata. A
//! Title-styled paragraph in a document whose metadata part carries no title is the only
//! copy of that text, and dropping it unconditionally would trade a duplicate for a
//! deletion -- the strictly worse failure.
//!
//! `Block` must expose `kind`, `content` and `inlines[].content`; both readers' block types
//! do, which is why this is a template rather than two copies.
template <typename Block>
void DropDuplicatedMetadataParagraphs(std::vector<Block> &blocks,
                                      const std::vector<std::pair<size_t, std::string>> &candidates) {
	if (candidates.empty()) {
		return;
	}
	std::set<std::string> meta_text;
	for (auto &b : blocks) {
		if (b.kind != DuckBlockTypes::KIND_VALUE) {
			continue;
		}
		for (auto &run : b.inlines) {
			if (!run.content.empty()) {
				meta_text.insert(run.content);
			}
		}
	}
	// Highest index first: erasing from the front would invalidate the later ones.
	for (auto it = candidates.rbegin(); it != candidates.rend(); ++it) {
		if (it->first < blocks.size() && meta_text.count(it->second)) {
			blocks.erase(blocks.begin() + static_cast<long>(it->first));
		}
	}
}

} // namespace duckdb
