# `doc_search` and the external index — design note

**Status:** design only, not implemented. Written by the tiibrarian/SurvivorLibrary side,
which needs a path-taking search verb and would rather use panduck's than grow its own.

## 1. The gap is real: `doc_search` was dropped, not migrated

Worth stating plainly, because the codebase reads as if it moved:

- `duck_block_utils` registers **zero** `doc_*` functions. Commit `a33a71f`
  (*"refactor!: delete doc_render; rename doc_* queries to db_*"*) gave up the namespace.
- panduck has `doc_toc`, `doc_section`, `doc_render` — **no `doc_search`**.
- What survives is block-level: `duck_blocks_sections_like`, `duck_blocks_get_section`.

So there is currently **no verb anywhere that takes a path and searches it**. Every consumer
hand-rolls it. duckeye's `-s` has a bash `case` with one arm for `.zim` (`zim_search`) and
another for prose; tiibrarian is about to write a third. That is the sprawl to stop.

## 2. What changed under you (removes a stated objection)

`docs/doc_namespace.md` says `doc_sections_like` is *"deliberately absent: it is a **search**
returning rendered text — a different shape and a different job."* That was true and is the
right call against the old signature.

**It is no longer true.** As of `duck_block_utils@f047b3e`:

```
duck_blocks_get_section(blocks, section_pattern)  -> LIST(duck_block)
duck_blocks_sections_like(blocks, query_term)     -> (section, start_order, blocks)
```

`output_format` is gone from both. It could never have worked — a macro has one return type,
so every branch collapsed to `VARCHAR` and `'blocks'` returned `to_json(...)::VARCHAR`. That
is the same silent degradation `reader_registry.cpp` cites. Rendering is now composed
(`duck_blocks_to_text`, `..._render_ansi`, `..._to_md`, `..._to_pandoc_ast`).

**Consequence for panduck:** the in-document arm of `doc_search` can wrap
`duck_blocks_sections_like` and still hand back a queryable table of blocks. No shape
mismatch, no `doc_*`-returns-text exception. (Same argument applies to `doc_section`, which
slices its own stream partly because the utils version returned `VARCHAR`. That reason has
expired; the *other* reason — no `json` dependency — has not.)

## 3. Constraints this must respect

From your own docs and source, not invented here:

1. **`doc_*` takes a path.** Search-by-path is therefore the IO engine's job.
2. **No catch-all.** A format the registry claims but dispatch cannot handle must error and
   name itself a panduck bug. A silent fallback is precisely how the old `doc_search` rotted.
3. **Bind time and catalog visibility.** Autoloading mid-statement needs the `query()`
   indirection, and a MACRO created by a load is not visible on the first call while a C++
   function is. An index reader that is a table function avoids the second trap.
4. **The registry is the extension point** — `panduck_register_doc_reader` /
   `..._table_reader`, with `kind` and `source` = `builtin|user`.

## 4. Proposed surface

```sql
doc_search(src, query, k := 10)
```

One shape out, regardless of what answered — mirroring how `zim_search` is already normalised
in duckeye:

| column | meaning |
|---|---|
| `score` | higher is better; comparable only within one source |
| `title` | section heading, article title, or book+page label |
| `path` | addressable locator to re-read the hit (`doc_section`, `read_pdf_blocks`, zim entry) |
| `snippet` | short context, plain text |
| `blocks` | `LIST(duck_block)` when the backend can produce them, else NULL |

`path` is the important one: it is what makes a hit **re-readable through panduck**, which is
what lets a consumer ground an answer without the index carrying the text (§7).

## 5. Resolution order — loud at every step

| # | condition | backend |
|---|---|---|
| 1 | source has its **own** index (`.zim`) | `zim_search(src, query, max_results := k)` |
| 2 | an **external index is discovered** for `src` | index backend (§6) |
| 3 | no index, source is a readable document | `duck_blocks_sections_like(panduck_read_blocks(src), query)` |
| 4 | registry claims the format, no branch | `error(... 'This is a panduck bug.')` |

Order matters: 1 before 2 so a `.zim` with a stale sidecar still uses its own index; 3 last so
"no index" degrades to honest in-document search rather than silence.

## 6. The external index: discovery and format

**Make the sidecar Parquet, not `.duckdb`.**

