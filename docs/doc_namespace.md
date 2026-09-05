# The `doc_` namespace

**`doc_*` takes a path. `db_*` takes blocks you already hold.**

That split is why this namespace belongs to panduck: taking a path is the IO engine's job,
while operating on blocks is `duck_block_utils`' job.

```sql
SELECT level, title, indent FROM doc_toc('report.docx');
SELECT doc_render('report.docx', 'md');
```

## `doc_toc(src, format := 'auto')`

Returns `level, title, id, indent, element_order` — the same table
`duck_blocks_toc(panduck_read_blocks(src))` produces, which the suite asserts directly rather
than by eye.

The same document read from two formats yields the same table of contents:

```
doc_toc('sample.docx')  ->  1  Heading One  indent 0
                            2  Heading Two  indent 1
doc_toc('sample.rtf')   ->  identical
```

Two formats, two readers, two different heading mechanisms, one vocabulary, one answer.

## `doc_render(src, output_format, format := 'auto')`

Renders a document to a **format**: `md`, `html`, or `text`.

```sql
SELECT doc_render('notes.rtf', 'md');
--  # Heading One
--  A paragraph with **bold**, *italic*, and ~~strike~~ text.
```

An unsupported output format is **named**, never silently defaulted:

```sql
SELECT doc_render('notes.rtf', 'nonsense');
--  panduck: doc_render supports md, html and text; nonsense is unsupported
--  or its extension is not installed
```

`duck_block_utils` deleted its own `doc_render` when it stopped depending on format
extensions. Rendering to md/html *is* format IO, which is panduck's job; ANSI and plain
text are vocabulary-level outputs and stay in `duck_block_utils`.

## On-demand loading

These load `duck_block_utils` when called, exactly as reading `.md` loads
`duckdb_markdown` and `.html` loads `duckdb_webbed`. panduck's core never needs it — every
reader, the registry and both dispatchers work with `duck_block_utils` absent — and when it
is missing the error names it:

```
panduck: doc_toc needs the duck_block_utils extension (INSTALL duck_block_utils)
```

That asymmetry is deliberate. When `duck_block_utils` owned path dispatch, its `doc_toc`
**could not work** without a reader. panduck's `doc_*` is sugar over a composition that
remains available directly:

```sql
SELECT duck_blocks_toc(panduck_read_blocks('report.docx'));
```

## `doc_section(src, section, format := 'auto')`

The blocks under one heading, still as `duck_blocks`. Matches a heading by its **text or
its `id`** — a heading's text is what a person names, its `id` is what a link names, and
an HTML document has both:

```sql
SELECT * FROM doc_section('report.docx', 'Methods');
SELECT * FROM doc_section('page.html', 'methods');   -- by id
```

The boundary is **the next heading at the same level or higher**, not the next heading — a
section contains its subsections, which is what makes `doc_section('Chapter 2')` mean what
a reader expects. A section that is not present returns no rows rather than an error.

It slices panduck's own block stream rather than wrapping `duck_block_utils`, and that was
a measurement rather than a preference. On build `3f2a0f0`, `duck_blocks_get_section`
returns `VARCHAR` — rendered text — for *every* `output_format` including `'blocks'`, and
`duck_blocks_sections_like` returns `(section, start_order, content)`, also rendered. Both
are useful; neither hands back `duck_blocks`. Wrapping either would make `doc_section` the
only `doc_*` that takes a path and returns text instead of a queryable table. Slicing
locally also needs no `json` extension, which those functions do.

**One of those two reasons has since expired upstream, and the other has not.** As of
`duck_block_utils` `f047b3e`/`0eb9e47` — on their `main`, *not* in the published build
measured above — `output_format` is gone entirely and the functions return blocks:
`duck_blocks_get_section(blocks, pattern)` and `duck_blocks_get_pages(blocks, first, last)`
return `LIST(duck_block)`, and `duck_blocks_sections_like(blocks, query)` returns
`(section, start_order, blocks)`. Their own reasoning matches the note above: one return
type per macro means every `output_format` branch collapsed to VARCHAR anyway.

So the "returns rendered text" reason no longer applies to their `main`. The `json`
dependency reason still does, and `doc_section` is unchanged on that basis. Recorded here
rather than silently left standing, because a justification that has half-expired reads as
though it were still whole.

`doc_sections_like` is deliberately absent: it is a **search** returning rendered text — a
different shape and a different job. Use `duck_block_utils`' version directly.

## `read_panduck_doc(src, …)` reads more than one document

