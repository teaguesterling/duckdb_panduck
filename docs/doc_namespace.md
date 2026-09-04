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

## What is missing, and why

`doc_section` and `doc_sections_like` do not exist yet, and `doc_render` has no `ansi` arm.
Not an oversight — their counterparts in `duck_block_utils` are **macros created by a
pragma**, and such a macro is unreachable from a panduck macro: invisible to the statement
that loads the extension, and panduck cannot invoke a pragma from inside a macro.

| `duck_block_utils` exposes | reachable from panduck |
|---|---|
| C++ scalars at LOAD — `duck_blocks_toc`, `duck_blocks_to_text` | yes |
| macros behind a PRAGMA — the render/section family, historically | no |

`doc_toc` is therefore built on the **scalar** `duck_blocks_toc`, not on a table macro.

### Both blockers have now cleared upstream — one is followed here, one is not

`duck_block_utils` did two things, and as of the community build **3f2a0f0** both are in
the published extension, not just on its `main`:

1. **Everything is renamed.** `db_*` reads as *database* everywhere else in SQL, so the
   surface moved to `duck_block_*` (one element) and `duck_blocks_*` (a collection) —
   `db_blocks_toc` → `duck_blocks_toc`, `db_block_types` → `duck_block_type_names`.
2. **The pragma blocker is gone.** The query surface is registered at LOAD rather than
   behind `PRAGMA duck_block_render`, so `doc_section` and `doc_sections_like` become
   possible.

**panduck has followed (1) and not (2).** The rename was not optional: `doc_*` names these
functions at runtime, so they follow whatever is `INSTALL`ed, not panduck's vendored
vocabulary header — that copy is a compile-time dependency on constant names and says
nothing about the function surface. Two independent clocks, and when the second one moved
`doc_toc` and `doc_render('…','text')` broke on installs that had nothing wrong with them.
(2) is an opportunity rather than a break, so it is recorded here and left for its own
change.

Measured into an empty `extension_directory`, so the shared `~/.duckdb` profile could not
colour the result:

| name | kind at LOAD | what it unblocks |
|---|---|---|
| `duck_blocks_sections_like` | `table_macro` | `doc_sections_like` |
| `duck_blocks_get_section` | `macro` | `doc_section` |
| `duck_blocks_render_ansi` | `scalar` | `doc_render(…, 'ansi')` |

Wiring any of them up means checking its signature against what the `doc_*` macro needs,
which is why none of it rode along with the rename.

`test/sql/doc_namespace.test` asserts which spelling the installed build has, so the day
that changes the suite says so by name rather than failing three assertions later with
`function ... does not exist`. It did exactly that on 2026-09-04.
