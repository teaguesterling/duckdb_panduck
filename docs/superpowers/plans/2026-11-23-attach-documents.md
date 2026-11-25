# ATTACH a Document Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Execution:** native, in this session (Teague, 2026-11-23), then one whole-branch review.

**Goal:** `ATTACH 'report.docx' AS r (TYPE panduck)` gives a read-only catalog of lazy views over the document.

**Architecture:** A DuckDB `StorageExtension` named `panduck`. Its attach callback reads the blocks once on a nested `Connection`, builds (view name → SQL) pairs, and returns an in-memory read-only `DuckCatalog` whose views come from a `DefaultGenerator`. Nothing binds until a query asks.

**Tech Stack:** C++17, DuckDB v2.0.0 (pin `d54cbbe`; v2.0.1 next), sqllogictest.

**Spec:** `docs/superpowers/specs/2026-11-23-attach-documents-design.md` (approved 2026-11-23, commit `1594dc7`). Read it before any task. The plan does not repeat its reasons.

## Global Constraints

- Read-only. No writes to the document, ever. `db.SetReadOnlyDatabase()` on the catalog.
- Eager to list, lazy to run. ATTACH reads the blocks once. No view binds during ATTACH or during `duckdb_views()`.
- No network: `http://`, `https://`, `s3://` fail at ATTACH, by name.
- No macros or functions in the attached catalog. Views only, schema `main`.
- No dependency on duck_block_utils.
- Messages start `panduck: `. Option error form, verbatim: `panduck: TABLE_NAMES must be one of slugs, numbered, named_only, both; got 'slug'`.
- Options: `TABLE_NAMES` (default `'slugs'`), `FORMAT` (default `'auto'`). Erase both from `options.options` in the callback; keys arrive lower-cased. Leave every other key for DuckDB.
- `options.default_table = QualifiedName::Parse("blocks")`.
- Do not change duckeye-android or the duckeye CLI. Do not push. Commit per task on `feat/attach-documents`.
- Builds and test runs go through Slurm (see "Build and test" below).

## Build and test (every task uses these)

Build (10–25 min, full rebuild per commit is normal):

```bash
cd .worktrees/attach-documents
srun -J panduck-attach-build --comment="session=$CLAUDE_CODE_SESSION_ID" -n 1 -c 8 --mem=16G -p batch -t 90 \
  bash -c 'VCPKG_TOOLCHAIN_PATH=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake GEN=ninja make release' \
  > $SCRATCH/build.log 2>&1; tail -5 $SCRATCH/build.log
```

Test one file:

```bash
srun -J panduck-attach-test -n 1 -c 4 --mem=8G -p ci -t 30 \
  build/release/test/unittest "test/sql/attach*.test"
```

Pass means the summary line says `All tests passed` with a non-zero assertion count. Then run the whole suite once per task: `build/release/test/unittest "test/sql/*"`. Report the counts, not the colour.

`build/release/` still holds the throwaway spike build. Task 1's first build replaces it.

## Review Focus

1. **A document with zero tables.** Expect the fixed views and an empty `tables`. Test in Task 2 (`lists.docx`).
2. **A path with a single quote** (`it's.docx`). The path goes into view SQL as a literal. Expect it to work. Test in Task 1: quote every literal with DuckDB's `KeywordHelper::WriteQuoted` (or `Value(...).ToSQLString()`), never string concatenation.
3. **Table content with a single quote or a backslash** in a cell. Same quoting rule. Test in Task 2 (an embedded csv cell `it's`).
4. **Two ATTACHes of the same file under two names**, and ATTACH while another connection queries. Expect both to work independently. Test in Task 5.
5. **A heading id with a hyphen** (`my-prices`). Expect the view `my_prices` (Teague, 2026-11-23). Test in Task 3.

---

### Task 1: The storage extension, the `blocks` view, options and paths

**Files:**
- Create: `src/attach_storage.cpp`, `src/include/attach_storage.hpp`
- Modify: `src/panduck_extension.cpp` (call `RegisterAttachStorage(loader)` after `RegisterReaderRegistry(loader)`, line 150); `CMakeLists.txt` (add `src/attach_storage.cpp` to the source list ending at `src/table_read.cpp)`)
- Test: `test/sql/attach.test`

