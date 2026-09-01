# The Pandoc AST Converter Handoff

**Status:** proposed
**Scope:** relocate `pandoc_ast_to_blocks` and its siblings from `duckdb_duck_block_utils`
into panduck, with the regression net that proves them.

## Why this is the largest prize, not the last chore

`json` is one of pandoc's own input and output formats, and it *is* the Pandoc AST. So a
reader for it is not another format alongside the seven panduck already has:

```
pandoc -f <anything> -t json   →   pandoc_ast_to_blocks(...)   →   duck_blocks
```

Pandoc reads **42 input formats**. panduck implements seven. This converter makes all 42
reachable with no per-format code, for anyone who has pandoc installed, while the native
readers keep serving people who do not. That is what "compatible with pandoc's data model,
not its ABI" cashes out to, and it means **the converter is the thing that makes the format
list finite** rather than the last item on it.

It also relocates the code to the repo whose subject it is. duck_block_utils owns a
vocabulary; converting Pandoc's AST is document IO, which is panduck's whole job.

## What moves

| | lines |
|---|---|
| `src/pandoc_block_convert.cpp` | 2,819 |
| `src/pandoc_inline_convert.cpp` | 845 |
| headers (`pandoc_block_convert`, `pandoc_inline_convert`, `pandoc_convert_util`) | ~130 |
| `NormalizeFunctions::CollapseLonePlainIntoParent` — one function of `normalize.cpp` | ~90 |

Measured 2026-09-01, after upstream removed the dead `inline_builders.hpp` include I
reported — that file does **not** travel, and neither do the ~1,289 lines behind it.

Seven public functions, already complete:

```
pandoc_ast_to_blocks(json)       LIST(duck_block)   the AST as a string
read_pandoc_ast(path)            LIST(duck_block)   the AST from a file
duck_blocks_to_pandoc_ast(blk)   STRUCT(...)        export
write_pandoc_ast(blk, path)      BOOLEAN
pandoc_inlines_to_db_inlines / pandoc_inlines_to_text / duck_blocks_to_pandoc_blocks
```

## Two things the port must DECIDE, not inherit

### 1. These are scalars; every panduck reader is a table function

`read_pandoc_ast` returns `LIST(duck_block)` from a scalar. Every panduck reader —
`read_latex_blocks`, `read_org_blocks`, `read_rst_blocks` and the rest — is a **table
function** returning rows.

That is not cosmetic. panduck's own README states the reason: *"`read_panduck_doc` is a
table function, so a filter pushes down to the reader. The scalar `LIST` form plants a
blocking aggregate that no predicate can pass, which is fine for a README and not for a
400-page EPUB."*

So the port **adds** `read_pandoc_blocks(path)` and `read_pandoc_blocks_string(src)` as
table functions, matching the convention every other reader follows, and keeps the scalar
forms as the compatibility surface duck_block_utils' consumers already call. Two shapes
for one reader is the pattern panduck already documents; a scalar-only reader would be the
one format that cannot be filtered.

### 2. `.json` must NOT auto-route to this reader

`.json` is already claimed by the registry as a **data** format, routed to
`read_panduck_table`. A Pandoc AST file is also `.json`, and **dispatch cannot tell them
apart by extension** — the overwhelming majority of `.json` files in the world are data,
not Pandoc ASTs.

So the extension stays mapped to `data`. The AST reader is reached by calling it directly,
or by `read_panduck_doc(src, format := 'pandoc')`. Auto-routing would break every existing
`.json` read to satisfy a rare case, which is the wrong trade in the direction that
silently changes what a working query returns.

## Fix before moving

duck_block_utils' standing rule for this relocation, and it is the right one: relocating
buggy code relocates the bugs. The regression net is built and green FIRST, and the code
moves under it.

What must travel intact rather than be rewritten here — their list, and they are making it
a deliverable rather than an assumption:

- the 34-constructor alignment ledger, with its two expiry audits
- the containment sweep across five containers with two block children each
- the build-driven container sweep over all 43 types
- the four-arm roundtrip sweep with its three exemption registries and their expiry audits

`test/check_roundtrip_sweep.py` and `test/check_spec_alignment.py` are the two that are
about the CONVERTER. The others in that directory — `check_vendorable`,
`check_consumer_alignment`, `check_vocabulary_header`, `check_constants_are_used` — are
about the vocabulary and stay where they are.

These sweeps were built against a converter serving one consumer. Under the framing above
they protect every format pandoc can read, so they get **stricter** on arrival, not looser.

## Sequencing

1. The net moves first and is green in panduck against the code still living upstream.
2. The code moves under it.
3. `panduck_supported_extensions()` gains `pandoc`, and `read_panduck_doc(format :=
   'pandoc')` routes to it — dispatch by explicit format only, per the decision above.
4. duck_block_utils removes its copy only once panduck's net is green, so there is never a
   window where neither repo owns it.

Step 4 is the one that must not be reordered: the two-copy window is safe and the
zero-copy window is not.

## What this does NOT do

It does not make pandoc a dependency. panduck still reads seven formats with no external
binary; this adds an eighth path for users who already have pandoc and want the other
thirty-five formats. The README's argument against subprocess-per-file stands unchanged —
a user piping `pandoc -t json` has made that choice explicitly, once, rather than having it
made for them per document.
