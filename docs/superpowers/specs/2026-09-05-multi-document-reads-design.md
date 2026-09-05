# Multi-document reads: globs, path lists, provenance, and reader options

**Status:** design, not implemented. Written 2026-09-05.

**Goal:** let `read_panduck_doc` and the `doc_*` family read many documents in one call, with
each row knowing which document it came from, and let dispatch pass reader-specific options
that are currently unreachable.

---

## 1. The three problems, and how they turned out to be one

**Globs and path lists do not work.** Measured:

```
SELECT count(*) FROM read_docx_blocks('test/fixtures/*.docx');
IO Error: read_docx_blocks: not a readable ZIP archive: test/fixtures/*.docx
```

Every panduck reader takes `[VARCHAR]` — one path, no list overload, no glob. webbed's
`read_html_blocks` already accepts `VARCHAR` **and** `VARCHAR[]` and globs natively, so
panduck is the outlier among the readers it dispatches to.

**There is no provenance.** Reader output is exactly the seven canonical `duck_block`
columns. Two files give you their rows interleaved with nothing to tell them apart. This is
not panduck-specific: webbed globs today and has the same gap in the field.

**Reader options are unreachable.** Dispatch builds exactly this:

```sql
'SELECT * FROM ' || panduck_reader_function_for(src) || '(' || panduck_quote(src) || ')'
```

A path and nothing else. So `capture_attributes` (webbed), `first_page`/`last_page` (pdf),
`ignore_errors`, `maximum_file_size` — none can be set through `read_panduck_doc`.

These are one problem because **a glob without provenance is not useful** — fifty files of
undifferentiated rows is worse than an error — and because the mechanism that carries a
path list is the same one that carries an option.

---

## 2. Constraints, all measured rather than assumed

**Table functions accept literals only.** A correlated argument is refused:

```
SELECT * FROM glob('*.odt') g, LATERAL read_odt_blocks(g.file);
Binder Error: Table function "read_odt_blocks" does not support lateral join column
parameters - cannot use column "g.file" in this context. The function only supports
literals as parameters.
```

So dispatch cannot iterate a file list with a join. This also rules out the obvious
"expand the glob in SQL" approach.

**`query()` is itself a table function, so its argument cannot be a subquery.**

```
SELECT * FROM query((SELECT string_agg(...) FROM glob('*.odt')));
Binder Error: Table function cannot contain subqueries
```

This is the same restriction that already forced `read_pdf_blocks` to inline its page
expressions instead of computing them in a CTE.

**But a scalar-built string works.** Verified:

```sql
SELECT count(*) FROM query(
  array_to_string(list_transform(['a.odt','b.odt'],
    lambda f: 'SELECT * FROM read_odt_blocks(' || panduck_quote(f) || ')'), ' UNION ALL '));
-- 70 rows, exactly the sum of the two individual reads
```

Everything in that expression is scalar, so `query()` accepts it. **This is the whole
mechanism.**

**There is no scalar glob.** `glob` exists only as a table function, so expanding a pattern
into a list inside a scalar expression needs one new primitive. That is the single piece of
C++ this design requires.

**Dispatch can synthesise a conformant provenance column.** Verified against
`duck_block_utils` 6.4:

```sql
SELECT *, '<path>' AS filename FROM read_odt_blocks('<path>')
-- STRUCT(kind, element_type, content, level, encoding, attributes, element_order,
--        filename VARCHAR)[]
-- duck_blocks_toc over two files -> 4 entries: it binds
```

Trailing **by construction**, because `SELECT *` emits the canonical seven first and the
literal lands after `element_order`. This matters more than it looks: spec 6.4 keys 8-field
acceptance on the exact type with `filename` last, and both shipped producers (webbed
`a865d37` before its fix, markdown `340c0cd`) got this wrong by putting the column first.
The projection form cannot make that mistake.

---

## 3. Design

### 3.1 One new scalar

```
panduck_glob(pattern VARCHAR) -> VARCHAR[]
```

Expands a filesystem pattern to a sorted list of paths, using DuckDB's own file system so
that remote and extension-provided filesystems behave as they do everywhere else.

**The primitive is neutral; the policy lives in dispatch.** `panduck_glob` on a pattern
matching nothing returns an **empty list**, and it is `read_panduck_doc` that raises on it
(§3.5). Keeping the raise in one place means a future caller that legitimately wants "the
files matching this, possibly none" has a primitive to build on, and there is exactly one
site to change if the fleet ever revisits the rule.

This is the only C++ in the design. Everything else is macro SQL.

