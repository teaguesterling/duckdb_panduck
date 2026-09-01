# Architecture

panduck is the **IO engine** of a family of DuckDB extensions that all speak one document
vocabulary. The layering has one rule: **nothing depends upward.**

```
  duck_block_utils        helpers over the vocabulary, and the vocabulary itself
                          depends on NOTHING (core `json` aside)
        ▲
        │ consumed at build time (the vocabulary header)
        │ loaded on demand      (the doc_* sugar only)
        │
     panduck              the IO engine: path -> blocks, path -> rows
        │
        │ loads on demand
        ▼
  duckdb_markdown  duckdb_webbed  pdf  sitting_duck  toml  yaml
                          each reads and writes its own format
```

## Why this shape

Every extension in the family emits `duck_block`, so **format extensions compose directly**
without anything in the middle. That is the property the layering exists to protect, and it
is checkable:

```sql
-- HTML to markdown, with neither panduck nor duck_block_utils loaded
LOAD webbed; LOAD markdown;
SELECT duck_blocks_to_md(html_to_duck_blocks(content)) FROM read_text('page.html');
--  # Title
--  Some **bold** text.

-- and blocks built by hand, with only duck_block_utils
LOAD duck_block_utils;
SELECT db_blocks_to_text(db_paragraph([db_text('hello '), db_bold('world')]));
--  hello world
```

If either of those stops working, the layering has been violated.

## What depends on what

| Extension | Depends on | When |
|---|---|---|
| `duck_block_utils` | nothing (core `json` for encoding utilities) | — |
| `panduck` | `duck_block_utils`' vocabulary header | build time, via submodule |
| `panduck` | format extensions | on demand, per format read |
| `panduck` | `duck_block_utils` | on demand, **`doc_*` only** |

The `doc_*` dependency is deliberately asymmetric with what it replaced. When
`duck_block_utils` owned path dispatch, `doc_toc` **could not work** without a reader —
structural. panduck's `doc_*` is sugar: every reader, the registry and both dispatchers
work with `duck_block_utils` absent. Only the convenience wrappers need it, and they name
it in the error when it is missing.

## The vocabulary is shared, not copied

`duck_block_utils` publishes `duck_block_vocabulary.hpp` link-free, specifically so siblings
consume it via submodule. panduck does:

```cpp
class DuckBlockTypes : public DuckBlockVocabulary { … };
```

There is exactly one definition of every `element_type` name in a panduck build — zero
local, one from the submodule. Both readers use `DuckBlockTypes::TYPE_HEADING` rather than
`"heading"`, so a typo is a compile error rather than a structurally valid block that no
consumer handles and every test passes.

This matters because the portfolio has already watched copies diverge: three separate
implementations of the same Pandoc inline walker drifted into three different bug sets.

## Derived, not maintained

Two registries drive dispatch, and neither is a hand-kept table:

- **`panduck_supported_extensions()`** — panduck's self-description. What it reads, what
  it plans to read, and which function does the reading.
- **`panduck_reader_registry()`** — derived from that, plus claims for formats whose
  extensions cannot describe themselves.

Adding a reader means flipping one row from `planned` to `implemented`. Dispatch picks it
up with **no change to dispatch code** — `read_panduck_doc` contains no branch naming
`rtf` or `docx`.

The exclusion rule falls out of this by construction rather than arithmetic. `sitting_duck`
genuinely claims `md`, `html`, `json`, `toml` and `css` as source code — `read_ast` on a
README yields thousands of tree-sitter nodes. Rather than enumerate its languages and
subtract, the rule is: **an extension in the registry routes to its reader; anything else
falls through to code.** `.md` is in the registry, so it can never reach the fallback.
