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
| `panduck` | `duck_block_utils`' vocabulary header | build time, vendored copy |
| `panduck` | format extensions | on demand, per format read |
| `panduck` | `duck_block_utils` | on demand, **`doc_*` only** |

The `doc_*` dependency is deliberately asymmetric with what it replaced. When
`duck_block_utils` owned path dispatch, `doc_toc` **could not work** without a reader —
structural. panduck's `doc_*` is sugar: every reader, the registry and both dispatchers
work with `duck_block_utils` absent. Only the convenience wrappers need it, and they name
it in the error when it is missing.

## The vocabulary has exactly one definition, and it is vendored

`duck_block_utils` publishes `duck_block_vocabulary.hpp` link-free. panduck inherits from
it:

```cpp
class DuckBlockTypes : public DuckBlockVocabulary { … };
```

There is exactly one definition of every `element_type` name in a panduck build — zero
local. Both readers use `DuckBlockTypes::TYPE_HEADING` rather than `"heading"`, so a typo
is a compile error rather than a structurally valid block that no consumer handles and
every test passes. That property is what matters, and it is unchanged by where the header
comes from.

It arrives as a **vendored byte-for-byte copy** at `src/include/duck_block_vocabulary.hpp`.
This was a submodule until `duck_block_utils` decided the other way: a whole submodule
checkout to place one 167-line constants header on the include path is a large mechanism
for a small dependency.

**The obvious objection, answered honestly.** The portfolio has already watched copies
diverge — three separate implementations of the same Pandoc inline walker drifted into
three different bug sets. That is a real scar and it is the reason this section exists.
The distinction is that those were copies of *logic*, which diverge by being edited on
purpose: each fork grew fixes the others never got, and no diff could tell you which of
the three was right. This is a copy of *constants*, kept byte-identical on purpose, where
any divergence at all is a defect and `diff` names it in one line.

So the risk does not vanish, it changes shape: it is no longer "three implementations
disagree" but "someone edits the local copy and nothing says so." That is the failure mode
to watch, which is why the file is marked do-not-edit at its point of use and the re-sync
command lives in `duck_block_types.hpp` next to the upstream SHA it was taken from.

A submodule would not have removed this risk either — it only moves it from "edited
without saying so" to "never synced", which is why `duck_block_utils` recommends asserting
agreement at *test* time regardless of how the header arrives.

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