```
read_panduck_doc(src, format := 'auto', pages := '', filename := false,
                      attributes := 'default', reader_params := MAP {})
```

`src` takes three forms. Every example below was run against this repo's fixtures.

| form | example | resolution |
|---|---|---|
| a plain path | `'test/fixtures/constructs.odt'` | dispatched as it always was |
| a glob — `*`, `?` or `[` | `'test/fixtures/*.odt'` | expanded, sorted, one arm per match |
| a `VARCHAR[]` | `['a.docx','b.odt']` | used in argument order; an element may itself be a glob |

```sql
SELECT count(*) FROM read_panduck_doc('test/fixtures/constructs.odt');   -- 54
SELECT count(*) FROM read_panduck_doc('test/fixtures/*.odt');            -- 180
```

**A list may mix formats, and that is the interesting case.** Each path resolves through
the registry independently, so one call reads a DOCX and an ODT into one block stream:

```sql
SELECT count(*) FROM read_panduck_doc(['test/fixtures/constructs.docx',
                                       'test/fixtures/constructs.odt']);
--  106      -- 52 from the DOCX + 54 from the ODT
```

A glob is sorted before the arms are built, so the result is deterministic across runs and
platforms rather than inheriting the filesystem's enumeration order.

### `filename := true` — off by default, for every source form

Off for a plain path, off for a glob, off for a list. The column is named `filename`, is
**trailing**, and holds the path panduck dispatched on — as written for a plain path or a
list element, as expanded for a glob:

```sql
SELECT filename, count(*)
FROM read_panduck_doc(['test/fixtures/constructs.docx',
                       'test/fixtures/constructs.odt'], filename := true)
GROUP BY 1 ORDER BY 1;
--  test/fixtures/constructs.docx  52
--  test/fixtures/constructs.odt   54
```

**Off by default is not conservatism, it is what the released fleet can consume.** Measured
against the installed `duck_block_utils` **3f2a0f0 (spec 6.3)**, an eight-field struct does
not bind at all:

```
SELECT duck_blocks_to_text(
    (SELECT list(b) FROM read_panduck_doc('test/fixtures/lists.odt', filename := true) b));

Binder Error: No function matches the given name and argument types
'duck_blocks_to_text(STRUCT(kind VARCHAR, element_type VARCHAR, "content" VARCHAR,
"level" INTEGER, "encoding" VARCHAR, attributes MAP(VARCHAR, VARCHAR),
element_order INTEGER, filename VARCHAR)[])'.
```

The same query with provenance off returns text. So panduck's **default** output stays the
canonical seven columns and works against 6.3 and 6.4 alike; a caller who writes
`filename := true` has, by opting in, taken on the requirement that their consumer accepts
eight fields. On 3f2a0f0 an explicit `::duck_block[]` cast *does* succeed and silently
**drops** the eighth field — `duck_block` on that build is exactly seven fields — so the
cast is not a way round the binder.

Provenance is **projected, not requested** from the reader. `SELECT *, '<path>' AS filename`
puts the column after the canonical seven by construction, which is what both shipped
sibling producers got wrong by emitting it first (webbed `a865d37`, markdown `340c0cd`).
It also means panduck needs nothing from a sibling to have provenance: the path is already
in hand at dispatch time.

**Core's `filename := 'custom_name'` rename form is deliberately unsupported.** panduck's
`filename` is a `BOOLEAN`, so the string form is refused by the cast:

```sql
SELECT * FROM read_csv('x.csv', filename := 'src');           -- core: renames the column
--  1|2|x.csv

SELECT count(*) FROM read_panduck_doc('test/fixtures/constructs.odt', filename := 'doc');
--  Conversion Error: Could not convert string 'doc' to BOOL
```

Spec 6.4 keys eight-field acceptance on the exact type with `filename` **last**, so a struct
whose eighth field is named anything else does not bind as a `duck_block` — honouring the
rename would hand back rows no consumer accepts. That measurement was taken against a 6.4
build (see the design note, `docs/superpowers/specs/2026-09-05-multi-document-reads-design.md`
§3.4); it is *not* reproducible against 3f2a0f0, where no eight-field struct binds under any
spelling. This is a conscious narrowing of core's contract rather than an oversight.

### `attributes := 'default' | 'all'` — an intent, not a parameter name

The caller names **panduck's** vocabulary. The sibling reader's actual spelling for it lives
in registry data, so a sibling renaming its parameter is fixed by re-registering at runtime
rather than by rebuilding panduck:

