#include "ipynb_reader.hpp"
#include "panduck_duckdb_compat.hpp"

#include "duck_block_types.hpp"
#include "yyjson.hpp"

#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <fstream>
#include <map>

using namespace duckdb_yyjson; // NOLINT -- the spelling the json extension uses

namespace duckdb {
namespace ipynb {
namespace {

//! `source` is an ARRAY OF LINES in every notebook nbformat 4 writes, and a plain string
//! in some hand-built ones. Both spellings are legal and a reader that handles one silently
//! produces nothing for the other.
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

//! Trim one trailing newline. A notebook's source and outputs end with "\n" as a line
//! terminator, not as content, and keeping it puts a blank line at the end of every cell.
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
			// MALFORMED JSON YIELDS NOTHING rather than throwing. A reader that fails the
			// whole query on one bad file is worse than one that reports an empty document,
			// and every other panduck reader degrades the same way.
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

	//! The notebook's kernel language, which is what a code cell is written in. Notebooks
	//! record it once at the top rather than per cell.
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

	//! A raw block: content verbatim, format in an ATTRIBUTE, encoding left at its default.
	void PushRaw(const std::string &content, int level, const char *format) {
		IpynbBlock b;
		b.element_type = DuckBlockTypes::TYPE_RAW;
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

			// EACH CELL IS A CONTAINER, matching pandoc's Div per cell. A notebook's cell
			// boundaries are structure a consumer needs -- "which cell produced this" is the
			// question notebooks exist to answer -- so they are not flattened away.
			Push(DuckBlockTypes::TYPE_DIV, std::string(), 1, {}, {}, type.empty() ? "cell" : type);

			if (type == "markdown") {
				// HELD RAW, AND THIS IS A DEFERRAL RATHER THAN A RESTING PLACE.
				//
				// A markdown cell contains a DOCUMENT, not data -- it would be duck_blocks.
				// That makes it a different case from the whole-file .toml/.yaml blob, where
				// verbatim is the correct and final answer because there is nothing it
				// should become.
				//
				// It is raw here because delegating would make this reader's output depend
				// on which extensions happen to be installed: panduck's delegation lives in
				// the SQL dispatch layer, which is where .md routes to duckdb_markdown, and
				// a C++ reader cannot reach those functions. One consistent behaviour beats
				// two that vary by environment.
				//
				// A consumer wanting blocks today can call md_to_blocks() on this content
				// and normalise the result. The deferral is discharged by a post-parse
				// helper for embedded formats -- NOT by markdown parsing landing in panduck,
				// which would violate the isolation that put it here.
				if (!source.empty()) {
					// FORMAT IN THE ATTRIBUTE, not in `encoding`. This carried encoding='markdown' with
					// no format at all, so the one field a consumer reads to learn what the markup
					// IS was empty and the one it does read said something the flat rule forbids.
					PushRaw(source, 2, "markdown");
				}
				continue;
			}
			if (type == "code") {
				if (!source.empty()) {
					Push(DuckBlockTypes::TYPE_CODE, source, 2, {}, language_);
				}
				Outputs(yyjson_obj_get(cell, "outputs"));
				continue;
			}
			// A `raw` cell carries its own target format in metadata.format; without one it
			// is plain text.
			auto *cm = yyjson_obj_get(cell, "metadata");
			auto fmt = StrField(cm, "format");
			if (!source.empty()) {
				Push(DuckBlockTypes::TYPE_RAW, source, 2, fmt.empty() ? DuckBlockTypes::ENCODING_TEXT : fmt);
			}
		}
	}

	//! A CODE CELL'S OUTPUTS ARE CONTENT. What a notebook computed is part of what it says
	//! -- a notebook read without its outputs is a script -- and pandoc keeps them too.
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
				// execute_result and display_data carry a bundle keyed by MIME type. text/plain
				// is the one every producer writes and the only one that is text rather than an
				// encoded image, so it is the one taken.
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
			Push(DuckBlockTypes::TYPE_DIV, std::string(), 2, {}, {}, kind.empty() ? "output" : kind);
			Push(DuckBlockTypes::TYPE_CODE, text, 3);
		}
	}

	//! NOTEBOOK METADATA, and this reader EXCEEDS pandoc here deliberately.
	//!
	//! Measured: pandoc puts the entire notebook metadata into ONE opaque `jupyter` MetaMap
	//! -- title, authors, kernelspec and all -- so a consumer asking "who wrote this" has to
	//! walk a blob. The fields are plainly in the file, and recovering them is the same
	//! approved exception the docx and odt readers take.
	//!
	//! Every field therefore carries attributes['source_type'] with its original path, so a
	//! format-derived field stays distinguishable from a pandoc-derived one.
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
			b.kind = DuckBlockTypes::KIND_VALUE;
			b.element_type = DuckBlockTypes::VALUE_INLINES;
			b.key = kv.first;
			b.source_type = kv.second.second;
			b.level = 1;
			IpynbInline run;
			run.element_type = DuckBlockTypes::INLINE_TEXT;
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

