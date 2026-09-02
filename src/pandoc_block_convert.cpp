#include "pandoc_block_convert.hpp"
#include "block_normalize.hpp"
// For the pandoc-api-version triple, which both export paths in this file derive rather
// than spell out -- see API_VERSION_PATCH's comment for why that matters.
#include "pandoc_ast_map.hpp"

#include <set>
#include "pandoc_convert_util.hpp"
#include "pandoc_inline_convert.hpp"
#include "duck_block_types.hpp"
#include "duckdb_compat.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/table_function.hpp"

#include <sstream>
#include <vector>
#include <map>
#include <fstream>
#include <utility>

namespace duckdb {

// Helper to create an attributes MAP
static Value CreateAttrsMap(const map<string, string> &attrs) {
	vector<Value> keys;
	vector<Value> values;
	for (auto &entry : attrs) {
		keys.push_back(Value(entry.first));
		values.push_back(Value(entry.second));
	}
	return Value::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR, keys, values);
}

// Helper to create a single duck_block Value (for block).
// `level` is NULL for top-level blocks; blocks nested inside a Div carry
// their nesting level so the emit side can reconstruct the tree (issue #11:
// div children were dropped on emit because every parsed block had a NULL
// level).
static Value CreateDocBlock(const string &block_type, const string &content, const map<string, string> &attrs,
                            int32_t order, const string &encoding = "text", const Value &level = Value()) {
	child_list_t<Value> struct_values;
	struct_values.push_back(make_pair("kind", Value(DuckBlockTypes::KIND_BLOCK)));
	struct_values.push_back(make_pair("element_type", Value(block_type)));
	struct_values.push_back(make_pair("content", Value(content)));
	struct_values.push_back(make_pair("level", level));
	struct_values.push_back(make_pair("encoding", Value(encoding)));
	struct_values.push_back(make_pair("attributes", CreateAttrsMap(attrs)));
	struct_values.push_back(make_pair("element_order", Value(order)));
	return Value::STRUCT(std::move(struct_values));
}

// kind='value' elements model Pandoc's recursive MetaValue tree. They are appended
// AFTER the document's blocks so that blocks[1] keeps pointing at the first content
// block; consumers must filter on `kind` rather than index blindly, which is already
// true for inlines and merely less obvious.
static Value CreateDocValue(const string &value_type, const string &content, const map<string, string> &attrs,
                            int32_t order, const Value &level) {
	child_list_t<Value> struct_values;
	struct_values.push_back(make_pair("kind", Value(DuckBlockTypes::KIND_VALUE)));
	struct_values.push_back(make_pair("element_type", Value(value_type)));
	struct_values.push_back(make_pair("content", Value(content)));
	struct_values.push_back(make_pair("level", level));
	struct_values.push_back(make_pair("encoding", Value("text")));
	struct_values.push_back(make_pair("attributes", CreateAttrsMap(attrs)));
	struct_values.push_back(make_pair("element_order", Value(order)));
	return Value::STRUCT(std::move(struct_values));
}

using namespace duckdb_yyjson;

static string ValToJsonString(yyjson_val *val) {
	if (!val) {
		return "";
	}
	size_t len = 0;
	char *str = yyjson_val_write(val, 0, &len);
	string res(str ? str : "", len);
	if (str) {
		free(str);
	}
	return res;
}

// A parsed Pandoc attr triple: ["id", ["class", ...], [["key","value"], ...]]
struct PandocAttr {
	string id;
	vector<string> classes;
	vector<std::pair<string, string>> key_values;
};

static void ParsePandocAttrVal(yyjson_val *attr_val, PandocAttr &attr) {
	if (!attr_val || !yyjson_is_arr(attr_val)) {
		return;
	}
	// Element 0: id
	yyjson_val *id_val = yyjson_arr_get(attr_val, 0);
	if (id_val && yyjson_is_str(id_val)) {
		attr.id = string(yyjson_get_str(id_val), yyjson_get_len(id_val));
	}
	// Element 1: classes
	yyjson_val *classes_val = yyjson_arr_get(attr_val, 1);
	if (classes_val && yyjson_is_arr(classes_val)) {
		size_t idx, max;
		yyjson_val *cls;
		yyjson_arr_foreach(classes_val, idx, max, cls) {
			if (yyjson_is_str(cls)) {
				attr.classes.emplace_back(yyjson_get_str(cls), yyjson_get_len(cls));
			}
		}
	}
	// Element 2: key-values
	yyjson_val *kvs_val = yyjson_arr_get(attr_val, 2);
	if (kvs_val && yyjson_is_arr(kvs_val)) {
		size_t idx, max;
		yyjson_val *kv;
		yyjson_arr_foreach(kvs_val, idx, max, kv) {
			if (yyjson_is_arr(kv) && yyjson_arr_size(kv) >= 2) {
				yyjson_val *k = yyjson_arr_get(kv, 0);
				yyjson_val *v = yyjson_arr_get(kv, 1);
				if (k && yyjson_is_str(k) && v && yyjson_is_str(v)) {
					attr.key_values.emplace_back(string(yyjson_get_str(k), yyjson_get_len(k)),
					                             string(yyjson_get_str(v), yyjson_get_len(v)));
				}
			}
		}
	}
}

static bool IsReservedAttrKey(const string &key) {
	return key == "id" || key == "class" || key == DuckBlockTypes::ATTR_HEADING_LEVEL || key == "language" ||
	       key == DuckBlockTypes::ATTR_LIST_TYPE || key == "format" || key == "src" || key == "alt" || key == "title" ||
	       key == "href" || key == "quote_type" || key == "display";
}

static void StorePandocAttr(const PandocAttr &attr, map<string, string> &attrs) {
	if (!attr.id.empty()) {
		attrs["id"] = attr.id;
	}
	if (!attr.classes.empty()) {
		string joined;
		for (auto &cls : attr.classes) {
			if (!joined.empty()) {
				joined += " ";
			}
			joined += cls;
		}
		attrs["class"] = joined;
	}
	for (auto &kv : attr.key_values) {
		if (IsReservedAttrKey(kv.first) || attrs.find(kv.first) != attrs.end()) {
			continue;
		}
		attrs[kv.first] = kv.second;
	}
}

static void ExtractInlinesTextValInto(yyjson_val *node, string &out, idx_t depth);

//! `depth` IS LOAD-BEARING, and it went missing for a day.
//!
//! This function and ExtractInlinesTextValInto are MUTUALLY RECURSIVE, and the recursion is
//! driven entirely by the shape of the input document. A Pandoc AST is
//! document-controlled -- anyone handing panduck a .json can choose its nesting -- so an
//! unbounded cycle here is a crash reachable from a file, not a theoretical concern.
//!
//! MEASURED, on a DefinitionList term holding N nested Emph:
//!
//!     depth 10,000  ->  reads fine
//!     depth 50,000  ->  SIGSEGV, core dumped
//!
//! The guard was lost when this function gained its Link/Image arms. The fix that recovered
//! formatted cell text ADDED the recursion that most needs a bound and dropped the
//! parameter carrying it, in the same edit -- so the change was right about the data and
//! wrong about the depth, and nothing in this repo's suite could see it because a fixture
//! that nests 50,000 deep is not a fixture anyone writes.
//!
//! Found by duckeye, statically, by diffing this file against duck_block_utils' copy: their
//! CheckPandocDepth appears 8 times and ours appeared 7. The defect existed only in the
//! RELATIONSHIP between two copies that each passed their own suite.
static string ExtractInlinesTextVal(yyjson_val *inlines_arr, idx_t depth = 0) {
	CheckPandocDepth(depth);
	if (!inlines_arr) {
		return "";
	}
	if (yyjson_is_str(inlines_arr)) {
		return string(yyjson_get_str(inlines_arr), yyjson_get_len(inlines_arr));
	}
	string result;
	auto process_item = [&](yyjson_val *item) {
		if (!yyjson_is_obj(item)) {
			return;
		}
		yyjson_val *t_val = yyjson_obj_get(item, "t");
		if (!t_val || !yyjson_is_str(t_val)) {
			return;
		}
		const char *t = yyjson_get_str(t_val);
		yyjson_val *c_val = yyjson_obj_get(item, "c");
		if (strcmp(t, "Str") == 0) {
			if (c_val && yyjson_is_str(c_val)) {
				result.append(yyjson_get_str(c_val), yyjson_get_len(c_val));
			}
		} else if (strcmp(t, "Space") == 0 || strcmp(t, "SoftBreak") == 0) {
			result += " ";
		} else if (strcmp(t, "LineBreak") == 0) {
			result += "\n";
		} else if (strcmp(t, "Code") == 0 || strcmp(t, "Math") == 0 || strcmp(t, "RawInline") == 0) {
			// SHAPE THREE: `c` is [attr-or-mathtype, "text"] -- the text is a BARE STRING and
			// is unreachable by descending at any depth. This is the one genuinely
			// unavoidable per-constructor arm, because a bare string is INDISTINGUISHABLE
			// from a Link's URL: a flattener that takes every string it walks past leaks
			// URLs and attr ids into cell text. Taking c[1] by position is what keeps
			// `http://x.example` out of the result while keeping the code text in.
			if (c_val && yyjson_is_arr(c_val) && yyjson_arr_size(c_val) >= 2) {
				yyjson_val *text_val = yyjson_arr_get(c_val, 1);
				if (text_val && yyjson_is_str(text_val)) {
					result.append(yyjson_get_str(text_val), yyjson_get_len(text_val));
				}
			}
		} else if (strcmp(t, "Link") == 0 || strcmp(t, "Image") == 0) {
			// SHAPE TWO: `c` is [attr, [inlines], target]. The inlines are in an INNER ARRAY,
			// and every one of the three elements is an array -- so a walk that only enters
			// objects stops dead here. Taking c[1] explicitly is also what keeps the TARGET
			// out: an Image cell keeps its alt text and not `pic.png`.
			if (c_val && yyjson_is_arr(c_val) && yyjson_arr_size(c_val) >= 2) {
				ExtractInlinesTextValInto(yyjson_arr_get(c_val, 1), result, depth + 1);
			}
		} else if (c_val) {
			// SHAPE ONE: `c` IS the inline list -- Strong, Emph, Underline, Strikeout, Span,
			// Quoted, SmallCaps and the rest. Descending reaches it.
			//
			// THE THREE SHAPES ARE WHY "recurse over the whole c" WAS NOT ENOUGH. I wrote
			// that a Link's inlines fall out of a general descent without a per-constructor
			// arm; measured, `| [text](http://x/p) and \`co**de\` |` still flattened to
			// " and " -- BOTH the link text and the code text lost. Correct about Strong,
			// wrong about the other two, and the fix looked complete because the fixture
			// only had Strong in it.
			//
			// Reported by duck_block_utils, who implemented my description and then measured
			// what it actually did rather than what it was supposed to do.
			ExtractInlinesTextValInto(c_val, result, depth + 1);
		}
	};

	if (yyjson_is_arr(inlines_arr)) {
		size_t idx, max;
		yyjson_val *item;
		yyjson_arr_foreach(inlines_arr, idx, max, item) {
			if (yyjson_is_arr(item)) {
				// NESTED ARRAYS are entered, not skipped: a Link's inlines sit one array
				// deeper than the walk used to reach.
				ExtractInlinesTextValInto(item, result, depth + 1);
				continue;
			}
			process_item(item);
		}
	} else if (yyjson_is_obj(inlines_arr)) {
		process_item(inlines_arr);
	}
	return result;
}

static void ExtractInlinesTextValInto(yyjson_val *node, string &out, idx_t depth) {
	out += ExtractInlinesTextVal(node, depth);
}

// Plain text of a Pandoc [Block] list -- used to project table cells, whose
// contents are blocks, into the flat {headers, rows} schema.
static string ExtractBlocksTextVal(yyjson_val *blocks_arr) {
	if (!blocks_arr || !yyjson_is_arr(blocks_arr)) {
		return string();
	}
	string out;
	size_t idx, max;
	yyjson_val *blk;
	yyjson_arr_foreach(blocks_arr, idx, max, blk) {
		yyjson_val *bc = blk ? yyjson_obj_get(blk, "c") : nullptr;
		if (!bc) {
			continue;
		}
		string piece = ExtractInlinesTextVal(bc);
		if (piece.empty()) {
			continue;
		}
		if (!out.empty()) {
			out += " ";
		}
		out += piece;
	}
	return out;
}

// One Pandoc Row -> a JSON array of its cells' text. Row = [Attr, [Cell]],
// Cell = [Attr, Alignment, RowSpan, ColSpan, [Block]].
static void AppendRowCellsJson(yyjson_val *row, string &out) {
	out += "[";
	yyjson_val *cells = (row && yyjson_is_arr(row) && yyjson_arr_size(row) >= 2) ? yyjson_arr_get(row, 1) : nullptr;
	bool first = true;
	if (cells && yyjson_is_arr(cells)) {
		size_t idx, max;
		yyjson_val *cell;
		yyjson_arr_foreach(cells, idx, max, cell) {
			if (!first) {
				out += ",";
			}
			first = false;
			yyjson_val *cell_blocks =
			    (cell && yyjson_is_arr(cell) && yyjson_arr_size(cell) >= 5) ? yyjson_arr_get(cell, 4) : nullptr;
			string text = ExtractBlocksTextVal(cell_blocks);
			out += "\"";
			for (char c : text) {
				if (c == '"') {
					out += "\\\"";
				} else if (c == '\\') {
					out += "\\\\";
				} else if (c == '\n') {
					out += "\\n";
				} else {
					out += c;
				}
			}
			out += "\"";
		}
	}
	out += "]";
}

// Project a Pandoc Table tuple into the NATIVE {headers, rows} schema.
//
// MEASURED: this extension's own renderer and render_macros.cpp consume it.
// RELAYED (2026-08-31, from those sessions, not verified here): duckdb_markdown's
// writer and webbed's decoder understand the same schema. That was the argument for
// choosing it, and it is worth knowing it is second-hand -- both repos changed
// substantially the same day, and a fact about a moving codebase expires like any
// other. If it matters to a decision, measure it rather than citing this line.
//
// Table = [Attr, Caption, [ColSpec], TableHead, [TableBody], TableFoot]
// TableHead = [Attr, [Row]];  TableBody = [Attr, RowHeadColumns, [Row], [Row]]
static string ProjectTableToNativeJson(yyjson_val *c_val) {
	if (!c_val || !yyjson_is_arr(c_val) || yyjson_arr_size(c_val) < 5) {
		return string();
	}
	string out = "{\"headers\":";
	yyjson_val *head = yyjson_arr_get(c_val, 3);
	yyjson_val *head_rows =
	    (head && yyjson_is_arr(head) && yyjson_arr_size(head) >= 2) ? yyjson_arr_get(head, 1) : nullptr;
	if (head_rows && yyjson_is_arr(head_rows) && yyjson_arr_size(head_rows) > 0) {
		AppendRowCellsJson(yyjson_arr_get(head_rows, 0), out);
	} else {
		out += "[]";
	}
	out += ",\"rows\":[";
	yyjson_val *bodies = yyjson_arr_get(c_val, 4);
	bool first_row = true;
	if (bodies && yyjson_is_arr(bodies)) {
		size_t bidx, bmax;
		yyjson_val *body;
		yyjson_arr_foreach(bodies, bidx, bmax, body) {
			if (!body || !yyjson_is_arr(body) || yyjson_arr_size(body) < 4) {
				continue;
			}
			// body[2] is intermediate head rows, body[3] the data rows.
			for (idx_t which : {size_t(2), size_t(3)}) {
				yyjson_val *rows = yyjson_arr_get(body, which);
				if (!rows || !yyjson_is_arr(rows)) {
					continue;
				}
				size_t ridx, rmax;
				yyjson_val *row;
				yyjson_arr_foreach(rows, ridx, rmax, row) {
					if (!first_row) {
						out += ",";
					}
					first_row = false;
					AppendRowCellsJson(row, out);
				}
			}
		}
	}
	out += "]}";
	return out;
}

static bool InlinesAreTextOnly(const vector<Value> &inlines) {
	for (auto &el : inlines) {
		if (el.IsNull()) {
			continue;
		}
		auto &children = StructValue::GetChildren(el);
		if (children[DuckBlockTypes::ELEMENT_TYPE_IDX].IsNull()) {
			continue;
		}
		auto element_type = children[DuckBlockTypes::ELEMENT_TYPE_IDX].GetValue<string>();
		// BREAKS ARE NOT TEXT. They were listed here, which asserted that a run
		// containing them survives being flattened into `content` -- and it does not:
		//
		//   Para[Str a, LineBreak, Str b]  ->  content "a\nb"  ->  Str "a\nb"
		//   Para[Str a, SoftBreak, Str b]  ->  content "a b"   ->  Str "a b"
		//
		// A HARD break came back as a raw newline inside a Str, and a SOFT break was
		// gone outright. Two distinct constructors collapsing onto one character, and
		// then onto no constructor at all. Same root cause as the line block that
		// destroyed its bold and links, one path over -- found by duckdb_markdown, who
		// went looking here after that fix made their rich case work.
		//
		// Consequence: a run containing any break now grows inline children, which is
		// where the distinction can live. Prose without breaks is untouched.
		if (element_type != DuckBlockTypes::INLINE_TEXT && element_type != DuckBlockTypes::INLINE_SPACE) {
			return false;
		}
	}
	return true;
}

