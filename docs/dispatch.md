# Dispatch

Two functions take a path and route it to whatever reads that format.

```sql
SELECT * FROM read_panduck_doc('report.docx');    -- duck_block rows
SELECT * FROM read_panduck_table('data.parquet'); -- rows and columns
SELECT panduck_read_blocks('report.docx');        -- LIST(duck_block)
```

## Why a table function

`read_panduck_doc` is a table function so a predicate reaches the reader. Measured on the
two shapes:

```
  table function                    scalar returning LIST
    PROJECTION                        HASH_GROUP_BY   <- a full optimisation barrier
      FILTER (element_type=…)           PROJECTION
        READ_MARKDOWN_BLOCKS              PROJECTION struct_pack(…)
                                            READ_MARKDOWN_BLOCKS
```

The scalar form materialises the whole document into one value before anything downstream
runs, and no predicate can push below the aggregate. Fine for a README; not for a 400-page
EPUB.

`panduck_read_blocks` exists for the `db_*` utilities that take a list, and orders
explicitly by `element_order` — section slicing depends on position, so the order is
guaranteed rather than incidental.

## The registry

```sql
SELECT ext, format, reader_ext, function, kind, source FROM panduck_reader_registry();
```

| column | meaning |
|---|---|
| `ext` | lowercase, dot-prefixed (`.rtf`) |
| `format` | format name; `data` means "not a document" |
| `reader_ext` | the DuckDB extension that reads it |
| `function` | the table function to call, or NULL for a built-in branch |
| `kind` | `doc` or `table` |
| `source` | `builtin` or `user` |

panduck's own formats are **derived** from `panduck_supported_extensions()` — a format
with `status='planned'` has a NULL reader and is skipped, so dispatch can never route to a
function that doesn't exist. Everything else in the registry is a claim for a format whose
extension cannot describe itself.

### A URL scheme is an extension, for registry purposes

Two registry keys are not dot-suffixes:

| key | format | what it is |
|---|---|---|
| `zim://` | `zim_article` | `zim://wiki.zim/A/Article` — ONE article, a document |
| `.zim` | `zim` | the whole archive — a corpus, and a **refusal** |

`zim://wiki.zim/A/Article` has no meaningful extension. Its dispatchable identity lives in
the *scheme*, exactly where `.docx` keeps its own, so the scheme is the key and
`panduck_format_for` matches it as one. A registry invariant asserting every key begins
with `.` fired on this and was widened to "a dot-suffix OR a `://` scheme" — the invariant
was right to fire and the rule was the thing that was too narrow.

The two keys pointing at one archive format is the substantive part. A `.zim` is thousands
of articles; "read it as a document" has no answer, so panduck **raises and names the zim
extension** rather than picking an article or concatenating them. An article resolves
through `zim_get_content` to HTML and then reads like any other HTML. Losing the corpus is
a gap; inventing a document out of one is a defect.

### `.json` does NOT route to the Pandoc AST reader

`json` is one of pandoc's own formats and it *is* the Pandoc AST, so
`read_pandoc_blocks` reaches every format pandoc can read. But `.json` is already claimed
as **data**, and dispatch cannot tell an AST from a config file by extension — the
overwhelming majority of `.json` in the world is data.

So `pandoc` is reachable by `format := 'pandoc'` and by calling the reader directly, never
by extension. Auto-routing would change what every existing `.json` read returns to serve
a rare case, which is the wrong trade in the direction that silently alters a working
query.

Lookups are pure string work with no I/O, so they answer for files that don't exist:

```sql
SELECT panduck_format_for('a.docx');   -- docx
SELECT panduck_can_read('a.nope');     -- false
SELECT panduck_supported_paths();      -- ['.arrow', '.csv', …]
```

`panduck_format_for` returns **NULL** for an unclaimed extension rather than guessing. If
it answered `code` for everything, `panduck_can_read` would be true for every path and
would mean nothing.

## Registering your own reader

```sql
CALL panduck_register_table_reader('spatial', 'st_read', ['.shp', 'geojson']);
CALL panduck_register_doc_reader('myext', 'read_my_blocks', ['.mine']);
```

Extensions normalise, so `.RTF`, `rtf` and `.rtf` are one entry. Registration **replaces**
rather than appends, so *one reader per extension* holds by construction and survives
customisation — a user entry overrides a built-in rather than competing with it.

A registered reader dispatches through the same generic branch as the built-in flat
readers: `read_panduck_doc` contains no branch naming `rtf` or `docx`. It must emit the
canonical `duck_block` schema.

Registrations are process-wide and last for the session.

## Errors name what is wrong

```sql
SELECT * FROM read_panduck_doc('data.csv');
--  panduck: data.csv is a data format, not a document. Use read_panduck_table instead.
```

The code fallback is guarded on `panduck_format_for(src) IS NULL` — genuinely unclaimed —
rather than a bare `ELSE`. A catch-all would swallow any format the registry *claims* but
dispatch has no branch for, routing it to `sitting_duck` and returning a parse tree instead
of a document: **silently wrong rather than loudly missing.** A format in that state raises
and says it is a panduck bug.

## A limitation worth knowing

Autoloading mid-statement is governed by two effects that compose:

| | direct call | inside `query()` |
|---|---|---|
| C++ function | fails | **works** |
| catalog macro | fails | fails |

A statement binds fully before it executes, so anything it names directly must already
exist — which is why every dispatch branch goes through `query()`, whose inner SQL binds at
execution. But a **macro** registered into the catalog at load time is still invisible to
the statement that triggered the load.

`sitting_duck`'s `ast_to_blocks` is a macro, so the first source-code read in a fresh
session fails and the second succeeds. `LOAD sitting_duck` once, or call twice. Eagerly
loading it whenever panduck loads would fix this at the cost of pulling tree-sitter into
every `.rtf` read.
