#pragma once

// The duck_block vocabulary comes from duck_block_utils' PUBLISHED header, VENDORED into
// this directory as src/include/duck_block_vocabulary.hpp. The header is link-free by
// design, so including it costs no linking and there is exactly one definition of every
// element_type name in this build.
//
// WHY VENDORED RATHER THAN A SUBMODULE. It was a submodule, and the header's own banner
// still recommends that. duck_block_utils has since decided the other way: a whole
// submodule checkout to put ONE 167-line constants header on the include path is a large
// mechanism for a small dependency, and it costs every consumer a --recurse-submodules
// clone plus a pin to keep current. The header carries no code, so the usual argument for
// a submodule -- avoiding a divergent fork of real logic -- does not apply.
//
// VENDORED IS NOT A LICENCE TO EDIT. src/include/duck_block_vocabulary.hpp is a
// byte-for-byte copy -- ALMOST. `make format-check` runs clang-format over src/ and does
// not exempt vendored files, so the copy acquires this repo's comment alignment on arrival.
// That is whitespace inside comments and nothing else, and it does not matter BECAUSE the
// check below compares names and values rather than diffing text. Recorded because
// "byte-for-byte" is what the paragraph used to say, and a doctrine that is quietly false
// is worse than one that states its own exception. Editing it locally is one failure mode; upstream moving without us
// is the other, and NEITHER is caught by the compiler in the way you would hope:
//
//     TYPE_HEADING -> TYPE_HEAD               a RENAME: compile error at every use site
//     TYPE_PAGE = "page_break" -> "pagebreak" a VALUE change: compiles CLEAN
//
// The constants protect against a rename and nothing else. A changed value compiles,
// every test written against its own literals keeps passing, and the readers silently
// stop emitting a type consumers recognise. So the copy comes with a check:
//
//     make check-vocabulary        # scripts/check_duck_block_vocabulary.py
//
// It compares BY NAME AND VALUE rather than diffing text, which is what lets it stay
// silent about churn that changes nothing: upstream rewrote every idx_t to uint64_t and
// later added ~88 lines of guidance without moving one name or value. A text diff screams
// at that, gets muted, and then catches nothing on the day it matters.
//
// Copied from duck_block_utils main @ b3b1e26, SPEC_VERSION 6.3 (publicly, the
// duck_blocks v1.1 spec -- the two are DELIBERATELY separate axes and
// duck_block_spec_version() still reports the 6.x number). Being behind by commits
// is not the same as being wrong -- what makes the copy correct is that check-vocabulary
// reports it in sync, not that the sha is the newest.
//
// WHAT THIS COPY DOES NOT COVER. It is a COMPILE-TIME dependency on constant names, and
// nothing more. The functions panduck calls at runtime -- duck_blocks_toc and friends,
// used by the doc_* macros -- come from whatever duck_block_utils is INSTALLED, which is
// the community build. So this header being current says NOTHING about whether those
// calls still resolve. They are two independent clocks, and reading one as evidence about
// the other is exactly the "fixed upstream is not fixed installed" mistake -- demonstrated
// on 2026-09-04, when the community build renamed db_blocks_* to duck_blocks_* and broke
// doc_toc and doc_render at runtime while this header, and the whole compile, stayed green.
#include "duck_block_vocabulary.hpp"
#include "duckdb.hpp"
#include "duckdb/common/types.hpp"

namespace duckdb {

/**
 * DuckBlockTypes provides type definitions and utilities for working with doc_element structures.
 *
 * This is a header-only interface that mirrors the duck_block_utils extension's type definitions,
 * enabling webbed to produce doc_element output without a compile-time dependency on duck_block_utils.
 *
 * The doc_element type represents a document element with the following structure:
 * STRUCT(
 *     kind VARCHAR,            -- 'block' or 'inline'
 *     element_type VARCHAR,    -- 'heading', 'paragraph', 'code', etc.
 *     content VARCHAR,         -- The element's text content
 *     level INTEGER,           -- Hierarchy level (NULL if not applicable)
 *     encoding VARCHAR,        -- 'text', 'json', 'yaml', 'html', 'xml'
 *     attributes MAP(VARCHAR, VARCHAR),  -- Key-value metadata
 *     element_order INTEGER    -- Position in document (0-indexed)
 * )
 *
 * For headings, the heading level (1-6) is stored in attributes['heading_level'],
 * not in the 'level' field. The 'level' field is reserved for hierarchy depth.
 */
class DuckBlockTypes : public DuckBlockVocabulary {
public:
	// Create the doc_element type (unified type for both blocks and inlines)
	static LogicalType DuckBlockType() {
		child_list_t<LogicalType> struct_children;
		struct_children.push_back(make_pair("kind", LogicalType::VARCHAR));
		struct_children.push_back(make_pair("element_type", LogicalType::VARCHAR));
		struct_children.push_back(make_pair("content", LogicalType::VARCHAR));
		struct_children.push_back(make_pair("level", LogicalType::INTEGER));
		struct_children.push_back(make_pair("encoding", LogicalType::VARCHAR));
		struct_children.push_back(
		    make_pair("attributes", LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR)));
		struct_children.push_back(make_pair("element_order", LogicalType::INTEGER));

