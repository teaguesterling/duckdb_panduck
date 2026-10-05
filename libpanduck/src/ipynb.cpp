#include "panduck/ipynb.hpp"

#include "panduck/vocabulary.hpp"
#include "yyjson.hpp"

#include <map>
#include <string>
#include <vector>

// THE SEAM'S SECOND CONCESSION, and the sharpest open question in L1.
//
// yyjson is a C library, but the copy this build uses is DuckDB's vendored one,
// which wraps it in `namespace duckdb_yyjson`. So the core depends not merely on
// yyjson but on DuckDB's *namespaced build* of it -- a standalone libpanduck
// linking a system yyjson would find these symbols at global scope instead and
// this directive would not compile.
//
// Left as-is rather than papered over with a macro: the real decision is whether
// libpanduck carries its own yyjson or takes one from the host, and that belongs
// to the L1 contract (issue #104), not to a reader move. Recorded in
// libpanduck/CMakeLists.txt as PANDUCK_YYJSON_INCLUDE_DIR.
using namespace duckdb_yyjson; // NOLINT -- the spelling the json extension uses

// The parse core, moved out of src/ipynb_reader.cpp (issue #104, milestone L2).
// Behaviour is unchanged: the only edits are the namespace (duckdb::ipynb ->
// panduck::ipynb) and DuckBlockVocabulary:: -> DuckBlockVocabulary::, which reaches
// the same constants -- DuckBlockTypes derives from DuckBlockVocabulary and adds
// only the LogicalType/Value helpers that cannot cross the seam.

namespace panduck {
namespace ipynb {

namespace {

//! `source` is an ARRAY OF LINES in every notebook nbformat 4 writes, and a
//! plain string in some hand-built ones. Both spellings are legal and a reader
//! that handles one silently produces nothing for the other.
std::string JoinSource(yyjson_val *val) {
	if (!val) {
		return std::string();
	}
	if (yyjson_is_str(val)) {
		return yyjson_get_str(val);
	}
	std::string out;
	if (yyjson_is_arr(val)) {
		size_t idx, max;
		yyjson_val *item;
		yyjson_arr_foreach(val, idx, max, item) {
			if (yyjson_is_str(item)) {
				out += yyjson_get_str(item);
			}
		}
	}
	return out;
}

std::string StrField(yyjson_val *obj, const char *key) {
	auto *v = obj ? yyjson_obj_get(obj, key) : nullptr;
	return v && yyjson_is_str(v) ? yyjson_get_str(v) : std::string();
}

//! Trim one trailing newline. A notebook's source and outputs end with "\n" as
//! a line terminator, not as content, and keeping it puts a blank line at the
//! end of every cell.
std::string TrimTrailingNewline(std::string s) {
	while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
		s.pop_back();
	}
	return s;
}

class Builder {
public:
	std::vector<IpynbBlock> Build(const std::string &src) {
		auto *doc = yyjson_read(src.data(), src.size(), 0);
		if (!doc) {
			// MALFORMED JSON YIELDS NOTHING rather than throwing. A reader that fails
			// the whole query on one bad file is worse than one that reports an empty
			// document, and every other panduck reader degrades the same way.
			return {};
		}
		auto *root = yyjson_doc_get_root(doc);
		if (root && yyjson_is_obj(root)) {
			auto *nb_meta = yyjson_obj_get(root, "metadata");
			language_ = KernelLanguage(nb_meta);
			Cells(yyjson_obj_get(root, "cells"));
			Metadata(nb_meta);
		}
		yyjson_doc_free(doc);
		return std::move(blocks_);
	}

private:
	std::vector<IpynbBlock> blocks_;
	std::string language_;

	//! The notebook's kernel language, which is what a code cell is written in.
	//! Notebooks record it once at the top rather than per cell.
	static std::string KernelLanguage(yyjson_val *nb_meta) {
		auto *ks = nb_meta ? yyjson_obj_get(nb_meta, "kernelspec") : nullptr;
		auto lang = StrField(ks, "language");
		if (!lang.empty()) {
			return lang;
		}
		auto *li = nb_meta ? yyjson_obj_get(nb_meta, "language_info") : nullptr;
		return StrField(li, "name");
	}

