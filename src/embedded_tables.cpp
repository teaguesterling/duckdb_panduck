#include "embedded_tables.hpp"

#include "duck_block_types.hpp"
#include "panduck/embedded_table.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"

#include <algorithm>

namespace duckdb {
namespace panduck {

namespace {

// THE DEFAULT FORMATS, which are every format EmbeddedToTable reads. A code block's
// language is matched against them ignoring case.
const vector<string> &DefaultFormats() {
	static const vector<string> formats {"csv", "tsv", "json", "jsonl"};
	return formats;
}

LogicalType ResultType() {
	child_list_t<LogicalType> children;
	children.push_back(make_pair("content", LogicalType::VARCHAR));
	children.push_back(make_pair("error", LogicalType::VARCHAR));
	return LogicalType::STRUCT(std::move(children));
}

// EXACTLY ONE FIELD IS SET: the content, or the reason it failed.
void EmbeddedToTableFun(DataChunk &args, ExpressionState &, Vector &result) {
	auto count = args.size();
	for (idx_t i = 0; i < count; i++) {
		auto text = args.data[0].GetValue(i);
		auto format = args.data[1].GetValue(i);
		if (text.IsNull() || format.IsNull()) {
			result.SetValue(i, Value(ResultType()));
			continue;
		}
		string content, error;
		bool ok = ::panduck::EmbeddedToTable(text.GetValue<string>(), format.GetValue<string>(), content, error);
		child_list_t<Value> out;
		out.push_back(make_pair("content", ok ? Value(content) : Value(LogicalType::VARCHAR)));
		out.push_back(make_pair("error", ok ? Value(LogicalType::VARCHAR) : Value(error)));
		result.SetValue(i, Value::STRUCT(std::move(out)));
	}
	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

// Attributes with each `set` key given its value (replacing an existing entry), and
// every `drop` key removed. The other entries keep their order.
Value WithAttrs(const Value &attrs, const vector<std::pair<string, string>> &set, const vector<string> &drop) {
	vector<Value> keys, vals;
	if (!attrs.IsNull()) {
		for (auto &entry : MapValue::GetChildren(attrs)) {
			auto &kv = StructValue::GetChildren(entry);
			auto key = kv[0].GetValue<string>();
			bool skip = std::find(drop.begin(), drop.end(), key) != drop.end();
			for (auto &s : set) {
				skip = skip || s.first == key;
			}
			if (!skip) {
				keys.push_back(kv[0]);
				vals.push_back(kv[1]);
			}
		}
	}
	for (auto &s : set) {
		keys.push_back(Value(s.first));
		vals.push_back(Value(s.second));
	}
	return Value::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR, std::move(keys), std::move(vals));
}

string Attr(const Value &attrs, const string &key) {
	if (attrs.IsNull()) {
		return string();
	}
	for (auto &entry : MapValue::GetChildren(attrs)) {
		auto &kv = StructValue::GetChildren(entry);
		if (!kv[0].IsNull() && kv[0].GetValue<string>() == key && !kv[1].IsNull()) {
			return kv[1].GetValue<string>();
		}
	}
	return string();
}

Value Rebuild(const Value &block, const Value &type, const Value &content, const Value &encoding, const Value &attrs) {
	auto &c = StructValue::GetChildren(block);
	child_list_t<Value> out;
	out.push_back(make_pair("kind", c[DuckBlockTypes::KIND_IDX]));
	out.push_back(make_pair("element_type", type));
	out.push_back(make_pair("content", content));
	out.push_back(make_pair("level", c[DuckBlockTypes::LEVEL_IDX]));
	out.push_back(make_pair("encoding", encoding));
	out.push_back(make_pair("attributes", attrs));
	out.push_back(make_pair("element_order", c[DuckBlockTypes::ELEMENT_ORDER_IDX]));
	return Value::STRUCT(std::move(out));
}

// ONE BLOCK. Only a `code` BLOCK whose language is in `formats` is touched: an inline
// `code` span is not a document's data. A table keeps every attribute, language
// included, gains `source_text`, and loses a stale `parse_error`. A failure keeps the
// block as it was, with the reason in `parse_error`, so one bad block cannot fail the
// document. Running this twice gives the same result as running it once.
Value ParseBlock(const Value &block, const vector<string> &formats) {
	auto &c = StructValue::GetChildren(block);
	auto &type = c[DuckBlockTypes::ELEMENT_TYPE_IDX];
	auto &kind = c[DuckBlockTypes::KIND_IDX];
	if (type.IsNull() || type.GetValue<string>() != DuckBlockTypes::TYPE_CODE || kind.IsNull() ||
	    kind.GetValue<string>() != DuckBlockTypes::KIND_BLOCK) {
		return block;
	}
	auto &attrs = c[DuckBlockTypes::ATTRIBUTES_IDX];
	auto language = StringUtil::Lower(Attr(attrs, "language"));
	if (language.empty() || std::find(formats.begin(), formats.end(), language) == formats.end()) {
		return block;
	}
	auto &content_value = c[DuckBlockTypes::CONTENT_IDX];
	auto text = content_value.IsNull() ? string() : content_value.GetValue<string>();
	string content, error;
	if (::panduck::EmbeddedToTable(text, language, content, error)) {
		return Rebuild(block, Value(DuckBlockTypes::TYPE_TABLE), Value(content), Value(DuckBlockTypes::ENCODING_JSON),
		               WithAttrs(attrs, {{"source_text", text}}, {"parse_error"}));
	}
	return Rebuild(block, type, content_value, c[DuckBlockTypes::ENCODING_IDX],
	               WithAttrs(attrs, {{"parse_error", error}}, {}));
}

void ParseEmbeddedImpl(DataChunk &args, Vector &result, bool has_formats) {
	auto count = args.size();
	for (idx_t i = 0; i < count; i++) {
		auto blocks = args.data[0].GetValue(i);
		if (blocks.IsNull()) {
			result.SetValue(i, blocks);
			continue;
		}
		vector<string> formats = DefaultFormats();
		if (has_formats) {
			auto f = args.data[1].GetValue(i);
			if (!f.IsNull()) {
				formats.clear();
				for (auto &v : ListValue::GetChildren(f)) {
					if (!v.IsNull()) {
						formats.push_back(StringUtil::Lower(v.GetValue<string>()));
					}
				}
			}
		}
		vector<Value> out;
		for (auto &b : ListValue::GetChildren(blocks)) {
			out.push_back(b.IsNull() ? b : ParseBlock(b, formats));
		}
		result.SetValue(i, Value::LIST(DuckBlockTypes::DuckBlockType(), std::move(out)));
	}
	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

void ParseEmbeddedFun(DataChunk &args, ExpressionState &, Vector &result) {
	ParseEmbeddedImpl(args, result, false);
}

void ParseEmbeddedFormatsFun(DataChunk &args, ExpressionState &, Vector &result) {
	ParseEmbeddedImpl(args, result, true);
}

FunctionDescription Describe(vector<string> parameter_names, const string &description, vector<string> examples) {
	FunctionDescription desc;
	desc.parameter_names = std::move(parameter_names);
	desc.description = description;
	desc.examples = std::move(examples);
	desc.categories = {"panduck"};
	return desc;
}

} // namespace

void RegisterEmbeddedTableFunctions(ExtensionLoader &loader) {
	{
		CreateScalarFunctionInfo info(ScalarFunction("panduck_embedded_to_table",
		                                             {LogicalType::VARCHAR, LogicalType::VARCHAR}, ResultType(),
		                                             EmbeddedToTableFun));
		info.descriptions.push_back(
		    Describe({"text", "format"},
		             "Parse csv, tsv, json (an array of objects) or jsonl text into table block content. Returns "
		             "STRUCT(content, error): the content, or the reason it failed.",
		             {"panduck_embedded_to_table(E'a,b\\n1,2', 'csv').content"}));
		loader.RegisterFunction(std::move(info));
	}
	auto list_type = DuckBlockTypes::DuckBlockListType();
	ScalarFunctionSet set("panduck_blocks_parse_embedded");
	set.AddFunction(ScalarFunction({list_type}, list_type, ParseEmbeddedFun));
	set.AddFunction(
	    ScalarFunction({list_type, LogicalType::LIST(LogicalType::VARCHAR)}, list_type, ParseEmbeddedFormatsFun));
	CreateScalarFunctionInfo info(std::move(set));
	const string what = "Turn each code block whose language is in formats (default csv, tsv, json, jsonl) into a "
	                    "table block. The source text is kept in attributes['source_text']. A block that fails to "
	                    "parse stays a code block, with the reason in attributes['parse_error'].";
	// ONE DESCRIPTION PER OVERLOAD, in overload order, each naming its own parameters.
	info.descriptions.push_back(Describe({"blocks"}, what, {"panduck_blocks_parse_embedded(blocks)"}));
	info.descriptions.push_back(
	    Describe({"blocks", "formats"}, what, {"panduck_blocks_parse_embedded(blocks, ['csv'])"}));
	loader.RegisterFunction(std::move(info));
}

} // namespace panduck
} // namespace duckdb
