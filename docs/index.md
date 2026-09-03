# panduck

Native, in-process document IO for DuckDB.

panduck reads rich document formats directly into the unified `duck_block` AST — no
external binary on `$PATH`, no subprocess per file — and dispatches any path to the right
reader from a registry that derives itself rather than being maintained by hand.

```sql
LOAD panduck;

SELECT * FROM read_panduck_doc('report.docx');    -- any document -> duck_blocks
SELECT * FROM read_panduck_table('data.parquet'); -- any data file -> rows
SELECT level, title FROM doc_toc('report.docx');  -- table of contents, by path
```

## Where to start

| | |
|---|---|
| [Architecture](architecture.md) | The layering, and why nothing depends upward |
| [Readers](readers.md) | All ten formats, the Pandoc AST reader, and what real writers actually emit |
| [Dispatch](dispatch.md) | The derived registry and runtime reader registration |
| [The doc_ namespace](doc_namespace.md) | Path-taking sugar over `db_*` |
| [Validation](validation.md) | How the pandoc-compatibility claim is tested |

## Status

Ten native readers — **RTF**, **DOCX**, **ODT**, **EPUB**, **LaTeX**, **Org**, **RST**,
**ipynb**, **MediaWiki** and **Textile** — plus a **Pandoc AST reader** that reaches every format pandoc
can read, a **write direction** back out to pandoc JSON, document metadata across all of
them, full path dispatch, runtime reader registration, and a differential validator that
checks panduck against a real pandoc on every run.

**The roadmap's `planned` list is now empty.** A regression test pinned "a planned format
must not be routable" against a concrete extension for the project's whole life — `.docx`,
then `.odt`, `.epub`, `.tex`, `.org`, `.rst`, `.wiki` — each promotion turning it red and
demanding the format be finished rather than the test patched. MediaWiki was the last, so
the test now states the invariant directly instead of naming an example.

## The function surface

**Reading**

| Function | Shape |
|---|---|
| `read_panduck_doc(src, format := 'auto', pages := '')` | table of `duck_block` rows |
| `read_panduck_table(src)` | table of rows and columns |
| `panduck_read_blocks(src, …)` | `LIST(duck_block)` |
| `read_rtf_blocks(path)` … `read_pandoc_blocks(path)` | one format, directly |
| `read_latex_blocks_string(src)` and the org, rst, ipynb, pandoc forms | the same reader over a string rather than a path |

**Writing** — back out to a pandoc AST

| Function | Returns |
|---|---|
| `panduck_blocks_to_pandoc_ast(blocks)` | `STRUCT(pandoc-api-version, meta, blocks)` |
| `panduck_blocks_to_pandoc_blocks(blocks)` | the blocks array alone, for splicing |
| `panduck_write_pandoc_ast(path, blocks)` | `BOOLEAN` |

panduck's readers are deliberately **more faithful than pandoc** in places. The rule that
keeps that from becoming incompatibility: diverge where the source justifies it, discard
nothing, but the mapping back to *valid* pandoc JSON stays total. `make check-writeback`
enforces it by feeding every fixture's output to a real pandoc.

**Documents by path**

| Function | Returns |
|---|---|
| `doc_toc(src, format := 'auto')` | `level, title, id, indent, element_order` |
| `doc_render(src, output_format, format := 'auto')` | `md`, `html` or `text` |

**Introspection**

| Function | Returns |
|---|---|
| `panduck_supported_extensions()` | panduck's self-description as a reader |
| `panduck_reader_registry()` | the derived dispatch table |
| `panduck_format_for(path)` | format name, or NULL if unclaimed |
| `panduck_can_read(path)` | boolean |
| `panduck_supported_paths()` | every claimed extension |
| `panduck_pandoc_ast_map()` | the pandoc-types 1.23 vocabulary correspondence |
| `panduck_version()`, `panduck_pandoc_api_version()`, `panduck_duck_block_type()` | build facts |

**Registration**

| Function | Effect |
|---|---|
| `CALL panduck_register_doc_reader(ext, function, [exts])` | route extensions to your own block reader |
| `CALL panduck_register_table_reader(ext, function, [exts])` | route extensions to your own table reader |
| `panduck_ensure_extension(name)` | load an installed extension; false if absent |

## Installing

panduck is **not yet published** to the DuckDB community extension repository, so for now
it is built from source — see [Building](https://github.com/teaguesterling/duckdb_panduck#building)
in the README.

Once published, installation will be:

```sql
INSTALL panduck FROM community;
LOAD panduck;
```
