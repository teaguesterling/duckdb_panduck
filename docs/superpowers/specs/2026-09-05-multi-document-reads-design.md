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

with `filename` present only when requested (§3.4), and `<options>` **rendered by panduck**
from structured registry data — never interpolated from a stored SQL string (§3.6). Arms are
joined in the order `panduck_glob` returns, which is sorted, so the result is deterministic
across runs and platforms.

Every literal in a generated arm comes from one of exactly three places: a path panduck
resolved itself, an identifier validated against `^[A-Za-z_][A-Za-z0-9_]*$` at registration,
or a value rendered by its declared type. That enumeration is the security argument, and it
is short on purpose — a longer one would mean the design had a hole to explain.

`element_order` is **per document**, not global. It is the position of a block within its
own document, and making it global would break every existing consumer that uses it to
reconstruct one document's order — including `doc_section`, `doc_container` and the pandoc
writer. Callers wanting a global order have `ORDER BY filename, element_order`.

### 3.4 Provenance: consistent, and OFF by default

**`filename` is off by default for every source form** — single path, glob, or list — and
`filename := true` requests it. Ruled 2026-09-05: *"i want filename off by default, always"*.

Two earlier drafts were wrong in opposite directions. The first made it plural-sensitive
(off for one path, on for many); the second made it on everywhere. Consistency was the right
half of the second draft and the default was not.

**Why off is right, and it is not merely conservatism.** An 8-field struct is refused by the
*released* `duck_block_utils`, which is what every user has today:

```
duck_block_utils 3f2a0f0 (spec 6.3)
duck_blocks_toc(list(b))   -- b carrying a trailing filename
Binder Error: No function matches ... STRUCT(kind, ..., element_order INTEGER,
                                             filename VARCHAR)[]
```

On-by-default would therefore have made panduck's **default** output something the released
fleet cannot consume, breaking the `list(b)` idiom in panduck's own published
community-extensions `hello_world` on the day it shipped — the same failure this repo spent
2026-09-04 fixing, only foreseeable.

Off-by-default needs **no release gate at all**. panduck's default output stays the canonical
seven columns, works identically against 6.3 and 6.4, and the feature can ship the day it is
built. A caller who opts in has, by opting in, taken on the requirement that their consumer
accepts eight fields.

This also matches `duck_block_utils`' spec, which already states it normatively —
*"Producers MAY emit the 8th field, and SHOULD do so only behind `filename := true`"* — and
DuckDB core, where `read_csv`, `read_json` and `read_parquet` all default it off.

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

### 3.6 Reader options: structured, never a SQL fragment

Dispatch must not hardcode a sibling's parameter names — three defects in this repo in two
days were exactly that shape, a compile-time claim about a sibling's runtime surface that
only a rebuild could fix. But the first draft solved that by storing a **raw SQL fragment**
on the registry row, and that is rejected. Ruled 2026-09-05: *"that's a dangerous approach.
either use a structured parameter (map, json, etc) or just pass through."*

**The rejection is right and the draft's own security note said why**, then talked itself
out of it: registrations are process-wide and persistent, so a fragment stored by one caller
is executed inside every later `read_panduck_doc` — including other sessions' calls in a
shared process. The draft answered that with "registration is already privileged", which is
a rationalisation, not a mitigation. A design that requires a paragraph explaining why its
injection vector is acceptable should not have the vector.

#### The mechanism: a structured mapping, rendered by panduck

`ReaderEntry` carries a list of structs, not a string:

```
options : LIST(STRUCT(intent VARCHAR, value VARCHAR,
                      param  VARCHAR, arg   VARCHAR, arg_type VARCHAR))
```

`panduck_register_doc_reader` takes it as structured data:

```sql
SELECT * FROM panduck_register_doc_reader(
    'webbed', 'read_html_blocks', ['.html', '.htm'],
    options := [{intent: 'attributes', value: 'all',
                 param:  'capture_attributes', arg: 'classes', arg_type: 'VARCHAR'}]);
```

Dispatch renders one argument from a matching row, and **nothing in it is caller- or
registrant-supplied SQL**:

- `param` must match `^[A-Za-z_][A-Za-z0-9_]*$` — a bare identifier. Anything else is
  rejected at registration time, not at query time, so a bad row cannot be stored.
- `arg` is rendered **by `arg_type`**: `VARCHAR` through `panduck_quote`, `BOOLEAN` as
  `true`/`false`, `INTEGER` as digits after a `try_cast` check, `LIST` as a bracketed list
  of quoted elements. An unrecognised `arg_type` is rejected at registration.

So the worst a hostile registration can produce is a well-formed `identifier := literal`
with a silly name, which the reader rejects as an unknown parameter. There is no path from
registration data to arbitrary SQL.

#### Worked examples

**D. An intent reaching a sibling's parameter.**

```sql
SELECT * FROM read_panduck_doc('page.html', attributes := 'all');
```
```sql
-- generated: 'attributes'/'all' matched in the html row, rendered by type
SELECT * FROM read_html_blocks('page.html', capture_attributes := 'classes')
```
The caller never types `capture_attributes`; that spelling lives in the registry row, not in
the query and not in panduck's binary.

**E. The rename, survived at runtime — structured this time.**

