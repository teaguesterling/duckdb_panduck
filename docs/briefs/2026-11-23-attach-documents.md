# Brief: ATTACH a document as a read-only database in panduck

Written 2026-11-23 by Claude (the duckeye-android business-and-play session) for Teague Sterling.
Branch: `feat/attach-documents` in a worktree at `.worktrees/attach-documents`, cut from `origin/main` (4901cef).

## STAR summary

**Situation.** duckdb_panduck is the DuckDB extension that reads documents (docx, odt, epub, rtf, LaTeX,
rst, org, MediaWiki, textile, ipynb, pandoc JSON) into duck_blocks, the shared row-per-block shape.
It already has a `doc_*` namespace that takes a path (`doc_toc`, `doc_render`, `doc_section`, ...;
see `docs/doc_namespace.md`). Two apps use panduck: duckeye (the CLI document reader,
`~/Projects/duckeye`) and duckeye for Android (`~/Projects/duckeye-android`, a Tauri app).

**Task.** Teague wants "attach/extract" built into panduck directly. Today the Android app does it
itself: when you open a document, the app lists the document's tables and runs the one you pick with its
own SQL. Teague confirmed (2026-11-23) the target shape is
`ATTACH 'report.docx' AS r (TYPE panduck)`: the document becomes a read-only database, and its tables are
the views listed below. The app's "Extract" menu kinds become those tables.

**Action.** Design first. Write a spec, publish it as an artifact (Teague reads on a phone), and get his
approval before any code. Then build it on this branch, with tests in `test/sql/`.

**Result (now).** Nothing is built. This brief and the empty branch exist. The app's own version ships in
duckeye-android (issue #63, branch `feat/documents-as-databases`, merged into the app's feature stack).
The app does not change before its 0.2.0 Play release; it moves to panduck's ATTACH later.

## Status

Idea, approved in shape by Teague. Not specced. **Run `superpowers:brainstorming` first**, then write the
spec at `docs/superpowers/specs/2026-11-23-attach-documents-design.md`.

## Your prompt

You are designing and then building `ATTACH ... (TYPE panduck)` for duckdb_panduck. A document attached
this way is a read-only catalog. Its tables, as the app lists them today:

| Table | Rows | Today in the app |
|---|---|---|
| `document` / `blocks` | the duck_blocks of the document | Doc mode, Data-of-blocks |
| `sections` | one row per section | built-in SQL |
| `code` | code blocks | built-in SQL |
| `metadata` | front matter / document metadata | built-in SQL |
| `headings` | level, text, id (the contents) | built-in SQL; compare `doc_toc` |
| `tables` | one row per table found: real tables, then embedded data (code fenced csv/tsv/json/jsonl/yaml) | `tables_sql` |
| `table_1`, ... (naming is yours to propose) | each found table as typed rows, header as column names | written to a cache CSV, then `read_csv`/`read_json_auto`/`read_yaml` |
| `links` | text, address, title | Extract › Links (webbed helpers) |
| `images` | alt text, address, size | Extract › Images (webbed helpers) |
| `structured`, `page info` | JSON-LD; title, description, share card, canonical | web pages only; webbed. Probably stays out of panduck: say so in the spec |

Read these first:
- `docs/doc_namespace.md`, `docs/architecture.md`, `docs/readers.md` in this repo.
- The app's design: `docs/superpowers/specs/2026-11-13-documents-as-databases-design.md` in
  `~/Projects/duckeye-android` (on main or `feat/documents-as-databases`). Teague's five decisions are
  quoted there. The key one: **"eager to identify and list, lazy to run"**. Listing the tables must not
  parse every table; only a query on a table runs it.
- The app's code: `core/src/source.rs` (how sources attach and list; `DbKind::Document`) in
  duckeye-android.
- duckeye CLI commit 987ba6f (`-E/--extract`, `-x/--as-data`): reads a document's tables as rows.
  Its message names `panduck_table_read` and `panduck_embedded_to_table`; I (the briefing session) found
  neither name in this repo's `src/`, so find where they live before you reuse them.

## Measured so far

- Nothing about ATTACH in panduck: `rg 'StorageExtension|TYPE panduck'` over `src/` finds nothing (2026-11-23).
- The app attaches real database files READ_ONLY as `duckeye_src` and never `USE`s them, because under
  `USE`, an attached file's macro `length` replaced the built-in (DuckDB v2.0.0; comment in `source.rs`).
  Keep that in mind if the attached document defines anything callable.
- The app forbids `ATTACH` in its AI-written SQL (`core/src/prompt.rs`); the app itself issues the ATTACH.

## Limits agreed up front

- Read-only. No writes to the document, ever.
- Eager to list, lazy to run (Teague, 2026-11-13).
- No network in panduck for this; a web page arrives as a file the app already fetched.
- Don't change duckeye-android or the duckeye CLI from this session. When panduck's ATTACH works, tell
  the app's owner session (`android-share-target`) and the CLI session; they switch over.
- Commit and push only when Teague says. Batch builds go through Slurm (load the `slurm` skill).

## The first question to settle

**Is a DuckDB storage extension (`ATTACH ... (TYPE panduck)`) the right mechanism, or should it be a set
of `doc_*` table functions plus a macro that creates views in a schema?** A storage extension gives the
exact `ATTACH` Teague asked for, but it is more C++ and ties panduck to DuckDB's catalog API, which
changes between releases. Measure what a minimal storage extension costs against the pinned DuckDB
version before you commit to it, and put both options in the spec with your recommendation.

## Links

- duckeye-android #63 (documents as databases), #62 (table links, deferred).
- duck_block_utils #78 (C++ table function for a block table's rows) and #79 (embedded data).
- Teague's decision for this brief: 2026-11-23, in the duckeye-android business session.
