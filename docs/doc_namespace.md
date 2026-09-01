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
`db_blocks_toc(panduck_read_blocks(src))` produces, which the suite asserts directly rather
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
SELECT db_blocks_toc(panduck_read_blocks('report.docx'));
```

## What is missing, and why

`doc_section` and `doc_sections_like` do not exist yet, and `doc_render` has no `ansi` arm.
Not an oversight — their counterparts in `duck_block_utils` are **macros created by a
pragma**, and such a macro is unreachable from a panduck macro: invisible to the statement
that loads the extension, and panduck cannot invoke a pragma from inside a macro.

| `duck_block_utils` exposes | reachable from panduck |
|---|---|
| C++ scalars at LOAD — `db_blocks_toc`, `db_blocks_to_text` | yes |
| macros behind a PRAGMA — `db_toc`, `db_section`, `db_ansi` | no |

`doc_toc` is therefore built on the **scalar** `db_blocks_toc`, not the table macro
`db_toc`.

### Both blockers are fixed upstream, and neither is fixed here yet

`duck_block_utils` has since done two things on its `main`, and it is worth being precise
that neither has reached panduck:

1. **The pragma blocker is gone.** The query macros are registered at LOAD via
   `DefaultMacro` / `DefaultTableMacro`, so `doc_section` and `doc_sections_like` become
   possible.
2. **Everything is renamed.** `db_*` reads as *database* everywhere else in SQL, so the
   surface moved to `duck_block_*` (one element) and `duck_blocks_*` (a collection) —
   `db_blocks_toc` → `duck_blocks_toc`, `db_block_types` → `duck_block_type_names`.

Both are blocked on the same thing: **the community build has not been republished.**
panduck's `doc_*` macros name these functions at runtime, so they follow whatever is
`INSTALL`ed, not panduck's vendored vocabulary header — that copy is a compile-time
dependency on constant names and says nothing about the function surface. Two independent
clocks.

`test/sql/doc_namespace.test` asserts which spelling the installed build has, so the day
that changes the suite says so by name rather than failing three assertions later with
`function db_blocks_toc does not exist`.
