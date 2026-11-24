# ATTACH a document as a read-only database

Status: **draft, for Teague's approval** (2026-11-23). Naming decided by Teague the same day. Nothing is built. Brief:
`docs/briefs/2026-11-23-attach-documents.md`. Branch: `feat/attach-documents`.

## Intent

```sql
ATTACH 'report.docx' AS r (TYPE panduck);
SELECT view_name FROM duckdb_views() WHERE database_name = 'r';   -- cheap: nothing is parsed
SELECT * FROM r.table_1;                                           -- only this table is parsed
```

A document becomes a read-only database. Its tables are the kinds the duckeye-android app's
Extract menu offers today, so the app (and the duckeye CLI) can stop building them in their own
SQL. Teague confirmed this shape on 2026-11-23.

The rule that governs the design is Teague's, from 2026-11-13: **"eager to identify and list,
lazy to run"**. Listing a document's tables must not parse them. Only a query on a table runs it.

## Goals

- `ATTACH '<path>' AS <name> (TYPE panduck)` and `ATTACH 'panduck:<path>' AS <name>` work for
  every format `panduck_read_blocks` reads.
- The attached database holds only views, all in schema `main`. Writes fail.
- Listing the views by name (`duckdb_views()`, `duckdb_tables()`, `information_schema.tables`)
  parses no table.
- Each table found in the document is its own view, with typed columns.

## Non-goals

- Writing to the document, ever.
- Network access. A web page arrives as a file the app already fetched.
- `structured` (JSON-LD) and `page info` (title, description, share card, canonical URL). They
  exist only for web pages and only through webbed's helpers. They stay in the app for now.
- Embedded YAML. `panduck_embedded_to_table` reads csv, tsv, json and jsonl only.
- Plain `ATTACH 'report.docx'` with no `TYPE` or prefix. DuckDB picks the storage by magic bytes
  before any extension can claim the path (`src/main/database_path_and_type.cpp`), and a `.docx`
  is a ZIP. That error stays DuckDB's.
- Changes to duckeye-android or the duckeye CLI. Their sessions switch over later.

## Anti-goals

- An ATTACH that parses every table "to be helpful". That breaks the rule above.
- Anything callable in the attached catalog (macros, functions). The app never `USE`s an attached
  file because, under `USE`, an attached file's macro `length` replaced the built-in (DuckDB
  v2.0.0, `core/src/source.rs` in duckeye-android). A catalog of views only cannot do that.

## Approaches (Teague chose A, 2026-11-23)

| | Approach | What it costs | Why not |
|---|---|---|---|
| **A** | **A storage extension.** `TYPE panduck` returns an in-memory catalog whose views are made on demand. This copies DuckDB's own `OpenFileStorageExtension` (`src/storage/open_file_storage_extension.cpp`, about 80 lines). | About 150 to 250 lines of C++. It depends on DuckDB's catalog API. | Chosen. |
| B | `CALL panduck_attach(path, schema)` creates views in a schema. | Also C++: a macro cannot run `CREATE VIEW`. | Not a real ATTACH. Nothing enforces read-only. The caller drops the schema. |
| C | Table functions only: `doc_tables(path)`, `doc_table(path, n)`. | Smallest. | Not the ATTACH Teague asked for. The app keeps its own listing. |

**What the catalog API costs, measured on 2026-11-23:**

- From DuckDB v1.5.5 to v2.0.0, about 10 lines changed in `open_file_storage_extension.cpp`:
  `Identifier` replaced `string`, and `CreateViewInfo::FromSelect` now takes a parser.
- From v2.0.0 to duckdb `main` at `c4d7b8bd` (2026-10-26, 469 commits later), these files are
  unchanged: `open_file_storage_extension.cpp`, `storage_extension.hpp`,
  `default_generator.hpp`, `create_view_info.hpp`, `duck_catalog.hpp`.