	void Push(const char *type, const std::string &content, int level, const std::string &encoding = {},
	          const std::string &language = {}, const std::string &source_type = {}) {
		IpynbBlock b;
		b.element_type = type;
		b.content = content;
		b.level = level;
		b.encoding = encoding;
		b.language = language;
		b.source_type = source_type;
		blocks_.push_back(std::move(b));
	}

	//! A raw block: content verbatim, format in an ATTRIBUTE, encoding left at
	//! its default.
	void PushRaw(const std::string &content, int level, const char *format) {
		IpynbBlock b;
		b.element_type = DuckBlockVocabulary::TYPE_RAW;
		b.content = content;
		b.level = level;
		b.raw_format = format;
		blocks_.push_back(std::move(b));
	}

	void Cells(yyjson_val *cells) {
		if (!cells || !yyjson_is_arr(cells)) {
			return;
		}
		size_t idx, max;
		yyjson_val *cell;
		yyjson_arr_foreach(cells, idx, max, cell) {
			if (!yyjson_is_obj(cell)) {
				continue;
			}
			auto type = StrField(cell, "cell_type");
			auto source = TrimTrailingNewline(JoinSource(yyjson_obj_get(cell, "source")));

			// EACH CELL IS A CONTAINER, matching pandoc's Div per cell. A notebook's
			// cell boundaries are structure a consumer needs -- "which cell produced
			// this" is the question notebooks exist to answer -- so they are not
			// flattened away.
			Push(DuckBlockVocabulary::TYPE_DIV, std::string(), 1, {}, {}, type.empty() ? "cell" : type);

			if (type == "markdown") {
				// HELD RAW, AND THIS IS A DEFERRAL RATHER THAN A RESTING PLACE.
				//
				// A markdown cell contains a DOCUMENT, not data -- it would be
				// duck_blocks. That makes it a different case from the whole-file
				// .toml/.yaml blob, where verbatim is the correct and final answer
				// because there is nothing it should become.
				//
				// It is raw here because delegating would make this reader's output
				// depend on which extensions happen to be installed: panduck's
				// delegation lives in the SQL dispatch layer, which is where .md routes
				// to duckdb_markdown, and a C++ reader cannot reach those functions.
				// One consistent behaviour beats two that vary by environment.
				//
				// A consumer wanting blocks today calls parse_markdown_to_duck_blocks()
				// -- a SCALAR from the markdown extension -- on this content, and
				// normalises the result. THIS SENTENCE NAMED md_to_blocks() UNTIL NOW,
				// WHICH DOES NOT EXIST: the markdown extension has never shipped that
				// name. Anyone following the advice got a Catalog Error, which is the
				// shape of #25 and l1t1's INSTALL report -- guidance that names a next
				// step which does not work costs a reader more than saying nothing. The
				// deferral is discharged by a post-parse helper for embedded formats --
				// NOT by markdown parsing landing in panduck, which would violate the
				// isolation that put it here.
				if (!source.empty()) {
					// FORMAT IN THE ATTRIBUTE, not in `encoding`. This carried
					// encoding='markdown' with no format at all, so the one field a
					// consumer reads to learn what the markup IS was empty and the one it
					// does read said something the flat rule forbids.
					PushRaw(source, 2, "markdown");
				}
				continue;
			}
			if (type == "code") {
				if (!source.empty()) {
					Push(DuckBlockVocabulary::TYPE_CODE, source, 2, {}, language_);
				}
				Outputs(yyjson_obj_get(cell, "outputs"));
				continue;
			}
			// A `raw` cell carries its own target format in metadata.format; without
			// one it is plain text.
			auto *cm = yyjson_obj_get(cell, "metadata");
			auto fmt = StrField(cm, "format");
			if (!source.empty()) {
				Push(DuckBlockVocabulary::TYPE_RAW, source, 2, fmt.empty() ? DuckBlockVocabulary::ENCODING_TEXT : fmt);
			}
		}
	}