### 3.2 The source argument becomes plural

`read_panduck_doc(src, ...)` accepts, in order of resolution:

| form | example | resolution |
|---|---|---|
| `VARCHAR` with no glob metacharacter | `'report.docx'` | single path, unchanged |
| `VARCHAR` with a glob metacharacter — `*`, `?`, `[` | `'docs/*.docx'` | `panduck_glob(src)` |
| `VARCHAR[]` | `['a.docx','b.odt']` | used as given, each element may itself glob |

Mixed formats in one call are allowed and are the interesting case: `['a.docx','b.odt']`
dispatches each path through the registry independently, so a single call can read a DOCX
and an ODT into one block stream. That falls out of the design rather than being added to
it, because dispatch already resolves per path.

### 3.3 The generated SQL

For each resolved path, dispatch emits one arm and `UNION ALL`s them:

```sql
SELECT *, '<path>' AS filename FROM <reader_function>('<path>'<options>)
```

with `filename` present only when provenance is on (§3.4) and `<options>` supplied by the
registry (§3.6). Arms are joined in the order `panduck_glob` returns, which is sorted, so
the result is deterministic across runs and platforms.

`element_order` is **per document**, not global. It is the position of a block within its
own document, and making it global would break every existing consumer that uses it to
reconstruct one document's order — including `doc_section`, `doc_container` and the pandoc
writer. Callers wanting a global order have `ORDER BY filename, element_order`.

### 3.4 Provenance: on by default when plural

- **Single path** → off by default, `filename := true` to request it.
- **Glob or list** → **on by default**, `filename := false` to suppress it.

The asymmetry is deliberate and is the one place this design departs from the fleet's
"opt-in" framing. For a single path the column is a constant repeated on every row: pure
overhead, and the caller already knows the path. For a glob it is the only thing
distinguishing one document's rows from another's, so a caller who omits it does not get a
cheaper result, they get a **silently unusable** one. Defaults should not have that shape.

The column is always named `filename`, always trailing, and always the full resolved path —
matching `read_csv`, `read_json` and `read_parquet`, and matching spec 6.4's accepted type.

**Core's `filename := 'custom_name'` string form is deliberately not supported.** Verified:
a struct whose eighth field is named anything other than `filename` does not bind as a
`duck_block`, so honouring the rename would hand callers rows no consumer accepts. This is a
conscious narrowing of core's contract and should be documented as one rather than looking
like an oversight.

### 3.5 A glob matching nothing: error, matching core

**A glob that matches no file is an error**, not an empty result.

The first draft of this section said the opposite — zero rows, "matching `read_csv`" — on
the assumption that a pattern matching nothing is an answer while a missing named file is a
mistake. That reasoning is defensible and it is not what DuckDB does. Measured:

```
SELECT count(*) FROM read_csv('/tmp/fn/*.nope');
IO Error: No files found that match the pattern "/tmp/fn/*.nope"

SELECT count(*) FROM read_csv('/tmp/fn/definitely_absent.csv');
IO Error: No files found that match the pattern "/tmp/fn/definitely_absent.csv"
```

Core errors on both, with the **same message**, and draws no distinction between the two
cases at all. Since the governing principle for this whole area is "same as core" — the
ruling that settled the `filename` parameter — panduck matches it: a glob matching nothing,
a missing explicit path, and a list containing a missing element all raise. panduck's
message should name the pattern, as core's does.

The nicer-sounding rule is available later if anyone wants it fleet-wide, but panduck is not
the place to introduce a divergence from core's file-resolution behaviour unilaterally.

### 3.6 Reader options: intent in, spelling from the registry

Dispatch must not hardcode a sibling's parameter names. Three separate defects in this
repo's recent history were exactly that shape — a compile-time claim about a sibling's
runtime surface (the `db_*` → `duck_*` rename, the stale html registry row, the
`page`/`page_number` model divergence). So:

**panduck exposes an intent vocabulary, and the registry row carries the spelling.**

The initial vocabulary is deliberately two entries. Both have a consumer today; nothing else
does.

| intent | values | why |
|---|---|---|
| `filename` | `true` / `false` | §3.4 |
| `attributes` | `'default'` / `'all'` | webbed drops `class` by default, and panduck writes `class` into Pandoc's `Attr` at four sites in `pandoc_block_convert.cpp` |

`ReaderEntry` gains one field:

```cpp
std::string options;  //!< SQL fragment appended inside the reader call, from registration
```

populated per intent, not free-form. `panduck_register_doc_reader` gains an optional
argument so a third-party or corrected registration supplies its own mapping:

