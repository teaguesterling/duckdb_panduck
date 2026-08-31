# panduck

Native, in-process document conversion and AST extraction for DuckDB.

`panduck` reads rich document formats — DOCX, EPUB, ODT, LaTeX, reStructuredText — directly
into the unified [`duck_block`](https://github.com/teaguesterling/duckdb_duck_block_utils)
document AST, with no external binary on `$PATH` and no subprocess per file.

> **Status: Phase 1 (scaffolding).** The build, the dependency chain, and the Pandoc AST
> contract are in place and tested. **No format readers are implemented yet** — see
> [Roadmap](#roadmap).

## Why not just call pandoc?

Because pandoc is a Haskell program, and there is no practical way to call it in-process:

- **[ShabbyX/libpandoc](https://github.com/ShabbyX/libpandoc)**, the only real C bindings,
  has been unmaintained since **2017** and targets pandoc 1.x. Pandoc is now 3.x.
- **Upstream has not shipped a C shared library.** [jgm/pandoc#6611](https://github.com/jgm/pandoc/issues/6611)
  has been open since 2020; as of the most recent comment, nothing has changed.
- Even if one existed, linking the **GHC runtime** — its own GC, signal handlers, and
  `hs_init`/`hs_exit` lifecycle — into a `dlopen`'d extension inside DuckDB's already
  multithreaded process would be a bad neighbour. And the ~200 MB doesn't disappear; it
  moves from `$PATH` into the extension binary, which then can't ship as a community
  extension.

That leaves the CLI subprocess (~50–200 ms of fork/exec per file, plus temp-file I/O, with
no vectorization or pushdown) or an HTTP server. Panduck does neither.

**Panduck is compatible with pandoc's data model, not its ABI.** Its readers emit
`duck_block` elements, and `duck_block_utils` already round-trips those to and from Pandoc
JSON — so anything that speaks Pandoc JSON still interoperates, with no Haskell anywhere.

That claim is *tested*, not asserted: see [Pandoc AST alignment](#pandoc-ast-alignment).

## Current SQL surface

```sql
LOAD panduck;

SELECT panduck_version();              -- extension version
SELECT panduck_pandoc_api_version();   -- pandoc-types AST version targeted: 1.23

-- The full pandoc-types 1.23 vocabulary and its duck_block correspondence
SELECT * FROM panduck_pandoc_ast_map();
```

```
┌────────────────┬─────────┬──────────────┬─────────┬─────────────────────────────────┐
│  pandoc_type   │  kind   │ element_type │ status  │              notes              │
├────────────────┼─────────┼──────────────┼─────────┼─────────────────────────────────┤
│ Header         │ block   │ heading      │ mapped  │ heading_level 1-6 in attributes │
│ CodeBlock      │ block   │ code         │ mapped  │ language from first Attr class  │
│ Underline      │ inline  │ underline    │ planned │ never matched in convert; …     │
│ Null           │ block   │ NULL         │ dropped │ intentionally yields no element │
└────────────────┴─────────┴──────────────┴─────────┴─────────────────────────────────┘
```

`status` is one of:

| status | meaning |
|---|---|
| `mapped` | round-trip implemented in `duck_block_utils` today |
| `planned` | named in the spec, not implemented on either side yet |
| `dropped` | intentionally yields no element (`Null` only) |

## Pandoc AST alignment

Panduck doesn't link pandoc, so nothing in the build would notice if pandoc changed its AST
underneath us. `test/pandoc/check_pandoc_alignment.py` closes that gap:

```sh
make test_pandoc_alignment
```

```
Checking panduck's AST mapping against pandoc 3.1.3
  api-version 1.23 matches target
  all 34 emitted constructors are present in the mapping
  4 known gap(s) unchanged: DefinitionList, Figure, LineBlock, Underline
  fixture exercises 34/35 mapped constructors
OK: panduck's Pandoc AST mapping is aligned with the installed pandoc.
```

It runs a real pandoc over `test/pandoc/fixtures/kitchen_sink.md` — which exercises 34 of
the 35 Block/Inline constructors in pandoc-types 1.23 — and fails on three kinds of drift:

1. pandoc emits a Block/Inline constructor absent from `src/pandoc_ast_map.cpp`
2. the `pandoc-api-version` no longer matches what the mapping targets
3. the set of unimplemented constructors changed without `KNOWN_GAPS` being updated

It **skips cleanly** (exit 0) when pandoc isn't installed, so it never blocks a build. CI
installs pandoc explicitly so the check actually runs there.

The complementary SQL test (`test/sql/pandoc_ast_map.test`) asserts the *built extension*
agrees with that same table. Together: real pandoc → C++ table → loaded extension.

### Known gaps

These are constructors real pandoc emits that the `duck_block` round-trip does not handle.
All four are recorded as `status='planned'` and tracked by the harness:

| Constructor | Issue |
|---|---|
| `LineBlock`, `DefinitionList`, `Figure` | `duck_block_utils` `docs/pandoc_ast_spec.md` maps these to `pandoc:lineblock` / `pandoc:deflist` / `pandoc:figure`, but no code path handles them — `pandoc_block_convert.cpp` ends its chain with a bare `else { return; }`, so they are **silently dropped**. |
| `Underline` | `block_types.hpp` defines `INLINE_UNDERLINE = "underline"`, but `pandoc_inline_convert.cpp` never matches it, so it falls through to `text` with the literal content `"[Underline]"`. |

## Building

Dependencies come from vcpkg (`pugixml` for XML, `miniz` for ZIP containers):

```sh
git clone https://github.com/Microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh
export VCPKG_TOOLCHAIN_PATH=`pwd`/vcpkg/scripts/buildsystems/vcpkg.cmake

git clone --recurse-submodules https://github.com/teaguesterling/duckdb_panduck.git
cd duckdb_panduck
make release
```

Targets DuckDB **v1.5.5**. Produces:

```
./build/release/duckdb
./build/release/test/unittest
./build/release/extension/panduck/panduck.duckdb_extension
```

Run the tests with `make test`, and the pandoc conformance check with
`make test_pandoc_alignment`.

## Roadmap

| Phase | Scope | Status |
|---|---|---|
| 1 | Scaffolding, vcpkg dependency chain, `duck_block` contract, Pandoc AST alignment harness | **done** |
| 2 | `read_docx_blocks()` — ZIP + `word/document.xml` via miniz + pugixml | not started |
| 3 | `read_epub_blocks()` — `container.xml` → `.opf` spine, `toc.ncx` / `nav.xhtml` | not started |
| 4 | `read_latex_blocks()` — streaming tokenizer for macros, environments, math | not started |
| 5 | `read_rst_blocks()`, and the `doc_to_blocks()` hook in `duck_block_utils` | not started |

## Related

Part of a family of DuckDB document extensions that all emit `duck_block`:

- [`duckdb_duck_block_utils`](https://github.com/teaguesterling/duckdb_duck_block_utils) — the unified AST and its Pandoc JSON bridge
- [`duckdb_markdown`](https://github.com/teaguesterling/duckdb_markdown) — CommonMark + GFM
- [`duckdb_webbed`](https://github.com/teaguesterling/duckdb_webbed) — XML and HTML
- [`sitting_duck`](https://github.com/teaguesterling/sitting_duck) — source code ASTs via tree-sitter

## License

MIT — see [LICENSE](LICENSE).