static void ProcessPandocBlockVal(yyjson_val *block_val, int32_t &order, vector<Value> &result, idx_t depth,
                                  int32_t parent_div_level) {
	CheckPandocDepth(depth);
	if (!block_val || !yyjson_is_obj(block_val)) {
		return;
	}

	const int32_t effective_level = (parent_div_level == 0) ? 1 : parent_div_level + 1;
	// Every element carries an EXPLICIT structural level -- there are no NULLs.
	// `level` is depth in a depth-first ordering, and level plus adjacency together
	// describe the whole document tree, which is why it cannot be optional.
	// (Teague, 2026-08-31: this was always the rule; the NULL-at-top-level
	// normalisation was never approved. Spec 3.0 restores it.)
	const Value block_level = Value(effective_level);

	yyjson_val *t_val = yyjson_obj_get(block_val, "t");
	if (!t_val || !yyjson_is_str(t_val)) {
		return;
	}
	const char *pandoc_type = yyjson_get_str(t_val);
	yyjson_val *c_val = yyjson_obj_get(block_val, "c");

	map<string, string> attrs;
	string content;
	string block_type;
	string encoding = "text";
	yyjson_val *inlines_val_p = nullptr;
	// LineBlock's `c` is an array OF arrays, so it cannot go through inlines_val_p --
	// the inline converter would misparse it. Kept aside and walked line by line below.
	yyjson_val *lineblock_lines = nullptr;

	if (strcmp(pandoc_type, "Header") == 0) {
		block_type = DuckBlockTypes::TYPE_HEADING;
		if (c_val && yyjson_is_arr(c_val) && yyjson_arr_size(c_val) >= 3) {
			yyjson_val *level_val = yyjson_arr_get(c_val, 0);
			yyjson_val *attr_val = yyjson_arr_get(c_val, 1);
			yyjson_val *inlines_val = yyjson_arr_get(c_val, 2);

			int32_t level = yyjson_is_num(level_val) ? yyjson_get_int(level_val) : 1;
			attrs[DuckBlockTypes::ATTR_HEADING_LEVEL] = std::to_string(level);

			PandocAttr pattr;
			ParsePandocAttrVal(attr_val, pattr);
			StorePandocAttr(pattr, attrs);

			content = ExtractInlinesTextVal(inlines_val);
			inlines_val_p = inlines_val;
		}
	} else if (strcmp(pandoc_type, "Para") == 0 || strcmp(pandoc_type, "Plain") == 0) {
		// Plain and Para are DIFFERENT constructors and this collapsed them, which is
		// how tight-vs-loose list items were lost. Plain is a block-level text run
		// with no paragraph semantics; Para is a paragraph.
		block_type = (strcmp(pandoc_type, "Plain") == 0) ? DuckBlockTypes::TYPE_PLAIN : DuckBlockTypes::TYPE_PARAGRAPH;

		// NOT promoted to a block `image` when the only child is an Image, though the
		// write-only sweep flagged it. Unlike `section` and `page_break`, the exporter
		// writes NO recoverable marker here because Pandoc has none to write: Para[Image]
		// is the only encoding it has, and a block image and a paragraph containing one
		// image are genuinely the same document to it. Promoting would invent a
		// distinction the source cannot carry, and it broke the existing behaviour that
		// a Para[Image] yields an INLINE image with its alt text and src.
		//
		// So this asymmetry is inherent rather than a defect -- worth recording, since
		// the sweep will flag it again and the next person needs to know it was checked.
		if (c_val) {
			content = ExtractInlinesTextVal(c_val);
			inlines_val_p = c_val;
		}
	} else if (strcmp(pandoc_type, "CodeBlock") == 0) {
		block_type = DuckBlockTypes::TYPE_CODE;
		if (c_val && yyjson_is_arr(c_val) && yyjson_arr_size(c_val) >= 2) {
			yyjson_val *attr_val = yyjson_arr_get(c_val, 0);
			yyjson_val *code_val = yyjson_arr_get(c_val, 1);

			PandocAttr pattr;
			ParsePandocAttrVal(attr_val, pattr);
			if (!pattr.classes.empty()) {
				attrs["language"] = pattr.classes[0];
			}
			StorePandocAttr(pattr, attrs);

			if (code_val && yyjson_is_str(code_val)) {
				content = string(yyjson_get_str(code_val), yyjson_get_len(code_val));
				// TRAILING NEWLINES ARE TRIMMED, matching every panduck native reader.
				//
				// Pandoc keeps them: its own Org reader gives `print("hi")\n`, 12 bytes for
				// an 11-byte line. panduck's org, latex, rst, ipynb and epub readers all
				// trim -- five independently written readers reaching the same conclusion,
				// which makes it a convention rather than an accident: a source line's
				// TERMINATOR is not its content.
				//
				// Left alone, panduck answered the same question two ways depending on which
				// path a user took -- read handwritten.org natively and get 11 bytes, read
				// `pandoc -f org -t json` of it through here and get 12. That is the
				// disagreement two implementations of one rule cannot detect about
				// themselves, and it was invisible until both lived in one repo:
				// round-trip stability cannot see it, because a consistently different
				// answer round-trips perfectly.
				//
				// So this diverges from the AST it was handed, deliberately, and it is the
				// one place the converter does. A user's answer must not depend on the route
				// they took to it. The pandoc-faithful behaviour is a `pandoc_compat`
				// candidate -- that is where "match the reference exactly" belongs, rather
				// than in a silent per-path difference.
				while (!content.empty() && (content.back() == '\n' || content.back() == '\r')) {
					content.pop_back();
				}
			}
		}
	} else if (strcmp(pandoc_type, "BlockQuote") == 0) {
		// STRUCTURAL. Was encoding='json', which put raw Pandoc AST on the screen in
		// every renderer that showed `content` -- three of them did.
		block_type = DuckBlockTypes::TYPE_BLOCKQUOTE;
		result.push_back(CreateDocBlock(block_type, "", attrs, order++, encoding, block_level));
		if (c_val && yyjson_is_arr(c_val)) {
			size_t idx, max;
			yyjson_val *child_block;
			yyjson_arr_foreach(c_val, idx, max, child_block) {
				ProcessPandocBlockVal(child_block, order, result, depth + 1, effective_level);
			}
		}
		return;
	} else if (strcmp(pandoc_type, "BulletList") == 0 || strcmp(pandoc_type, "OrderedList") == 0) {
		// STRUCTURAL, not opaque JSON. This used to store the whole Pandoc `c` as
		// encoding='json', which made decoding every consumer's problem and left the
		// same four defects in three independent implementations -- and, unnoticed by
		// any of them, exported back to an EMPTY BulletList, because the exporter
		// walks children and there were none. See "encoding='json' does not say whose
		// json" in docs/duck_blocks_spec.md.
		//
		// Emits list -> list_item at level+1 -> the item's own blocks at level+2, the
		// shape the builders already produce and the exporter already understands.
		const bool is_ordered = (strcmp(pandoc_type, "OrderedList") == 0);
		block_type = DuckBlockTypes::TYPE_LIST;
		attrs[DuckBlockTypes::ATTR_LIST_TYPE] = is_ordered ? DuckBlockTypes::LIST_TYPE_ORDERED : DuckBlockTypes::LIST_TYPE_BULLET;
		// `ordered` is the attribute spec v1.0 documents for this; `list_type` arrived
		// later with this reader and nothing ever said which was canonical. Emitting
		// only list_type meant a consumer written against the PUBLISHED v1 spec read
		// nothing at all from a Pandoc-produced list. Both are emitted; v1's name is
		// the canonical one.
		attrs[DuckBlockTypes::ATTR_ORDERED_LEGACY] = is_ordered ? "true" : "false";

		yyjson_val *items_arr = c_val;
		if (is_ordered && c_val && yyjson_is_arr(c_val) && yyjson_arr_size(c_val) >= 2) {
			// OrderedList c = [ListAttributes, [[Block]]] where
			// ListAttributes = [start, {t: style}, {t: delim}]. The start number lived
			// only inside the JSON, so a consumer reading attributes could not find it
			// and every ordered list restarted at 1.
			yyjson_val *list_attrs = yyjson_arr_get(c_val, 0);
			if (list_attrs && yyjson_is_arr(list_attrs) && yyjson_arr_size(list_attrs) >= 3) {
				yyjson_val *start_val = yyjson_arr_get(list_attrs, 0);
				if (start_val && yyjson_is_int(start_val)) {
					attrs["start"] = to_string(yyjson_get_int(start_val));
				}
				for (idx_t k = 1; k < 3; k++) {
					yyjson_val *spec = yyjson_arr_get(list_attrs, k);
					if (!spec || !yyjson_is_obj(spec)) {
						continue;
					}
					yyjson_val *tag = yyjson_obj_get(spec, "t");
					if (tag && yyjson_is_str(tag)) {
						attrs[k == 1 ? "number_style" : "number_delim"] = yyjson_get_str(tag);
					}
				}
			}
			items_arr = yyjson_arr_get(c_val, 1);
		}

		result.push_back(CreateDocBlock(block_type, "", attrs, order++, encoding, block_level));

		if (items_arr && yyjson_is_arr(items_arr)) {
			size_t idx, max;
			yyjson_val *item_val;
			yyjson_arr_foreach(items_arr, idx, max, item_val) {
				map<string, string> item_attrs;
				result.push_back(CreateDocBlock(DuckBlockTypes::TYPE_LIST_ITEM, "", item_attrs, order++, "text",
				                                Value(effective_level + 1)));
				if (item_val && yyjson_is_arr(item_val)) {
					size_t bidx, bmax;
					yyjson_val *item_block;
					yyjson_arr_foreach(item_val, bidx, bmax, item_block) {
						ProcessPandocBlockVal(item_block, order, result, depth + 1, effective_level + 1);
					}
				}
			}
		}
		return;
	} else if (strcmp(pandoc_type, "DefinitionList") == 0) {
		// A definition list IS A LIST KIND -- `list` with list_type='definition',
		// terms and definitions as list_items distinguished by attributes['role'].
		//
		// That is the extensibility `list_type` was made canonical FOR: a boolean
		// cannot say DuckBlockTypes::LIST_TYPE_DEFINITION, which is why `ordered` lost. Zero new types, and
		// every consumer that already walks lists gets definition lists free.
		//
		// It was opaque JSON before, which meant it RENDERED ITS OWN AST to the screen
		// and its serialisation polluted search -- worse than table, which merely
		// rendered nothing.
		//
		// DefinitionList c = [([Inline], [[Block]])] -- term/definitions pairs.
		block_type = DuckBlockTypes::TYPE_LIST;
		attrs[DuckBlockTypes::ATTR_LIST_TYPE] = DuckBlockTypes::LIST_TYPE_DEFINITION;
		attrs[DuckBlockTypes::ATTR_ORDERED_LEGACY] = "false";
		result.push_back(CreateDocBlock(block_type, "", attrs, order++, encoding, block_level));

		if (c_val && yyjson_is_arr(c_val)) {
			size_t idx, max;
			yyjson_val *pair;
			yyjson_arr_foreach(c_val, idx, max, pair) {
				if (!pair || !yyjson_is_arr(pair) || yyjson_arr_size(pair) < 2) {
					continue;
				}
				yyjson_val *term_inlines = yyjson_arr_get(pair, 0);
				yyjson_val *definitions = yyjson_arr_get(pair, 1);

				map<string, string> term_attrs;
				term_attrs[DuckBlockTypes::ATTR_ROLE] = DuckBlockTypes::ROLE_TERM;
				result.push_back(CreateDocBlock(DuckBlockTypes::TYPE_LIST_ITEM, "", term_attrs, order++, "text",
				                                Value(effective_level + 1)));
				// The term is a bare inline run, so its block-level home is `plain`.
				map<string, string> no_attrs;
				result.push_back(CreateDocBlock(DuckBlockTypes::TYPE_PLAIN, ExtractInlinesTextVal(term_inlines), no_attrs,
				                                order++, "text", Value(effective_level + 2)));

				if (definitions && yyjson_is_arr(definitions)) {
					size_t didx, dmax;
					yyjson_val *def_blocks;
					yyjson_arr_foreach(definitions, didx, dmax, def_blocks) {
						map<string, string> def_attrs;
						def_attrs[DuckBlockTypes::ATTR_ROLE] = DuckBlockTypes::LIST_TYPE_DEFINITION;
						result.push_back(CreateDocBlock(DuckBlockTypes::TYPE_LIST_ITEM, "", def_attrs, order++, "text",
						                                Value(effective_level + 1)));
						if (def_blocks && yyjson_is_arr(def_blocks)) {
							size_t bidx, bmax;
							yyjson_val *blk;
							yyjson_arr_foreach(def_blocks, bidx, bmax, blk) {
								ProcessPandocBlockVal(blk, order, result, depth + 1, effective_level + 1);
							}
						}
					}
				}
			}
		}
		return;
	} else if (strcmp(pandoc_type, "Table") == 0) {
		// NATIVE {headers, rows} in content, full Pandoc tuple in an attribute.
		//
		// `table` had TWO json schemas under one element_type -- exactly the `list`
		// defect 2.0 was created to remove, still live for this type. The reader
		// emitted the Pandoc tuple, which NOTHING understood: tables rendered as
		// nothing, and to_text emitted the raw AST so table text was unsearchable
		// while "AlignDefault" polluted every search.
		//
		// The projection is lossy -- it flattens colspan, rowspan, alignment and
		// multiple bodies -- which is exactly why the tuple is kept verbatim beside
		// it. Nothing is lost; the renderable form is simply the one in `content`.
		block_type = DuckBlockTypes::TYPE_TABLE;
		encoding = "json";
		content = ProjectTableToNativeJson(c_val);
		attrs[DuckBlockTypes::ATTR_PANDOC_AST] = ValToJsonString(c_val);
		if (content.empty()) {
			content = ValToJsonString(c_val);
			attrs.erase(DuckBlockTypes::ATTR_PANDOC_AST);
		}
	} else if (strcmp(pandoc_type, "LineBlock") == 0) {
		// LineBlock c = [[Inline]] -- an array OF ARRAYS, one per line.
		//
		// Flattening every line to text was DESTRUCTIVE, not merely lossy: a line
		// containing anything but Str and Space lost it, and a line whose only content
		// was a Link came back EMPTY. `| plain **bold**` exported as `| plain`.
		// Every other block type has carried rich inlines as children since 1.x; this
		// one kept the text-only path it was written with, and the round trip could
		// not see it because a LineBlock of plain text -- the only case anyone tests --
		// is genuinely lossless.
		//
		// Reported by duckdb_markdown as a hard break degrading to a soft one. That was
		// the visible half; this is what was underneath it.
		block_type = DuckBlockTypes::TYPE_LINEBLOCK;
		if (c_val && yyjson_is_arr(c_val)) {
			string joined;
			size_t idx, max;
			yyjson_val *line;
			yyjson_arr_foreach(c_val, idx, max, line) {
				if (idx > 0) {
					joined += "\n";
				}
				joined += ExtractInlinesTextVal(line);
			}
			content = joined;
			lineblock_lines = c_val;
		}
	} else if (strcmp(pandoc_type, "HorizontalRule") == 0) {
		block_type = DuckBlockTypes::TYPE_HR;
		content = "";
	} else if (strcmp(pandoc_type, "RawBlock") == 0) {
		block_type = DuckBlockTypes::TYPE_RAW;
		if (c_val && yyjson_is_arr(c_val) && yyjson_arr_size(c_val) >= 2) {
			yyjson_val *fmt = yyjson_arr_get(c_val, 0);
			yyjson_val *str = yyjson_arr_get(c_val, 1);
			if (fmt && yyjson_is_str(fmt)) {
				attrs["format"] = string(yyjson_get_str(fmt), yyjson_get_len(fmt));
			}
			if (str && yyjson_is_str(str)) {
				content = string(yyjson_get_str(str), yyjson_get_len(str));
			}
		}
	} else if (strcmp(pandoc_type, "Figure") == 0) {
		// Figure c = [Attr, Caption, [Block]] where Caption = [ShortCaption?, [Block]].
		// A figure carries TWO block lists, so the flat duck_block list must keep them
		// distinguishable: content blocks are emitted first at level+1, then a
		// `caption` container at level+1 whose own children are the caption blocks.
		// Content-before-caption so a renderer walking the list in order emits the
		// image before the words describing it.
		block_type = DuckBlockTypes::TYPE_FIGURE;
		if (c_val && yyjson_is_arr(c_val) && yyjson_arr_size(c_val) >= 3) {
			yyjson_val *attr_val = yyjson_arr_get(c_val, 0);
			yyjson_val *caption_val = yyjson_arr_get(c_val, 1);
			yyjson_val *blocks_arr = yyjson_arr_get(c_val, 2);

			PandocAttr pattr;
			ParsePandocAttrVal(attr_val, pattr);
			StorePandocAttr(pattr, attrs);

			result.push_back(CreateDocBlock(block_type, "", attrs, order++, encoding, block_level));

			if (blocks_arr && yyjson_is_arr(blocks_arr)) {
				size_t idx, max;
				yyjson_val *child_block;
				yyjson_arr_foreach(blocks_arr, idx, max, child_block) {
					ProcessPandocBlockVal(child_block, order, result, depth + 1, effective_level);
				}
			}

			// Emit the caption container only when the caption actually has blocks,
			// then recurse so caption formatting (bold, links) survives as real
			// inline children rather than being flattened to text.
			if (caption_val && yyjson_is_arr(caption_val) && yyjson_arr_size(caption_val) >= 2) {
				yyjson_val *short_val = yyjson_arr_get(caption_val, 0);
				yyjson_val *cap_blocks = yyjson_arr_get(caption_val, 1);
				if (cap_blocks && yyjson_is_arr(cap_blocks) && yyjson_arr_size(cap_blocks) > 0) {
					map<string, string> cap_attrs;
					if (short_val && yyjson_is_arr(short_val)) {
						string short_text = ExtractInlinesTextVal(short_val);
						if (!short_text.empty()) {
							cap_attrs["short_caption"] = short_text;
						}
					}
					// The caption container is a SIBLING of the content blocks -- both
					// are children of the figure -- so it sits at effective_level + 1,
					// and its own children are recursed one deeper again.
					result.push_back(CreateDocBlock(DuckBlockTypes::TYPE_CAPTION, "", cap_attrs, order++, "text",
					                                Value(effective_level + 1)));
					size_t idx, max;
					yyjson_val *cap_block;
					yyjson_arr_foreach(cap_blocks, idx, max, cap_block) {
						ProcessPandocBlockVal(cap_block, order, result, depth + 1, effective_level + 1);
					}
				}
			}
			return;
		}
	} else if (strcmp(pandoc_type, "Div") == 0) {
		block_type = DuckBlockTypes::TYPE_DIV;
		if (c_val && yyjson_is_arr(c_val) && yyjson_arr_size(c_val) >= 2) {
			yyjson_val *attr_val = yyjson_arr_get(c_val, 0);
			yyjson_val *blocks_arr = yyjson_arr_get(c_val, 1);

			PandocAttr pattr;
			ParsePandocAttrVal(attr_val, pattr);
			StorePandocAttr(pattr, attrs);

			// A Div carrying a sectioning role is a `section`, not a `div`. Pandoc has
			// no Section constructor, so the exporter writes one as a Div whose class is
			// the role -- and this reader never read it back, making `section` WRITE-ONLY
			// from the Pandoc path: section -> Div classed 'article' -> div. The type
			// survived a round trip as a different, less specific type, which is the
			// asymmetry `generic` exists to prevent and this had instead of it.
			//
			// The role set is HTML5's sectioning elements exactly, which is why an HTML
			// reader mapping <article> onto role='article' is reading the spec rather
			// than guessing from the names lining up.
			{
				auto role_it = attrs.find(DuckBlockTypes::ATTR_ROLE);
				string role = role_it != attrs.end() ? role_it->second : string();
				if (role.empty()) {
					auto class_it = attrs.find("class");
					if (class_it != attrs.end()) {
						role = class_it->second;
					}
				}
				static const std::set<string> SECTIONING_ROLES = {"section", "article", "aside", "nav",
				                                                  "header",  "footer",  "main"};
				if (!role.empty() && SECTIONING_ROLES.find(role) != SECTIONING_ROLES.end()) {
					block_type = DuckBlockTypes::TYPE_SECTION;
					attrs[DuckBlockTypes::ATTR_ROLE] = role;
				} else if (role == "page") {
					// Same write-only defect as `section`, one type over: a page_break
					// exports as a Div classed 'page' carrying page_number, and nothing
					// read it back -- so every round trip turned a pagination marker into
					// an anonymous div. Found by sweeping EVERY block type through
					// export-then-read rather than by looking at page_break.
					block_type = DuckBlockTypes::TYPE_PAGE;
					attrs.erase("class");
				}
			}

			result.push_back(CreateDocBlock(block_type, "", attrs, order++, encoding, block_level));

			if (blocks_arr && yyjson_is_arr(blocks_arr)) {
				size_t idx, max;
				yyjson_val *child_block;
				yyjson_arr_foreach(blocks_arr, idx, max, child_block) {
					ProcessPandocBlockVal(child_block, order, result, depth + 1, effective_level);
				}
			}
			return;
		}
	} else {
		// Never drop a constructor silently: preserve it verbatim so document length is
		// stable and the gap stays visible instead of invisible. Serialises the whole
		// constructor object (not just `c`) so export can reconstitute it including `t`.
		block_type = DuckBlockTypes::TYPE_GENERIC;
		encoding = "json";
		attrs[DuckBlockTypes::ATTR_SOURCE_TYPE] = string(pandoc_type);
		content = ValToJsonString(block_val);
	}

	if (block_type.empty()) {
		return;
	}

	const int32_t block_order = order++;
	vector<Value> inline_children;
	if (lineblock_lines) {
		// One line's inlines after another, with a `linebreak` between -- which is what
		// a LineBlock MEANS, and it lets the same either/or rule below decide the shape:
		// InlinesAreTextOnly already counts `linebreak` as text, so a plain-text line
		// block still keeps its newline-joined `content` and emits no children, exactly
		// as before. Only a line block that would have LOST something grows children.
		// Richness is decided PER LINE, before the separators go in. The separators are
		// `linebreak`s this code inserts itself, and breaks no longer count as text --
		// so asking the question of the assembled run would call every multi-line block
		// rich and migrate the plain case that has always lived in `content`.
		bool any_line_rich = false;
		{
			size_t pidx, pmax;
			yyjson_val *pline;
			yyjson_arr_foreach(lineblock_lines, pidx, pmax, pline) {
				int32_t probe_order = 0;
				vector<Value> probe;
				PandocInlineConvert::ConvertPandocInlinesValToDbInlines(pline, effective_level + 1, probe_order, probe,
				                                                        depth);
				if (!InlinesAreTextOnly(probe)) {
					any_line_rich = true;
					break;
				}
			}
		}
		const int32_t order_before_children = order;
		size_t idx, max;
		yyjson_val *line;
		yyjson_arr_foreach(lineblock_lines, idx, max, line) {
			if (idx > 0) {
				map<string, string> no_attrs;
				child_list_t<Value> br;
				br.push_back(make_pair("kind", Value(DuckBlockTypes::KIND_INLINE)));
				br.push_back(make_pair("element_type", Value(DuckBlockTypes::INLINE_LINEBREAK)));
				br.push_back(make_pair("content", Value("")));
				br.push_back(make_pair("level", Value(effective_level + 1)));
				br.push_back(make_pair("encoding", Value(DuckBlockTypes::ENCODING_TEXT)));
				br.push_back(make_pair("attributes", CreateAttrsMap(no_attrs)));
				br.push_back(make_pair("element_order", Value(order++)));
				inline_children.push_back(Value::STRUCT(std::move(br)));
			}
			PandocInlineConvert::ConvertPandocInlinesValToDbInlines(line, effective_level + 1, order, inline_children,
			                                                        depth);
		}
		if (!any_line_rich) {
			inline_children.clear();
			order = order_before_children;
		} else {
			content.clear();
		}
	} else if (inlines_val_p) {
		const int32_t order_before_children = order;
		PandocInlineConvert::ConvertPandocInlinesValToDbInlines(inlines_val_p, effective_level + 1, order,
		                                                        inline_children, depth);
		if (InlinesAreTextOnly(inline_children)) {
			inline_children.clear();
			order = order_before_children;
		} else if (block_type == DuckBlockTypes::TYPE_HEADING) {
			// A HEADING KEEPS BOTH -- duck_block ruling d003d32. Clearing the content here
			// left a formatted heading with NO title in `content`, which every consumer
			// reading an outline depends on: doc_toc, section slugs, table-of-contents
			// queries. The children carry the formatting and the content carries a DERIVED
			// flattening; children are authoritative when both are present, and the shape
			// marks itself because a lone text child produces no children at all.
			//
			// Restricted to `heading` deliberately. Elsewhere two copies of one fact is the
			// shape that hid an image alt-text loss upstream for months.
		} else {
			content.clear();
		}
	}

	result.push_back(CreateDocBlock(block_type, content, attrs, block_order, encoding, block_level));
	for (auto &child : inline_children) {
		result.push_back(child);
	}
}