A database has to be `ATTACH`ed. `ATTACH` is a *statement*, not a table function, so it cannot
live inside the `query()` string dispatch already depends on, and it cannot be emitted by a
registry branch the way `read_parquet('…')` can. Parquet sidesteps all of it: it is already a
legal dispatch target, needs no `vss`, and needs no
`hnsw_enable_experimental_persistence`.

Measured on a real 1.31M-page corpus (SurvivorLibrary, Qwen3-Embedding-0.6B, 1024-d):
brute-force cosine over **256-d truncations** of 2.5M rows is **~0.15 s**, then exact re-rank
of the shortlist on the full vector. Recall of that two-stage shape against exact search:
**0.987 @ shortlist 100, 0.996 @ 200**. An ANN index buys nothing at this size; it starts to
matter around ~32M rows, which is when a `.duckdb` sidecar with C++-managed attachment earns
its complexity. Not before.

Discovery should mirror the reader registry rather than invent a second mechanism:

```sql
panduck_register_index(pattern, index_path, backend)   -- source = 'user'
SELECT * FROM panduck_index_registry();                -- ext/pattern, index, backend, source
```

…plus a filename convention so the common case needs no registration (e.g. `X` →
`X.panduck-index.parquet`). Convention for the default, registry for everything else — the
same split the reader registry already uses.

## 7. The embedding wrinkle, stated rather than hidden

**Text → vector needs a model.** Pure SQL cannot embed a query string, and panduck should not
grow an inference dependency. So the index backend needs two entry points:

- `corpus_search_vec(qvec, k)` — semantic. The caller (which has an embedder) supplies the
  vector. This is tiibrarian's path: it embeds the question on the device's NPU.
- `corpus_search_text(q, k)` — BM25/FTS over a text column. Works with no model attached, and
  is what `doc_search(src, 'some words')` resolves to.

`doc_search` takes text, so it uses the second. A vector overload
(`doc_search(src, qvec, k)`) is the natural way to expose the first without making panduck
depend on an embedder.

## 8. Do not put page text in the index

The index should carry **identity + vector**, and let panduck resolve text by path at read
time. Two reasons, one principled and one measured:

- **Provenance.** A grounding layer checks that a quoted span is verbatim *on the page cited*.
  Quoting a copy of an extraction is weaker than quoting what `read_pdf_blocks(book, pages :=
  n)` returns right now. Re-reading through panduck is the stronger claim, and it is exactly
  "path → content".
- **Size.** On the corpus above, text is **2,646 B/row** — ~6.6 GB at 2.5M rows. Not the
  dominant cost (one 1024-d float32 vector is 2.4× the page it represents), but it is pure
  duplication of bytes that already exist on disk in the `.pdf`/`.zim`.

This is the half of the design that most needs panduck: **`doc_search` finds, `panduck` fetches.**

## 9. Test checklist

Each resolution step should fail loudly and be asserted separately:

- `.zim` uses its own index even when a sidecar exists (ordering).
- A discovered sidecar is used, and `panduck_index_registry()` reports it with `source='user'`.
- No index → in-document search returns blocks, not text (now possible; see §2).
- A registry-claimed format with no branch errors and says *panduck bug* — the assertion that
  would have caught the original `doc_search` rot.
- A sidecar that does not match its source (wrong row count / missing column) errors rather
  than returning plausible wrong hits.
- The uniform hit shape holds across all three backends.

## 10. Open questions for whoever implements it

1. Is `blocks` NULL-when-unavailable acceptable, or should every backend be required to
   produce blocks (which would mean the index sidecar carries enough to rebuild them)?
2. Should `path` be a string locator or a struct? A struct composes better with
   `doc_section`/`read_pdf_blocks`; a string is friendlier to duckeye and MCP output.
3. Does the sidecar need a provenance/versioning field so a stale index against a changed
   source is detectable rather than silently wrong? (Recommend yes — it is the same failure
   class as everything else in this note.)

---
*Context: the corpus driving this is ~2.5M pages of the SurvivorLibrary, embedded on a Tiiny
NPU. tiibrarian (`ground_answer.py`) is the consumer: it needs `retrieve()` to return hits it
can re-read verbatim. It will call whatever panduck exposes rather than growing its own
retrieval layer.*