```sql
SELECT * FROM panduck_register_doc_reader(
    'webbed', 'read_html_blocks', ['.html', '.htm'],
    options := [{intent: 'attributes', value: 'all',
                 param:  'attribute_capture', arg: 'classes', arg_type: 'VARCHAR'}]);
```
One statement, no rebuild, and the only thing that changed is a **value in a struct field**.
The runtime-fix property that motivated the design survives the security fix intact — which
is the test of whether the fix was the right one.

**F. An option that does not apply is not silently dropped.**

```sql
SELECT * FROM read_panduck_doc('notes.odt', attributes := 'all');
-- Invalid Input Error: panduck: the odt reader has no mapping for attributes := 'all'
```
A row without a mapping for a requested intent raises. This is the `pages`-was-a-lie lesson
applied in advance: a parameter accepted and ignored reads as a feature at the call site.

#### The escape hatch: direct passthrough

The ruling offered "or just pass through" as an alternative. It is better as a **complement**
than a replacement, and both should exist:

```sql
SELECT * FROM read_panduck_doc('page.html',
                               reader_params := MAP{'ignore_errors': 'true'});
```

rendered with the same identifier and type discipline. This covers every parameter panduck
has no intent for — `maximum_file_size`, `ignore_errors`, `ocr_language` — without panduck
growing a vocabulary entry per sibling option, which it should never do.

**Why passthrough cannot replace the intent mapping.** panduck itself needs to request
`class`: `doc_render('page.html', 'md')` converts HTML to Pandoc, and Pandoc's `Attr` is
`(id, classes, kvs)`, so a faithful conversion needs the class attribute. There is **no
caller** in that path to supply a parameter — `doc_render` takes a path and a format. panduck
must know to ask, which means panduck must hold the mapping. Passthrough serves callers who
know what they want; the intent mapping serves panduck's own correctness.

**Why the intent vocabulary stays tiny.** Exactly one entry, `attributes`, needs this
mechanism today. `filename` does not — §3.4 synthesises that column by projection and never
passes a parameter at all. Everything else is passthrough's job until something in panduck
needs to ask on its own behalf.

### 3.7 Document boundaries: withdrawn

An earlier revision proposed `document_start` / `document_end` attributes marking the first
and last block of each document, mirroring a `page_start` / `page_end` proposal one level
down. **Both are withdrawn.** Teague, 2026-09-05, on the page version:

> "i think my page start attribute idea is wrong. you can have it always on and selectively
> propagate, however!"

The same reasoning retires the document version, and it is the better design. The markers
are **always on** — `page_break` for pages, and for documents the arm boundary is known to
dispatch because dispatch built it. What varies is not what is *stored* but what is
*propagated*: a utility fills the information onto the blocks a caller actually asked about,
at query time.

That keeps the vocabulary out of it entirely. No `ATTR_` constants, no producer obligation,
no one-derivation rule and no instrument needed to enforce one, because there is only ever
one derivation. It also means a boundary is not frozen into rows at read time by a producer
that cannot know which slice the caller will eventually take.

Nothing in the rest of this design depends on it. `filename` (§3.4) already answers "which
document is this row from" for any caller who asks for it, and
`row_number() OVER (PARTITION BY filename ORDER BY element_order)` answers "is this the
first block of its document" without any new vocabulary at all.

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
3. **Provenance is consistent across source forms, and off by default.** A single path, a
   glob and a list all produce seven columns with no `filename` argument, and eight under
   `filename := true`. Asserted by `DESCRIBE`, so a column-order regression and a
   re-introduced plural/singular asymmetry both fail here.
4. **The emitted type is exact.** `typeof(list(b))` asserted as the full string, ending
   `element_order INTEGER, filename VARCHAR)[]`. This is the producer-side half of the
   junction check — the instrument that both webbed and markdown lacked when they shipped a
   leading column.
5. **It binds.** `duck_blocks_toc(list(b))` over a glob result, so a consumer in another
   extension accepts what panduck emits. Run through the `.parquet_duck_blocks` bridge when
   pins differ; directly when they do not.
6. **A glob matching nothing errors, and so do a missing explicit path and a missing list
   element** — all three with a message naming the pattern, matching core (§3.5).
7. **The default output is byte-identical to today's** for every source form — the check
   that this feature is additive, and that a caller on a released 6.3 `duck_block_utils` is
   unaffected until they opt in.
8. **Options reach the reader.** With `attributes := 'all'`, a `class` attribute present in
   an HTML fixture appears in the output; with the default it does not. This asserts the
   passthrough end to end rather than asserting that a string was built.
9. **A re-registered row overrides the builtin spelling**, proving the runtime-fix property
   that motivates §3.6 — example E.
9a. **A registration with a non-identifier `param` is rejected at registration time**, not
   at query time, so a malformed row cannot be stored. Assert the error, and assert the row
   is absent from `panduck_reader_registry()` afterwards — the second half is what proves
   rejection rather than a warning.
9b. **Each `arg_type` renders correctly**: a `VARCHAR` containing a quote survives
   `panduck_quote`, a `BOOLEAN` renders bare, an `INTEGER` rejects a non-numeric `arg`. This
   is the test that there is no path from registration data to arbitrary SQL.
10. **A requested intent with no mapping raises**, rather than being dropped — example F.
    This is the assertion `pages` never had.

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
