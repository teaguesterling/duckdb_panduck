# panduck

Native, in-process document IO for DuckDB.

`panduck` reads rich document formats directly into the unified
[`duck_block`](https://github.com/teaguesterling/duckdb_duck_block_utils) AST — no external
binary on `$PATH`, no subprocess per file — and dispatches any path to the right reader
from a registry that derives itself rather than being maintained by hand.

```sql
LOAD panduck;

SELECT * FROM read_panduck_doc('report.docx');   -- any document -> duck_blocks
SELECT * FROM read_panduck_table('data.parquet'); -- any data file -> rows
SELECT * FROM doc_toc('report.docx');             -- table of contents, by path
```

**Status:** five native readers (RTF, DOCX, ODT, EPUB, LaTeX), full path dispatch, and a
differential validator that checks panduck against real pandoc on every CI run. RST, Org
and MediaWiki are declared but not yet implemented — and the registry knows the
difference, so nothing routes to a reader that doesn't exist.

## Why not just call pandoc?

Because pandoc is a Haskell program and there is no practical way to call it in-process:

- **[ShabbyX/libpandoc](https://github.com/ShabbyX/libpandoc)**, the only real C bindings,
  has been unmaintained since **2017** and targets pandoc 1.x. Pandoc is now 3.x.
- **Upstream has never shipped a C shared library.**
  [jgm/pandoc#6611](https://github.com/jgm/pandoc/issues/6611) has been open since 2020.
- Even if one existed, linking the **GHC runtime** into a `dlopen`'d extension inside
  DuckDB's already-multithreaded process would be a bad neighbour — and the ~200 MB moves
  from `$PATH` into the extension binary, which then can't ship as a community extension.

That leaves a CLI subprocess (~50–200 ms of fork/exec per file, no vectorization, no
pushdown) or an HTTP server. panduck does neither.

**panduck is compatible with pandoc's data model, not its ABI** — and that claim is
[tested, not asserted](docs/validation.md).

## Reading documents

| Function | Shape | Returns |
|---|---|---|
| `read_panduck_doc(src, format := 'auto', pages := '')` | table | `duck_block` rows |
| `panduck_read_blocks(src, …)` | scalar | `LIST(duck_block)` |
| `read_panduck_table(src)` | table | rows and columns |
| `read_rtf_blocks(path)` / `read_docx_blocks(path)` | table | one format, directly |

Two surfaces, mirroring [duckeye](https://github.com/teaguesterling/duckeye)'s own split:
a document is prose and becomes blocks; `--raw` is *"read FILE as data, not prose"* and
becomes a table. A `.csv` has no document structure and a `.docx` has no rows — one
function doing both would have to lie about one of them.

`read_panduck_doc` is a **table function**, so a filter pushes down to the reader. The
scalar `LIST` form plants a blocking aggregate that no predicate can pass, which is fine
for a README and not for a 400-page EPUB. See [Dispatch](docs/dispatch.md).

## The doc_* namespace

`doc_*` takes a **path**; `db_*` (in `duck_block_utils`) takes blocks you already hold.

```sql
SELECT level, title FROM doc_toc('report.docx');
SELECT doc_render('report.docx', 'md');
```

These load `duck_block_utils` on demand, exactly as reading `.md` loads `duckdb_markdown`.
panduck's core never needs it. See [The doc_ namespace](docs/doc_namespace.md).

## Formats

| Format | Extensions | Status |
|---|---|---|
| `rtf` | `.rtf` | **implemented** — `read_rtf_blocks` |
| `docx` | `.docx` | **implemented** — `read_docx_blocks` |
| `odt` | `.odt` | **implemented** — `read_odt_blocks` |
| `epub` | `.epub` | **implemented** — `read_epub_blocks` |
| `latex` | `.tex` `.latex` | **implemented** — `read_latex_blocks` |
| `rst` `org` `mediawiki` | | declared, not implemented |
| `markdown` `html` `pdf` | `.md` `.html` `.pdf` | routed to `duckdb_markdown`, `duckdb_webbed`, `pdf` |
| `toml` `yaml` | `.toml` `.yaml` | read as a `metadata` block |
| `data` | `.csv` `.parquet` `.json` `.xlsx` … | `read_panduck_table` only |
| `code` | anything unclaimed | falls through to `sitting_duck` |

`panduck_supported_extensions()` is panduck's self-description; `panduck_reader_registry()`
is the derived dispatch table. Adding a reader means flipping one row from `planned` to
`implemented` — dispatch picks it up with no code change. See [Dispatch](docs/dispatch.md).

## Building

Dependencies come from vcpkg (`pugixml` for XML, `miniz` for ZIP containers). The
`duck_block` vocabulary is a vendored copy of `duck_block_utils`' published header at
`src/include/duck_block_vocabulary.hpp`. The submodules below are DuckDB itself and the
build tooling; the vocabulary is not one.

Because the C++ constants catch a renamed type but *not* a changed value — which compiles
clean and silently stops matching — the copy comes with a check:

```sh
make check-vocabulary   # compares against upstream by name and value
```

It skips cleanly when upstream is unreachable (`--strict` makes that a failure). See
[Architecture](docs/architecture.md) for why the vocabulary is copied rather than pinned.

```sh
git clone --recurse-submodules https://github.com/teaguesterling/duckdb_panduck.git
cd duckdb_panduck
export VCPKG_TOOLCHAIN_PATH=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
make release
```

Targets DuckDB **v1.5.5**.

```sh
make test                   # sqllogictests
make test_pandoc_alignment  # AST vocabulary vs a real pandoc
make test_roundtrip         # differential validation vs a real pandoc
```

## Documentation

- [Architecture](docs/architecture.md) — the layering, and why nothing depends upward
- [Readers](docs/readers.md) — all five readers, and what real writers actually emit
- [Dispatch](docs/dispatch.md) — the derived registry and runtime reader registration
- [The doc_ namespace](docs/doc_namespace.md) — path-taking sugar over `db_*`
- [Validation](docs/validation.md) — how the pandoc-compatibility claim is tested

## Related

- [`duckdb_duck_block_utils`](https://github.com/teaguesterling/duckdb_duck_block_utils) — the vocabulary and helpers over it
- [`duckdb_markdown`](https://github.com/teaguesterling/duckdb_markdown) — CommonMark + GFM
- [`duckdb_webbed`](https://github.com/teaguesterling/duckdb_webbed) — XML and HTML
- [`sitting_duck`](https://github.com/teaguesterling/sitting_duck) — source code ASTs via tree-sitter

## License

MIT — see [LICENSE](LICENSE).