// Walk one MetaValue into kind='value' elements. `list` and `map` nest their children
// via `level`, exactly as `div` and `figure` do. Recursive, so it honours the depth cap.
static void ProcessPandocMetaVal(const string &key, yyjson_val *val, int32_t &order, vector<Value> &result,
                                 int32_t level, idx_t depth) {
	CheckPandocDepth(depth);
	if (!val || !yyjson_is_obj(val)) {
		return;
	}
	yyjson_val *t_val = yyjson_obj_get(val, "t");
	if (!t_val || !yyjson_is_str(t_val)) {
		return;
	}
	const char *mt = yyjson_get_str(t_val);
	yyjson_val *c_val = yyjson_obj_get(val, "c");

	map<string, string> attrs;
	if (!key.empty()) {
		attrs["key"] = key;
	}

	if (strcmp(mt, "MetaString") == 0) {
		string s;
		if (c_val && yyjson_is_str(c_val)) {
			s = string(yyjson_get_str(c_val), yyjson_get_len(c_val));
		}
		result.push_back(CreateDocValue(DuckBlockTypes::VALUE_STRING, s, attrs, order++, Value(level)));
	} else if (strcmp(mt, "MetaBool") == 0) {
		const bool b = c_val && yyjson_is_true(c_val);
		result.push_back(CreateDocValue(DuckBlockTypes::VALUE_BOOL, b ? "true" : "false", attrs, order++, Value(level)));
	} else if (strcmp(mt, "MetaInlines") == 0) {
		result.push_back(CreateDocValue(DuckBlockTypes::VALUE_INLINES, "", attrs, order++, Value(level)));
		if (c_val) {
			PandocInlineConvert::ConvertPandocInlinesValToDbInlines(c_val, level + 1, order, result, depth);
		}
	} else if (strcmp(mt, "MetaBlocks") == 0) {
		result.push_back(CreateDocValue(DuckBlockTypes::VALUE_BLOCKS, "", attrs, order++, Value(level)));
		if (c_val && yyjson_is_arr(c_val)) {
			size_t i, n;
			yyjson_val *b;
			yyjson_arr_foreach(c_val, i, n, b) {
				ProcessPandocBlockVal(b, order, result, depth + 1, level);
			}
		}
	} else if (strcmp(mt, "MetaList") == 0) {
		result.push_back(CreateDocValue(DuckBlockTypes::VALUE_LIST, "", attrs, order++, Value(level)));
		if (c_val && yyjson_is_arr(c_val)) {
			size_t i, n;
			yyjson_val *e;
			yyjson_arr_foreach(c_val, i, n, e) {
				ProcessPandocMetaVal("", e, order, result, level + 1, depth + 1);
			}
		}
	} else if (strcmp(mt, "MetaMap") == 0) {
		result.push_back(CreateDocValue(DuckBlockTypes::VALUE_MAP, "", attrs, order++, Value(level)));
		if (c_val && yyjson_is_obj(c_val)) {
			size_t i, n;
			yyjson_val *k, *v;
			yyjson_obj_foreach(c_val, i, n, k, v) {
				ProcessPandocMetaVal(string(yyjson_get_str(k), yyjson_get_len(k)), v, order, result, level + 1,
				                     depth + 1);
			}
		}
	} else {
		// Same no-silent-drops rule as blocks and inlines: an unrecognised MetaValue is
		// preserved verbatim rather than discarded.
		attrs[DuckBlockTypes::ATTR_SOURCE_TYPE] = string(mt);
		result.push_back(CreateDocValue(DuckBlockTypes::TYPE_GENERIC, ValToJsonString(val), attrs, order++, Value(level)));
	}
}

void PandocBlockConvert::ConvertPandocAstToBlocks(const string &json, vector<Value> &blocks) {
	if (json.empty()) {
		return;
	}
	yyjson_doc *doc = yyjson_read(json.c_str(), json.size(), 0);
	if (!doc) {
		return;
	}
	yyjson_val *root = yyjson_doc_get_root(doc);
	if (!root) {
		yyjson_doc_free(doc);
		return;
	}

	int32_t order = 0;
	yyjson_val *blocks_val = root;
	if (yyjson_is_obj(root)) {
		yyjson_val *b = yyjson_obj_get(root, "blocks");
		if (b) {
			blocks_val = b;
		}
	}

	if (yyjson_is_arr(blocks_val)) {
		size_t idx, max;
		yyjson_val *block_val;
		yyjson_arr_foreach(blocks_val, idx, max, block_val) {
			ProcessPandocBlockVal(block_val, order, blocks, 1, 0);
		}
	} else if (yyjson_is_obj(blocks_val)) {
		ProcessPandocBlockVal(blocks_val, order, blocks, 1, 0);
	}

	// SPEC 6.0's content rule. Lives in normalize.cpp, not here: the rule is the
	// VOCABULARY's, not Pandoc's, and this file is being handed to duckdb_panduck.
	// Every other producer needs the same pass, and it is exported as
	// duck_blocks_normalize() so they can call it instead of reimplementing it.
	CollapseLonePlainIntoParent(blocks);

	// Document metadata, AFTER the blocks so blocks[1] still points at the first
	// content block. Previously dropped entirely: title, tags, author and draft all
	// round-tripped to {}.
	if (yyjson_is_obj(root)) {
		yyjson_val *meta = yyjson_obj_get(root, "meta");
		if (meta && yyjson_is_obj(meta)) {
			size_t i, n;
			yyjson_val *k, *v;
			yyjson_obj_foreach(meta, i, n, k, v) {
				ProcessPandocMetaVal(string(yyjson_get_str(k), yyjson_get_len(k)), v, order, blocks, 1, 1);
			}
		}
	}

	yyjson_doc_free(doc);
}

void PandocBlockConvert::PandocAstToBlocksFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &json_vec = args.data[0];
	auto count = args.size();

	for (idx_t i = 0; i < count; i++) {
		auto json_val = json_vec.GetValue(i);

		if (json_val.IsNull()) {
			result.SetValue(i, Value::LIST(DuckBlockTypes::DuckBlockType(), vector<Value>()));
			continue;
		}

		string json = json_val.GetValue<string>();
		vector<Value> blocks;
		ConvertPandocAstToBlocks(json, blocks);

		result.SetValue(i, Value::LIST(DuckBlockTypes::DuckBlockType(), std::move(blocks)));
	}
}

static string GetElementStringField(const Value &element, idx_t field_idx) {
	auto &children = StructValue::GetChildren(element);
	if (children[field_idx].IsNull()) {
		return "";
	}
	return children[field_idx].GetValue<string>();
}

static string GetElementAttribute(const Value &element, const string &key) {
	auto &children = StructValue::GetChildren(element);
	auto &attrs = children[DuckBlockTypes::ATTRIBUTES_IDX];
	if (attrs.IsNull()) {
		return "";
	}

	auto &map_entries = MapValue::GetChildren(attrs);
	for (auto &entry : map_entries) {
		if (entry.IsNull()) {
			continue;
		}
		auto &kv = StructValue::GetChildren(entry);
		if (kv.size() >= 2 && !kv[0].IsNull() && kv[0].GetValue<string>() == key) {
			if (!kv[1].IsNull()) {
				return kv[1].GetValue<string>();
			}
		}
	}
	return "";
}

static int32_t GetElementLevel(const Value &element) {
	auto &children = StructValue::GetChildren(element);
	if (children[DuckBlockTypes::LEVEL_IDX].IsNull()) {
		return 1;
	}
	return children[DuckBlockTypes::LEVEL_IDX].GetValue<int32_t>();
}

static yyjson_mut_val *CreatePandocAttrVal(yyjson_mut_doc *doc, const Value &element, const string &fallback_class) {
	auto id = GetElementAttribute(element, "id");
	auto classes = GetElementAttribute(element, "class");
	if (classes.empty()) {
		classes = fallback_class;
	}

	yyjson_mut_val *attr_arr = yyjson_mut_arr(doc);
	yyjson_mut_arr_add_strncpy(doc, attr_arr, id.data(), id.size());

	yyjson_mut_val *classes_arr = yyjson_mut_arr(doc);
	size_t start = 0;
	while (start < classes.length()) {
		size_t space = classes.find(' ', start);
		size_t len = (space == string::npos) ? string::npos : space - start;
		string cls = classes.substr(start, len);
		if (!cls.empty()) {
			yyjson_mut_arr_add_strncpy(doc, classes_arr, cls.data(), cls.size());
		}
		if (space == string::npos) {
			break;
		}
		start = space + 1;
	}
	yyjson_mut_arr_add_val(attr_arr, classes_arr);

	yyjson_mut_val *kvs_arr = yyjson_mut_arr(doc);
	auto &children = StructValue::GetChildren(element);
	auto &attrs = children[DuckBlockTypes::ATTRIBUTES_IDX];
	if (!attrs.IsNull()) {
		auto &map_entries = MapValue::GetChildren(attrs);
		for (auto &entry : map_entries) {
			if (entry.IsNull()) {
				continue;
			}
			auto &kv = StructValue::GetChildren(entry);
			if (kv.size() < 2 || kv[0].IsNull() || kv[1].IsNull()) {
				continue;
			}
			string key = kv[0].GetValue<string>();
			if (IsReservedAttrKey(key)) {
				continue;
			}
			string val = kv[1].GetValue<string>();
			yyjson_mut_val *pair_arr = yyjson_mut_arr(doc);
			yyjson_mut_arr_add_strncpy(doc, pair_arr, key.data(), key.size());
			yyjson_mut_arr_add_strncpy(doc, pair_arr, val.data(), val.size());
			yyjson_mut_arr_add_val(kvs_arr, pair_arr);
		}
	}
	yyjson_mut_arr_add_val(attr_arr, kvs_arr);
	return attr_arr;
}

static yyjson_mut_val *ConvertListToPandocVal(yyjson_mut_doc *doc, const vector<Value> &blocks_list, idx_t &start_idx,
                                              int32_t list_level, idx_t depth);
static void ConvertContainerChildrenToPandocVal(yyjson_mut_doc *doc, const vector<Value> &blocks_list, idx_t &start_idx,
                                                int32_t parent_level, idx_t depth, yyjson_mut_val *target_arr,
                                                yyjson_mut_val *switch_arr, const char *switch_type);
static yyjson_mut_val *ConvertDivToPandocVal(yyjson_mut_doc *doc, const vector<Value> &blocks_list, idx_t &start_idx,
                                             int32_t div_level, idx_t depth);

// A block child no walk enumerates. Emitted as a Div classed with its element_type,
// carrying whatever content it had -- visible and correctly nested rather than
// silently skipped.
//
// Shared by the container walk and the list walk deliberately. Those two had
// SEPARATE terminal arms, and that duplication is the root of this whole class:
// a type added to one dispatch is invisible to the other, which is how table,
// deflist and lineblock came to be dropped inside containers, and code blocks,
// blockquotes and horizontal rules inside list items.
//! The pandoc RawBlock/RawInline format name for a duck_block `raw` element.
//!
//! duck_block documents TYPE_RAW as "literal content in a NAMED format", and `encoding` is
//! the field that names it -- ipynb emits encoding='markdown' for a held-raw markdown cell.
//! So `encoding` is consulted, and it is consulted FIRST after the explicit attribute,
//! because it is the vocabulary's own answer rather than this converter's private key.
//!
//! attributes['format'] still wins where present. It is not in the vocabulary at all, but
//! it is what the export path has always read, and a reader carrying a format the encoding
//! enumeration cannot yet spell -- `mediawiki`, today -- has nowhere else to put it.
//!
//! "html" remains the default, which is what pandoc assumes for unlabelled raw content, and
//! `text` is treated as ABSENT rather than as a format: it is the vocabulary's default
//! encoding, present on every element, so honouring it literally would relabel every raw
//! block in the tree as format "text".
static string ResolveRawFormat(const Value &block) {
	auto format = GetElementAttribute(block, "format");
	if (!format.empty()) {
		return format;
	}
	auto encoding = GetElementStringField(block, DuckBlockTypes::ENCODING_IDX);
	if (!encoding.empty() && encoding != DuckBlockTypes::ENCODING_TEXT) {
		return encoding;
	}
	return "html";
}

static yyjson_mut_val *ConvertUnhandledChildToPandocVal(yyjson_mut_doc *doc, const vector<Value> &blocks_list,
                                                        idx_t &idx, idx_t depth, const Value &child,
                                                        const string &child_type, const string &content,
                                                        const vector<Value> &inline_children, int32_t child_level) {
	// A type Pandoc CAN express exactly should be written exactly, even from the
	// fallback. `hr` reached here from the list and definition walks -- which do not
	// enumerate it the way the container walk does -- and came out as a Div classed
	// "hr": not lost, but degraded, and degraded differently depending on which
	// container it sat in. The same constructor should not depend on its parent.
	//
	// This belongs in the SHARED fallback rather than as another arm in each walk.
	// Adding it per-caller is what produced the divergence in the first place.
	if (child_type == DuckBlockTypes::TYPE_HR) {
		yyjson_mut_val *hr_obj = yyjson_mut_obj(doc);
		yyjson_mut_obj_add_str(doc, hr_obj, "t", "HorizontalRule");
		idx_t skip = idx;
		ConvertContainerChildrenToPandocVal(doc, blocks_list, skip, child_level, depth + 1, yyjson_mut_arr(doc),
		                                    nullptr, nullptr);
		idx = skip;
		return hr_obj;
	}

	// `raw` is the same case as `hr` above, and arrives here for the same reason: the
	// container walk enumerates it at top level and nowhere else, so a raw block nested in
	// a div, list item or blockquote came out as `Div class="raw"` wrapping a Plain.
	//
	// The content survived that; THE FORMAT DID NOT. ipynb's markdown cell reached the AST
	// with "markdown" nowhere in it, which is the one fact `encoding` exists to carry being
	// discarded on export. Degraded, and -- exactly as the hr comment says -- degraded
	// differently depending on which container it sat in.
	if (child_type == DuckBlockTypes::TYPE_RAW) {
		yyjson_mut_val *raw_obj = yyjson_mut_obj(doc);
		yyjson_mut_obj_add_str(doc, raw_obj, "t", "RawBlock");
		yyjson_mut_val *rc = yyjson_mut_arr(doc);
		auto format = ResolveRawFormat(child);
		yyjson_mut_arr_add_strncpy(doc, rc, format.data(), format.size());
		yyjson_mut_arr_add_strncpy(doc, rc, content.data(), content.size());
		yyjson_mut_obj_add_val(doc, raw_obj, "c", rc);
		// A raw block's content is LITERAL, so it has no children to descend into -- but the
		// index still has to advance past any that exist, or they are re-emitted as siblings.
		idx_t skip = idx;
		ConvertContainerChildrenToPandocVal(doc, blocks_list, skip, child_level, depth + 1, yyjson_mut_arr(doc),
		                                    nullptr, nullptr);
		idx = skip;
		return raw_obj;
	}

	yyjson_mut_val *fallback = yyjson_mut_obj(doc);
	yyjson_mut_obj_add_str(doc, fallback, "t", "Div");
	yyjson_mut_val *fc_arr = yyjson_mut_arr(doc);
	yyjson_mut_arr_add_val(fc_arr, CreatePandocAttrVal(doc, child, child_type));
	yyjson_mut_val *fb_blocks = yyjson_mut_arr(doc);
	// The element's own content is NOT written here. The delegated walk below writes
	// it, because under SPEC 6.0 "a container carrying content emits a Plain" is one
	// rule that belongs in one place -- and while this function had its own copy of
	// it, the two both fired and every unhandled child came out with its text twice.
	//
	// The child's OWN descendants. Without this the fallback kept an element's own
	// text and dropped everything below it -- a blockquote inside a list item came
	// through as an empty Div and its quoted paragraph vanished, the same silent
	// loss one level deeper. Delegating means the fallback needs to know nothing
	// about what the subtree contains.
	ConvertContainerChildrenToPandocVal(doc, blocks_list, idx, child_level, depth + 1, fb_blocks, nullptr, nullptr);
	yyjson_mut_arr_add_val(fc_arr, fb_blocks);
	yyjson_mut_obj_add_val(doc, fallback, "c", fc_arr);
	return fallback;
}

