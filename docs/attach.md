# ATTACH a document

**A document attaches as a read-only database of views. Listing the views parses nothing.**

```sql
ATTACH 'report.docx' AS r (TYPE panduck);
SELECT view_name FROM duckdb_views() WHERE database_name = 'r';  -- parses no table
SELECT * FROM r.tables;                                           -- the tables, as literal data
SELECT * FROM r.prices;                                           -- parses only this table
FROM r;                                                           -- the blocks
```

`ATTACH 'panduck:report.docx' AS r` is the same, with no `TYPE`. A plain
`ATTACH 'report.docx'` does not work: DuckDB picks the storage by the file's first bytes, and a
`.docx` is a ZIP.

## The views

All views are in schema `main`. The database is read-only: `CREATE`, `DROP` and `INSERT` fail.

| View | Columns |
|---|---|
| `blocks` | the `duck_blocks` columns, from `read_panduck_doc` |
| `tables` | `n, view, kind, title, columns, element_order` |
| one view per table | the table's header cells, with sniffed types |
| `headings` | `level, title, id, element_order` |
| `sections` | `id, level, title, start_order, end_order` |
| `code` | `language, content, element_order, section` |
| `metadata` | `key, value` |
| `links` | `text, address, title, element_order` |
| `images` | `alt, address, title, element_order` |

`tables` has one row per table: real tables first, then embedded data (csv, tsv, json and jsonl
code blocks), each group in document order. `n` is the row number. `title` is the nearest
heading above the table. `columns` is NULL for embedded data, because its header is known only
after it is parsed.

A broken embedded table fails only when you query it, with its parse reason:

```
SELECT * FROM r.table_4;
Invalid Input Error: csv: unterminated quote in record 2
```

`images.alt` and the `title` columns are NULL today. No reader sets those attributes yet.

## Options

Each option is a named mode with a default. A bad value fails at ATTACH and lists the valid
values:

```
panduck: TABLE_NAMES must be one of slugs, numbered, named_only, both; got 'slug'
```

### `TABLE_NAMES`, default `'slugs'`

| Value | Views for the tables | A table with no heading |
|---|---|---|
| `'slugs'` | the slug of the nearest heading | `table_N` |
| `'numbered'` | `table_1 … table_N` | `table_N` |
| `'named_only'` | the slug | no view; its row stays in `tables` |
| `'both'` | `table_N` and the slug, over the same SQL | `table_N` only |

- The slug is the heading's own `id` when it has one, else panduck's heading-id rule.
- A character that is not a letter, a digit or `_` becomes `_`. So every name is a plain SQL
  identifier: `r.my_prices`, not `r."my-prices"`.
- A repeated slug gets `_2`, `_3`.
- A slug never takes a fixed view's name (`blocks`, `tables`, ...) or any `table_N`. So
  `table_N` is the same table in every mode.

### `FORMAT`, default `'auto'`

Passes `format :=` to `read_panduck_doc`. The valid values are the reader registry's formats,
plus `'auto'`, `'pandoc'` and `'code'`.

Use it for a file whose extension does not say its format, such as a page the app saved
under a temporary name:

```sql
ATTACH '/tmp/share-81723' AS r (TYPE panduck, FORMAT 'html');
```

The reader policy applies to the format named. A wrong format fails with the reader's own
error.

## Paths

- A plain path becomes absolute at ATTACH. The views do not depend on the working directory
  later.
- A local scheme (`zim://`, `git://...`) passes through unchanged.
- A network address (`http://`, `https://`, `s3://`) fails at ATTACH. A web page must be a local
  file first.
- A missing file fails at ATTACH: `panduck: cannot attach '<path>': file not found`.

## Costs

- **ATTACH reads the document once**, to find its tables. The table views hold their content as
  a literal, so they never read the file again.
- **Every query on `blocks` or a content view reads the document again.** Nothing is cached
  between statements.
- **Listing columns runs every view.** `duckdb_columns()`, `information_schema.columns` and
  `SHOW ALL TABLES` bind every view, and a filter does not stop that. Use `duckdb_views()` to
  list names, and `DESCRIBE r.<view>` for one view's columns.
- **A changed file:** `blocks` and the content views see the new file. The table views keep the
  old content. Attach again after an edit.

## Reader policy

ATTACH honours `panduck_enabled_readers`, `panduck_disabled_readers` and
`panduck_allow_registration`, including a `SET SESSION` value. A disabled reader makes ATTACH
fail.

## Out of scope

- Writes to the document.
- Network access.
- JSON-LD and web page information (title, share card, canonical URL).
- Embedded YAML.

Tests: `test/sql/attach*.test`. Design:
`docs/superpowers/specs/2026-11-23-attach-documents-design.md`.
