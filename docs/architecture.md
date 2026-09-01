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

A submodule pin is also just a copy with a SHA attached — it sits at whatever you checked
out and never tracks `main` — so the choice was never "copy versus not a copy". It was
which kind of copy, and the submodule form additionally drags a ~290 MB nested DuckDB
clone through the extension CI templates' `submodules: 'recursive'` checkout to deliver
12 KB.

**The obvious objection, answered honestly.** The portfolio has already watched copies
diverge — three separate implementations of the same Pandoc inline walker drifted into
three different bug sets. That is a real scar and it is the reason this section exists.
Those were copies of *logic*, which diverge by being edited on purpose: each fork grew
fixes the others never got, and no diff could tell you which of the three was right. A
copy of *constants* has no such freedom.

**But "any divergence is a defect" is true of the values, not of the bytes**, and the
difference is the whole design of the check. A vendored copy can sit several commits
behind upstream and still be exactly correct: upstream rewrote every `idx_t` to `uint64_t`
and later added ~88 lines of vendoring guidance without moving a single constant name or
value. Hundreds of changed bytes, zero changed vocabulary. Reach for a byte comparison and
it fires on all of that, gets muted within a week, and catches nothing on the day a value
actually moves.

## What the compiler does not catch

The constants protect against a **rename**, and only a rename:

| Change | Compiler | Effect |
|---|---|---|
| `TYPE_HEADING` → `TYPE_HEAD` | error at every use site | caught immediately |
| `TYPE_PAGE = "page_break"` → `"pagebreak"` | **compiles clean** | readers silently emit a type no consumer matches |

A value change survives the build, survives every test written against its own string
literals, and shows up as documents that quietly stop rendering correctly. This is a
property of constants, not of how the header arrives — vendoring and submoduling are
equally blind to it.

So the copy comes with `make check-vocabulary`
(`scripts/check_duck_block_vocabulary.py`), which fetches the published header and
compares **by name and value**. It reports three things separately, because they call for
different responses: `DRIFT` (renamed, removed, or value changed) fails the check; `NEW`
(published upstream, missing here) means re-sync; `GAPS` (published, but nothing in
panduck branches on it) means a type can only reach a fallthrough.

`GAPS` is the arm that earns its keep — the equivalent check found inline `generic`
silently dropping `source_type` in `duckdb_markdown` and in `duck_block_utils`
independently in the same week. Intentional gaps are allowlisted **with reasons**, because
an unexplained gap and a deliberate one otherwise look identical.

The printed counts are context, not the assertion. A pure rename leaves the count
unchanged while breaking every consumer; `duck_block_utils` had a check that printed
"42 vs 42" one line above a genuine failure. `--self-test` verifies this checker
classifies a rename, a value change and cosmetic churn correctly with the count held
constant.

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
