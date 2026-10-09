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
//! `src/include/duck_block_vocabulary.hpp` is a vendored copy stamped at upstream
//! `95a84e6` = duck_block_utils **v3.3.0**, and upstream is on **v3.5.0**. In
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
//! WHAT IS ACTUALLY TRUE, measured against build/release on 2026-10-08. Where
//! `content` is JSON the distinction already survives, because JSON forces the
//! choice: org `| a |   | c |` yields `{"headers":[],"rows":[["a","","c"]]}` --
//! the empty cell is `""`. Where `content` is text it collapses, because every
//! DuckDB tail emits
//! `HasContent() ? Value(row.content.value) : Value(LogicalType::VARCHAR)` and
//! `std::string` has no null state; the information dies at the emission
//! boundary. That was an accident of the type, not a decision -- and producers
//! disagree today because of it: an empty org code block gives NULL, a
//! declared-but-valueless `#+TITLE:` gives NULL, latex `\section{}` drops the
//! row entirely, and the pandoc reader gives `''`.
//!
//! IT IS OPEN, and duck_block owns it as vocabulary owner:
//! https://github.com/teaguesterling/duckdb_duck_block_utils/issues/60
//! The candidate rule under discussion there is about APPLICABILITY, not
//! emptiness: NULL would mean `content` does not apply to this `element_type`
//! (a `list`, whose children are separate rows), `''` that it applies and is
//! empty. Teague has indicated he favours that direction; it is not ratified.
//! The empty-document case needs the document-level concept the vocabulary does
//! not have, which is question 4 on that issue.
//!
//! None of which puts `Nullable<T>` in question. Whichever way #60 lands, the
//! row type has to be ABLE to tell absent from empty, and it is the only layer
//! that currently can. The type stays; only the justification above it was wrong.
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
// i.e. an empty string becomes SQL NULL. Introducing Nullable does NOT change
// that: the emission helper preserves it exactly, treating absent and empty
// alike, so this commit is a refactor and nothing else. Changing a reader to
// emit a real empty string is a separate, per-reader change with its own test --
// otherwise a semantic change rides along inside a refactor and no failure is
// attributable to either.

} // namespace panduck