static yyjson_mut_val *ConvertListToPandocVal(yyjson_mut_doc *doc, const vector<Value> &blocks_list, idx_t &start_idx,
                                              int32_t list_level, idx_t depth) {
	CheckPandocDepth(depth);
	auto &list_block = blocks_list[start_idx];
	auto list_type = GetElementAttribute(list_block, DuckBlockTypes::ATTR_LIST_TYPE);
	auto ordered_attr = GetElementAttribute(list_block, DuckBlockTypes::ATTR_ORDERED_LEGACY);
	bool is_ordered = (list_type == DuckBlockTypes::LIST_TYPE_ORDERED) || (ordered_attr == "true");
	const bool is_definition = (list_type == DuckBlockTypes::LIST_TYPE_DEFINITION);
	const char *pandoc_type = is_definition ? "DefinitionList" : (is_ordered ? "OrderedList" : "BulletList");

	struct ListItem {
		string content;
		// Under SPEC 6.1 a TIGHT item carries its text in `content` and a LOOSE item
		// has a `paragraph` child. Pandoc spells those Plain and Para, so the flag has
		// to survive to the emit below or the distinction dies on the way out -- which
		// is how it was lost before `plain` existed.
		//
		// This said "a tight item's child is `plain`" until 6.1 narrowed `plain` out of
		// leaf position. The flag and the code were unaffected; the SENTENCE described
		// the shape 5.0 shipped, and would have told the next reader to look for a child
		// that is no longer emitted. A comment can be falsified by a change that does not
		// touch it.
		bool tight = true;
		// attributes['role'] -- 'term' or 'definition' in a definition list.
		string role;
		// (text, tight). Was a bare vector<string>, which could not carry the
		// constructor -- so a second Plain block came back as a Para while the FIRST
		// one round-tripped correctly. `plain` is a first-class distinction as of spec
		// 6.0, and discarding it from block two onward is the same loss the type was
		// minted to stop, just moved past the position anyone was looking at.
		struct ExtraPara {
			string text;
			bool tight;
		};
		vector<ExtraPara> extra_paragraphs;
		// Block children this walk does not enumerate -- a code block, blockquote or
		// horizontal rule inside a list item, all legal Pandoc and all silently
		// dropped before. Carried through rather than skipped.
		vector<yyjson_mut_val *> extra_blocks;
		vector<Value> inlines;
		yyjson_mut_val *nested_list_val = nullptr;
	};
	vector<ListItem> items;
	ListItem current_item;
	bool in_item = false;

	idx_t j = start_idx + 1;
	while (j < blocks_list.size()) {
		auto &child = blocks_list[j];
		if (child.IsNull()) {
			j++;
			continue;
		}

		auto child_kind = GetElementStringField(child, DuckBlockTypes::KIND_IDX);
		auto child_type = GetElementStringField(child, DuckBlockTypes::ELEMENT_TYPE_IDX);
		int32_t child_level = GetElementLevel(child);

		if (child_kind == DuckBlockTypes::KIND_BLOCK) {
			if (child_type == DuckBlockTypes::TYPE_LIST_ITEM && child_level == list_level + 1) {
				if (in_item) {
					items.push_back(current_item);
				}
				current_item = ListItem();
				current_item.content = GetElementStringField(child, DuckBlockTypes::CONTENT_IDX);
				current_item.role = GetElementAttribute(child, DuckBlockTypes::ATTR_ROLE);
				in_item = true;
				j++;
			} else if (child_type == DuckBlockTypes::TYPE_LIST &&
			           (child_level == list_level + 1 || child_level == list_level + 2)) {
				// A nested list is a child of the LIST_ITEM, so it sits at list_level + 2.
				// This only accepted list_level + 1, so every nested list from the Pandoc
				// reader fell past it -- to a bare `j++` before today, meaning nested
				// lists were dropped on export entirely. The +1 case is kept for a list
				// directly under a list, which the builders can produce.
				if (in_item) {
					current_item.nested_list_val = ConvertListToPandocVal(doc, blocks_list, j, child_level, depth + 1);
				} else {
					j++;
				}
			} else if ((child_type == DuckBlockTypes::TYPE_PARAGRAPH || child_type == DuckBlockTypes::TYPE_PLAIN) &&
			           child_level == list_level + 2 && in_item) {
				// Two legal item shapes, so accept both. The builders hang the words on
				// list_item itself; the Pandoc reader emits list_item -> paragraph ->
				// inlines, because a Pandoc list item holds BLOCKS. Reading only the
				// builder shape is what made a Pandoc list export as an empty
				// BulletList -- right number of items, every one of them blank.
				//
				// EVERY paragraph, not just the first. A Pandoc list item holds a list
				// of blocks and <li><p>a</p><p>b</p></li> is ordinary in EPUB and HTML;
				// keeping only the first silently dropped the rest on export. Found by
				// testing the multi-block case panduck asked about before answering
				// them, rather than after.
				auto para_text = GetElementStringField(child, DuckBlockTypes::CONTENT_IDX);
				if (current_item.content.empty() && current_item.extra_paragraphs.empty()) {
					current_item.tight = (child_type == DuckBlockTypes::TYPE_PLAIN);
					current_item.content = para_text;
				} else {
					current_item.extra_paragraphs.push_back({para_text, child_type == DuckBlockTypes::TYPE_PLAIN});
				}
				j++;
			} else if (child_level <= list_level) {
				break;
			} else if (child_level == list_level + 1) {
				// A block directly under the list that is not a list_item or a nested
				// list. Malformed -- Pandoc's BulletList holds only items -- but the
				// bare `j++` here dropped it and its whole subtree, so a list with a
				// stray paragraph exported as `[{"t":"BulletList","c":[]}]` and the text
				// was gone. The RENDERER showed it, so the two disagreed about whether
				// the document contained the words at all.
				//
				// Wrapped as its own item: losing list structure costs formatting,
				// dropping it costs the text. Found by duckdb_markdown reporting the
				// identical defect in their writer's list walk, and checking here
				// rather than assuming the shape did not transfer.
				if (in_item) {
					items.push_back(current_item);
					current_item = ListItem();
					in_item = false;
				}
				auto stray_content = GetElementStringField(child, DuckBlockTypes::CONTENT_IDX);
				vector<Value> stray_inlines;
				for (idx_t k = j + 1; k < blocks_list.size(); k++) {
					auto &inl = blocks_list[k];
					if (inl.IsNull()) {
						continue;
					}
					if (GetElementStringField(inl, DuckBlockTypes::KIND_IDX) != DuckBlockTypes::KIND_INLINE ||
					    GetElementLevel(inl) <= child_level) {
						break;
					}
					stray_inlines.push_back(inl);
				}
				ListItem stray_item;
				stray_item.extra_blocks.push_back(ConvertUnhandledChildToPandocVal(
				    doc, blocks_list, j, depth, child, child_type, stray_content, stray_inlines, child_level));
				items.push_back(stray_item);
			} else if (in_item && child_level == list_level + 2) {
				// NEVER SILENTLY DROP -- this was a bare `j++`, so a code block,
				// blockquote or horizontal rule inside a list item vanished. Same
				// defect as the container walk had, one function over, which is why
				// the fallback is now shared rather than written twice.
				auto child_content = GetElementStringField(child, DuckBlockTypes::CONTENT_IDX);
				vector<Value> child_inlines;
				for (idx_t k = j + 1; k < blocks_list.size(); k++) {
					auto &inl = blocks_list[k];
					if (inl.IsNull()) {
						continue;
					}
					if (GetElementStringField(inl, DuckBlockTypes::KIND_IDX) != DuckBlockTypes::KIND_INLINE ||
					    GetElementLevel(inl) <= child_level) {
						break;
					}
					child_inlines.push_back(inl);
				}
				current_item.extra_blocks.push_back(ConvertUnhandledChildToPandocVal(
				    doc, blocks_list, j, depth, child, child_type, child_content, child_inlines, child_level));
			} else {
				j++;
			}
		} else if (child_kind == DuckBlockTypes::KIND_INLINE && in_item) {
			// level+2 is the builder shape, level+3 the Pandoc one (under a paragraph).
			if (child_level == list_level + 2 || child_level == list_level + 3) {
				current_item.inlines.push_back(child);
			}
			j++;
		} else {
			j++;
		}
	}

	if (in_item) {
		items.push_back(current_item);
	}

	start_idx = j;

	yyjson_mut_val *root_obj = yyjson_mut_obj(doc);
	yyjson_mut_obj_add_str(doc, root_obj, "t", pandoc_type);

	yyjson_mut_val *c_outer = nullptr;
	yyjson_mut_val *items_arr = yyjson_mut_arr(doc);

	if (is_definition) {
		// DefinitionList c = [([Inline], [[Block]])]. Terms and definitions arrive as
		// sibling list_items tagged by role, so pair each term with the definitions
		// that follow it before the next term.
		yyjson_mut_obj_add_val(doc, root_obj, "c", items_arr);
		for (idx_t k = 0; k < items.size();) {
			if (items[k].role != DuckBlockTypes::ROLE_TERM) {
				k++;
				continue;
			}
			yyjson_mut_val *pair = yyjson_mut_arr(doc);
			yyjson_mut_val *term_inls = yyjson_mut_arr(doc);
			yyjson_mut_val *term_str = yyjson_mut_obj(doc);
			yyjson_mut_obj_add_str(doc, term_str, "t", "Str");
			yyjson_mut_obj_add_strncpy(doc, term_str, "c", items[k].content.data(), items[k].content.size());
			yyjson_mut_arr_add_val(term_inls, term_str);
			yyjson_mut_arr_add_val(pair, term_inls);

			yyjson_mut_val *defs = yyjson_mut_arr(doc);
			idx_t d = k + 1;
			for (; d < items.size() && items[d].role == DuckBlockTypes::LIST_TYPE_DEFINITION; d++) {
				// A definition is [Block], PLURAL, exactly like a bullet item -- and this
				// emitted only the FIRST block, so every block after it was silently lost.
				// `TLS: <para> <code>` came back as `TLS: <para>`.
				//
				// The bullet path twenty lines below already collected extra_paragraphs,
				// extra_blocks and a nested list into each item; the definition path was
				// written against the same ListItem struct and read one field of it. Two
				// walks over one structure, one of them incomplete -- the same shape as
				// the container and list terminal arms that dropped table, deflist and
				// lineblock, and the reason those were merged into one shared fallback.
				yyjson_mut_val *one_def = yyjson_mut_arr(doc);
				yyjson_mut_val *pl = yyjson_mut_obj(doc);
				yyjson_mut_obj_add_str(doc, pl, "t", items[d].tight ? "Plain" : "Para");
				if (!items[d].inlines.empty()) {
					// Rich inline content -- a definition containing bold or a link kept
					// only its flattened text before this.
					idx_t inl_end = 0;
					yyjson_mut_obj_add_val(doc, pl, "c",
					                       PandocInlineConvert::ConvertDbInlinesToPandocVal(
					                           doc, items[d].inlines, 0, list_level + 2, inl_end, 1));
				} else {
					yyjson_mut_val *inl = yyjson_mut_arr(doc);
					if (!items[d].content.empty()) {
						yyjson_mut_val *s = yyjson_mut_obj(doc);
						yyjson_mut_obj_add_str(doc, s, "t", "Str");
						yyjson_mut_obj_add_strncpy(doc, s, "c", items[d].content.data(), items[d].content.size());
						yyjson_mut_arr_add_val(inl, s);
					}
					yyjson_mut_obj_add_val(doc, pl, "c", inl);
				}
				yyjson_mut_arr_add_val(one_def, pl);

				for (auto &extra : items[d].extra_paragraphs) {
					yyjson_mut_val *para_obj = yyjson_mut_obj(doc);
					yyjson_mut_obj_add_str(doc, para_obj, "t", extra.tight ? "Plain" : "Para");
					yyjson_mut_val *pinl = yyjson_mut_arr(doc);
					yyjson_mut_val *pstr = yyjson_mut_obj(doc);
					yyjson_mut_obj_add_str(doc, pstr, "t", "Str");
					yyjson_mut_obj_add_strncpy(doc, pstr, "c", extra.text.data(), extra.text.size());
					yyjson_mut_arr_add_val(pinl, pstr);
					yyjson_mut_obj_add_val(doc, para_obj, "c", pinl);
					yyjson_mut_arr_add_val(one_def, para_obj);
				}
				for (auto *extra : items[d].extra_blocks) {
					yyjson_mut_arr_add_val(one_def, extra);
				}
				if (items[d].nested_list_val) {
					yyjson_mut_arr_add_val(one_def, items[d].nested_list_val);
				}

				yyjson_mut_arr_add_val(defs, one_def);
			}
			yyjson_mut_arr_add_val(pair, defs);
			yyjson_mut_arr_add_val(items_arr, pair);
			k = d;
		}
		return root_obj;
	}

	if (is_ordered) {
		// Honour the ListAttributes the reader preserved rather than hardcoding
		// [1, Decimal, Period]. A list starting at 3, or using roman numerals or
		// parens, used to silently come back as "1." -- the numbers lived only
		// inside the opaque JSON, so nothing downstream could see them.
		c_outer = yyjson_mut_arr(doc);
		yyjson_mut_val *order_spec = yyjson_mut_arr(doc);
		yyjson_mut_arr_add_int(doc, order_spec, ParseInt32OrDefault(GetElementAttribute(list_block, "start"), 1));
		auto number_style = GetElementAttribute(list_block, "number_style");
		auto number_delim = GetElementAttribute(list_block, "number_delim");
		yyjson_mut_val *style_obj = yyjson_mut_obj(doc);
		yyjson_mut_obj_add_strcpy(doc, style_obj, "t", number_style.empty() ? "Decimal" : number_style.c_str());
		yyjson_mut_arr_add_val(order_spec, style_obj);
		yyjson_mut_val *delim_obj = yyjson_mut_obj(doc);
		yyjson_mut_obj_add_strcpy(doc, delim_obj, "t", number_delim.empty() ? "Period" : number_delim.c_str());
		yyjson_mut_arr_add_val(order_spec, delim_obj);
		yyjson_mut_arr_add_val(c_outer, order_spec);
		yyjson_mut_arr_add_val(c_outer, items_arr);
		yyjson_mut_obj_add_val(doc, root_obj, "c", c_outer);
	} else {
		yyjson_mut_obj_add_val(doc, root_obj, "c", items_arr);
	}

	for (auto &item : items) {
		yyjson_mut_val *item_blocks = yyjson_mut_arr(doc);
		// An item that carries nothing of its own but DOES hold blocks -- a stray child
		// wrapped as an item -- must not lead with an empty Plain.
		if (item.content.empty() && item.inlines.empty() && !item.extra_blocks.empty()) {
			for (auto *extra : item.extra_blocks) {
				yyjson_mut_arr_add_val(item_blocks, extra);
			}
			if (item.nested_list_val) {
				yyjson_mut_arr_add_val(item_blocks, item.nested_list_val);
			}
			yyjson_mut_arr_add_val(items_arr, item_blocks);
			continue;
		}
		yyjson_mut_val *plain_obj = yyjson_mut_obj(doc);
		yyjson_mut_obj_add_str(doc, plain_obj, "t", item.tight ? "Plain" : "Para");

		if (!item.inlines.empty()) {
			idx_t inl_end = 0;
			yyjson_mut_val *inl_arr =
			    PandocInlineConvert::ConvertDbInlinesToPandocVal(doc, item.inlines, 0, list_level + 2, inl_end, 1);
			yyjson_mut_obj_add_val(doc, plain_obj, "c", inl_arr);
		} else if (!item.content.empty()) {
			yyjson_mut_val *inl_arr = yyjson_mut_arr(doc);
			yyjson_mut_val *str_obj = yyjson_mut_obj(doc);
			yyjson_mut_obj_add_str(doc, str_obj, "t", "Str");
			yyjson_mut_obj_add_strncpy(doc, str_obj, "c", item.content.data(), item.content.size());
			yyjson_mut_arr_add_val(inl_arr, str_obj);
			yyjson_mut_obj_add_val(doc, plain_obj, "c", inl_arr);
		} else {
			yyjson_mut_obj_add_val(doc, plain_obj, "c", yyjson_mut_arr(doc));
		}
		yyjson_mut_arr_add_val(item_blocks, plain_obj);

		// A multi-block item's remaining blocks, each with the constructor it arrived
		// as. This hardcoded "Para" on the reasoning that Pandoc's own reader emits Para
		// here -- true of Pandoc's MARKDOWN reader and not of the AST, which is what we
		// are round-tripping. A Plain in block two came back as a Para, so the very
		// distinction `plain` was minted for was preserved in block one and discarded
		// immediately after it.
		for (auto &extra : item.extra_paragraphs) {
			yyjson_mut_val *para_obj = yyjson_mut_obj(doc);
			yyjson_mut_obj_add_str(doc, para_obj, "t", extra.tight ? "Plain" : "Para");
			yyjson_mut_val *pinl = yyjson_mut_arr(doc);
			yyjson_mut_val *pstr = yyjson_mut_obj(doc);
			yyjson_mut_obj_add_str(doc, pstr, "t", "Str");
			yyjson_mut_obj_add_strncpy(doc, pstr, "c", extra.text.data(), extra.text.size());
			yyjson_mut_arr_add_val(pinl, pstr);
			yyjson_mut_obj_add_val(doc, para_obj, "c", pinl);
			yyjson_mut_arr_add_val(item_blocks, para_obj);
		}

		for (auto *extra : item.extra_blocks) {
			yyjson_mut_arr_add_val(item_blocks, extra);
		}

		if (item.nested_list_val) {
			yyjson_mut_arr_add_val(item_blocks, item.nested_list_val);
		}
		yyjson_mut_arr_add_val(items_arr, item_blocks);
	}

	return root_obj;
}

//! Build pandoc's `Attr` -- ["", [], []] -- which every table part below needs one of.
static yyjson_mut_val *EmptyAttr(yyjson_mut_doc *doc) {
	yyjson_mut_val *attr = yyjson_mut_arr(doc);
	yyjson_mut_arr_add_str(doc, attr, "");
	yyjson_mut_arr_add_val(attr, yyjson_mut_arr(doc));
	yyjson_mut_arr_add_val(attr, yyjson_mut_arr(doc));
	return attr;
}

//! One pandoc table Cell: [attr, alignment, rowspan, colspan, [blocks]].
static yyjson_mut_val *TableCellVal(yyjson_mut_doc *doc, const char *text) {
	yyjson_mut_val *cell = yyjson_mut_arr(doc);
	yyjson_mut_arr_add_val(cell, EmptyAttr(doc));
	yyjson_mut_val *align = yyjson_mut_obj(doc);
	yyjson_mut_obj_add_str(doc, align, "t", "AlignDefault");
	yyjson_mut_arr_add_val(cell, align);
	yyjson_mut_arr_add_int(doc, cell, 1); // rowspan
	yyjson_mut_arr_add_int(doc, cell, 1); // colspan

	yyjson_mut_val *cell_blocks = yyjson_mut_arr(doc);
	if (text && text[0] != '\0') {
		yyjson_mut_val *para = yyjson_mut_obj(doc);
		yyjson_mut_obj_add_str(doc, para, "t", "Para");
		yyjson_mut_val *inl = yyjson_mut_arr(doc);
		yyjson_mut_val *s = yyjson_mut_obj(doc);
		yyjson_mut_obj_add_str(doc, s, "t", "Str");
		yyjson_mut_obj_add_strcpy(doc, s, "c", text);
		yyjson_mut_arr_add_val(inl, s);
		yyjson_mut_obj_add_val(doc, para, "c", inl);
		yyjson_mut_arr_add_val(cell_blocks, para);
	}
	yyjson_mut_arr_add_val(cell, cell_blocks);
	return cell;
}

