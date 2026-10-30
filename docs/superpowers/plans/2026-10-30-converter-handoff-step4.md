# Converter handoff, step 4: the canonical names move to panduck

**Date:** 2026-10-30
**Follows:** `docs/superpowers/specs/2026-09-01-pandoc-converter-handoff.md` (steps 1–3 done)
**Decisions (Teague, 2026-10-30):**

- `read_pandoc_ast` and its siblings move to panduck. "Dbu is only supposed to be
  utility macros." This supersedes the earlier "delete it" answer for #136.
- Same-day paired release: both registry PRs go up together, and either merge order is
  accepted.
- dbu's tests that use the converter as machinery are rewritten with dbu's own `db_*`
  builders. Converter-specific tests move to panduck. dbu takes no dependency on panduck.

## The surface, measured

From `duckdb_functions()` on a dbu build (`snape-ci-pin` worktree, DuckDB v2.0.0) and on
panduck `b762217` (v1.5.6):

| dbu name | kind | overloads | panduck today |
|---|---|---|---|
| `pandoc_ast_to_blocks(VARCHAR)` | scalar | 1 | `panduck_pandoc_ast_to_blocks` |
| `read_pandoc_ast(VARCHAR)` | scalar, VOLATILE | 1 | unregistered (`ReadPandocAstFun`, #136) |
| `write_pandoc_ast(VARCHAR, blocks)` | scalar, VOLATILE | 1 | `panduck_write_pandoc_ast`, **CONSISTENT** |
| `duck_blocks_to_pandoc_ast(blocks)` | scalar | 1 | `panduck_blocks_to_pandoc_ast`, meta/blocks **VARCHAR** (dbu: JSON) |
| `duck_blocks_to_pandoc_blocks(blocks)` | scalar | 1 | `panduck_blocks_to_pandoc_blocks` |
| `pandoc_inlines_to_db_inlines(VARCHAR)` | scalar | 1 | unregistered |
| `pandoc_inlines_to_text(VARCHAR[, VARCHAR])` | scalar | 2 | unregistered |
| `duck_blocks_inlines_to_pandoc(blocks / blocks[])` | scalar | 2 | unregistered |
| `pandoc_ast(blocks, meta, api_version)` | table macro | 1 | none |

panduck's copy of the converter has every implementation function dbu's has. Neither
copy reads or writes through DuckDB's file system (dbu #65, panduck #136 and #144).

The copies have diverged: 2002 differing diff lines in `pandoc_block_convert.cpp`, 468 in
`pandoc_inline_convert.cpp`, and `CheckPandocDepth` sites 8 (dbu) against 10 (panduck).
That is a signal to investigate, not a diagnosis.

## Phase 0 result: the differential, 2026-10-30

Stock DuckDB v1.5.6 with community `duck_block_utils` `e00db69` and a local panduck build
at `b762217`, loaded into one session. The input set was 68 Pandoc ASTs: pandoc 3.1.3
over 56 readable fixtures under `test/`, the 3 `*.pandoc.json` fixtures, and 9
adversarial inputs. Each input went through 3 pairs: `*_to_blocks`, the round trip to
Pandoc blocks, and the round trip to the AST struct. That makes 204 comparisons: 136
are identical and 68 differ. Neither copy fails where the other succeeds.

**Errors are identical.** Depth 200 and depth 50,000 give the same `Invalid Input Error:
... maximum nesting depth of 128` from both copies. Malformed, empty, non-object, old-API
and unknown-constructor inputs give the same result from both.

**Every difference is a behaviour difference in panduck's copy.** Rows 3, 4 and 6 were
first read as deliberate panduck choices. They were not. Each is a panduck LOSS of a fix
that dbu made after the 2026-09-01 copy (see the follow-up below):

| class | dbu (community) | panduck | inputs | verdict |
|---|---|---|---|---|
| footnote body | `note` row with no children: the footnote text is lost | the note's inlines as children | 14 | panduck keeps |
| code block content | keeps the trailing `\n` | strips it | 9 | deliberate (`pandoc_block_convert.cpp`, CodeBlock read arm) |
| link `title` | keeps pandoc's `wikilink` marker | drops it | 3 | panduck loss: no link title read or written (dbu faaebf7) |
| image `title` equal to its alt | keeps it | drops it | 1 | panduck loss: no image title read or written (dbu faaebf7) |
| citation | dropped | `[@knuth1984]` kept as text | 1 | panduck keeps |
| write: inline image alt | keeps the alt (`Str "image"` in rich.rtf is the image's real alt text) | writes an empty alt | 1 | panduck loss: every inline image's alt (dbu faaebf7) |
| write: bare `deflist` block (superseded type) | `Para` | `Div` | sweep | deliberate (#81) |

The seventh row is not from the differential: pandoc never emits the superseded `deflist`
type, so no AST in the corpus contains it. `test/converter/check_roundtrip_sweep.py` found
it, run against panduck's own converter for the first time. Its `INHERENT` entry for
`deflist` recorded `paragraph`, which was dbu's behaviour; it is re-baselined to `div`
(c8d5ece). The sweep's later arms also need dbu's vocabulary and render functions
(`duck_block_type_names`, `duck_blocks_render_ansi`, `duck_blocks_to_text`). On this branch
the community dbu cannot load beside panduck (its init fails on the canonical names), so
those arms can run only with the NEW dbu loaded next to panduck. Phase 3 runs it.

The table does not list figure captions, which panduck lost and dbu kept (10 inputs), or
three losses that both copies share. The follow-up found them with a word-level round
trip.

## Phase 0 follow-up: dbu's post-copy fixes, ported (2026-10-30)

Teague's ruling: "Fix panduck first". Port the fixes, add a permanent word-loss guard to
`make check`, and correct this list.

**The guard.** `test/converter/check_converter_wordloss.py` (`make
check-converter-wordloss`) builds pandoc ASTs from every fixture and round-trips them
through panduck alone. Every word in must come out. Before the port, 14 of 59 ASTs lost
words. After it, 0 lose words, with an empty `EXEMPT`. dbu's copy, measured the same way,
loses words on 17.

**Each dbu converter commit since the copy, decided by test or probe:**

| dbu commit | in panduck? |
|---|---|
| faaebf7 inline image alt, link/image title | ported, e56b59b |
| 442ac16 inline flattener recurses (formatted table cells) | already in (probes, dbu's three inputs: a `Strong` cell gives `BoldCell`; a `Link`+`Code` cell gives `text and co**de`, with no URL; an `Image` cell gives `alt text`, with no `pic.png`) |
| fb4a7c9 `read_pandoc_ast` VOLATILE | already in, and superseded by the VFS read (`duckdb_functions()`: VOLATILE) |
| b3c9183 a rich heading keeps its flattened title and its children | already in (probe: `heading 1 Bold title` plus children) |
| 1243c77 a native table exports a real pandoc Table | already in, as panduck's own shape-keyed `NativeTableToPandocVal`, except row padding: dbu pads a short row to the header's column count, panduck leaves it short and widens the colspecs to the widest row. pandoc 3.1.3 reads panduck's output (exit 0, both shapes), so it is not ported |
| e6a1f79 the writers skip the document root | already in (`pandoc_writer.test`, the document-root records) |
| cfd8e28 v2.0 compatibility: `SetFallible`, `SetStability` | already in (the registrations) |
| 1c5f2a6 fragments wrapped; the caption `switch_content` arm | wrapping already in (`pandoc_writer.test`); caption arm ported, 751b6f5, and extended to a formatted caption |
| 508c427 `ATTR_ID` / `ATTR_NAME` constants | no behaviour change: panduck's literal `"id"` has the same value; not ported |
| 193a7c0, 82241ea, 7493332, f80989e, b2ff31b, e00db69 | infrastructure (deprecation notice, CI matrix, function descriptions, registration helpers, a check script); n/a |

**Losses neither copy had noticed, fixed in panduck:** the last paragraph of a container
in a document with metadata (49c8157); a list or definition item with formatted text
(f9f4a37); a pandoc table inside a container losing its preserved tuple (ad564bc). Each has
a few-line reproduction in `test/sql/pandoc_roundtrip_fidelity.test`.

**What still differs from dbu, measured after the port:** the seven places listed in the
README, under "The converter names that moved from duck_block_utils".

The other `to_blocks` differences are `element_order` shifts that follow from these. The
inline converters (`pandoc_inlines_to_*`, `duck_blocks_inlines_to_pandoc`) are not
registered in panduck, so this check does not cover them; their expected values in
`test/sql/pandoc_canonical_names.test` come from community duck_block_utils instead. The
script is `test/converter/check_converter_differential.py`.

## Phases

0. **Differential (precondition, per the handoff spec).** Run both copies over the same
   inputs: panduck's alignment fixtures, the roundtrip-sweep inputs, a depth-50,000 AST,
   and malformed JSON. Compare outputs and error classes. Report every difference before
   either side changes.
1. **panduck PR (draft until phase 3).**
   - Tests first, confirmed failing: flip `pandoc_reader.test:41-45` from 0, one parity
     case per name, and a VFS read/write test.
   - Then the implementation: route `ReadPandocAstFun` and the write path through the VFS,
     register the nine names with dbu's exact signatures and stability, and keep the
     `panduck_*` names.
   - Settle the two mismatches (`write` stability, `JSON` against `VARCHAR`) from the
     differential, not by default.
2. **dbu PR (draft until phase 3), in a dbu worktree.**
   - Converter-specific tests move to panduck unless panduck already has the equivalent.
   - The remaining tests switch to `db_*` builders.
   - Remove the converter sources, registrations, docs and the fixture generator's
     dependency on `read_pandoc_ast`.
3. **Joint check.** Load both branch builds into one DuckDB session at the same version.
   Every moved name resolves to exactly one function, and each build passes alone.
4. **Release.** Tag both, then open both registry PRs the same day.
5. **Tell consumers** (duckeye, webbed): these names now need `LOAD panduck`.

## What breaks during the window

Either order breaks someone until both registry PRs are served:

- panduck first: anyone with both extensions loaded who calls a moved name gets
  bind-time ambiguity.
- dbu first: anyone with only dbu loaded who calls a moved name gets "does not exist".
