#include "duck_block_repair.hpp"

#include "duck_block_types.hpp"
#include "panduck_duckdb_compat.hpp"
#include "duckdb/common/types/value.hpp"

// Ported verbatim from duck_block_utils v3.1.0 (6c1c2e5) src/repair.cpp, with two changes
// and no others: BlockTypes:: -> DuckBlockTypes:: for panduck's class name, and the public
// duck_blocks_repair registration omitted (see the header for why). Keeping the passes
// byte-comparable to upstream is the point -- check-divergence compares the two copies.

namespace duckdb {
namespace panduck {

namespace {

// A working copy of one element: the fields the passes read and rewrite, plus the
// original Value, from which every OTHER field is carried verbatim on output.
struct El {
	string kind;
	string type;
	int32_t level;
	Value value;
};

string Field(const Value &v, idx_t idx) {
	auto &c = StructValue::GetChildren(v);
	return (idx < c.size() && !c[idx].IsNull()) ? c[idx].GetValue<string>() : string();
}

int32_t IntField(const Value &v, idx_t idx, int32_t def) {
	auto &c = StructValue::GetChildren(v);
	return (idx < c.size() && !c[idx].IsNull()) ? c[idx].GetValue<int32_t>() : def;
}

// Rebuild a duck_block Value with a new level and order, every other field verbatim.
Value WithLevelAndOrder(const Value &v, int32_t level, int32_t order) {
	auto &c = StructValue::GetChildren(v);
	child_list_t<Value> out;
	out.push_back(make_pair("kind", c[DuckBlockTypes::KIND_IDX]));
	out.push_back(make_pair("element_type", c[DuckBlockTypes::ELEMENT_TYPE_IDX]));
	out.push_back(make_pair("content", c[DuckBlockTypes::CONTENT_IDX]));
	out.push_back(make_pair("level", Value(level)));
	out.push_back(make_pair("encoding", c[DuckBlockTypes::ENCODING_IDX]));
	out.push_back(make_pair("attributes", c[DuckBlockTypes::ATTRIBUTES_IDX]));
	out.push_back(make_pair("element_order", Value(order)));
	return Value::STRUCT(std::move(out));
}

// The wrapper an orphan run gets. A `list` says which kind of list it is, because
// list_type is the canonical attribute and a consumer reads it.
Value Wrapper(const string &type, int32_t level) {
	vector<Value> keys, vals;
	if (type == DuckBlockTypes::TYPE_LIST) {
		keys.push_back(Value(DuckBlockTypes::ATTR_LIST_TYPE));
		vals.push_back(Value(DuckBlockTypes::LIST_TYPE_BULLET));
	}
	child_list_t<Value> out;
	out.push_back(make_pair("kind", Value(DuckBlockTypes::KIND_BLOCK)));
	out.push_back(make_pair("element_type", Value(type)));
	out.push_back(make_pair("content", Value("")));
	out.push_back(make_pair("level", Value(level)));
	out.push_back(make_pair("encoding", Value(DuckBlockTypes::ENCODING_TEXT)));
	out.push_back(make_pair("attributes",
	                        Value::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR, std::move(keys), std::move(vals))));
	out.push_back(make_pair("element_order", Value(0)));
	return Value::STRUCT(std::move(out));
}

struct Frame {
	int32_t level;
	string kind;
	string type;
};

// Is this element's ancestor requirement met by what is on the stack?
bool Satisfied(const El &e, const vector<Frame> &stack) {
	const char *parent = DuckBlockTypes::ImplicitParentOf(e.type.c_str(), e.kind.c_str());
	if (parent[0] == '\0') {
		return true;
	}
	for (auto &f : stack) {
		if (e.kind == DuckBlockTypes::KIND_INLINE) {
			// An inline needs a non-inline above it: a block, or a `value` element
			// whose inline children belong to it.
			if (f.kind != DuckBlockTypes::KIND_INLINE) {
				return true;
			}
			continue;
		}
		if (f.kind == DuckBlockTypes::KIND_BLOCK &&
		    DuckBlockTypes::RequiresAncestor(e.type.c_str(), e.kind.c_str(), f.type.c_str())) {
			return true;
		}
	}
	return false;
}

// Pass 1 (L4, L5): wrap each maximal run of orphans that share an implicit parent.
// A run starts at an element lacking its required ancestor and continues through
// consecutive elements that are either the same kind of orphan at the same level or
// deeper than it (its descendants). The wrapper goes at level-1; if that would be 0
// the whole run shifts down by one instead, so nothing sits at level 0.
void WrapOrphans(vector<El> &els) {
	vector<El> out;
	vector<Frame> stack;
	idx_t i = 0;
	while (i < els.size()) {
		const El &e = els[i];
		while (!stack.empty() && stack.back().level >= e.level) {
			stack.pop_back();
		}
		if (Satisfied(e, stack)) {
			stack.push_back({e.level, e.kind, e.type});
			out.push_back(e);
			i++;
			continue;
		}
		const string wrap_type = DuckBlockTypes::ImplicitParentOf(e.type.c_str(), e.kind.c_str());
		const int32_t run_level = e.level;
		const bool run_is_inline = e.kind == DuckBlockTypes::KIND_INLINE;
		const string run_kind = e.kind;
		const string run_type = e.type;
		idx_t j = i;
		while (j < els.size()) {
			const El &x = els[j];
			const bool same_orphan =
			    x.level == run_level &&
			    (run_is_inline ? x.kind == DuckBlockTypes::KIND_INLINE : (x.kind == run_kind && x.type == run_type));
			if (!(same_orphan || x.level > run_level)) {
				break;
			}
			j++;
		}
		const int32_t shift = run_level <= 1 ? 1 : 0; // the wrapper cannot sit at level 0
		El w;
		w.kind = DuckBlockTypes::KIND_BLOCK;
		w.type = wrap_type;
		w.level = run_level - 1 + shift;
		w.value = Wrapper(wrap_type, w.level);
		out.push_back(w);
		for (idx_t k = i; k < j; k++) {
			El c = els[k];
			c.level += shift;
			out.push_back(c);
		}
		stack.push_back({w.level, w.kind, w.type});
		i = j;
	}
	els.swap(out);
}

// Pass 2 (L2): the shallowest element sits at 1.
void Rebase(vector<El> &els) {
	bool seen = false;
	int32_t min_level = 0;
	for (auto &e : els) {
		if (!seen || e.level < min_level) {
			seen = true;
			min_level = e.level;
		}
	}
	if (seen && min_level != 1) {
		for (auto &e : els) {
			e.level -= (min_level - 1);
		}
	}
}

// Pass 3 (L3): a jump from p to l > p+1 pulls the jumped element and everything at or
// below its depth, until something shallower than l, up by the excess.
void CollapseJumps(vector<El> &els) {
	int32_t prev = 0;
	for (idx_t i = 0; i < els.size(); i++) {
		const int32_t l = els[i].level;
		if (l > prev + 1) {
			const int32_t excess = l - (prev + 1);
			for (idx_t j = i; j < els.size() && els[j].level >= l; j++) {
				els[j].level -= excess;
			}
		}
		prev = els[i].level;
	}
}

} // namespace

void RepairBlocks(vector<Value> &blocks) {
	vector<El> els;
	for (auto &v : blocks) {
		if (v.IsNull()) {
			continue;
		}
		El e;
		e.kind = Field(v, DuckBlockTypes::KIND_IDX);
		e.type = Field(v, DuckBlockTypes::ELEMENT_TYPE_IDX);
		e.level = IntField(v, DuckBlockTypes::LEVEL_IDX, 1);
		e.value = v;
		els.push_back(std::move(e));
	}
	WrapOrphans(els);
	Rebase(els);
	CollapseJumps(els);
	vector<Value> out;
	int32_t order = 0; // Pass 4 (L1): dense from 0 in list order.
	for (auto &e : els) {
		out.push_back(WithLevelAndOrder(e.value, e.level, order++));
	}
	blocks.swap(out);
}

} // namespace panduck
} // namespace duckdb