namespace {

struct IpynbRow {
	std::string kind, element_type, content;
	std::string encoding = DuckBlockTypes::ENCODING_TEXT;
	int32_t level = 0;
	std::map<std::string, std::string> attributes;
	int32_t element_order = 0;
};

struct IpynbBindData : public TableFunctionData {
	std::vector<IpynbRow> rows;
};

struct IpynbGlobalState : public GlobalTableFunctionState {
	idx_t offset = 0;
	static unique_ptr<GlobalTableFunctionState> Init(ClientContext &, TableFunctionInitInput &) {
		return make_uniq<IpynbGlobalState>();
	}
};

void IpynbColumns(vector<LogicalType> &types, panduck::BindNames &names) {
	types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	         LogicalType::INTEGER, LogicalType::VARCHAR, LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR),
	         LogicalType::INTEGER};
	names = {"kind", "element_type", "content", "level", "encoding", "attributes", "element_order"};
}

void BuildRows(const std::string &src, std::vector<IpynbRow> &rows) {
	int32_t order = 0;
	for (auto &block : ParseIpynbString(src)) {
		IpynbRow row;
		row.kind = block.kind.empty() ? DuckBlockTypes::KIND_BLOCK : block.kind;
		row.element_type = block.element_type;
		row.content = block.content;
		if (!block.encoding.empty()) {
			row.encoding = block.encoding;
		}
		row.element_order = order++;
		if (!block.key.empty()) {
			row.attributes[DuckBlockTypes::ATTR_KEY] = block.key;
		}
		if (!block.raw_format.empty()) {
			row.attributes["format"] = block.raw_format;
		}
		if (!block.source_type.empty()) {
			// A div's cell or output kind, or a metadata field's original path. On metadata
			// it is what keeps a format-derived field distinguishable from a pandoc-derived
			// one, which is the condition attached to exceeding the reference.
			row.attributes[DuckBlockTypes::ATTR_SOURCE_TYPE] = block.source_type;
		}
		if (!block.language.empty()) {
			row.attributes["language"] = block.language;
		}
		// NO heading, role or list branches here, unlike every other reader: a notebook's
		// block structure is cells, code and raw content. Carrying fields the format cannot
		// produce would be dead weight that reads as an oversight.
		const int32_t block_level = block.level > 0 ? block.level : 1;
		row.level = block_level;
		rows.push_back(std::move(row));

		for (auto &inl : block.inlines) {
			IpynbRow child;
			child.kind = DuckBlockTypes::KIND_INLINE;
			child.element_type = inl.element_type;
			child.content = inl.content;
			child.level = inl.level > 0 ? inl.level : block_level + 1;
			child.element_order = order++;
			// No href branch: the only inlines this reader emits are the metadata values'
			// text runs. Markdown cells are held raw, so their links never become inlines
			// here -- they are still inside the raw content, awaiting the post-parse helper.
			rows.push_back(std::move(child));
		}
	}
}

unique_ptr<FunctionData> IpynbFileBind(ClientContext &, TableFunctionBindInput &input,
                                       vector<LogicalType> &return_types, panduck::BindNames &names) {
	IpynbColumns(return_types, names);
	auto path = input.inputs[0].GetValue<string>();
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		throw IOException("read_ipynb_blocks: cannot open %s", path);
	}
	std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	auto result = make_uniq<IpynbBindData>();
	BuildRows(src, result->rows);
	return std::move(result);
}

unique_ptr<FunctionData> IpynbStringBind(ClientContext &, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, panduck::BindNames &names) {
	IpynbColumns(return_types, names);
	auto result = make_uniq<IpynbBindData>();
	BuildRows(input.inputs[0].GetValue<string>(), result->rows);
	return std::move(result);
}

void IpynbScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<IpynbBindData>();
	auto &state = input.global_state->Cast<IpynbGlobalState>();
	idx_t count = 0;
	while (state.offset < data.rows.size() && count < STANDARD_VECTOR_SIZE) {
		const auto &row = data.rows[state.offset];
		output.SetValue(0, count, Value(row.kind));
		output.SetValue(1, count, Value(row.element_type));
		output.SetValue(2, count, row.content.empty() ? Value(LogicalType::VARCHAR) : Value(row.content));
		output.SetValue(3, count, Value::INTEGER(row.level));
		output.SetValue(4, count, Value(row.encoding));
		output.SetValue(5, count, DuckBlockTypes::CreateAttributesMap(row.attributes));
		output.SetValue(6, count, Value::INTEGER(row.element_order));
		state.offset++;
		count++;
	}
	output.SetCardinality(count);
}

} // namespace

void RegisterIpynbReader(ExtensionLoader &loader) {
	TableFunction file_fn("read_ipynb_blocks", {LogicalType::VARCHAR}, IpynbScan, IpynbFileBind,
	                      IpynbGlobalState::Init);
	loader.RegisterFunction(file_fn);

	// The string form, as the LaTeX reader has: asserting a two-line snippet is how the
	// nesting and inline rules stay readable in the tests.
	TableFunction string_fn("read_ipynb_blocks_string", {LogicalType::VARCHAR}, IpynbScan, IpynbStringBind,
	                        IpynbGlobalState::Init);
	loader.RegisterFunction(string_fn);
}

} // namespace ipynb
} // namespace duckdb