**Interfaces:**
- Produces: `void RegisterAttachStorage(ExtensionLoader &loader)` in namespace `duckdb`.
- Produces (internal, for Tasks 2–4): `struct AttachPlan { string path; string format; TableNames mode; vector<pair<string,string>> views; }` and `enum class TableNames { SLUGS, NUMBERED, NAMED_ONLY, BOTH };`. The `DefaultGenerator` serves `views` by name; `GetDefaultEntries()` returns their names in order. Later tasks only append to `views`.

- [ ] **Step 1: Write the failing tests** in `test/sql/attach.test` (header comment says WHY, house style of `test/sql/embedded_tables.test`):
  - `ATTACH 'test/fixtures/constructs.docx' AS r (TYPE panduck)`; `SELECT count(*) > 0 FROM r` → `true`; `SELECT count(*) = (SELECT count(*) FROM read_panduck_doc('test/fixtures/constructs.docx')) FROM r.blocks` → `true`.
  - `ATTACH 'panduck:test/fixtures/rich.docx' AS p` works with no TYPE.
  - The `blocks` view's SQL holds an absolute path: `SELECT sql LIKE '%/test/fixtures/constructs.docx%' AND sql NOT LIKE '%''test/fixtures%' FROM duckdb_views() WHERE database_name='r' AND view_name='blocks'` → `true`.
  - `statement error` for `CREATE TABLE r.x (i INT)`, `CREATE VIEW r.v AS SELECT 1`, `DROP VIEW r.blocks`, each expecting `read-only mode`.
  - `DETACH r`, then re-ATTACH as `r` works.
  - Missing file: `ATTACH 'test/fixtures/nope.docx' AS m (TYPE panduck)` → error containing `panduck:` and `nope.docx`.
  - `ATTACH 'https://example.com/x.html' AS w (TYPE panduck)` → error containing `panduck:` and `network`.
  - `TABLE_NAMES 'slug'` → the verbatim error from Global Constraints.
  - `FORMAT 'nope'` → `panduck: FORMAT must be one of auto, ` … (the registry's formats) … `; got 'nope'`.
  - `FORMAT 'docx'` on `test/fixtures/constructs_dup.no_ext_needed` works (that fixture has no usable extension; confirm with `file` first, else copy `constructs.docx` to a new extensionless fixture).
  - A path with a quote: commit a copy `test/fixtures/it's.docx` of `constructs.docx`; ATTACH it and `FROM` it.
- [ ] **Step 2: Build and run; expect FAIL** with `Unrecognized storage type "panduck"` or similar.
- [ ] **Step 3: Implement.** Follow DuckDB's `src/storage/open_file_storage_extension.cpp` at the pin. The callback: parse and erase `table_names`/`format`; refuse network schemes; make a plain path absolute through `FileSystem::GetFileSystem(context).ExpandPath` plus the cwd join (leave `zim://`, `git://` alone); check the file exists (`panduck: cannot attach '<path>': file not found`); set `info.path = ":memory:"`, `options.access_mode = READ_WRITE`, `options.default_table`; create the `DuckCatalog`, `SetReadOnlyDatabase()`, install the generator. The `blocks` SQL: `SELECT * FROM read_panduck_doc(<p>, format := <f>)`. FORMAT's valid list comes from the reader registry (`src/reader_registry.cpp`), not a hard-coded list.
- [ ] **Step 4: Build and run; expect PASS.** Then the full suite.
- [ ] **Step 5: Commit** `feat(attach): TYPE panduck storage with a read-only blocks view`.

### Task 2: Identify tables; `tables` and `table_N` views (numbered names only)

**Files:**
- Modify: `src/attach_storage.cpp`
- Test: `test/sql/attach.test`

**Interfaces:**
- Consumes: `AttachPlan` (Task 1).
- Produces: `struct FoundTable { idx_t n; string kind; string content; string title; string heading_id; Value columns; idx_t element_order; };` and `vector<FoundTable> IdentifyTables(ClientContext &, const AttachPlan &)`. Task 3 names views from it.

- [ ] **Step 1: Measure risk 5 first**, in the shell: `SELECT * FROM panduck_table_read(coalesce(panduck_embedded_to_table('a,b' || chr(10) || '1,2', 'csv').content, error('x')))`. If it binds, use that form. If not, bake the converted content at ATTACH and store the parse error, so the view SQL is `SELECT error('<reason>')` for a failed one. Record which in the commit message.
- [ ] **Step 2: Write the failing tests:**
  - `constructs.docx`: `SELECT n, view, kind FROM r.tables` → `1 table_1 table`; `DESCRIBE r.table_1` → `Name VARCHAR, Value BIGINT, Note VARCHAR`.
  - `lists.docx` (no tables): `SELECT count(*) FROM l.tables` → `0`; views are exactly the fixed set.
  - `embedded_data.md` (needs markdown; add `require` or install as `test/sql/multidoc.test` does): `tables` has three rows, kinds `csv, json, csv`, `columns` NULL; `duckdb_views()` lists `table_1..table_3` without error; `FROM table_1` returns 2 rows with `b` = `x, y`; `FROM table_3` fails with the csv parse reason (not `the table content is NULL`); `FROM table_2` still works afterwards.
  - Order: in a fixture with an embedded csv before a real table (Review Focus 1–3 fixture, create `test/fixtures/attach_tables.md` with a real pipe table, a csv cell `it's`, and a backslash cell), the real table is `table_1`.
- [ ] **Step 3: Build and run; expect FAIL.**
- [ ] **Step 4: Implement `IdentifyTables`** with one query on a fresh `Connection(*db_instance)` over `read_panduck_doc`: table blocks, and `code` blocks whose language is csv/tsv/json/jsonl. Title is the caption attribute, else the nearest preceding heading's text, else NULL; keep that heading's `id` attribute. Sort real tables first, then embedded, each by `element_order`; `n` is 1-based. `tables` is a `VALUES` list with typed columns `n BIGINT, view VARCHAR, kind VARCHAR, title VARCHAR, columns VARCHAR[], element_order BIGINT` (empty list → `SELECT ... WHERE false`). Name every table `table_N` in this task.
- [ ] **Step 5: Build and run; expect PASS.** Full suite.
- [ ] **Step 6: Commit** `feat(attach): one view per found table, and the tables listing`.

### Task 3: `TABLE_NAMES` modes and slugs

**Files:**
- Modify: `src/attach_storage.cpp`
- Create: `test/fixtures/attach_names.<ext>` (see Step 1)
- Test: `test/sql/attach_names.test`

**Interfaces:**
- Consumes: `FoundTable`, `TableNames` (Tasks 1–2); `panduck::HeadingSlug(const std::string &, char)` from `libpanduck/include/panduck/slugify.hpp:147`.
- Produces: `vector<pair<string,string>> NameTables(const vector<FoundTable> &, TableNames)` returning (view name, SQL); it also sets each row's `view` in `tables`.

- [ ] **Step 1: Make the fixture.** It needs: a captioned table, a table under a heading with no other title, a table with no caption and no heading (first, before any heading), two tables under one heading `Prices`, and a heading `Blocks` over a table. First measure which reader emits a table caption (`SELECT attributes FROM read_panduck_doc(...) WHERE block_type='table'` on `rich.docx`, `constructs.html`, an rst `.. table::` and a markdown `Table:` caption). Prefer a format panduck reads natively (rst) so the test needs no sibling extension.
- [ ] **Step 2: Write the failing tests** — `SELECT n, view, title FROM r.tables ORDER BY n` for each mode, expected:
  - `slugs`: `table_1` (no title), the caption slug, the heading slug, `prices`, `prices_2`, `blocks_2`.
  - `numbered`: `table_1 … table_6`.
  - `named_only`: as `slugs` but row 1 `view` NULL, and `r.table_1` does not exist.
  - `both`: `duckdb_views()` has `table_1..table_6` plus the five slugs; `SELECT count(*) FROM r.prices` equals `FROM r.table_4`.
  - `r.blocks` is still the blocks view in every mode.
  - Hyphen: a heading id `my-prices` gives the view `my_prices`.
- [ ] **Step 3: Build and run; expect FAIL.**
- [ ] **Step 4: Implement `NameTables`.** Slug = the heading `id` if present, else `HeadingSlug(title, '_')`. Replace every `-` in the result with `_` (Teague, 2026-11-23), so every view name is a plain SQL identifier. Dedupe in `n` order against the fixed view names, every `table_K`, and earlier slugs, suffix `_2`, `_3`.
- [ ] **Step 5: Build and run; expect PASS.** Full suite.
- [ ] **Step 6: Commit** `feat(attach): TABLE_NAMES slugs, numbered, named_only, both`.

### Task 4: The content views (`headings`, `sections`, `code`, `metadata`, `links`, `images`)

**Files:**
- Modify: `src/attach_storage.cpp`
- Test: `test/sql/attach_views.test`

**Interfaces:**
- Consumes: `AttachPlan.views` (Task 1). Each view's SQL reads `read_panduck_doc(<p>, format := <f>)` directly, not the `blocks` view, so it does not depend on how the attached name resolves.

- [ ] **Step 1: Measure risk 4** on `rich.docx`, `rich.odt`, `rich.rtf`, `constructs.html`, `rich.source.md`: the attribute keys on inline `link` and `image` rows, and the content shape of `metadata` blocks. Write the findings as a comment at the top of the test file.
- [ ] **Step 2: Write the failing tests:** column names and types of each view (`DESCRIBE`); `headings` row count equals `SELECT count(*) FROM read_panduck_doc(...) WHERE block_type='heading'`; `sections` on a two-level fixture ends each section at the next heading of the same or higher level (one exact expected table); `code` on `embedded_data.md` has 4 rows including `python`; one known link and image from `rich.docx` with exact `address`; `metadata` key/value from `rich.docx`. Where duck_block_utils is present, `headings` matches `doc_toc` (guard with `require duck_block_utils` in a separate test block or skip note).
- [ ] **Step 3: Build and run; expect FAIL.**
- [ ] **Step 4: Implement** the six SQL strings per the spec's views table, using the keys Step 1 measured.
- [ ] **Step 5: Check binding `blocks` reads the file** (spec, "A finding for the app"): time `DESCRIBE r.blocks` on a large fixture, or count reader calls. Record the answer in the commit message. Fix it only if it is cheap; otherwise note it.
- [ ] **Step 6: Build and run; expect PASS.** Full suite.
- [ ] **Step 7: Commit** `feat(attach): headings, sections, code, metadata, links, images views`.

### Task 5: Close risks 1–3 (nested Connection, session state, md/html)

**Files:**
- Modify: `src/attach_storage.cpp` only if a test fails
- Test: `test/sql/attach_concurrency.test`

- [ ] **Step 1: Write the tests:** `SET threads=4` then ATTACH and query; ATTACH inside `BEGIN; ... COMMIT;`; ATTACH inside `BEGIN` then `ROLLBACK` (expect no `r` afterwards); two connections (`con1`/`con2` labels) attaching the same file under two names; ATTACH `constructs.html` (webbed) and `rich.source.md` (markdown), installed as `test/sql/multidoc.test` does; a reader registered with `panduck_register_doc_reader` in this session is used by ATTACH (or, if it is session state and not seen, a `statement error` that documents the limit).
- [ ] **Step 2: Build and run.** If all pass, commit the tests alone. If one fails, stop and report it before changing code (Golden Rule: test first, then implementation, separately).
- [ ] **Step 3: Commit** `test(attach): nested connection under threads, transactions and two connections`.

### Task 6: Docs, and the handoff drafts

**Files:**
- Create: `docs/attach.md`
- Modify: `docs/doc_namespace.md` (one line linking `attach.md`)
- Create: `$SCRATCH/handoff-android.md`, `$SCRATCH/handoff-cli.md` (not committed)

- [ ] **Step 1: Write `docs/attach.md`:** the ATTACH forms, both options with their values, the views table, the costs (no cache; listing columns binds every view; the file is read once at ATTACH for tables), and what is out of scope. STE style.
- [ ] **Step 2: Draft the two handoff messages.** Each says: ATTACH works on branch `feat/attach-documents` at `<sha>` (not merged), the views and options, and the `columns_sql()` finding with its one-line fix (`DESCRIBE duckeye_data`). Do NOT send them. Sending waits for Teague's go and for the branch to merge.
- [ ] **Step 3: Commit** `docs(attach): ATTACH a document`.

---

## Spec coverage (self-review)

| Spec part | Task |
|---|---|
| TYPE and prefix forms, read-only, DETACH, default_table, paths, network refusal, options and errors | 1 |
| Identify once, `tables`, per-table views, embedded via `error()`, risk 5 | 2 |
| TABLE_NAMES, slug rule, collisions | 3 |
| Content views, risk 4, cheap `blocks` bind check | 4 |
| Risks 1–3 | 5 |
| `docs/attach.md`, handoff | 6 |
| odt and rtf ATTACH (spec tests list) | 1: add `constructs.odt` and `lists.rtf` to the Step 1 smoke list |

Not testable in sqllogictest: "ATTACH and `duckdb_views()` bind no view." The spike measured it with a bind counter. Task 2's broken-csv test covers the user-visible half. The builder re-measures it once with a temporary counter and does not commit the counter.