		return LogicalType::STRUCT(std::move(struct_children));
	}

	// Alias for semantic clarity
	static LogicalType DocElementType() {
		return DuckBlockType();
	}

	// Create a LIST(doc_element) type
	static LogicalType DuckBlockListType() {
		return LogicalType::LIST(DuckBlockType());
	}

	// Alias for semantic clarity
	static LogicalType DocElementListType() {
		return DuckBlockListType();
	}

	// Field indices for doc_element struct
	static constexpr idx_t KIND_IDX = 0;
	static constexpr idx_t ELEMENT_TYPE_IDX = 1;
	static constexpr idx_t CONTENT_IDX = 2;
	static constexpr idx_t LEVEL_IDX = 3;
	static constexpr idx_t ENCODING_IDX = 4;
	static constexpr idx_t ATTRIBUTES_IDX = 5;
	static constexpr idx_t ELEMENT_ORDER_IDX = 6;

	// Kind values

	// Core block type names

	// Inline element type names

	// ENCODING_* AND ATTR_HEADING_LEVEL ARE INHERITED, not redeclared here.
	//
	// They WERE declared locally, shadowing DuckBlockVocabulary's. That built clean --
	// C++ name hiding is legal, not an error -- so every `DuckBlockTypes::ENCODING_JSON`
	// in this repo silently resolved to the local copy while appearing to use the
	// vendored vocabulary. All six values were byte-identical, so nothing behaved
	// differently; the hazard was that upstream could change one and this copy would
	// keep the old value, compile, and pass every check.
	//
	// It is invisible to duck_block_utils' consumer-alignment check BY CONSTRUCTION:
	// that compares the VENDORED header against canonical, and the vendored header was
	// always correct. The divergence would have lived in the subclass that hides it.
	// Raised by duck_block_utils after duckdb_webbed hit the same shape.
	//
	// Verified byte-identical BEFORE removing rather than after -- deleting first and
	// checking later is how a real value difference becomes an unexplained behaviour
	// change three commits downstream.

	// MIME type for frontmatter in HTML (RFC 9512 compliant)
	static constexpr const char *FRONTMATTER_MIME_TYPE = "application/vnd.frontmatter+yaml";

	// Attribute keys: see the note above -- ATTR_HEADING_LEVEL is inherited.

	// Helper to create an attributes MAP from a std::map
	static Value CreateAttributesMap(const std::map<std::string, std::string> &attrs) {
		vector<Value> keys;
		vector<Value> values;
		for (auto &entry : attrs) {
			keys.push_back(Value(entry.first));
			values.push_back(Value(entry.second));
		}
		return Value::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR, keys, values);
	}

	// Helper to create a doc_element Value (block kind)
	static Value CreateBlock(const std::string &element_type, const std::string &content, const Value &level,
	                         const std::string &encoding, const std::map<std::string, std::string> &attributes,
	                         int32_t element_order = 0) {
		child_list_t<Value> struct_values;
		struct_values.push_back(make_pair("kind", Value(KIND_BLOCK)));
		struct_values.push_back(make_pair("element_type", Value(element_type)));
		// Empty content => NULL (spec convention for containers whose text lives
		// in structured inline children).
		struct_values.push_back(make_pair("content", content.empty() ? Value(LogicalType::VARCHAR) : Value(content)));
		struct_values.push_back(make_pair("level", level));
		struct_values.push_back(make_pair("encoding", Value(encoding)));
		struct_values.push_back(make_pair("attributes", CreateAttributesMap(attributes)));
		struct_values.push_back(make_pair("element_order", Value(element_order)));

		return Value::STRUCT(std::move(struct_values));
	}

	// Convenience overload for blocks without level
	static Value CreateBlock(const std::string &element_type, const std::string &content, const std::string &encoding,
	                         const std::map<std::string, std::string> &attributes, int32_t element_order = 0) {
		return CreateBlock(element_type, content, Value(), encoding, attributes, element_order);
	}

	// Helper to create an inline doc_element Value
	static Value CreateInline(const std::string &element_type, const std::string &content, const Value &level,
	                          const std::string &encoding, const std::map<std::string, std::string> &attributes,
	                          int32_t element_order = 0) {
		child_list_t<Value> struct_values;
		struct_values.push_back(make_pair("kind", Value(KIND_INLINE)));
		struct_values.push_back(make_pair("element_type", Value(element_type)));
		// Empty content => NULL (a formatting container that recurses into
		// structured child inlines carries no literal content of its own).
		struct_values.push_back(make_pair("content", content.empty() ? Value(LogicalType::VARCHAR) : Value(content)));
		struct_values.push_back(make_pair("level", level));
		struct_values.push_back(make_pair("encoding", Value(encoding)));
		struct_values.push_back(make_pair("attributes", CreateAttributesMap(attributes)));
		struct_values.push_back(make_pair("element_order", Value(element_order)));

		return Value::STRUCT(std::move(struct_values));
	}
};

} // namespace duckdb
