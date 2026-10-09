#pragma once

#include <map>
#include <string>
#include <utility>

// THE ENGINE'S CURRENCY: one duck_block row, with no DuckDB in it.
//
// Every reader panduck maintains produces the same seven-column duck_block row,
// and until now every reader declared its OWN private copy of that struct
// (IpynbRow, RstRow, ...) inside its DuckDB-facing half. Eleven copies of one
// vocabulary type, none of them reachable by a non-DuckDB consumer. This is the
// single shared declaration they collapse into (issue #104, milestone L1).
//
// Formats panduck does NOT maintain -- pdf and toml today -- are reached through
// adapters at the edge rather than by widening this type. The vocabulary is the
// contract; an adapter's job is to meet it.

namespace panduck {

//! A value that may be absent, distinctly from being empty.
//!
//! WHY THIS EXISTS AT ALL, since it reads like a reinvented std::optional: the
//! engine compiles at C++11 (libpanduck/CMakeLists.txt), where std::optional is
//! not available. It is deliberately minimal -- absent-or-present and nothing
//! else -- because that is the whole distinction the vocabulary needs.
//!
//! AND WHY THE DISTINCTION MATTERS -- CORRECTED 2026-10-08. This comment used to
//! read "An empty document is a Doc row plus a Text row whose content is the
//! empty string; NULL means the field is absent. ... Teague's ruling,
//! 2026-10-05." Both halves of that were wrong, and the error spread from here
//! to six test files and docs/libpanduck.md before it was caught.
//!
//! It was not a ruling. On 2026-10-05 Teague asked a QUESTION -- would an empty
//! document be a Doc row, a Text row holding `""`, whereas null is null? -- and
//! it was written down here as his answer to it. Nobody decided anything.
//!
//! CORRECTED AGAIN, SAME DAY, and this one is the more instructive error. An
//! earlier version of this very paragraph said the vocabulary "cannot say that
//! sentence" -- that `KIND_*` is only `block`/`inline`/`value` and none of the 25
//! `TYPE_*` constants is a document or root, so "a Doc row" was never
//! expressible. All of that is true of THIS FILE'S NEIGHBOUR and false of the
//! spec.
//!
//! `src/include/duck_block_vocabulary.hpp` WAS a vendored copy stamped at upstream
//! `95a84e6` = duck_block_utils **v3.3.0**, while upstream was on **v3.5.0** -- it
//! has since been re-vendored to `e00db698`, so the gap described here is closed
//! and the reasoning is kept only because the mistake it caused is instructive. In
//! v3.4.0 (2026-09-16) upstream added
//!
//!     static constexpr const char *TYPE_DOCUMENT = "document";
//!
//! and legalised `level = 0` for exactly that row and nothing else. So the
//! question asked on 2026-10-05 was WELL FOUNDED -- it was about a document root
//! that had existed upstream for three weeks. The gap was panduck's, not the
//! question's.
//!
//! WHY THE STALENESS WAS INVISIBLE, which is the part worth carrying: upstream
//! declined to bump the version for that amendment, deliberately ("add it to 1.4,
//! we don't need to churn versions any more"), and recorded the consequence in
//! the header itself -- "two builds can both say SPEC_VERSION 1.4 and differ on
//! whether they accept a level-0 root, and a consumer cannot tell them apart from
//! the version alone." Three dbu releases all say 1.4. Reading `SPEC_VERSION`
//! here and matching it against upstream's therefore proves nothing, and that is
//! documented, not accidental. Upstream also prescribes the remedy: "a consumer
//! that needs to know tests for TYPE_DOCUMENT's presence in its vendored copy."
//!
//! The lesson for anyone measuring this repo against the spec: a vendored copy is
//! not the spec, and a version string that two different vocabularies share is
//! not a comparator. Re-vendoring is tracked separately from the semantics
//! question.
//!
//! AND THE ANSWER WAS ALREADY WRITTEN DOWN -- CORRECTED AGAIN 2026-10-09, the
//! fourth pass over this one comment and the one that should have been first.
//!
//! An earlier version of this paragraph said the collapse of `""` to SQL NULL was
//! "an accident of the type, not a decision", that producers therefore "disagree",
//! and that duck_block_utils#60 had the question open with an applicability rule
//! under discussion. All of that is wrong. duck_block ruled on it in
//! `docs/duck_blocks_spec.md` on 2026-09-01, commit 9c22810, five weeks before
//! anyone here asked:
//!
//!     Consumers MUST treat NULL and `''` as the same absence. Producers SHOULD
//!     emit NULL. The portable test is `coalesce(content, '') <> ''`.
//!
//! And it had already weighed the distinction this file was about to argue for,
//! declining it in terms that name the cost: "an INTENTIONALLY empty value cannot
//! be distinguished from a container carrying no content ... If a body case ever
//! needs the distinction, the fix is a real one and not a re-spelling."
//!
//! SO THERE IS NOTHING TO FIX IN THE READERS. What every DuckDB tail does --
//!
//!     HasContent() ? Value(row.content.value) : Value(LogicalType::VARCHAR)
//!
//! -- is the PREFERRED spelling, not a lossy artifact. The pandoc reader emitting
//! `''` is conformant but not preferred, which makes it a style item rather than
//! the divergence it was reported as. #60 is closed as already-answered.
//!
//! WHICH LEAVES `Nullable<T>` NEEDING AN HONEST JUSTIFICATION, since the one above
//! it ("the row type has to be ABLE to tell absent from empty, whichever way #60
//! lands") assumed a pending wire-format decision that does not exist.
//!
//! The real one is narrower and better: `HasContent()` IS the spec's portable
//! test. `!is_null && !value.empty()` is exactly `coalesce(content, '') <> ''`,
//! with a name on it, in one place, instead of eleven readers each spelling a
//! two-part condition and one of them eventually spelling it differently. That is
//! worth a type on its own, and it is all this type claims.
//!
//! What it does NOT claim: that the distinction it can carry internally will ever
//! reach SQL. By the rule above it should not, and if a body case ever needs it
//! the spec says that takes a real mechanism rather than re-reading these two
//! spellings.
//!
//! The one genuinely open question from all of this is narrower still and is not
//! about `content` at all -- whether an empty document should have a spine, now
//! that TYPE_DOCUMENT makes a level-0 root expressible:
//! https://github.com/teaguesterling/duckdb_duck_block_utils/issues/63
template <class T>
struct Nullable {
	T value;
	bool is_null = false;