//! One pandoc table Row: [attr, [cells]], built from a JSON array of cell strings.
static yyjson_mut_val *TableRowVal(yyjson_mut_doc *doc, yyjson_val *cells_arr) {
	yyjson_mut_val *row = yyjson_mut_arr(doc);
	yyjson_mut_arr_add_val(row, EmptyAttr(doc));
	yyjson_mut_val *cells = yyjson_mut_arr(doc);
	if (cells_arr && yyjson_is_arr(cells_arr)) {
		size_t ci, cmax;
		yyjson_val *cv;
		yyjson_arr_foreach(cells_arr, ci, cmax, cv) {
			yyjson_mut_arr_add_val(cells, TableCellVal(doc, yyjson_is_str(cv) ? yyjson_get_str(cv) : ""));
		}
	}
	yyjson_mut_arr_add_val(row, cells);
	return row;
}

//! CONVERT duck_block's NATIVE table projection into pandoc's Table constructor.
//!
//! The native form is spec 5.0's `{"headers": [...], "rows": [[...]]}` -- a shape chosen so
//! a table is queryable from SQL without walking an AST. Pandoc's Table is a six-element
//! ARRAY: [attr, caption, colspecs, head, [bodies], foot].
//!
//! Before this existed, the fallback branch dumped the native OBJECT straight into `c`, and
//! pandoc refused the entire document:
//!
//!     When parsing the constructor Table of type Text.Pandoc.Definition.Block
//!     expected Array but got Object
//!
//! That made every table from every NATIVE reader unexportable -- only tables carrying a
//! preserved attributes['pandoc_ast'] tuple, which is to say tables that came from pandoc
//! in the first place, could survive the trip out. It went unnoticed because nothing wrote
//! blocks back out until the write direction was registered, and because exactly one
//! fixture in the tree contains a table at all.
//!
//! Cell text becomes a single `Str` inside a `Para`, matching what the definition-list arm
//! already does. Pandoc's own reader splits runs into Str/Space/Str; one Str is valid
//! pandoc JSON and renders identically, and matching the existing convention beats
//! introducing a second one here.
static yyjson_mut_val *NativeTableToPandocVal(yyjson_mut_doc *doc, const string &content) {
	yyjson_mut_val *c = yyjson_mut_arr(doc);
	yyjson_doc *src = content.empty() ? nullptr : yyjson_read(content.c_str(), content.size(), 0);
	yyjson_val *root = src ? yyjson_doc_get_root(src) : nullptr;

	// THE DISCRIMINATION LIVES HERE, not at the call sites, because there are two of them --
	// a top-level table and a table nested in a div, blockquote or figure -- and the pair
	// had already drifted once: the nested arm's comment asserts "these store their whole
	// Pandoc tuple as JSON", which is true only of tables that CAME from pandoc.
	//
	// The two forms are distinguishable by shape alone and cannot be confused: a pandoc
	// Table tuple is an ARRAY, duck_block's native projection is an OBJECT. Splice the
	// former, convert the latter.
	if (root && yyjson_is_arr(root)) {
		yyjson_mut_val *copy = yyjson_val_mut_copy(doc, root);
		yyjson_doc_free(src);
		return copy;
	}

	yyjson_val *headers = root ? yyjson_obj_get(root, "headers") : nullptr;
	yyjson_val *rows = root ? yyjson_obj_get(root, "rows") : nullptr;

	// Column count comes from the header row when there is one, widened by the longest body
	// row. Pandoc wants one ColSpec per column, and a count that disagrees with the widest
	// row renders wrong rather than failing.
	size_t ncols = (headers && yyjson_is_arr(headers)) ? yyjson_arr_size(headers) : 0;
	if (rows && yyjson_is_arr(rows)) {
		size_t ri, rmax;
		yyjson_val *rv;
		yyjson_arr_foreach(rows, ri, rmax, rv) {
			if (yyjson_is_arr(rv) && yyjson_arr_size(rv) > ncols) {
				ncols = yyjson_arr_size(rv);
			}
		}
	}

	yyjson_mut_arr_add_val(c, EmptyAttr(doc)); // attr

	yyjson_mut_val *caption = yyjson_mut_arr(doc); // caption: [null, []]
	yyjson_mut_arr_add_null(doc, caption);
	yyjson_mut_arr_add_val(caption, yyjson_mut_arr(doc));
	yyjson_mut_arr_add_val(c, caption);

	yyjson_mut_val *colspecs = yyjson_mut_arr(doc);
	for (size_t i = 0; i < ncols; i++) {
		yyjson_mut_val *spec = yyjson_mut_arr(doc);
		yyjson_mut_val *al = yyjson_mut_obj(doc);
		yyjson_mut_obj_add_str(doc, al, "t", "AlignDefault");
		yyjson_mut_arr_add_val(spec, al);
		yyjson_mut_val *w = yyjson_mut_obj(doc);
		yyjson_mut_obj_add_str(doc, w, "t", "ColWidthDefault");
		yyjson_mut_arr_add_val(spec, w);
		yyjson_mut_arr_add_val(colspecs, spec);
	}
	yyjson_mut_arr_add_val(c, colspecs);

	// TableHead: [attr, [rows]]. An EMPTY headers array yields a head with no rows, which is
	// how pandoc spells a headerless table -- not an absent head, which is invalid.
	yyjson_mut_val *head = yyjson_mut_arr(doc);
	yyjson_mut_arr_add_val(head, EmptyAttr(doc));
	yyjson_mut_val *head_rows = yyjson_mut_arr(doc);
	if (headers && yyjson_is_arr(headers) && yyjson_arr_size(headers) > 0) {
		yyjson_mut_arr_add_val(head_rows, TableRowVal(doc, headers));
	}
	yyjson_mut_arr_add_val(head, head_rows);
	yyjson_mut_arr_add_val(c, head);

	// TableBody list: [[attr, rowHeadColumns, [intermediate head], [rows]]]
	yyjson_mut_val *bodies = yyjson_mut_arr(doc);
	yyjson_mut_val *body = yyjson_mut_arr(doc);
	yyjson_mut_arr_add_val(body, EmptyAttr(doc));
	yyjson_mut_arr_add_int(doc, body, 0);
	yyjson_mut_arr_add_val(body, yyjson_mut_arr(doc));
	yyjson_mut_val *body_rows = yyjson_mut_arr(doc);
	if (rows && yyjson_is_arr(rows)) {
		size_t ri, rmax;
		yyjson_val *rv;
		yyjson_arr_foreach(rows, ri, rmax, rv) {
			yyjson_mut_arr_add_val(body_rows, TableRowVal(doc, rv));
		}
	}
	yyjson_mut_arr_add_val(body, body_rows);
	yyjson_mut_arr_add_val(bodies, body);
	yyjson_mut_arr_add_val(c, bodies);

	yyjson_mut_val *foot = yyjson_mut_arr(doc); // TableFoot: [attr, []]
	yyjson_mut_arr_add_val(foot, EmptyAttr(doc));
	yyjson_mut_arr_add_val(foot, yyjson_mut_arr(doc));
	yyjson_mut_arr_add_val(c, foot);

	if (src) {
		yyjson_doc_free(src);
	}
	return c;
}

static yyjson_mut_val *ConvertFigureToPandocVal(yyjson_mut_doc *doc, const vector<Value> &blocks_list, idx_t &start_idx,
                                                int32_t fig_level, idx_t depth);

// Walks a container's children -- everything at a level deeper than the container --
// converting each into `target_arr`. Shared by Div and Figure so the eight child block
// types are handled in one place rather than duplicated per container.
static void ConvertContainerChildrenToPandocVal(yyjson_mut_doc *doc, const vector<Value> &blocks_list, idx_t &start_idx,
                                                int32_t parent_level, idx_t depth, yyjson_mut_val *target_arr,
                                                yyjson_mut_val *switch_arr, const char *switch_type) {
	yyjson_mut_val *child_blocks_arr = target_arr;

	// SPEC 6.0, the export half of CollapseLonePlainIntoParent. A container carrying its
	// own `content` HAS a single text child, and Pandoc's encoding of that is a lone
	// `Plain` -- so write one back. Without this the read side's narrowing of `plain`
	// silently DESTROYS text: `Div[Plain[x]]` read to `div(content='x')` exported as an
	// empty `Div[]`, because every child of this walk is found by level and a container
	// that owns no children has none to find.
	//
	// Emitted here rather than in each container's converter so Div, BlockQuote, Figure,
	// Section and anything added later get it from one place -- the same reason the walk
	// itself is shared.
	//
	// `Plain` and not `Para`: content on the container is the tight shape by definition,
	// and a container whose text is a paragraph has a `paragraph` CHILD instead, which
	// this walk emits as `Para` further down. That pair is what carries tight-vs-loose
	// now that `plain` no longer appears in leaf position.
	{
		auto own_content = GetElementStringField(blocks_list[start_idx], DuckBlockTypes::CONTENT_IDX);
		if (!own_content.empty()) {
			yyjson_mut_val *plain_obj = yyjson_mut_obj(doc);
			yyjson_mut_obj_add_str(doc, plain_obj, "t", "Plain");
			idx_t inl_end = 0;
			yyjson_mut_val *inl_arr = PandocInlineConvert::ConvertDbInlinesToPandocVal(
			    doc, blocks_list, start_idx + 1, parent_level + 1, inl_end, depth + 1);
			if (!inl_arr || yyjson_mut_arr_size(inl_arr) == 0) {
				// No inline children, so the text lives only in `content`.
				inl_arr = yyjson_mut_arr(doc);
				yyjson_mut_val *str_obj = yyjson_mut_obj(doc);
				yyjson_mut_obj_add_str(doc, str_obj, "t", "Str");
				yyjson_mut_obj_add_strncpy(doc, str_obj, "c", own_content.data(), own_content.size());
				yyjson_mut_arr_add_val(inl_arr, str_obj);
			}
			yyjson_mut_obj_add_val(doc, plain_obj, "c", inl_arr);
			yyjson_mut_arr_add_val(child_blocks_arr, plain_obj);
		}
	}

	idx_t j = start_idx + 1;
	while (j < blocks_list.size()) {
		auto &child = blocks_list[j];
		if (child.IsNull()) {
			j++;
			continue;
		}

		auto child_kind = GetElementStringField(child, DuckBlockTypes::KIND_IDX);
		auto child_type = GetElementStringField(child, DuckBlockTypes::ELEMENT_TYPE_IDX);
		int32_t child_level = GetElementLevel(child);

		// Figure separates content blocks from caption blocks in a single walk: on
		// meeting the switch block at parent_level + 1, output redirects and the
		// switching block itself is not emitted. Div passes nullptr and is unaffected.
		if (switch_type && switch_arr && child_kind == DuckBlockTypes::KIND_BLOCK && child_type == switch_type &&
		    child_level == parent_level + 1) {
			child_blocks_arr = switch_arr;
			j++;
			continue;
		}

		if (child_level <= parent_level && child_kind == DuckBlockTypes::KIND_BLOCK) {
			break;
		}
		if (child_kind == DuckBlockTypes::KIND_INLINE || child_type == DuckBlockTypes::TYPE_LIST_ITEM) {
			j++;
			continue;
		}

		if (child_kind == DuckBlockTypes::KIND_BLOCK) {
			auto content = GetElementStringField(child, DuckBlockTypes::CONTENT_IDX);

			vector<Value> inline_children;
			for (idx_t k = j + 1; k < blocks_list.size(); k++) {
				auto &inl = blocks_list[k];
				if (inl.IsNull()) {
					continue;
				}
				auto inl_kind = GetElementStringField(inl, DuckBlockTypes::KIND_IDX);
				if (inl_kind == DuckBlockTypes::KIND_BLOCK) {
					break;
				}
				if (inl_kind == DuckBlockTypes::KIND_INLINE) {
					inline_children.push_back(inl);
				}
			}

			if (child_type == DuckBlockTypes::TYPE_PARAGRAPH || child_type == DuckBlockTypes::TYPE_PLAIN) {
				// `plain` rides the paragraph path and differs only in the constructor
				// emitted. Found by sweeping every container after adding the type
				// rather than by reasoning about which paths reach it: a `plain` inside
				// a div, blockquote or figure was DROPPED ENTIRELY, because this branch
				// tested one type name and the fallthrough consumed the rest.
				yyjson_mut_val *para_obj = yyjson_mut_obj(doc);
				yyjson_mut_obj_add_str(doc, para_obj, "t", child_type == DuckBlockTypes::TYPE_PLAIN ? "Plain" : "Para");
				if (!inline_children.empty()) {
					idx_t inl_end = 0;
					yyjson_mut_val *inl_arr = PandocInlineConvert::ConvertDbInlinesToPandocVal(
					    doc, inline_children, 0, child_level + 1, inl_end, 1);
					yyjson_mut_obj_add_val(doc, para_obj, "c", inl_arr);
				} else {
					yyjson_mut_val *inl_arr = yyjson_mut_arr(doc);
					yyjson_mut_val *str_obj = yyjson_mut_obj(doc);
					yyjson_mut_obj_add_str(doc, str_obj, "t", "Str");
					yyjson_mut_obj_add_strncpy(doc, str_obj, "c", content.data(), content.size());
					yyjson_mut_arr_add_val(inl_arr, str_obj);
					yyjson_mut_obj_add_val(doc, para_obj, "c", inl_arr);
				}
				yyjson_mut_arr_add_val(child_blocks_arr, para_obj);
				j++;
			} else if (child_type == DuckBlockTypes::TYPE_HEADING) {
				auto level_str = GetElementAttribute(child, DuckBlockTypes::ATTR_HEADING_LEVEL);
				int level = ParseInt32OrDefault(level_str, 1);
				yyjson_mut_val *header_obj = yyjson_mut_obj(doc);
				yyjson_mut_obj_add_str(doc, header_obj, "t", "Header");
				yyjson_mut_val *hc_arr = yyjson_mut_arr(doc);
				yyjson_mut_arr_add_int(doc, hc_arr, level);
				yyjson_mut_arr_add_val(hc_arr, CreatePandocAttrVal(doc, child, ""));
				if (!inline_children.empty()) {
					idx_t inl_end = 0;
					yyjson_mut_val *inl_arr = PandocInlineConvert::ConvertDbInlinesToPandocVal(
					    doc, inline_children, 0, child_level + 1, inl_end, 1);
					yyjson_mut_arr_add_val(hc_arr, inl_arr);
				} else {
					yyjson_mut_val *inl_arr = yyjson_mut_arr(doc);
					yyjson_mut_val *str_obj = yyjson_mut_obj(doc);
					yyjson_mut_obj_add_str(doc, str_obj, "t", "Str");
					yyjson_mut_obj_add_strncpy(doc, str_obj, "c", content.data(), content.size());
					yyjson_mut_arr_add_val(inl_arr, str_obj);
					yyjson_mut_arr_add_val(hc_arr, inl_arr);
				}
				yyjson_mut_obj_add_val(doc, header_obj, "c", hc_arr);
				yyjson_mut_arr_add_val(child_blocks_arr, header_obj);
				j++;
			} else if (child_type == DuckBlockTypes::TYPE_CODE) {
				auto language = GetElementAttribute(child, "language");
				yyjson_mut_val *code_obj = yyjson_mut_obj(doc);
				yyjson_mut_obj_add_str(doc, code_obj, "t", "CodeBlock");
				yyjson_mut_val *cc_arr = yyjson_mut_arr(doc);
				yyjson_mut_arr_add_val(cc_arr, CreatePandocAttrVal(doc, child, language));
				yyjson_mut_arr_add_strncpy(doc, cc_arr, content.data(), content.size());
				yyjson_mut_obj_add_val(doc, code_obj, "c", cc_arr);
				yyjson_mut_arr_add_val(child_blocks_arr, code_obj);
				j++;
			} else if (child_type == DuckBlockTypes::TYPE_BLOCKQUOTE) {
				auto child_encoding = GetElementStringField(child, DuckBlockTypes::ENCODING_IDX);
				yyjson_mut_val *bq_obj = yyjson_mut_obj(doc);
				yyjson_mut_obj_add_str(doc, bq_obj, "t", "BlockQuote");
				if (child_encoding == DuckBlockTypes::ENCODING_JSON && !content.empty()) {
					yyjson_doc *sub_doc = yyjson_read(content.c_str(), content.size(), 0);
					if (sub_doc) {
						yyjson_mut_val *imported = yyjson_val_mut_copy(doc, yyjson_doc_get_root(sub_doc));
						yyjson_mut_obj_add_val(doc, bq_obj, "c", imported);
						yyjson_doc_free(sub_doc);
					} else {
						yyjson_mut_obj_add_val(doc, bq_obj, "c", yyjson_mut_arr(doc));
					}
				} else if (!inline_children.empty()) {
					yyjson_mut_val *bqc_arr = yyjson_mut_arr(doc);
					yyjson_mut_val *para_obj = yyjson_mut_obj(doc);
					yyjson_mut_obj_add_str(doc, para_obj, "t", "Para");
					idx_t inl_end = 0;
					yyjson_mut_val *inl_arr = PandocInlineConvert::ConvertDbInlinesToPandocVal(
					    doc, inline_children, 0, child_level + 1, inl_end, 1);
					yyjson_mut_obj_add_val(doc, para_obj, "c", inl_arr);
					yyjson_mut_arr_add_val(bqc_arr, para_obj);
					yyjson_mut_obj_add_val(doc, bq_obj, "c", bqc_arr);
				} else if (!content.empty()) {
					yyjson_mut_val *bqc_arr = yyjson_mut_arr(doc);
					yyjson_mut_val *para_obj = yyjson_mut_obj(doc);
					yyjson_mut_obj_add_str(doc, para_obj, "t", "Para");
					yyjson_mut_val *inl_arr = yyjson_mut_arr(doc);
					yyjson_mut_val *str_obj = yyjson_mut_obj(doc);
					yyjson_mut_obj_add_str(doc, str_obj, "t", "Str");
					yyjson_mut_obj_add_strncpy(doc, str_obj, "c", content.data(), content.size());
					yyjson_mut_arr_add_val(inl_arr, str_obj);
					yyjson_mut_obj_add_val(doc, para_obj, "c", inl_arr);
					yyjson_mut_arr_add_val(bqc_arr, para_obj);
					yyjson_mut_obj_add_val(doc, bq_obj, "c", bqc_arr);
				} else {
					yyjson_mut_val *bqc_arr = yyjson_mut_arr(doc);
					yyjson_mut_val *para_obj = yyjson_mut_obj(doc);
					yyjson_mut_obj_add_str(doc, para_obj, "t", "Para");
					yyjson_mut_obj_add_val(doc, para_obj, "c", yyjson_mut_arr(doc));
					yyjson_mut_arr_add_val(bqc_arr, para_obj);
					yyjson_mut_obj_add_val(doc, bq_obj, "c", bqc_arr);
				}
				yyjson_mut_arr_add_val(child_blocks_arr, bq_obj);
				j++;
			} else if (child_type == DuckBlockTypes::TYPE_HR) {
				yyjson_mut_val *hr_obj = yyjson_mut_obj(doc);
				yyjson_mut_obj_add_str(doc, hr_obj, "t", "HorizontalRule");
				yyjson_mut_arr_add_val(child_blocks_arr, hr_obj);
				j++;
			} else if (child_type == DuckBlockTypes::TYPE_LIST) {
				yyjson_mut_val *list_obj = ConvertListToPandocVal(doc, blocks_list, j, child_level, depth + 1);
				yyjson_mut_arr_add_val(child_blocks_arr, list_obj);
			} else if (child_type == DuckBlockTypes::TYPE_DIV) {
				yyjson_mut_val *nested_div_obj = ConvertDivToPandocVal(doc, blocks_list, j, child_level, depth + 1);
				yyjson_mut_arr_add_val(child_blocks_arr, nested_div_obj);
			} else if ((child_type == DuckBlockTypes::TYPE_TABLE || child_type == DuckBlockTypes::TYPE_DEFLIST) &&
			           GetElementStringField(child, DuckBlockTypes::ENCODING_IDX) == DuckBlockTypes::ENCODING_JSON &&
			           !content.empty()) {
				// Without this arm, a table or definition list inside a div, blockquote or
				// figure vanished entirely.
				//
				// THE OLD COMMENT HERE SAID "these store their whole Pandoc tuple as JSON",
				// and that is true only of blocks that came FROM pandoc. A native reader's
				// table stores duck_block's {"headers":…,"rows":…} projection instead, and
				// splicing that in produced an object where pandoc's grammar demands an
				// array -- which made pandoc reject the whole document. Tables therefore go
				// through the shared converter, which tells the two forms apart by shape.
				yyjson_mut_val *obj = yyjson_mut_obj(doc);
				if (child_type == DuckBlockTypes::TYPE_TABLE) {
					yyjson_mut_obj_add_str(doc, obj, "t", "Table");
					yyjson_mut_obj_add_val(doc, obj, "c", NativeTableToPandocVal(doc, content));
				} else {
					yyjson_mut_obj_add_str(doc, obj, "t", "DefinitionList");
					yyjson_doc *sub_doc = yyjson_read(content.c_str(), content.size(), 0);
					if (sub_doc) {
						yyjson_mut_obj_add_val(doc, obj, "c", yyjson_val_mut_copy(doc, yyjson_doc_get_root(sub_doc)));
						yyjson_doc_free(sub_doc);
					} else {
						yyjson_mut_obj_add_val(doc, obj, "c", yyjson_mut_arr(doc));
					}
				}
				yyjson_mut_arr_add_val(child_blocks_arr, obj);
				j++;
			} else {
				// NEVER SILENTLY DROP. This was a bare `j++`, so any block type the chain
				// did not enumerate vanished inside every container -- lineblock, deflist
				// and table all were, long before today.
				yyjson_mut_arr_add_val(child_blocks_arr,
				                       ConvertUnhandledChildToPandocVal(doc, blocks_list, j, depth, child, child_type,
				                                                        content, inline_children, child_level));
			}
		} else {
			j++;
		}
	}

	start_idx = j;
}