- v2.0.0 to v2.0.1 (the fleet's next pin, approved 2026-11-23): the duckdb-maintenance session
  measured with `git diff` that `storage_extension.hpp`, `default_generator.hpp`, `duck_catalog`
  and `create_view_info` are unchanged. The attach-option check is still at
  `storage_manager.cpp:147`. I (Claude) have not re-run the spike at v2.0.1.
- panduck CI builds v2.0.0 (the pin, `d54cbbe`) and duckdb `main`. The v1.5 leg has retired
  (commit `8ab1483`).
- plinking_duck ships the same pattern on the same pin (`src/pfile_catalog_storage.cpp`).

## What the spike measured

A throwaway storage extension, built at the pin and run in the v2.0.0 shell on
`test/fixtures/constructs.docx`, which has one table. It was never committed. Each listing call
ran on a fresh attach. A probe view counted its binds: two probe views, so `binds=2` means
every view was bound.

| Call | Views bound (= tables parsed) |
|---|---|
| `ATTACH ... (TYPE panduck)` | 0 |
| `duckdb_views()` | 0 |
| `duckdb_tables()` | 0 (views are not tables) |
| `information_schema.tables` | 0 |
| `duckdb_columns()` | every view |
| `information_schema.columns` | every view |
| `SHOW ALL TABLES` | every view |
| `DESCRIBE r.v` | that view |
| `FROM r.v` twice | that view, twice: no cache between statements |

Also measured:

- **A query inside the attach callback does not deadlock.** The spike opened a fresh `Connection`
  on the same database instance and read the blocks. This is one run, in the single-threaded shell.
- **Writes fail:** `CREATE TABLE r.x`, `CREATE VIEW r.v` and `DROP VIEW r.blocks` each give
  `Cannot execute statement of type "CREATE" on database "r" which is attached in read-only mode!`
- `DETACH r` removes it. `ATTACH 'panduck:rich.docx' AS p` works with no `TYPE`.
- `TYPE panduck` autoloads the panduck extension when it is not loaded yet.
- `table_1` came back typed: `Name VARCHAR, Value BIGINT, Note VARCHAR`.

A second spike run, the same day, measured four more things:

- **Custom ATTACH options are rejected by DuckDB unless panduck removes them.**
  `ATTACH ... (TYPE panduck, TABLE_NAMES 'numbered')` failed with
  `Binder Error: Unrecognized option for attach "table_names"`. The check is in DuckDB's file
  storage manager (`duckdb/src/storage/storage_manager.cpp:147`), which an in-memory `DuckCatalog`
  also builds. The attach callback runs first (`src/main/attached_database.cpp:188`, then `:195`).
  When the callback erased `table_names` and `format` from `options.options`, the ATTACH worked,
  and the callback saw `numbered` and `docx`. The keys arrive in lower case.
- **`FROM r` works** when the callback sets `options.default_table` to `blocks`: it returned 52 rows.
- **A view that cannot bind does not break a listing.** With the file deleted after ATTACH,
  `duckdb_columns()` (unfiltered), `information_schema.columns` and `SHOW ALL TABLES` all
  succeeded. They showed the broken view with one placeholder column. Only `FROM r.blocks` failed,
  with the reader's own `file not found` message.
- **A filter does not stop the binding.** `information_schema.columns WHERE table_name = 'x'`
  bound every view of the attached document, and so did `duckdb_columns()` with
  `database_name = 'memory'` as well. See "A finding for the app" below.

**What this means:** listing *names* is free. Listing *columns* runs every view. That cost is
inherent, because a view's columns are only known by binding it, and binding a table runs its
type sniffing. The app lists through `duckdb_views()` (`list_sql()` in `core/src/source.rs`),
which stays free. The spec says the rest plainly rather than hiding it.

## The design

### How ATTACH works

```
ATTACH 'report.docx' AS r (TYPE panduck)
  │
  ├─ 1. resolve the path (see "Paths and options"; views must not depend on the cwd later)
  ├─ 2. IDENTIFY: one read of the blocks, on a fresh Connection
  │       SELECT ... FROM panduck_read_blocks('<abs>')
  │       keeps: each table block's content, each csv/tsv/json/jsonl code block's text,
  │       and the nearest heading and the caption, for the listing
  ├─ 3. build (view name -> SQL) pairs; table contents go in as SQL string literals
  ├─ 4. in-memory DuckCatalog, SetReadOnlyDatabase(), a DefaultGenerator for the views
  └─ done: no view has been bound
```

### Paths and options (Claude's calls, after the builder check)

- **A plain path** becomes an absolute path at ATTACH, through DuckDB's `FileSystem`.
- **A local scheme** (`zim://`, `git://...@rev`) passes through unchanged. Making it absolute
  would break it.
- **A network scheme** (`http://`, `https://`, `s3://`) fails at ATTACH, by name. The brief rules
  out network access for this feature. `panduck_read_blocks` itself still reads them.
- **`FORMAT`**: `ATTACH 'page' AS r (TYPE panduck, FORMAT 'html')` passes `format :=` to the
  reader. A file the app fetched often has no extension, so without this option it cannot be
  attached. Default `'auto'`. Every view that reads the file again bakes in the same format.
- **`pages`** and other reader options are left out. Add them when a caller needs them.
- Any other ATTACH option fails by name, so a typo is not ignored.

Step 2 reads the document once. That counts as identifying, not running. It must happen at
ATTACH because the names of the table views depend on how many tables there are, and on their captions and headings.

The table views take their content as a literal rather than the path. `panduck_table_read` needs a
constant argument, and a view cannot hand it a subquery. A literal also means a table view never
reads the file again.

### The views

Every view is SQL over functions panduck already has. None needs duck_block_utils, so the attach
works when it is absent. `<p>` is the absolute path.

| View | Columns | SQL source |
|---|---|---|
| `blocks` | the duck_blocks columns | `read_panduck_doc(<p>, format := <f>)`. `panduck_read_blocks` returns a list, so the view needs the table-shaped reader. |
| `headings` | `level, title, id, element_order` | heading blocks of `blocks` |
| `sections` | `id, level, title, start_order, end_order` | one row per heading. A section ends at the next heading of the same or higher level, as in `doc_section`. |
| `code` | `language, content, element_order, section` | `code` blocks, `kind = 'block'` |
| `metadata` | `key, value` | `metadata` blocks |
| `links` | `text, address, title, element_order` | inline `link` rows |
| `images` | `alt, address, title, element_order` | inline `image` rows |
| `tables` | `n, view, kind, title, columns, element_order` | a `VALUES` list baked at ATTACH |
| one view per found table, named by `TABLE_NAMES` | the table's header cells, typed | `panduck_table_read('<content>')` |

The default table is `blocks`, so `FROM r` reads the blocks: the callback sets
`options.default_table` (measured).

**`tables`** has one row per found table: real tables first, then embedded data, each group in
document order, as the app lists them. `n` is the row number. `kind` is `table`, `csv`, `tsv`, `json` or `jsonl`. `title` is the
caption, else the nearest heading above, else NULL. `columns` is the header cells for a real
table, and NULL for embedded data, because its header is known only after parsing. The listing
is literal data, so it parses nothing.

**An embedded table** reads as
`panduck_table_read(coalesce(e.content, error(e.error)))`, where `e` is
`panduck_embedded_to_table('<text>', '<kind>')`. The `error(...)` is there because a failed parse
leaves `content` NULL. Without it, the user sees `panduck_table_read: the table content is NULL`
(`src/table_read.cpp:167`), not the parse reason. A broken table fails only when you query it,
and the others still work. A listing that binds it does not fail (measured, above).

### Names: an ATTACH option, `TABLE_NAMES` (Teague, 2026-11-23)

Teague's answer: "Make it configurable on attach, slugs with numbers as a fallback by default but
numbered, named_only, and both as options... apply the same conventions for other sorts of
options too".

```sql
ATTACH 'report.docx' AS r (TYPE panduck);                           -- TABLE_NAMES 'slugs'
ATTACH 'report.docx' AS r (TYPE panduck, TABLE_NAMES 'numbered');
```

| `TABLE_NAMES` | Views made for the found tables | A table with no caption and no heading |
|---|---|---|
| `'slugs'` (default) | the slug of its caption, else of the nearest heading above it | `table_N` |
| `'numbered'` | `table_1 … table_N` | `table_N` |
| `'named_only'` | the slug, as `'slugs'` | no view. It still has a row in `tables`. |
| `'both'` | `table_N` and, when it has one, the slug, as two views over the same SQL | `table_N` only |

Rules that apply in every mode (Claude's calls):

- **N is the table's row in `tables`**: real tables in document order, then embedded data in
  document order, one sequence. It does not depend on the mode, so `table_3` is the same table in `'numbered'` and `'both'`.
- **The slug**: a table under a heading that has an `id` takes that `id` as it is. Some readers
  supply their own ids, such as an HTML `id` attribute (`test/sql/heading_ids.test`). A caption,
  or a heading with no `id`, is slugged with panduck's heading-id rule. The builder finds that
  rule's C++ home and reuses it.
- **A repeated slug** gets `_2`, `_3`, in document order: `prices`, `prices_2`.
- **A slug equal to a fixed view** (`blocks`, `tables`, `headings`, ...) or to a `table_N` gets a
  suffix too. A fixed view never loses its name.
- **`tables`** gains a column `view` (the name to query, or NULL under `'named_only'`), and keeps
  `title` (the caption or heading text, before slugging).

How Claude reads "the same conventions for other sorts of options" (Teague may correct this):
every ATTACH option is a named mode with a stated default, spelled in lower case. An unknown value
fails at ATTACH and lists the valid ones:

```
panduck: TABLE_NAMES must be one of slugs, numbered, named_only, both; got 'slug'
```

`FORMAT` follows the same form. Its valid values are the reader registry's formats plus `'auto'`.

**How the options reach panduck (measured):** the attach callback reads `table_names` and `format`
from `options.options` and erases them. If it does not erase them, DuckDB rejects them. Any other
key stays, so DuckDB's own options (`READ_ONLY` and the others) behave as they do for any ATTACH.

### Costs to know about

- **Every query on `blocks`, `headings`, `sections`, `code`, `metadata`, `links` or `images`
  reads the document again.** Nothing is cached between statements (measured: `FROM r.v` twice
  binds twice). A cached copy of the blocks is possible later, as an in-memory table filled at
  ATTACH, but that makes ATTACH cost more. Deferred.
- **Listing columns runs every view** (the table above). `SHOW ALL TABLES` on a document with 40
  tables sniffs 40 tables, and (inferred, not measured) reads the document once for each of the 7 non-table views.
- **The document is read at ATTACH time and not again.** If the file changes, `blocks` sees the
  new file while `table_N` keeps the old content. Re-attach after an edit.

## A finding for the app (not changed by this work)

The app reads the columns of its own result table with
`information_schema.columns WHERE table_name = 'duckeye_data'` (`columns_sql()`, `core/src/data.rs`
in duckeye-android). The spike measured that this query binds every view of every attached panduck
document. So once the app attaches a document this way, each column lookup parses every table in
it, and re-reads the file for each view that reads the file.

- **This breaks the lazy rule in the app, not in panduck.** panduck cannot stop DuckDB from
  binding a view when something asks for its columns.
- **The fix is in the app**, and is one line: `DESCRIBE duckeye_data`, or read the column names
  from the result of the query itself. The handoff message to the app session must say this.
- **Inside panduck, the builder makes binding cheap where it can.** Check whether binding the
  `blocks` view reads the file. Its columns are fixed, so a bind that reads nothing is possible.

## Risks the builder must close

1. **The nested Connection** is measured once, in the single-threaded shell. Test it with
   `threads = 4`, inside an explicit transaction, and from a second connection.
2. **What the nested Connection sees.** Registered readers are safe: `ReaderRegistry::Get()` is a
   process-wide singleton (`src/reader_registry.cpp:385`, found by the builder check). It sees global settings. It does not see the caller's
   session state: TEMP objects, session variables, a `SET` that is session-local. Find out
   whether a reader registered with `panduck_register_doc_reader` or a `panduck_*` setting is
   session state. If it is, either pass it to the nested Connection or document the limit.
3. **Markdown and HTML** read through duckdb_markdown and duckdb_webbed. The nested query must
   autoload them as `panduck_read_blocks` does today. Test one `.md` and one `.html` fixture.
4. **The attribute names** for links (`href`?), images (`src`? `alt`?) and the shape of a
   `metadata` block's content are not checked here. Measure them across readers before you write
   those views' SQL.
5. **A constant-folded argument.** Confirm that `panduck_table_read(panduck_embedded_to_table(...).content)`
   counts as a constant for the table function. If it does not, bake the converted content at
   ATTACH instead, and keep the parse error for query time.

## Tests (`test/sql/attach.test`)

- ATTACH with `TYPE panduck`, and with the `panduck:` prefix, on docx, odt, rtf, md and html.
- `duckdb_views()` lists the fixed views plus one view per found table, for a document
  with no tables, one table, and real plus embedded tables.
- Listing parses nothing: a broken embedded csv does not make `duckdb_views()` or ATTACH fail.
  Querying that table fails with the parse reason, and the other tables still read.
- `TABLE_NAMES`: each of the four modes, on a fixture with a captioned table, a table under a
  heading, a table with neither, two tables under one heading, and a heading named `blocks`. A bad
  value fails and lists the valid ones.
- `r.table_1` has the header as column names and sniffed types (BIGINT, DOUBLE, VARCHAR, NULL).
- `r.headings` matches `doc_toc` on the same file, where duck_block_utils is present.
- `CREATE TABLE r.x`, `CREATE VIEW r.v`, `DROP VIEW r.blocks` and `INSERT` all fail.
- `DETACH` works. Re-ATTACH under the same name after DETACH works.
- A relative path still works after the cwd changes. A missing file fails at ATTACH, by name.
- `FROM r` reads `blocks`.

## Left to the builder

- The file layout. A new `src/attach_storage.cpp` registered from `LoadInternal`, as the spike
  did, is the obvious shape.
- The exact SQL of each view, after risk 4.
- The message wording, in the house form `panduck: ...`.
- A `docs/attach.md` page, and a line in `docs/doc_namespace.md`.

## Handoff when it works

Tell the duckeye-android owner session (`android-share-target`) and the duckeye CLI session.
Include the column-query finding above. They
switch from their own SQL to `ATTACH ... (TYPE panduck)`. The app keeps its own version for its
0.2.0 Play release.
