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
| [Readers](readers.md) | RTF, DOCX, ODT, EPUB and LaTeX, and what real writers actually emit |
| [Dispatch](dispatch.md) | The derived registry and runtime reader registration |
| [The doc_ namespace](doc_namespace.md) | Path-taking sugar over `db_*` |
| [Validation](validation.md) | How the pandoc-compatibility claim is tested |

## Status

Four native readers — **RTF**, **DOCX**, **ODT** and **EPUB** — plus full path dispatch,
runtime reader registration, and a differential validator that checks panduck against a
real pandoc on every CI run.

RST, Org and MediaWiki are **declared but not implemented**, and the registry knows
the difference: a format with `status='planned'` has a NULL reader and is skipped, so
dispatch can never route to a function that doesn't exist.

## The function surface

**Reading**

| Function | Shape |
|---|---|
| `read_panduck_doc(src, format := 'auto', pages := '')` | table of `duck_block` rows |
| `read_panduck_table(src)` | table of rows and columns |
| `panduck_read_blocks(src, …)` | `LIST(duck_block)` |
| `read_rtf_blocks(path)`, `read_docx_blocks(path)` | one format, directly |

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