static yyjson_mut_val *ConvertDivToPandocVal(yyjson_mut_doc *doc, const vector<Value> &blocks_list, idx_t &start_idx,
                                             int32_t div_level, idx_t depth) {
	CheckPandocDepth(depth);
	auto &div_block = blocks_list[start_idx];

	yyjson_mut_val *div_obj = yyjson_mut_obj(doc);
	yyjson_mut_obj_add_str(doc, div_obj, "t", "Div");

	yyjson_mut_val *c_arr = yyjson_mut_arr(doc);
	yyjson_mut_arr_add_val(c_arr, CreatePandocAttrVal(doc, div_block, ""));

	yyjson_mut_val *child_blocks_arr = yyjson_mut_arr(doc);
	yyjson_mut_arr_add_val(c_arr, child_blocks_arr);
	yyjson_mut_obj_add_val(doc, div_obj, "c", c_arr);

	ConvertContainerChildrenToPandocVal(doc, blocks_list, start_idx, div_level, depth, child_blocks_arr, nullptr,
	                                    nullptr);
	return div_obj;
}

// Does the container at `idx` own block children (the structural shape), as opposed
// to carrying its text directly (the builder shape)? Both are legal, so the exporter
// has to tell them apart rather than assume one.
static bool HasBlockChildren(const vector<Value> &blocks_list, idx_t idx) {
	const int32_t own_level = GetElementLevel(blocks_list[idx]);
	for (idx_t j = idx + 1; j < blocks_list.size(); j++) {
		auto &child = blocks_list[j];
		if (child.IsNull()) {
			continue;
		}
		if (GetElementStringField(child, DuckBlockTypes::KIND_IDX) != DuckBlockTypes::KIND_BLOCK) {
			continue;
		}
		return GetElementLevel(child) > own_level;
	}
	return false;
}

// BlockQuote c = [Block] -- no Attr, so the children array IS `c`. Needed once the
// reader stopped storing the quote as opaque JSON: without it a structural quote
// exported as an empty BlockQuote followed by its own children as SIBLINGS, which
// silently lifts quoted text out of the quote and into the body.
static yyjson_mut_val *ConvertBlockquoteToPandocVal(yyjson_mut_doc *doc, const vector<Value> &blocks_list,
                                                    idx_t &start_idx, int32_t quote_level, idx_t depth) {
	CheckPandocDepth(depth);
	yyjson_mut_val *bq_obj = yyjson_mut_obj(doc);
	yyjson_mut_obj_add_str(doc, bq_obj, "t", "BlockQuote");
	yyjson_mut_val *child_blocks_arr = yyjson_mut_arr(doc);
	yyjson_mut_obj_add_val(doc, bq_obj, "c", child_blocks_arr);
	ConvertContainerChildrenToPandocVal(doc, blocks_list, start_idx, quote_level, depth, child_blocks_arr, nullptr,
	                                    nullptr);
	return bq_obj;
}

// Figure c = [Attr, Caption, [Block]] where Caption = [ShortCaption?, [Block]].
// Children were emitted as content blocks followed by a `caption` container, so one
// walk with a switch at that container reconstitutes both lists.
static yyjson_mut_val *ConvertFigureToPandocVal(yyjson_mut_doc *doc, const vector<Value> &blocks_list, idx_t &start_idx,
                                                int32_t fig_level, idx_t depth) {
	CheckPandocDepth(depth);
	auto &fig_block = blocks_list[start_idx];

	// short_caption is stored on the caption child, so read it before the walk consumes it.
	string short_text;
	for (idx_t k = start_idx + 1; k < blocks_list.size(); k++) {
		auto &c = blocks_list[k];
		if (c.IsNull()) {
			continue;
		}
		if (GetElementStringField(c, DuckBlockTypes::KIND_IDX) == DuckBlockTypes::KIND_BLOCK &&
		    GetElementLevel(c) <= fig_level) {
			break;
		}
		if (GetElementStringField(c, DuckBlockTypes::ELEMENT_TYPE_IDX) == DuckBlockTypes::TYPE_CAPTION) {
			short_text = GetElementAttribute(c, "short_caption");
			break;
		}
	}

	yyjson_mut_val *content_arr = yyjson_mut_arr(doc);
	yyjson_mut_val *caption_arr = yyjson_mut_arr(doc);
	ConvertContainerChildrenToPandocVal(doc, blocks_list, start_idx, fig_level, depth, content_arr, caption_arr,
	                                    DuckBlockTypes::TYPE_CAPTION);

	yyjson_mut_val *fig_obj = yyjson_mut_obj(doc);
	yyjson_mut_obj_add_str(doc, fig_obj, "t", "Figure");
	yyjson_mut_val *c_arr = yyjson_mut_arr(doc);
	yyjson_mut_arr_add_val(c_arr, CreatePandocAttrVal(doc, fig_block, ""));

	yyjson_mut_val *cap_arr = yyjson_mut_arr(doc);
	if (short_text.empty()) {
		yyjson_mut_arr_add_val(cap_arr, yyjson_mut_null(doc));
	} else {
		// ShortCaption is Maybe [Inline], so wrap the stored text in a single Str.
		yyjson_mut_val *short_arr = yyjson_mut_arr(doc);
		yyjson_mut_val *str_obj = yyjson_mut_obj(doc);
		yyjson_mut_obj_add_str(doc, str_obj, "t", "Str");
		yyjson_mut_obj_add_strncpy(doc, str_obj, "c", short_text.data(), short_text.size());
		yyjson_mut_arr_add_val(short_arr, str_obj);
		yyjson_mut_arr_add_val(cap_arr, short_arr);
	}
	yyjson_mut_arr_add_val(cap_arr, caption_arr);

	yyjson_mut_arr_add_val(c_arr, cap_arr);
	yyjson_mut_arr_add_val(c_arr, content_arr);
	yyjson_mut_obj_add_val(doc, fig_obj, "c", c_arr);
	return fig_obj;
}