	//! A CODE CELL'S OUTPUTS ARE CONTENT. What a notebook computed is part of
	//! what it says
	//! -- a notebook read without its outputs is a script -- and pandoc keeps
	//! them too.
	void Outputs(yyjson_val *outputs) {
		if (!outputs || !yyjson_is_arr(outputs)) {
			return;
		}
		size_t idx, max;
		yyjson_val *out;
		yyjson_arr_foreach(outputs, idx, max, out) {
			if (!yyjson_is_obj(out)) {
				continue;
			}
			auto kind = StrField(out, "output_type");
			std::string text;
			if (kind == "stream") {
				text = JoinSource(yyjson_obj_get(out, "text"));
			} else {
				// execute_result and display_data carry a bundle keyed by MIME type.
				// text/plain is the one every producer writes and the only one that is
				// text rather than an encoded image, so it is the one taken.
				auto *data = yyjson_obj_get(out, "data");
				text = JoinSource(data ? yyjson_obj_get(data, "text/plain") : nullptr);
				if (text.empty() && kind == "error") {
					text = JoinSource(yyjson_obj_get(out, "evalue"));
				}
			}
			text = TrimTrailingNewline(text);
			if (text.empty()) {
				continue;
			}
			Push(DuckBlockVocabulary::TYPE_DIV, std::string(), 2, {}, {}, kind.empty() ? "output" : kind);
			Push(DuckBlockVocabulary::TYPE_CODE, text, 3);
		}
	}

	//! NOTEBOOK METADATA, and this reader EXCEEDS pandoc here deliberately.
	//!
	//! Measured: pandoc puts the entire notebook metadata into ONE opaque
	//! `jupyter` MetaMap
	//! -- title, authors, kernelspec and all -- so a consumer asking "who wrote
	//! this" has to walk a blob. The fields are plainly in the file, and
	//! recovering them is the same approved exception the docx and odt readers
	//! take.
	//!
	//! Every field therefore carries attributes['source_type'] with its original
	//! path, so a format-derived field stays distinguishable from a
	//! pandoc-derived one.
	void Metadata(yyjson_val *nb_meta) {
		if (!nb_meta || !yyjson_is_obj(nb_meta)) {
			return;
		}
		std::map<std::string, std::pair<std::string, std::string>> found; // key -> {text, source}
		auto title = StrField(nb_meta, "title");
		if (!title.empty()) {
			found["title"] = {title, "metadata.title"};
		}
		auto *authors = yyjson_obj_get(nb_meta, "authors");
		if (authors && yyjson_is_arr(authors)) {
			std::string joined;
			size_t idx, max;
			yyjson_val *a;
			yyjson_arr_foreach(authors, idx, max, a) {
				auto name = yyjson_is_str(a) ? std::string(yyjson_get_str(a)) : StrField(a, "name");
				if (!name.empty()) {
					joined += (joined.empty() ? "" : " ") + name;
				}
			}
			if (!joined.empty()) {
				found["author"] = {joined, "metadata.authors"};
			}
		}
		if (!language_.empty()) {
			found["kernel"] = {language_, "metadata.kernelspec.language"};
		}
		for (auto &kv : found) {
			IpynbBlock b;
			b.kind = DuckBlockVocabulary::KIND_VALUE;
			b.element_type = DuckBlockVocabulary::VALUE_INLINES;
			b.key = kv.first;
			b.source_type = kv.second.second;
			b.level = 1;
			IpynbInline run;
			run.element_type = DuckBlockVocabulary::INLINE_TEXT;
			run.content = kv.second.first;
			run.level = 2;
			b.inlines.push_back(std::move(run));
			blocks_.push_back(std::move(b));
		}
	}
};

} // namespace

std::vector<IpynbBlock> ParseIpynbString(const std::string &src) {
	Builder builder;
	return builder.Build(src);
}

} // namespace ipynb
} // namespace panduck