```sql
panduck_register_doc_reader('webbed', 'read_html_blocks', ['.html'],
                            attributes_all := 'capture_attributes := ''classes''');
```

**This is the point of the design.** When webbed renames `capture_attributes`, a user
re-registers the row at runtime and nothing rebuilds. Every alternative bakes the sibling's
spelling into panduck's binary.

**Security.** The options fragment is interpolated into generated SQL, and registrations are
**process-wide and persistent** — one caller's registration is executed inside every later
`read_panduck_doc`, including other sessions' calls in a shared process. A free-form
fragment is therefore an injection vector. The fragment is accepted only from
`panduck_register_doc_reader`, which is already a privileged operation in the sense that it
changes global dispatch; it is never derived from a path, a document, or a query result.
Builtin rows carry fragments written in this repo.

---

## 4. What this design does not do

- **It does not make readers glob.** panduck's C++ readers still take one path. Dispatch
  fans out; the readers are untouched. A reader that globs natively (webbed) still gets one
  path per arm, which is slower than letting it glob but keeps one code path and keeps
  provenance uniform.
- **It does not parallelise.** `UNION ALL` over N arms is whatever DuckDB makes of it.
- **It does not deduplicate.** A path appearing twice in a list is read twice.
- **It does not touch `element_order` semantics** (§3.3).
- **It does not add `pages` to the plural forms.** `pages` is PDF-specific and already
  rejects formats that have none; a page range across a glob is meaningless.

---

## 5. Testing

The junction is where this class of feature breaks, so the tests are shaped around it.

1. **Glob returns the sum.** `read_panduck_doc('fixtures/*.odt')` row count equals the sum
   of the individual reads. Asserted as a computed equality, not a literal, so adding a
   fixture cannot silently invalidate it.
2. **Mixed-format list.** `['constructs.odt','constructs.html']` returns rows from both,
   with two distinct `filename` values.
3. **Provenance default is plural-sensitive.** A single path has seven columns; a glob has
   eight. Asserted by `DESCRIBE`, so a column-order regression fails here.
4. **The emitted type is exact.** `typeof(list(b))` asserted as the full string, ending
   `element_order INTEGER, filename VARCHAR)[]`. This is the producer-side half of the
   junction check — the instrument that both webbed and markdown lacked when they shipped a
   leading column.
5. **It binds.** `duck_blocks_toc(list(b))` over a glob result, so a consumer in another
   extension accepts what panduck emits. Run through the `.parquet_duck_blocks` bridge when
   pins differ; directly when they do not.
6. **A glob matching nothing errors, and so do a missing explicit path and a missing list
   element** — all three with a message naming the pattern, matching core (§3.5).
7. **`filename := false` on a glob** returns seven columns, and the result is byte-identical
   to today's output for a single path.
8. **Options reach the reader.** With `attributes := 'all'`, a `class` attribute present in
   an HTML fixture appears in the output; with the default it does not. This asserts the
   passthrough end to end rather than asserting that a string was built.
9. **A re-registered row overrides the builtin spelling**, proving the runtime-fix property
   that motivates §3.6.

---

## 6. Open questions

1. **Does `doc_toc` / `doc_section` / `doc_container` accept plural sources?** They compose
   over `read_panduck_doc`, so they would work mechanically, but `doc_section(glob, 'Intro')`
   returning the Intro of every matching document is a different operation from what the
   name suggests. Recommendation: allow it, and make the tests state what it means.
2. **Should `panduck_read_blocks` (the scalar macro returning a list) accept plural?** It
   feeds `duck_blocks_toc` and friends, which have no notion of multiple documents.
   Recommendation: no, for now — plural belongs to the table-returning forms.
3. **Ordering guarantee.** Sorted by path is proposed. Worth confirming that
   `panduck_glob`'s sort is byte-order and stated as such, since callers will rely on it.
4. **Does the `pdf` reader want its native `first_page`/`last_page` exposed through the
   options mechanism** rather than through `read_pdf_blocks`' bespoke `pages`? Deferred; the
   current arrangement works and unifying it is a separate change.

---

## 7. Sequencing

`filename` emission is last in the fleet ordering — acceptance everywhere, then emission —
and `markdown` PR #52 is the outstanding gate. This design can be **built** before that
lands, because §3.4's provenance is synthesised by panduck's own dispatch rather than
requested from a sibling, and it binds against 6.4 today. Only the *option* passthrough for
a sibling's own `filename` parameter has to wait, and this design does not use it.