static string BuildBlocksJson(const vector<Value> &blocks_list) {
	yyjson_mut_doc *doc = yyjson_mut_doc_new(nullptr);
	yyjson_mut_val *blocks_arr = yyjson_mut_arr(doc);
	yyjson_mut_doc_set_root(doc, blocks_arr);

	for (idx_t block_idx = 0; block_idx < blocks_list.size();) {
		auto &block = blocks_list[block_idx];
		if (block.IsNull()) {
			block_idx++;
			continue;
		}

		auto kind = GetElementStringField(block, DuckBlockTypes::KIND_IDX);
		if (kind != DuckBlockTypes::KIND_BLOCK) {
			block_idx++;
			continue;
		}

		auto element_type = GetElementStringField(block, DuckBlockTypes::ELEMENT_TYPE_IDX);
		auto content = GetElementStringField(block, DuckBlockTypes::CONTENT_IDX);

		if (element_type == DuckBlockTypes::TYPE_LIST_ITEM || element_type == DuckBlockTypes::TYPE_METADATA) {
			block_idx++;
			continue;
		}

		vector<Value> inline_children;
		for (idx_t j = block_idx + 1; j < blocks_list.size(); j++) {
			auto &child = blocks_list[j];
			if (child.IsNull()) {
				continue;
			}
			// STOP at anything that is not an inline, not merely at a block. This broke
			// on KIND_BLOCK only, so it walked straight PAST a kind='value' element and
			// harvested the metadata's inline children as the paragraph's own:
			//
			//   meta title "TITLE" + body paragraph "BODY"  ->  Para["TITLE"]
			//
			// The document's body was replaced by its title. That is the metadata leak
			// in the EXPORT direction, in this converter, and it fires only when a
			// document has BOTH metadata and blocks -- which is why nothing caught it:
			// every fixture had one or the other. duck_blocks_to_pandoc_ast round-trips
			// the meta perfectly while corrupting the body, so the half everyone checks
			// looked right.
			//
			// Written as "not an inline" rather than "block or value" on purpose: a kind
			// added later must end the run too, and enumerating the kinds we know is the
			// failure this file has already had three times today.
			auto child_kind = GetElementStringField(child, DuckBlockTypes::KIND_IDX);
			if (child_kind != DuckBlockTypes::KIND_INLINE) {
				break;
			}
			inline_children.push_back(child);
		}

		if (element_type == DuckBlockTypes::TYPE_HEADING) {
			auto level_str = GetElementAttribute(block, DuckBlockTypes::ATTR_HEADING_LEVEL);
			int level = ParseInt32OrDefault(level_str, 1);
			yyjson_mut_val *header_obj = yyjson_mut_obj(doc);
			yyjson_mut_obj_add_str(doc, header_obj, "t", "Header");
			yyjson_mut_val *hc_arr = yyjson_mut_arr(doc);
			yyjson_mut_arr_add_int(doc, hc_arr, level);
			yyjson_mut_arr_add_val(hc_arr, CreatePandocAttrVal(doc, block, ""));
			if (!inline_children.empty()) {
				idx_t inl_end = 0;
				yyjson_mut_val *inl_arr =
				    PandocInlineConvert::ConvertDbInlinesToPandocVal(doc, inline_children, 0, 2, inl_end, 1);
				yyjson_mut_arr_add_val(hc_arr, inl_arr);
			} else {
				yyjson_mut_val *inl_arr = yyjson_mut_arr(doc);
				yyjson_mut_val *str_obj = yyjson_mut_obj(doc);
				yyjson_mut_obj_add_str(doc, str_obj, "t", "Str");
				yyjson_mut_obj_add_strncpy(doc, str_obj, "c", content.data(), content.size());
				yyjson_mut_arr_add_val(inl_arr, str_obj);
				yyjson_mut_arr_add_val(hc_arr, inl_arr);
			}
			yyjson_mut_obj_add_val(doc, header_obj, "c", hc_arr);
			yyjson_mut_arr_add_val(blocks_arr, header_obj);
			block_idx++;
		} else if (element_type == DuckBlockTypes::TYPE_PARAGRAPH) {
			yyjson_mut_val *para_obj = yyjson_mut_obj(doc);
			yyjson_mut_obj_add_str(doc, para_obj, "t", "Para");
			if (!inline_children.empty()) {
				idx_t inl_end = 0;
				yyjson_mut_val *inl_arr =
				    PandocInlineConvert::ConvertDbInlinesToPandocVal(doc, inline_children, 0, 2, inl_end, 1);
				yyjson_mut_obj_add_val(doc, para_obj, "c", inl_arr);
			} else {
				yyjson_mut_val *inl_arr = yyjson_mut_arr(doc);
				yyjson_mut_val *str_obj = yyjson_mut_obj(doc);
				yyjson_mut_obj_add_str(doc, str_obj, "t", "Str");
				yyjson_mut_obj_add_strncpy(doc, str_obj, "c", content.data(), content.size());
				yyjson_mut_arr_add_val(inl_arr, str_obj);
				yyjson_mut_obj_add_val(doc, para_obj, "c", inl_arr);
			}
			yyjson_mut_arr_add_val(blocks_arr, para_obj);
			block_idx++;
		} else if (element_type == DuckBlockTypes::TYPE_CODE) {
			auto language = GetElementAttribute(block, "language");
			yyjson_mut_val *code_obj = yyjson_mut_obj(doc);
			yyjson_mut_obj_add_str(doc, code_obj, "t", "CodeBlock");
			yyjson_mut_val *cc_arr = yyjson_mut_arr(doc);
			yyjson_mut_arr_add_val(cc_arr, CreatePandocAttrVal(doc, block, language));
			yyjson_mut_arr_add_strncpy(doc, cc_arr, content.data(), content.size());
			yyjson_mut_obj_add_val(doc, code_obj, "c", cc_arr);
			yyjson_mut_arr_add_val(blocks_arr, code_obj);
			block_idx++;
		} else if (element_type == DuckBlockTypes::TYPE_BLOCKQUOTE &&
		           GetElementStringField(block, DuckBlockTypes::ENCODING_IDX) == DuckBlockTypes::ENCODING_JSON &&
		           !content.empty()) {
			yyjson_mut_val *bq_obj = yyjson_mut_obj(doc);
			yyjson_mut_obj_add_str(doc, bq_obj, "t", "BlockQuote");
			yyjson_doc *sub_doc = yyjson_read(content.c_str(), content.size(), 0);
			if (sub_doc) {
				yyjson_mut_val *imported = yyjson_val_mut_copy(doc, yyjson_doc_get_root(sub_doc));
				yyjson_mut_obj_add_val(doc, bq_obj, "c", imported);
				yyjson_doc_free(sub_doc);
			} else {
				yyjson_mut_obj_add_val(doc, bq_obj, "c", yyjson_mut_arr(doc));
			}
			yyjson_mut_arr_add_val(blocks_arr, bq_obj);
			block_idx++;
		} else if (element_type == DuckBlockTypes::TYPE_BLOCKQUOTE &&
		           (HasBlockChildren(blocks_list, block_idx) || !content.empty())) {
			// `|| !content.empty()` is 6.0: a quote carrying its own text has a single
			// text child, so it goes through the shared walk, which writes that text back
			// as `Plain`. The hand-rolled branch below wrote `Para` unconditionally, which
			// downgraded `BlockQuote[Plain]` on every round trip -- write-only in the small.
			int32_t quote_level = GetElementLevel(block);
			yyjson_mut_arr_add_val(blocks_arr,
			                       ConvertBlockquoteToPandocVal(doc, blocks_list, block_idx, quote_level, 1));
		} else if (element_type == DuckBlockTypes::TYPE_BLOCKQUOTE) {
			yyjson_mut_val *bq_obj = yyjson_mut_obj(doc);
			yyjson_mut_obj_add_str(doc, bq_obj, "t", "BlockQuote");
			yyjson_mut_val *bqc_arr = yyjson_mut_arr(doc);
			yyjson_mut_val *para_obj = yyjson_mut_obj(doc);
			yyjson_mut_obj_add_str(doc, para_obj, "t", "Para");
			if (!inline_children.empty()) {
				idx_t inl_end = 0;
				yyjson_mut_val *inl_arr =
				    PandocInlineConvert::ConvertDbInlinesToPandocVal(doc, inline_children, 0, 2, inl_end, 1);
				yyjson_mut_obj_add_val(doc, para_obj, "c", inl_arr);
			} else if (!content.empty()) {
				yyjson_mut_val *inl_arr = yyjson_mut_arr(doc);
				yyjson_mut_val *str_obj = yyjson_mut_obj(doc);
				yyjson_mut_obj_add_str(doc, str_obj, "t", "Str");
				yyjson_mut_obj_add_strncpy(doc, str_obj, "c", content.data(), content.size());
				yyjson_mut_arr_add_val(inl_arr, str_obj);
				yyjson_mut_obj_add_val(doc, para_obj, "c", inl_arr);
			} else {
				yyjson_mut_obj_add_val(doc, para_obj, "c", yyjson_mut_arr(doc));
			}
			yyjson_mut_arr_add_val(bqc_arr, para_obj);
			yyjson_mut_obj_add_val(doc, bq_obj, "c", bqc_arr);
			yyjson_mut_arr_add_val(blocks_arr, bq_obj);
			block_idx++;
		} else if (element_type == DuckBlockTypes::TYPE_HR) {
			yyjson_mut_val *hr_obj = yyjson_mut_obj(doc);
			yyjson_mut_obj_add_str(doc, hr_obj, "t", "HorizontalRule");
			yyjson_mut_arr_add_val(blocks_arr, hr_obj);
			block_idx++;
		} else if (element_type == DuckBlockTypes::TYPE_RAW) {
			// Shared with the nested arm in ConvertUnhandledChildToPandocVal. Two copies of
			// "how is a raw block's format decided" is how the two came to disagree in the
			// first place: this one read attributes['format'] and the nested path read
			// nothing at all.
			auto format = ResolveRawFormat(block);
			yyjson_mut_val *raw_obj = yyjson_mut_obj(doc);
			yyjson_mut_obj_add_str(doc, raw_obj, "t", "RawBlock");
			yyjson_mut_val *rc_arr = yyjson_mut_arr(doc);
			yyjson_mut_arr_add_strncpy(doc, rc_arr, format.data(), format.size());
			yyjson_mut_arr_add_strncpy(doc, rc_arr, content.data(), content.size());
			yyjson_mut_obj_add_val(doc, raw_obj, "c", rc_arr);
			yyjson_mut_arr_add_val(blocks_arr, raw_obj);
			block_idx++;
		} else if (element_type == DuckBlockTypes::TYPE_LIST &&
		           GetElementStringField(block, DuckBlockTypes::ENCODING_IDX) == DuckBlockTypes::ENCODING_JSON &&
		           !content.empty() && !HasBlockChildren(blocks_list, block_idx)) {
			// A spec-1.x list: items packed into content as a JSON array, no children.
			// Nothing produces this any more, but the spec PROMISES stored 1.x block
			// lists keep converting -- and until this branch existed that promise was
			// false: ConvertListToPandocVal walks children, found none, and emitted
			// `[{"t":"BulletList","c":[]}]`. Silent total loss of every item, which is
			// the same defect the 2.0 migration fixed for the reader, still live for
			// stored data. Found by auditing rulings against code rather than trusting
			// what I had written down.
			bool json_ordered =
			    (GetElementAttribute(block, DuckBlockTypes::ATTR_LIST_TYPE) == DuckBlockTypes::LIST_TYPE_ORDERED) ||
			    (GetElementAttribute(block, DuckBlockTypes::ATTR_ORDERED_LEGACY) == "true");
			yyjson_mut_val *list_obj = yyjson_mut_obj(doc);
			yyjson_mut_obj_add_str(doc, list_obj, "t", json_ordered ? "OrderedList" : "BulletList");
			yyjson_mut_val *items_arr = yyjson_mut_arr(doc);
			yyjson_doc *sub_doc = yyjson_read(content.c_str(), content.size(), 0);
			if (sub_doc) {
				yyjson_val *root = yyjson_doc_get_root(sub_doc);
				if (root && yyjson_is_arr(root)) {
					size_t idx, max;
					yyjson_val *item;
					yyjson_arr_foreach(root, idx, max, item) {
						yyjson_mut_val *item_blocks = yyjson_mut_arr(doc);
						yyjson_mut_val *plain_obj = yyjson_mut_obj(doc);
						yyjson_mut_obj_add_str(doc, plain_obj, "t", "Plain");
						yyjson_mut_val *inl = yyjson_mut_arr(doc);
						if (item && yyjson_is_str(item)) {
							const char *s = yyjson_get_str(item);
							yyjson_mut_val *str_obj = yyjson_mut_obj(doc);
							yyjson_mut_obj_add_str(doc, str_obj, "t", "Str");
							yyjson_mut_obj_add_strcpy(doc, str_obj, "c", s ? s : "");
							yyjson_mut_arr_add_val(inl, str_obj);
						}
						yyjson_mut_obj_add_val(doc, plain_obj, "c", inl);
						yyjson_mut_arr_add_val(item_blocks, plain_obj);
						yyjson_mut_arr_add_val(items_arr, item_blocks);
					}
				}
				yyjson_doc_free(sub_doc);
			}
			if (json_ordered) {
				yyjson_mut_val *c_outer = yyjson_mut_arr(doc);
				yyjson_mut_val *spec = yyjson_mut_arr(doc);
				yyjson_mut_arr_add_int(doc, spec, 1);
				yyjson_mut_val *sty = yyjson_mut_obj(doc);
				yyjson_mut_obj_add_str(doc, sty, "t", "Decimal");
				yyjson_mut_arr_add_val(spec, sty);
				yyjson_mut_val *dlm = yyjson_mut_obj(doc);
				yyjson_mut_obj_add_str(doc, dlm, "t", "Period");
				yyjson_mut_arr_add_val(spec, dlm);
				yyjson_mut_arr_add_val(c_outer, spec);
				yyjson_mut_arr_add_val(c_outer, items_arr);
				yyjson_mut_obj_add_val(doc, list_obj, "c", c_outer);
			} else {
				yyjson_mut_obj_add_val(doc, list_obj, "c", items_arr);
			}
			yyjson_mut_arr_add_val(blocks_arr, list_obj);
			block_idx++;
		} else if (element_type == DuckBlockTypes::TYPE_LIST) {
			int32_t list_level = GetElementLevel(block);
			yyjson_mut_val *list_obj = ConvertListToPandocVal(doc, blocks_list, block_idx, list_level, 1);
			yyjson_mut_arr_add_val(blocks_arr, list_obj);
		} else if (element_type == DuckBlockTypes::TYPE_DIV) {
			int32_t div_level = GetElementLevel(block);
			yyjson_mut_val *div_obj = ConvertDivToPandocVal(doc, blocks_list, block_idx, div_level, 1);
			yyjson_mut_arr_add_val(blocks_arr, div_obj);
		} else if (element_type == DuckBlockTypes::TYPE_PAGE) {
			// Pandoc has no page constructor. A Div classed `page` carrying the
			// number is its nearest honest equivalent, and reads correctly to
			// anything that understands classes.
			auto page_number = GetElementAttribute(block, "page_number");
			yyjson_mut_val *pg = yyjson_mut_obj(doc);
			yyjson_mut_obj_add_str(doc, pg, "t", "Div");
			yyjson_mut_val *pg_c = yyjson_mut_arr(doc);
			yyjson_mut_arr_add_val(pg_c, CreatePandocAttrVal(doc, block, "page"));
			yyjson_mut_arr_add_val(pg_c, yyjson_mut_arr(doc)); // a marker owns no blocks
			yyjson_mut_obj_add_val(doc, pg, "c", pg_c);
			yyjson_mut_arr_add_val(blocks_arr, pg);
			// A leaf advances the cursor itself, like TYPE_HR. Container branches
			// advance by reference inside their converter; leaving this out looped
			// forever appending Divs until memory ran out.
			block_idx++;
		} else if (element_type == DuckBlockTypes::TYPE_SECTION) {
			// Pandoc has no Section constructor, so a section degrades to a Div whose
			// class carries the role -- pandoc's nearest honest equivalent, and what
			// its own HTML reader produces for <section>.
			int32_t sec_level = GetElementLevel(block);
			auto role = GetElementAttribute(block, DuckBlockTypes::ATTR_ROLE);
			yyjson_mut_val *sec_obj = yyjson_mut_obj(doc);
			yyjson_mut_obj_add_str(doc, sec_obj, "t", "Div");
			yyjson_mut_val *sec_c = yyjson_mut_arr(doc);
			yyjson_mut_arr_add_val(sec_c, CreatePandocAttrVal(doc, block, role.empty() ? "section" : role));
			yyjson_mut_val *sec_children = yyjson_mut_arr(doc);
			yyjson_mut_arr_add_val(sec_c, sec_children);
			yyjson_mut_obj_add_val(doc, sec_obj, "c", sec_c);
			ConvertContainerChildrenToPandocVal(doc, blocks_list, block_idx, sec_level, 1, sec_children, nullptr,
			                                    nullptr);
			yyjson_mut_arr_add_val(blocks_arr, sec_obj);
		} else if (element_type == DuckBlockTypes::TYPE_FIGURE) {
			// A top-level figure has NULL level; treat it as 1 so its children, which
			// sit at 2, are correctly seen as deeper.
			int32_t fig_level = GetElementLevel(block);
			if (fig_level == 0) {
				fig_level = 1;
			}
			yyjson_mut_val *fig_obj = ConvertFigureToPandocVal(doc, blocks_list, block_idx, fig_level, 1);
			yyjson_mut_arr_add_val(blocks_arr, fig_obj);
		} else if (element_type == DuckBlockTypes::TYPE_TABLE &&
		           !GetElementAttribute(block, DuckBlockTypes::ATTR_PANDOC_AST).empty()) {
			// Round trip through the PRESERVED tuple, not the lossy projection. This is
			// the whole reason the tuple is kept: the renderable form lives in content
			// and the faithful form lives here, so nothing has to choose between them.
			auto tuple = GetElementAttribute(block, DuckBlockTypes::ATTR_PANDOC_AST);
			yyjson_mut_val *tbl_obj = yyjson_mut_obj(doc);
			yyjson_mut_obj_add_str(doc, tbl_obj, "t", "Table");
			yyjson_doc *sub_doc = yyjson_read(tuple.c_str(), tuple.size(), 0);
			if (sub_doc) {
				yyjson_mut_obj_add_val(doc, tbl_obj, "c", yyjson_val_mut_copy(doc, yyjson_doc_get_root(sub_doc)));
				yyjson_doc_free(sub_doc);
			} else {
				yyjson_mut_obj_add_val(doc, tbl_obj, "c", yyjson_mut_arr(doc));
			}
			yyjson_mut_arr_add_val(blocks_arr, tbl_obj);
			block_idx++;
		} else if (element_type == DuckBlockTypes::TYPE_TABLE) {
			// A table with NO preserved pandoc_ast tuple -- which is every table any native
			// reader produces. It must be CONVERTED from duck_block's native
			// {"headers":…,"rows":…} projection into pandoc's Table constructor.
			//
			// This branch previously imported `content` verbatim as `c`, handing pandoc an
			// object where its grammar requires a six-element array, and pandoc rejected the
			// whole document rather than that one block.
			yyjson_mut_val *tbl_obj = yyjson_mut_obj(doc);
			yyjson_mut_obj_add_str(doc, tbl_obj, "t", "Table");
			yyjson_mut_obj_add_val(doc, tbl_obj, "c", NativeTableToPandocVal(doc, content));
			yyjson_mut_arr_add_val(blocks_arr, tbl_obj);
			block_idx++;
		} else if (element_type == DuckBlockTypes::TYPE_IMAGE) {
			auto src = GetElementAttribute(block, "src");
			auto alt = GetElementAttribute(block, "alt");
			auto title = GetElementAttribute(block, "title");
			if (alt.empty()) {
				// An image's alt text IS its content under the vocabulary's content rule,
				// and this read only the attribute. Our own reader happens to write the alt
				// into BOTH places, so a round trip through this repo looked clean while a
				// producer that put the alt only where the rule says to put it lost it --
				// silently, since an Image with an empty alt is still a valid Image.
				//
				// Two copies of one fact, checked against each other by the one party that
				// writes both: that pair cannot detect its own disagreement, which is why
				// this needed a sweep over hand-built blocks rather than a round trip.
				alt = content;
			}

			yyjson_mut_val *para_obj = yyjson_mut_obj(doc);
			yyjson_mut_obj_add_str(doc, para_obj, "t", "Para");
			yyjson_mut_val *pc_arr = yyjson_mut_arr(doc);
			yyjson_mut_val *img_obj = yyjson_mut_obj(doc);
			yyjson_mut_obj_add_str(doc, img_obj, "t", "Image");
			yyjson_mut_val *ic_arr = yyjson_mut_arr(doc);

			yyjson_mut_val *attr_arr = yyjson_mut_arr(doc);
			yyjson_mut_arr_add_str(doc, attr_arr, "");
			yyjson_mut_arr_add_val(attr_arr, yyjson_mut_arr(doc));
			yyjson_mut_arr_add_val(attr_arr, yyjson_mut_arr(doc));
			yyjson_mut_arr_add_val(ic_arr, attr_arr);

			yyjson_mut_val *alt_arr = yyjson_mut_arr(doc);
			yyjson_mut_val *str_obj = yyjson_mut_obj(doc);
			yyjson_mut_obj_add_str(doc, str_obj, "t", "Str");
			yyjson_mut_obj_add_strncpy(doc, str_obj, "c", alt.data(), alt.size());
			yyjson_mut_arr_add_val(alt_arr, str_obj);
			yyjson_mut_arr_add_val(ic_arr, alt_arr);

			yyjson_mut_val *tgt_arr = yyjson_mut_arr(doc);
			yyjson_mut_arr_add_strncpy(doc, tgt_arr, src.data(), src.size());
			yyjson_mut_arr_add_strncpy(doc, tgt_arr, title.data(), title.size());
			yyjson_mut_arr_add_val(ic_arr, tgt_arr);

			yyjson_mut_obj_add_val(doc, img_obj, "c", ic_arr);
			yyjson_mut_arr_add_val(pc_arr, img_obj);
			yyjson_mut_obj_add_val(doc, para_obj, "c", pc_arr);
			yyjson_mut_arr_add_val(blocks_arr, para_obj);
			block_idx++;
		} else if (element_type == DuckBlockTypes::TYPE_PLAIN) {
			// Without this branch a `plain` fell to the terminal Para fallback, losing
			// the very distinction the type exists to carry. Same shape as the deflist
			// and lineblock gaps found by sweeping the exporter earlier today -- a new
			// type needs an export branch or it silently becomes something else.
			yyjson_mut_val *plain_obj = yyjson_mut_obj(doc);
			yyjson_mut_obj_add_str(doc, plain_obj, "t", "Plain");
			if (!inline_children.empty()) {
				idx_t inl_end = 0;
				yyjson_mut_obj_add_val(
				    doc, plain_obj, "c",
				    PandocInlineConvert::ConvertDbInlinesToPandocVal(doc, inline_children, 0, 2, inl_end, 1));
			} else {
				yyjson_mut_val *inl_arr = yyjson_mut_arr(doc);
				yyjson_mut_val *str_obj = yyjson_mut_obj(doc);
				yyjson_mut_obj_add_str(doc, str_obj, "t", "Str");
				yyjson_mut_obj_add_strncpy(doc, str_obj, "c", content.data(), content.size());
				yyjson_mut_arr_add_val(inl_arr, str_obj);
				yyjson_mut_obj_add_val(doc, plain_obj, "c", inl_arr);
			}
			yyjson_mut_arr_add_val(blocks_arr, plain_obj);
			block_idx++;
		} else if (element_type == DuckBlockTypes::TYPE_DEFLIST &&
		           GetElementStringField(block, DuckBlockTypes::ENCODING_IDX) == DuckBlockTypes::ENCODING_JSON &&
		           !content.empty()) {
			// Found by sweeping every block type through the exporter after fixing the
			// same defect for `generic`: deflist had NO export branch, so it fell to the
			// terminal Para fallback and came back out as a paragraph whose visible text
			// was its own raw AST. Splicing the stored tuple back makes it lossless.
			yyjson_mut_val *dl_obj = yyjson_mut_obj(doc);
			yyjson_mut_obj_add_str(doc, dl_obj, "t", "DefinitionList");
			yyjson_doc *sub_doc = yyjson_read(content.c_str(), content.size(), 0);
			if (sub_doc) {
				yyjson_mut_obj_add_val(doc, dl_obj, "c", yyjson_val_mut_copy(doc, yyjson_doc_get_root(sub_doc)));
				yyjson_doc_free(sub_doc);
			} else {
				yyjson_mut_obj_add_val(doc, dl_obj, "c", yyjson_mut_arr(doc));
			}
			yyjson_mut_arr_add_val(blocks_arr, dl_obj);
			block_idx++;
		} else if (element_type == DuckBlockTypes::TYPE_LINEBLOCK) {
			// Same sweep, same fallback. LineBlock c = [[Inline]] -- one inline array per
			// line -- and the reader stores the lines newline-separated in content, so
			// the split is the inverse of the join it did on the way in.
			yyjson_mut_val *lb_obj = yyjson_mut_obj(doc);
			yyjson_mut_obj_add_str(doc, lb_obj, "t", "LineBlock");
			yyjson_mut_val *lines_arr = yyjson_mut_arr(doc);

			// A line block with INLINE CHILDREN carries rich content -- bold, a link --
			// that `content` cannot hold, so the lines are delimited by `linebreak`
			// children rather than by newlines. Splitting on '\n' here would have thrown
			// all of it away a second time, on the way back out.
			if (!inline_children.empty()) {
				vector<Value> one_line;
				auto flush_line = [&]() {
					idx_t inl_end = 0;
					yyjson_mut_arr_add_val(
					    lines_arr, one_line.empty() ? yyjson_mut_arr(doc)
					                                : PandocInlineConvert::ConvertDbInlinesToPandocVal(
					                                      doc, one_line, 0, GetElementLevel(block) + 1, inl_end, 1));
					one_line.clear();
				};
				for (auto &inl : inline_children) {
					if (GetElementStringField(inl, DuckBlockTypes::ELEMENT_TYPE_IDX) == DuckBlockTypes::INLINE_LINEBREAK &&
					    GetElementLevel(inl) == GetElementLevel(block) + 1) {
						flush_line();
						continue;
					}
					one_line.push_back(inl);
				}
				flush_line();
				yyjson_mut_obj_add_val(doc, lb_obj, "c", lines_arr);
				yyjson_mut_arr_add_val(blocks_arr, lb_obj);
				block_idx++;
				continue;
			}

			size_t line_start = 0;
			while (line_start <= content.size()) {
				size_t nl = content.find('\n', line_start);
				string line = content.substr(line_start, nl == string::npos ? string::npos : nl - line_start);
				yyjson_mut_val *line_arr = yyjson_mut_arr(doc);
				if (!line.empty()) {
					yyjson_mut_val *str_obj = yyjson_mut_obj(doc);
					yyjson_mut_obj_add_str(doc, str_obj, "t", "Str");
					yyjson_mut_obj_add_strncpy(doc, str_obj, "c", line.data(), line.size());
					yyjson_mut_arr_add_val(line_arr, str_obj);
				}
				yyjson_mut_arr_add_val(lines_arr, line_arr);
				if (nl == string::npos) {
					break;
				}
				line_start = nl + 1;
			}
			yyjson_mut_obj_add_val(doc, lb_obj, "c", lines_arr);
			yyjson_mut_arr_add_val(blocks_arr, lb_obj);
			block_idx++;
		} else if (element_type == DuckBlockTypes::TYPE_GENERIC &&
		           GetElementStringField(block, DuckBlockTypes::ENCODING_IDX) == DuckBlockTypes::ENCODING_JSON &&
		           !content.empty()) {
			// The export half the import side already promised. ProcessPandocBlockVal
			// stores the WHOLE constructor object -- `t` included -- and says in its
			// comment that it does so "so export can reconstitute it including `t`".
			// Nothing did. An unmapped block fell through to the Para fallback below,
			// which emitted its stored JSON as a Str: a round trip printed a raw AST
			// blob into the document body as visible text. That is worse than a drop,
			// because the corruption looks like content.
			//
			// Splicing the object back verbatim round-trips the block LOSSLESSLY --
			// stronger than the inline side manages, where `generic` can only degrade
			// to a Span carrying source_type as a class, its children having become
			// separate elements by the time export sees them.
			yyjson_doc *sub_doc = yyjson_read(content.c_str(), content.size(), 0);
			if (sub_doc) {
				yyjson_mut_arr_add_val(blocks_arr, yyjson_val_mut_copy(doc, yyjson_doc_get_root(sub_doc)));
				yyjson_doc_free(sub_doc);
			} else {
				// Unparseable content: falling through would re-print the blob as text,
				// so emit an empty Div tagged with source_type instead. Lossy in
				// content, but it does not fabricate prose the document never had.
				yyjson_mut_val *gen_obj = yyjson_mut_obj(doc);
				yyjson_mut_obj_add_str(doc, gen_obj, "t", "Div");
				yyjson_mut_val *gc_arr = yyjson_mut_arr(doc);
				yyjson_mut_arr_add_val(
				    gc_arr, CreatePandocAttrVal(doc, block, GetElementAttribute(block, DuckBlockTypes::ATTR_SOURCE_TYPE)));
				yyjson_mut_arr_add_val(gc_arr, yyjson_mut_arr(doc));
				yyjson_mut_obj_add_val(doc, gen_obj, "c", gc_arr);
				yyjson_mut_arr_add_val(blocks_arr, gen_obj);
			}
			block_idx++;
		} else {
			yyjson_mut_val *para_obj = yyjson_mut_obj(doc);
			yyjson_mut_obj_add_str(doc, para_obj, "t", "Para");
			if (!inline_children.empty()) {
				idx_t inl_end = 0;
				yyjson_mut_val *inl_arr =
				    PandocInlineConvert::ConvertDbInlinesToPandocVal(doc, inline_children, 0, 2, inl_end, 1);
				yyjson_mut_obj_add_val(doc, para_obj, "c", inl_arr);
			} else {
				yyjson_mut_val *inl_arr = yyjson_mut_arr(doc);
				yyjson_mut_val *str_obj = yyjson_mut_obj(doc);
				yyjson_mut_obj_add_str(doc, str_obj, "t", "Str");
				yyjson_mut_obj_add_strncpy(doc, str_obj, "c", content.data(), content.size());
				yyjson_mut_arr_add_val(inl_arr, str_obj);
				yyjson_mut_obj_add_val(doc, para_obj, "c", inl_arr);
			}
			yyjson_mut_arr_add_val(blocks_arr, para_obj);
			block_idx++;
		}
	}

	size_t len = 0;
	char *json_out = yyjson_mut_write(doc, 0, &len);
	string res(json_out ? json_out : "[]", len);
	if (json_out) {
		free(json_out);
	}
	yyjson_mut_doc_free(doc);
	return res;
}

static LogicalType GetPandocAstType() {
	child_list_t<LogicalType> struct_children;
	struct_children.push_back(make_pair("pandoc-api-version", LogicalType::LIST(LogicalType::INTEGER)));
	struct_children.push_back(make_pair("meta", LogicalType::JSON()));
	struct_children.push_back(make_pair("blocks", LogicalType::JSON()));
	return LogicalType::STRUCT(std::move(struct_children));
}

