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

## `read_pdf_blocks(src, pages := '')`

PDF into `duck_blocks`, with page selection that works:

```sql
SELECT * FROM read_pdf_blocks('report.pdf', pages := '2');
SELECT * FROM read_pdf_blocks('report.pdf', pages := '3-7');
```

`pages` accepts `N` or `N-M` and **rejects anything else** rather than ignoring it. It was
previously declared on `read_panduck_doc` and never referenced in the macro body, so
`pages := 'utter nonsense'` returned the whole document. Formats that have no pages now
say so:

```
read_panduck_doc('report.odt', pages := '2')
Invalid Input Error: panduck: pages applies only to paginated formats (pdf); odt has no pages
```

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