	Nullable() = default;
	Nullable(T v) : value(std::move(v)), is_null(false) { // NOLINT: implicit on purpose
	}

	static Nullable<T> Null() {
		Nullable<T> n;
		n.is_null = true;
		return n;
	}

	//! True when this carries a value that is present AND non-empty. Named for
	//! what callers actually ask, so no site has to spell the two conditions and
	//! risk spelling them differently.
	bool HasContent() const {
		return !is_null && !value.empty();
	}
};

using NullableString = Nullable<std::string>;

//! One duck_block row. Field names and order match the vocabulary's seven
//! columns exactly -- kind, element_type, content, level, encoding, attributes,
//! element_order -- because a consumer reading SQL output and a consumer reading
//! this struct should not have to translate between them.
struct Block {
	std::string kind;
	std::string element_type;

	//! NULLABLE, and the only field that is today. Every reader currently leaves
	//! this as a plain string and lets the DuckDB layer map "" to SQL NULL; that
	//! mapping is a lossy artifact rather than the semantics, and this type is
	//! what makes fixing it possible. See the migration note below.
	NullableString content;

	int32_t level = 0;
	std::string encoding;
	std::map<std::string, std::string> attributes;
	int32_t element_order = 0;
};

// MIGRATION NOTE, so the next change to this is deliberate rather than a
// surprise.
//
// Today every reader's Scan writes content as
//
//     row.content.empty() ? Value(LogicalType::VARCHAR) : Value(row.content)
//
// i.e. an empty string becomes SQL NULL. Introducing Nullable did NOT change
// that: the emission helper preserves it exactly, treating absent and empty
// alike, so that commit was a refactor and nothing else.
//
// AND IT SHOULD STAY THAT WAY. An earlier version of this note said changing a
// reader to "emit a real empty string" would be a separate per-reader change with
// its own test, which read as a plan. It is not one: the spec's rule is that
// producers SHOULD emit NULL, so this behaviour is already the preferred one and
// the change that note contemplated would move AWAY from it. If a case ever
// genuinely needs absent and empty told apart, the spec is explicit that the fix
// is a real mechanism and not a re-spelling of these two.

} // namespace panduck