```sql
CALL panduck_register_doc_reader('webbed', 'read_html_blocks', ['.htmltest'],
     options := [{intent: 'attributes', value: 'all',
                  param: 'capture_attributes', arg: 'classes', arg_type: 'VARCHAR'}]);

SELECT panduck_read_arms_opt(['x.htmltest'], false, 'attributes', 'all');
--  SELECT * FROM read_html_blocks('x.htmltest', capture_attributes := 'classes')
```

That is the generated SQL, and it also runs: with `.htmltest` registered against `webbed`
**093856b** and a real page at that extension,
`read_panduck_doc('page.htmltest', attributes := 'all')` reads the document with the
option threaded, rather than the option existing only inside its own assertion.

`'default'` is a sentinel that renders to nothing **without consulting the registry**, so an
unchanged call generates byte-identical SQL to what it generated before options existed.

**An intent the reader has no mapping for raises rather than being dropped**, which is the
whole point — a reader that cannot honour what was asked for must say so instead of
returning a document quietly missing it:

```sql
SELECT count(*) FROM read_panduck_doc('test/fixtures/constructs.odt', attributes := 'all');
--  Invalid Input Error: panduck: the reader for test/fixtures/constructs.odt
--  has no mapping for attributes = 'all'
```

**No built-in reader ships an `attributes` mapping today.** Measured on this tree by
sweeping the registry: all 22 `kind='doc'` rows answer that error, `.md` and `.html`
included. The intent is reachable only through a reader registered with `options :=` — the
`.htmltest` registration above is not an illustration of a shipped mapping, it *is* the only
way to get one. Saying so is the point: documenting `attributes := 'all'` as though a
built-in honoured it would be the same defect `pages` had.

### `reader_params := MAP{'name':'value'}` — the escape hatch

For any reader parameter panduck has no intent for. Keys are validated as identifiers,
values are always rendered as quoted `VARCHAR` literals and the reader's own cast does the
rest — there is no bare-typed arm here, because that is precisely the arm measured to be a
live injection under an unchecked value.

```sql
SELECT panduck_read_arms_opt(['a.odt'], false, 'attributes', 'default',
                             reader_params := MAP{'ignore_errors': 'true'});
--  SELECT * FROM read_odt_blocks('a.odt', ignore_errors := 'true')

SELECT count(*) FROM read_panduck_doc('test/fixtures/constructs.html',
                                      reader_params := MAP{'ignore_errors': 'true'});
--  15
```

panduck does not interpret the value; `ignore_errors` above is `webbed` **093856b**'s own
parameter, and what it means is that reader's business. A key that is not an identifier is
refused before anything is rendered:

```sql
SELECT count(*) FROM read_panduck_doc('test/fixtures/constructs.html',
    reader_params := MAP{'ignore_errors) UNION SELECT 1 --': 'true'});
--  Invalid Input Error: panduck: reader_params key must be an identifier,
--  got 'ignore_errors) UNION SELECT 1 --'
```

A NULL value is not a value, and neither is a NULL map:

```sql
--  reader_params := MAP{'ignore_errors': NULL}
--  Invalid Input Error: panduck: reader_params['ignore_errors'] must name a value; NULL is not one
--  reader_params := NULL
--  Invalid Input Error: panduck: cannot use NULL as argument for "reader_params"
```

### Every named parameter refuses NULL

DuckDB core is the standard this was aligned to, on the very parameter whose name and
column were both settled by "same as core":

```sql
SELECT * FROM read_csv('x.csv', filename := NULL);
--  Invalid Input Error: Cannot use NULL as argument for "filename"

SELECT count(*) FROM read_panduck_doc('test/fixtures/constructs.odt', filename := NULL);
--  Invalid Input Error: panduck: cannot use NULL as argument for "filename"
```

`format`, `pages`, `attributes` and `reader_params` all answer the same way. This mattered
because a NULL took the same path the *default* takes, so the caller could not tell that
what they asked for did not happen — measured before the fix: `filename := NULL` returned
seven columns with provenance silently off, and `pages := NULL` returned all 54 rows of an
ODT and all 180 of a glob.

### Options are refused, not ignored, where they cannot be honoured

The special-cased branches — pdf, zim, toml/yaml, the text fallbacks and the code fallback —
build their own SQL rather than going through the arm builder, so they cannot thread a
reader argument. They say so by name:

```sql
SELECT count(*) FROM read_panduck_doc('test/fixtures/two_pages.pdf', attributes := 'all');
--  Invalid Input Error: panduck: attributes is not supported for pdf

SELECT count(*) FROM read_panduck_doc('test/fixtures/two_pages.pdf',
                                      reader_params := MAP{'ignore_errors': 'true'});
--  Invalid Input Error: panduck: reader_params is not supported for pdf

SELECT count(*) FROM read_panduck_doc('test/fixtures/config.toml', filename := true);
--  Invalid Input Error: panduck: filename is not supported for toml

SELECT count(*) FROM read_panduck_doc('src/reader_registry.cpp', filename := true);
--  Invalid Input Error: panduck: filename is not supported for this source
```

PDF is the one special-cased branch that *does* project `filename`, so it is excluded from
that refusal by name. "this source" is the code fallback, which has no registry format to
name.

### `element_order` is per document

It is the position of a block within **its own** document. Making it global would break
every consumer that uses it to reconstruct one document's order — `doc_section`,
`doc_container` and the pandoc writer among them:

```sql
SELECT element_order, count(*) AS docs
FROM read_panduck_doc(['test/fixtures/constructs.docx','test/fixtures/constructs.odt'])
GROUP BY 1 ORDER BY 1 LIMIT 4;
--  0  2
--  1  2
--  2  2
--  3  2
```

A global order is `ORDER BY filename, element_order`, which is also why the two features
belong in the same section:

```sql
SELECT filename, element_order, element_type
FROM read_panduck_doc(['test/fixtures/constructs.docx',
                       'test/fixtures/constructs.odt'], filename := true)
ORDER BY filename, element_order LIMIT 4;
--  test/fixtures/constructs.docx  0  heading
--  test/fixtures/constructs.docx  1  paragraph
--  test/fixtures/constructs.docx  2  text
--  test/fixtures/constructs.docx  3  bold
```

### A glob matching nothing raises

Zero rows would be indistinguishable from a corpus that happens to be empty, and core does
not do that either — `read_csv` errors identically for a pattern that matches nothing and
for a named file that is absent (`IO Error: No files found that match the pattern "…"`).

```sql
SELECT count(*) FROM read_panduck_doc('test/fixtures/*.nosuch');
--  Invalid Input Error: panduck: no files matched test/fixtures/*.nosuch
```

**Each element of a list is checked as it expands**, before the results are flattened,
because flattening throws away which element produced which paths — after it, a source that
named itself in the call would vanish silently:

```sql
SELECT count(*) FROM read_panduck_doc(['test/fixtures/constructs.odt',
                                       'test/fixtures/*.nosuch']);
--  Invalid Input Error: panduck: no files matched test/fixtures/*.nosuch
```

A literal empty list is caught at dispatch, where there is no element to name:

```sql
SELECT count(*) FROM read_panduck_doc([]);
--  Invalid Input Error: panduck: no files matched []
```

A **plain path** deliberately does not go through the filesystem, so a caller naming one
missing file still gets the reader's own error rather than a glob-shaped one from panduck:
`read_panduck_doc('test/fixtures/nope.odt')` raises `IO Error: read_odt_blocks: not a
readable ZIP archive: test/fixtures/nope.odt`.

## `panduck_register_doc_reader(ext, function, extensions, options := …)`

`options` is **structured data, not a stored SQL fragment**:
`LIST(STRUCT(intent, value, param, arg, arg_type))`, all five fields required, all validated
at registration — so an ill-formed row can never be stored, let alone rendered.

```sql
CALL panduck_register_doc_reader('markdown', 'read_markdown_blocks', ['.mdx'],
     options := [{intent: 'attributes', value: 'all',
                  param: 'extract_metadata', arg: 'true', arg_type: 'BOOLEAN'}]);

SELECT panduck_read_arms_opt(['notes.mdx'], false, 'attributes', 'all');
--  SELECT * FROM read_markdown_blocks('notes.mdx', extract_metadata := true)
```

| field | meaning |
|---|---|
| `intent` | panduck's vocabulary — `'attributes'` |
| `value` | the intent's value — `'all'` |
| `param` | this reader's spelling, validated as `^[A-Za-z_][A-Za-z0-9_]*$` |
| `arg` | the literal to pass |
| `arg_type` | `VARCHAR`, `BOOLEAN` or `INTEGER` |

`VARCHAR` renders quoted with `''` doubling; `BOOLEAN` and `INTEGER` render **bare** and are
therefore checked against their declared type, which is the entire reason `arg_type` exists:

```sql
--  param: 'x := 1) UNION SELECT'
--  Invalid Input Error: panduck: param must be an identifier, got 'x := 1) UNION SELECT'
--  arg: 'true) UNION SELECT 1 --', arg_type: 'BOOLEAN'
--  Invalid Input Error: panduck: arg 'true) UNION SELECT 1 --' is not a BOOLEAN
--  arg_type: 'DOUBLE'
--  Invalid Input Error: panduck: unknown arg_type 'DOUBLE' (VARCHAR, BOOLEAN or INTEGER)
--  a row with no `arg` at all
--  Invalid Input Error: panduck: an option needs a non-NULL 'arg'
```

The struct casts **by name**, so field order does not matter and a *misspelled* field
arrives as NULL rather than as a bind error — which is why every field being required and
NULL being refused is the check that actually stands there. Rendering re-checks `param` and
`arg` a second time, because that is the only place registration data becomes SQL text and
it does not delegate its own safety to a check that ran somewhere else.

`panduck_register_table_reader` takes the same `options` parameter.

## `read_pdf_blocks(src, pages := '')`

PDF into `duck_blocks`, with page selection that works:

```sql
LOAD pdf;   -- calling the reader directly does not autoload it; dispatch does
SELECT count(*) FROM read_pdf_blocks('test/fixtures/two_pages.pdf');              -- 45
SELECT count(*) FROM read_pdf_blocks('test/fixtures/two_pages.pdf', pages := '1'); -- 28
SELECT count(*) FROM read_pdf_blocks('test/fixtures/two_pages.pdf', pages := '2'); -- 17
```

`pages` accepts `N` or `N-M` and **rejects anything else** rather than ignoring it. It was
previously declared on `read_panduck_doc` and never referenced in the macro body, so
`pages := 'utter nonsense'` returned the whole document. It is now real on both surfaces,
and dispatch refuses in `read_pdf_blocks`' own words so the two paths cannot disagree about
the same argument:

```sql
SELECT count(*) FROM read_panduck_doc('test/fixtures/two_pages.pdf', pages := '1');
--  28

SELECT count(*) FROM read_panduck_doc('test/fixtures/two_pages.pdf', pages := 'utter nonsense');
--  Invalid Input Error: panduck: pages must be N or N-M, got utter nonsense

SELECT count(*) FROM read_panduck_doc('test/fixtures/two_pages.pdf', pages := NULL);
--  Invalid Input Error: panduck: pages must be N or N-M, got <NULL>
```

Formats that have no pages say so, and so does a source naming more than one document:

```sql
SELECT count(*) FROM read_panduck_doc('test/fixtures/constructs.odt', pages := '2');
--  Invalid Input Error: panduck: pages applies only to paginated formats (pdf); odt has no pages

SELECT count(*) FROM read_panduck_doc('test/fixtures/*.odt', pages := '2');
--  Invalid Input Error: panduck: pages applies to a single document;
--  test/fixtures/*.odt names more than one
```

That plural guard runs **before** the format is resolved, so it holds for `'*.pdf'` too —
which the format guard alone would let through, since a glob of PDFs still resolves to
`pdf`. Forward-safety rather than redundancy: the day any format grows a plural arm, that
combination would otherwise start silently dropping `pages` again.

This goes through `read_pdf_elements`, not `pdf_to_markdown`, because `pdf_to_markdown`
takes a path and nothing else and therefore cannot select pages at all. The trade is
measured and real: the elements route **keeps list structure** that the markdown route
flattens into one paragraph, and **loses inline emphasis** that the markdown route keeps.
Neither dominates. Heading levels are ranked over the whole document before the content is
sliced, so a section cut from the middle keeps the depth it has in the full document
instead of being promoted to level 1 — which costs a full-document scan, so `pages` selects
content rather than saving work.

Requires the `pdf` extension, which declares excluded platforms (wasm, windows
mingw/rtools/arm64). `test/sql/pdf_reader.test` is gated on `PANDUCK_TEST_PDF` for that
reason and runs via `make check-pdf`; until it is wired into a platform-restricted CI job
the pdf path is verified locally and nowhere else.

## What is missing, and why

`doc_render` has no `ansi` arm. `duck_block_utils` build `3f2a0f0` does register
`duck_blocks_render_ansi` as a scalar at LOAD, so the old pragma blocker is gone and this
is now only unwired — adding it means checking that signature against what `doc_render`
needs.

`doc_toc` is built on the **scalar** `duck_blocks_toc`, not on a table macro.

`test/sql/doc_namespace.test` asserts which spelling the installed build has, so the day
that changes the suite says so by name rather than failing three assertions later with
`function ... does not exist`. It did exactly that on 2026-09-04.
