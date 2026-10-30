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

**Every difference is a behaviour difference in panduck's copy:**

| class | dbu (community) | panduck | inputs |
|---|---|---|---|
| footnote body | `note` row with no children: the footnote text is lost | the note's inlines as children | 14 |
| code block content | keeps the trailing `\n` | strips it | 9 |
| link `title` | keeps pandoc's `wikilink` marker | drops it | 3 |
| image `title` equal to its alt | keeps it | drops it | 1 |
| citation | dropped | `[@knuth1984]` kept as text | 1 |
| write: image with no alt | writes a placeholder `Str "image"` | writes an empty alt | 1 |

The other `to_blocks` differences are `element_order` shifts that follow from these. The
inline converters (`pandoc_inlines_to_*`, `duck_blocks_inlines_to_pandoc`) are not
registered in panduck, so this check does not cover them. The script is
`differential.py` in the session scratchpad; it should be committed with phase 1.

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