// Rebuild one MetaValue from the kind='value' element at `i`, advancing `i` past it
// and everything nested beneath it.
static yyjson_mut_val *BuildMetaValueJson(yyjson_mut_doc *doc, const vector<Value> &blocks_list, idx_t &i,
                                          int32_t my_level, idx_t depth) {
	CheckPandocDepth(depth);
	auto &el = blocks_list[i];
	auto vtype = GetElementStringField(el, DuckBlockTypes::ELEMENT_TYPE_IDX);
	auto content = GetElementStringField(el, DuckBlockTypes::CONTENT_IDX);
	i++; // consume the value element itself

	yyjson_mut_val *obj = yyjson_mut_obj(doc);

	if (vtype == DuckBlockTypes::VALUE_STRING) {
		yyjson_mut_obj_add_str(doc, obj, "t", "MetaString");
		yyjson_mut_obj_add_strncpy(doc, obj, "c", content.data(), content.size());
		return obj;
	}
	if (vtype == DuckBlockTypes::VALUE_BOOL) {
		yyjson_mut_obj_add_str(doc, obj, "t", "MetaBool");
		yyjson_mut_obj_add_bool(doc, obj, "c", content == "true");
		return obj;
	}
	if (vtype == DuckBlockTypes::TYPE_GENERIC) {
		// Preserved verbatim on the way in; hand it back unchanged.
		yyjson_doc *parsed = yyjson_read(content.c_str(), content.size(), 0);
		if (parsed) {
			yyjson_mut_val *copied = yyjson_val_mut_copy(doc, yyjson_doc_get_root(parsed));
			yyjson_doc_free(parsed);
			if (copied) {
				return copied;
			}
		}
		return nullptr;
	}
	if (vtype == DuckBlockTypes::VALUE_INLINES) {
		yyjson_mut_obj_add_str(doc, obj, "t", "MetaInlines");
		vector<Value> inls;
		while (i < blocks_list.size()) {
			auto &child = blocks_list[i];
			if (child.IsNull()) {
				i++;
				continue;
			}
			if (GetElementStringField(child, DuckBlockTypes::KIND_IDX) != DuckBlockTypes::KIND_INLINE) {
				break;
			}
			inls.push_back(child);
			i++;
		}
		idx_t end_idx = 0;
		yyjson_mut_val *arr =
		    inls.empty()
		        ? yyjson_mut_arr(doc)
		        : PandocInlineConvert::ConvertDbInlinesToPandocVal(doc, inls, 0, my_level + 1, end_idx, depth + 1);
		yyjson_mut_obj_add_val(doc, obj, "c", arr);
		return obj;
	}

	// Container shapes: consume every element nested deeper than this one.
	const bool is_map = (vtype == DuckBlockTypes::VALUE_MAP);
	const bool is_blocks = (vtype == DuckBlockTypes::VALUE_BLOCKS);
	yyjson_mut_obj_add_str(doc, obj, "t", is_map ? "MetaMap" : (is_blocks ? "MetaBlocks" : "MetaList"));
	yyjson_mut_val *container = is_map ? yyjson_mut_obj(doc) : yyjson_mut_arr(doc);

	while (i < blocks_list.size()) {
		auto &child = blocks_list[i];
		if (child.IsNull()) {
			i++;
			continue;
		}
		auto child_kind = GetElementStringField(child, DuckBlockTypes::KIND_IDX);
		int32_t child_level = GetElementLevel(child);
		if (child_kind == DuckBlockTypes::KIND_VALUE && child_level <= my_level) {
			break;
		}
		if (child_kind != DuckBlockTypes::KIND_VALUE) {
			// MetaBlocks children are ordinary blocks; anything else here is not ours.
			if (!is_blocks) {
				break;
			}
			i++;
			continue;
		}
		string key = GetElementAttribute(child, "key");
		yyjson_mut_val *v = BuildMetaValueJson(doc, blocks_list, i, child_level, depth + 1);
		if (!v) {
			continue;
		}
		if (is_map) {
			yyjson_mut_obj_add(container, yyjson_mut_strncpy(doc, key.data(), key.size()), v);
		} else {
			yyjson_mut_arr_add_val(container, v);
		}
	}
	yyjson_mut_obj_add_val(doc, obj, "c", container);
	return obj;
}

// Reconstitute the document's `meta` object from its kind='value' elements.
static string BuildMetaJson(const vector<Value> &blocks_list) {
	yyjson_mut_doc *doc = yyjson_mut_doc_new(nullptr);
	yyjson_mut_val *root = yyjson_mut_obj(doc);

	idx_t i = 0;
	while (i < blocks_list.size()) {
		auto &el = blocks_list[i];
		if (el.IsNull()) {
			i++;
			continue;
		}
		if (GetElementStringField(el, DuckBlockTypes::KIND_IDX) != DuckBlockTypes::KIND_VALUE || GetElementLevel(el) != 1) {
			i++;
			continue;
		}
		string key = GetElementAttribute(el, "key");
		yyjson_mut_val *v = BuildMetaValueJson(doc, blocks_list, i, 1, 1);
		if (v && !key.empty()) {
			yyjson_mut_obj_add(root, yyjson_mut_strncpy(doc, key.data(), key.size()), v);
		}
	}

	yyjson_mut_doc_set_root(doc, root);
	size_t len = 0;
	char *json = yyjson_mut_write(doc, 0, &len);
	string res(json ? json : "{}", json ? len : 2);
	if (json) {
		free(json);
	}
	yyjson_mut_doc_free(doc);
	return res;
}

static void DuckBlocksToPandocAstFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &blocks_vec = args.data[0];
	auto count = args.size();

	for (idx_t i = 0; i < count; i++) {
		auto blocks_val = blocks_vec.GetValue(i);

		// pandoc 3.x rejects anything below [1,23] outright; [1,20] made every export
		// unreadable by the installed pandoc. Taken from pandoc_ast_map.hpp rather than
		// written here, so this and the file writer below cannot drift apart.
		vector<Value> api_version_vals = {Value::INTEGER(pandoc_ast::API_VERSION_MAJOR),
		                                  Value::INTEGER(pandoc_ast::API_VERSION_MINOR),
		                                  Value::INTEGER(pandoc_ast::API_VERSION_PATCH)};
		Value api_version = Value::LIST(LogicalType::INTEGER, api_version_vals);
		Value meta = Value("{}");

		if (blocks_val.IsNull()) {
			child_list_t<Value> struct_values;
			struct_values.push_back(make_pair("pandoc-api-version", api_version));
			struct_values.push_back(make_pair("meta", meta));
			struct_values.push_back(make_pair("blocks", Value("[]")));
			result.SetValue(i, Value::STRUCT(std::move(struct_values)));
			continue;
		}

		auto &blocks_list = ListValue::GetChildren(blocks_val);
		string blocks_json = BuildBlocksJson(blocks_list);
		meta = Value(BuildMetaJson(blocks_list));

		child_list_t<Value> struct_values;
		struct_values.push_back(make_pair("pandoc-api-version", api_version));
		struct_values.push_back(make_pair("meta", meta));
		struct_values.push_back(make_pair("blocks", Value(blocks_json)));
		result.SetValue(i, Value::STRUCT(std::move(struct_values)));
	}
}

void PandocBlockConvert::DuckBlocksToPandocBlocksFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &blocks_vec = args.data[0];
	auto count = args.size();

	for (idx_t i = 0; i < count; i++) {
		auto blocks_val = blocks_vec.GetValue(i);

		if (blocks_val.IsNull()) {
			result.SetValue(i, Value("[]"));
			continue;
		}

		auto &blocks_list = ListValue::GetChildren(blocks_val);
		result.SetValue(i, Value(BuildBlocksJson(blocks_list)));
	}
}

static yyjson_mut_val *MapToMetaObj(yyjson_mut_doc *doc, const Value &meta_map) {
	yyjson_mut_val *meta_obj = yyjson_mut_obj(doc);
	if (meta_map.IsNull()) {
		return meta_obj;
	}

	auto &map_entries = MapValue::GetChildren(meta_map);
	for (auto &entry : map_entries) {
		if (entry.IsNull()) {
			continue;
		}
		auto &kv = StructValue::GetChildren(entry);
		if (kv.size() < 2 || kv[0].IsNull() || kv[1].IsNull()) {
			continue;
		}

		string key = kv[0].GetValue<string>();
		string value = kv[1].GetValue<string>();

		yyjson_mut_val *meta_val_obj = yyjson_mut_obj(doc);
		yyjson_mut_obj_add_str(doc, meta_val_obj, "t", "MetaInlines");
		yyjson_mut_val *c_arr = yyjson_mut_arr(doc);
		yyjson_mut_val *str_obj = yyjson_mut_obj(doc);
		yyjson_mut_obj_add_str(doc, str_obj, "t", "Str");
		yyjson_mut_obj_add_strncpy(doc, str_obj, "c", value.data(), value.size());
		yyjson_mut_arr_add_val(c_arr, str_obj);
		yyjson_mut_obj_add_val(doc, meta_val_obj, "c", c_arr);

		yyjson_mut_val *key_val = yyjson_mut_strncpy(doc, key.data(), key.size());
		yyjson_mut_obj_add(meta_obj, key_val, meta_val_obj);
	}
	return meta_obj;
}

void PandocBlockConvert::ReadPandocAstFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &path_vec = args.data[0];
	auto count = args.size();

	for (idx_t i = 0; i < count; i++) {
		auto path_val = path_vec.GetValue(i);

		if (path_val.IsNull()) {
			result.SetValue(i, Value::LIST(DuckBlockTypes::DuckBlockType(), vector<Value>()));
			continue;
		}

		string file_path = path_val.GetValue<string>();

		std::ifstream file(file_path);
		if (!file.is_open()) {
			throw IOException("Could not open file: " + file_path);
		}

		std::stringstream buffer;
		buffer << file.rdbuf();
		string json = buffer.str();

		vector<Value> blocks;
		ConvertPandocAstToBlocks(json, blocks);

		result.SetValue(i, Value::LIST(DuckBlockTypes::DuckBlockType(), std::move(blocks)));
	}
}

struct PandocAstBindData : public TableFunctionData {
	vector<Value> blocks;
	string meta_json = "{}";
	vector<int32_t> api_version = {1, 23, 1};
	bool done = false;
};

static string ConvertMetaMapToJson(const Value &meta_map) {
	if (meta_map.IsNull()) {
		return "{}";
	}

	auto &map_entries = MapValue::GetChildren(meta_map);
	if (map_entries.empty()) {
		return "{}";
	}

	yyjson_mut_doc *doc = yyjson_mut_doc_new(nullptr);
	yyjson_mut_val *root = yyjson_mut_obj(doc);
	yyjson_mut_doc_set_root(doc, root);

	for (auto &entry : map_entries) {
		if (entry.IsNull()) {
			continue;
		}
		auto &kv = StructValue::GetChildren(entry);
		if (kv.size() < 2 || kv[0].IsNull() || kv[1].IsNull()) {
			continue;
		}

		string key = kv[0].GetValue<string>();
		string value = kv[1].GetValue<string>();

		yyjson_mut_val *meta_obj = yyjson_mut_obj(doc);
		yyjson_mut_obj_add_str(doc, meta_obj, "t", "MetaInlines");
		yyjson_mut_val *c_arr = yyjson_mut_arr(doc);
		yyjson_mut_val *str_obj = yyjson_mut_obj(doc);
		yyjson_mut_obj_add_str(doc, str_obj, "t", "Str");
		yyjson_mut_obj_add_strncpy(doc, str_obj, "c", value.data(), value.size());
		yyjson_mut_arr_add_val(c_arr, str_obj);
		yyjson_mut_obj_add_val(doc, meta_obj, "c", c_arr);

		yyjson_mut_val *key_val = yyjson_mut_strncpy(doc, key.data(), key.size());
		yyjson_mut_obj_add(root, key_val, meta_obj);
	}

	size_t len = 0;
	char *json = yyjson_mut_write(doc, 0, &len);
	string res(json ? json : "{}", len);
	if (json) {
		free(json);
	}
	yyjson_mut_doc_free(doc);
	return res;
}

static unique_ptr<FunctionData> PandocAstBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<PandocAstBindData>();

	if (!input.inputs.empty() && !input.inputs[0].IsNull()) {
		auto &blocks_val = input.inputs[0];
		auto &blocks_list = ListValue::GetChildren(blocks_val);
		for (auto &block : blocks_list) {
			result->blocks.push_back(block);
		}
	}

	for (auto &kv : input.named_parameters) {
		if (kv.first == "meta") {
			if (!kv.second.IsNull()) {
				result->meta_json = ConvertMetaMapToJson(kv.second);
			}
		} else if (kv.first == "api_version") {
			if (!kv.second.IsNull()) {
				auto &version_list = ListValue::GetChildren(kv.second);
				result->api_version.clear();
				for (auto &v : version_list) {
					result->api_version.push_back(v.GetValue<int32_t>());
				}
			}
		}
	}

	names.push_back("pandoc-api-version");
	return_types.push_back(LogicalType::LIST(LogicalType::INTEGER));

	names.push_back("meta");
	return_types.push_back(LogicalType::JSON());

	names.push_back("blocks");
	return_types.push_back(LogicalType::JSON());

	return std::move(result);
}

static void PandocAstFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->CastNoConst<PandocAstBindData>();

	if (bind_data.done) {
		return;
	}

	vector<Value> api_version_vals;
	for (auto v : bind_data.api_version) {
		api_version_vals.push_back(Value::INTEGER(v));
	}
	Value api_version = Value::LIST(LogicalType::INTEGER, api_version_vals);

	string blocks_json = BuildBlocksJson(bind_data.blocks);

	CompatSetOutputCardinality(output, 1);
	output.data[0].SetValue(0, api_version);
	output.data[1].SetValue(0, Value(bind_data.meta_json));
	output.data[2].SetValue(0, Value(blocks_json));

	bind_data.done = true;
}

static void WritePandocAstFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &path_vec = args.data[0];
	auto &blocks_vec = args.data[1];
	auto count = args.size();

	// Derived, not written out, so this cannot drift from DuckBlocksToPandocAstFun above.
	const string api_version = "[" + to_string(pandoc_ast::API_VERSION_MAJOR) + "," +
	                           to_string(pandoc_ast::API_VERSION_MINOR) + "," +
	                           to_string(pandoc_ast::API_VERSION_PATCH) + "]";

	for (idx_t i = 0; i < count; i++) {
		auto path_val = path_vec.GetValue(i);
		auto blocks_val = blocks_vec.GetValue(i);

		if (path_val.IsNull()) {
			result.SetValue(i, Value(false));
			continue;
		}

		string file_path = path_val.GetValue<string>();
		std::ofstream file(file_path);
		if (!file.is_open()) {
			throw IOException("Could not open file for writing: " + file_path);
		}

		if (blocks_val.IsNull()) {
			file << "{\"pandoc-api-version\":" << api_version << ",\"meta\":{},\"blocks\":[]}";
			file.close();
			result.SetValue(i, Value(true));
			continue;
		}

		auto &blocks_list = ListValue::GetChildren(blocks_val);
		string blocks_json = BuildBlocksJson(blocks_list);

		// METADATA GOES TO DISK TOO. This wrote a hardcoded "meta":{} while
		// DuckBlocksToPandocAstFun, the same conversion one function up, composed it with
		// BuildMetaJson -- so every kind='value' row survived the in-memory export and
		// vanished from the written file.
		//
		// Two copies of one rule, and only one of them implemented it. Neither body looks
		// wrong on its own, which is exactly why the divergence lasted: nothing compared
		// them. It matters here because panduck deliberately recovers metadata pandoc does
		// not extract at all -- dcterms:created, dc:language, ipynb's authors -- and that
		// claim was true right up until you wrote the file.
		string meta_json = BuildMetaJson(blocks_list);

		file << "{\"pandoc-api-version\":" << api_version << ",\"meta\":" << meta_json << ",\"blocks\":" << blocks_json
		     << "}";
		file.close();
		result.SetValue(i, Value(true));
	}
}

void PandocBlockConvert::Register(ExtensionLoader &loader) {
	// REGISTERS THE WRITE DIRECTION ONLY, UNDER PANDUCK-OWNED NAMES.
	//
	// Every name this body registered upstream -- pandoc_ast_to_blocks, read_pandoc_ast,
	// duck_blocks_to_pandoc_ast, pandoc_ast and the rest -- is still registered by
	// duck_block_utils, and A NAME IS OWNED BY EXACTLY ONE EXTENSION IN THIS FAMILY.
	//
	// Measured, not assumed: when two loaded extensions register the same name, BOTH
	// registrations survive as ambiguous overloads and every call then fails at BIND TIME
	// with "Could not choose a best candidate function" -- naming a construct the caller
	// never wrote. It does not degrade, it breaks, and it would break duckeye's thirteen
	// formats for anyone with both extensions loaded.
	//
	// So the names below are panduck_-prefixed, which is the same choice the read side
	// made in taking read_pandoc_blocks rather than upstream's read_pandoc_ast. When
	// upstream drops its copy after a RELEASED panduck (converter handoff, step 4), the
	// canonical names can be added here as aliases. Not before: the two-copy window is
	// safe and the zero-copy window is not.
	//
	// THE READ SIDE IS STILL NOT REGISTERED HERE. pandoc_ast_to_blocks and read_pandoc_ast
	// remain upstream's; panduck's read surface is read_pandoc_blocks /
	// read_pandoc_blocks_string in pandoc_reader.cpp, and the conversion is reached
	// through ConvertPandocAstToBlocks() rather than through those SQL names.
	//
	// WHY THE WRITE DIRECTION IS REGISTERED AT ALL. panduck's readers are deliberately
	// more faithful than pandoc in places -- richer attributes, better block types, and
	// metadata pandoc does not extract. The standing rule is that being richer is allowed
	// so long as the result is still writable back to VALID pandoc JSON. That rule was
	// unenforceable while nothing could write, so it was an aspiration rather than a
	// constraint. test/sql/pandoc_writer.test is what turns it into a test.
	auto ast_type = LogicalType::STRUCT({{"pandoc-api-version", LogicalType::LIST(LogicalType::INTEGER)},
	                                     {"meta", LogicalType::VARCHAR},
	                                     {"blocks", LogicalType::VARCHAR}});
	auto blocks_type = DuckBlockTypes::DuckBlockListType();

	// SPECIAL_HANDLING, because both bodies contain a deliberate NULL branch that builds an
	// EMPTY DOCUMENT -- a valid AST with no blocks -- and under DuckDB's default null
	// handling that branch CANNOT EXECUTE: the engine propagates NULL before the function
	// is ever called. The code was written as though it ran, which makes it the
	// check-that-cannot-fire shape rather than a policy anyone chose.
	//
	// Making it reachable is the right resolution rather than deleting it. A document with
	// no readable blocks exporting as a valid empty AST is a usable answer; a NULL makes
	// the caller's own write fail further downstream, where the cause is no longer visible.
	// Now is the moment to settle it -- these names have no callers yet.
	auto to_ast = ScalarFunction("panduck_blocks_to_pandoc_ast", {blocks_type}, ast_type, DuckBlocksToPandocAstFun);
	to_ast.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	loader.RegisterFunction(to_ast);

	loader.RegisterFunction(ScalarFunction("panduck_blocks_to_pandoc_blocks", {blocks_type}, LogicalType::VARCHAR,
	                                       PandocBlockConvert::DuckBlocksToPandocBlocksFun));

	auto write_ast = ScalarFunction("panduck_write_pandoc_ast", {LogicalType::VARCHAR, blocks_type},
	                                LogicalType::BOOLEAN, WritePandocAstFun);
	write_ast.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	loader.RegisterFunction(write_ast);
}

} // namespace duckdb
