# libpanduck — the format engine, with no DuckDB in it

panduck's readers are hand-written because no `libpandoc` exists: pandoc is a
Haskell library with no C ABI, so "just link pandoc" was never on the table.
That accident of history produced something unexpectedly valuable — ~10,500
lines of C++ that parse eleven document formats into one vocabulary, and which
depend on DuckDB far less than their location in `src/` suggested.

`libpanduck/` is that code, moved to where it belongs. Issue #104 tracks the
work; this page is the contract and the procedure.

## The measurement that justifies the seam

Across the eleven readers, **~195 of ~10,500 lines name a DuckDB type — about
2%**. Every DuckDB API break this project has absorbed lived in that 2%:

| break | what it actually touched |
|---|---|
| `SimpleNamedParameterFunction` deleted in v2.0 (#101) | two call sites in `reader_registry.cpp` |
| `DataChunk::SetValue` deprecated (#102) | 136 scan sites, all in table-function tails |
| `ExecuteWithNulls` (in a sibling extension) | its binder, not its parser |

None of them touched a parser. That is the whole argument: **code behind the
seam cannot break on a DuckDB API change.** The seam is therefore a
*mitigation* for the v2.0 migration rather than a competitor for the same time.

## What the boundary is, and what enforces it

Nothing under `libpanduck/` may include a DuckDB header or name a DuckDB
symbol. Three mechanisms enforce that, and they are not redundant — each has
caught a real defect the other two could not see:

| mechanism | proves | cannot see |
|---|---|---|
| `scripts/check_libpanduck_seam.py` | no DuckDB **name** appears | a type arriving transitively |
| `cmake -S libpanduck` (standalone project) | the engine **compiles** with no duckdb on the include path | whether the output is correct |
| `libpanduck/test/` via `ctest` | the engine **works** without DuckDB | — |

The middle one is why the standalone CMake project exists at all. panduck's own
`CMakeLists.txt` applies DuckDB's include paths **directory-wide**, so a child
target declared there inherits them and a stray `#include "duckdb.hpp"` under
`libpanduck/` would compile perfectly. The compiler cannot be the guard in that
build. Configuring the standalone project is real enforcement; CI runs it as the
`libpanduck-seam` job.

The third closes a gap that stayed open through the first several moves. "The
engine compiles without DuckDB" is a much weaker claim than "the engine produces
correct rows without DuckDB" — and for a while every assertion about `ReadOrg`
ran through SQL, i.e. through DuckDB. A consumer who linked libpanduck and
called `ReadOrg("* Heading")` was exercising a path no test covered.

## The module contract

Every format module exposes the same two entry points:

```cpp
std::vector<XBlock> ParseXString(const std::string &src);  // format-shaped intermediate
std::vector<Block>  ReadX(const std::string &src);         // duck_block rows
```

`ReadX` is the one that matters: **source in, vocabulary rows out.** In most
readers the flattening originally sat inside the DuckDB `Bind` function, which
is where it accidentally ended up rather than where it belonged — it names no
DuckDB type, only vocabulary constants. Moving it is what lets a non-DuckDB
consumer get rows at all.

`panduck::Block` is the engine's currency: one duck_block row, field-for-field
in the vocabulary's column order. Before milestone L1 there were **eleven**
private copies of that struct (`IpynbRow`, `OrgRow`, `RstRow`, `MwRow`,
`BlockRow`, …), each inside a reader's DuckDB half, none reachable from outside.

`Block::content` is a `NullableString`, because an empty document and an absent
field are different facts: an empty document is a `Doc` row plus a `Text` row
whose content is the empty string, while NULL means absent. The emission helper
currently preserves the older behaviour exactly — `HasContent()` is "present
AND non-empty", so absent and empty both still become SQL NULL. Changing any
reader to emit a real empty string is a separate, per-reader change with its own
test.

### Modules are compile-time options

A composition may carry fewer modules than the engine supports. The DuckDB
extension delegates markdown, HTML and PDF to sibling extensions rather than
building them; a standalone consumer would want them built in. **The provider is
a property of the composition, not of the format** — the sitting_duck
sub-extension model — and that is what makes "libpanduck supports more formats
than panduck exposes" coherent rather than contradictory.

## What is behind the seam today

| module | reader before | DuckDB tail now | option | standalone default |
|---|---|---|---|---|
| ipynb | 453 | **128** | `PANDUCK_WITH_IPYNB` | off — needs a standalone yyjson |
| textile | 708 | **118** | `PANDUCK_WITH_TEXTILE` | on |
| org | 672 | **123** | `PANDUCK_WITH_ORG` | on |
| rst | 996 | **125** | `PANDUCK_WITH_RST` | on |
| mediawiki | 911 | **124** | `PANDUCK_WITH_MEDIAWIKI` | on |

Four companion scanners moved whole (textile 290, org 257, rst 232, mediawiki
378 lines), along with the shared helpers `block_json.hpp` and `slugify.hpp`.

**That every tail landed between 118 and 128 lines is the useful number here.**
Five readers written at different times, by different hands, over different
formats, each reduce to the same ~120 lines of DuckDB: a bind pair, a scan, a
column list and a registration. The seam is a real structural boundary that was
already there, not a line fitted per reader.

Not yet moved:

- **latex** (1,780 + a 465-line tokenizer + a 217-line macro table) — the
  tokenizer has a table function at the bottom, so it splits rather than moves.
- **docx, epub, odt, rtf** — their flatten must first be extracted from their
  `Bind` functions.
- **pandoc** (177, plus a 3,522-line DuckDB-saturated converter) — needs #107
  and the compatibility-mode decision first.

## Moving a reader behind the seam

1. Move the companion scanner, if there is one — usually a whole-file `git mv`
   plus a namespace swap.
2. Create `libpanduck/include/panduck/<fmt>.hpp` with the format structs,
   `ParseXString` and `ReadX`; move the core and the flatten into
   `libpanduck/src/<fmt>.cpp`.
3. Turn `src/include/<fmt>_reader.hpp` into a shim re-exporting into
   `namespace duckdb`, so no call site outside changes.
4. Shrink `src/<fmt>_reader.cpp` to the DuckDB tail; delete the private row
   struct.
5. Wire both `CMakeLists.txt` files and add a `HasModule()` branch.
6. Gates in this order: seam guard → standalone configure → standalone build →
   `make format-fix && format-check` → `make release` → `make test_release`.
   Formatting **before** the build, because `format-fix` rewrites every source's
   mtime and would invalidate the build you were about to cite.

### The traps

Every one of these was hit for real. Each was caught by a different mechanism,
and **three of the four would have shipped silently.**

**1. The `encoding` default — silent.** Each reader's private row struct
defaulted `encoding` to `ENCODING_TEXT`, and its flatten overrode that only when
the block carried one; inline children frequently never assigned it at all.
`panduck::Block` defaults `encoding` to the **empty string**. So a faithful
copy-paste of the loop blanks the encoding on every row and every child: no
compile error, and no failing test.

Hit on ipynb, predicted for textile and confirmed, then confirmed again on org,
rst and mediawiki — five for five. Assign it explicitly, and preserve what *that
reader's* loop did. They are **not** all the same: mediawiki's `MwInline` has its
own `encoding` field (`'mediawiki'` for an inline raw template) and its child
loop overrode the default conditionally, so pasting org's unconditional
`ENCODING_TEXT` there would have silently rewritten every inline raw template to
`'text'` — and would have read in review as "the same fix as the other three".

The same class of trap, one field over: rst assigns `kind` **unconditionally**,
because `RstBlock` has no `kind` field at all (RST has no document metadata — a
field list is a definition list, measured against pandoc). Copying another
reader's `block.kind.empty() ? ...` would have named a field that does not
exist.

**2. A shim missing a symbol — loud.** The first textile scanner shim
re-exported the structs and functions but not `enum class LineKind`, because the
grep used to enumerate the header's surface matched only `^std::|^struct`. 26
build errors. Enumerate with a pattern covering
**`struct|class|enum|using|constexpr`** plus function signatures. The counts
differ per reader for real reasons: mediawiki's scanner shim needs four
`using`-declarations (`SplitCells` as well) where org's and rst's need three.

**3. Transitive includes — only the standalone build sees it.** textile's core
called `strlen`/`strchr` and the original file never included `<cstring>`; it
arrived through `duckdb.hpp`. This is invisible to the extension build **and** to
the seam script, which only sees *named* DuckDB types. Only compiling with
DuckDB off the include path surfaces unnamed dependence on its include graph.

A caveat worth recording: on the Linux/libstdc++ toolchain used here,
`<cctype>`, `<cstddef>` and `<utility>` arrive transitively behind `<string>`
and `<vector>`, so later moves added them without being forced to. They are
named anyway — a file's includes should promise what it uses — but "not strictly
required" is a statement about *this* standard library. A future claim that
libpanduck is portable should be measured against a second one.

**4. `panduck::` binding to the wrong namespace — loud but confusing.** Inside
`namespace duckdb`, a bare `panduck::` resolves to `duckdb::panduck`, the compat
helpers in `panduck_duckdb_compat.hpp`, not to the library. Always write
`::panduck::`. One header compiled only because of include order before this was
understood. (`panduck::BindNames` in a bind signature is correct as written —
that one really is the compat alias.)

**And a meta-trap: the stale-artifact false green.** Six times in this project a
gate reported `TEST_EXIT=0` while the build had actually failed, so the suite ran
the *previous* binary; twice, `format-fix` modified sources after the build being
cited. Capture `$?` on every gate — never `| grep -c error` as the success test —
and compare the artifact's mtime against every changed source before citing a
green run.

## The two known gaps

The seam script prints these on every clean run, so "seam holds" is never read as
more than it is. Both are dependencies on DuckDB's *build*, not its API —
nothing behind the seam would break on a DuckDB API change, which is the property
the seam exists to buy — but a standalone build has to answer both.

**1. The vocabulary's namespace.** `duck_block_vocabulary.hpp` is a vendored
byte-exact copy of duck_block_utils' header, and it declares everything inside
`namespace duckdb`. `panduck/vocabulary.hpp` aliases
`::duckdb::DuckBlockVocabulary` once so the spelling does not spread, and the
seam script masks **that one site** rather than the file — a file-wide exemption
would take the real leaks with it. A vocabulary intended for consumers outside
DuckDB would be more useful in a neutral namespace, or offered in both; that is
upstream's call, since the copy here must stay byte-exact.

**2. yyjson.** DuckDB's vendored copy is not reusable outside DuckDB — its
header includes `duckdb/common/fast_mem.hpp`, so pointing at it configures fine
and then fails to compile. libpanduck needs its own, currently a `vcpkg.json`
entry, and that entry costs a dependency on every platform build (Wasm included)
for a target only the standalone configure uses. To resolve: either drop it once
libpanduck links its own copy another way, or make it conditional so the
extension build keeps using DuckDB's. Until then `PANDUCK_WITH_IPYNB` defaults
off in the standalone build.

## Why the repo has not split

**Split-after-stability**, taken from sitting_duck: the split happens only once
the contract has stopped moving (measured as no API-breaking change for a full
milestone). Splitting earlier makes every interface iteration a multi-repo dance
during exactly the phase when the interface churns most. That is milestone L3.

An in-tree seam publishes nothing, which is why L1 and L2 could proceed during
the v2.0 migration instead of waiting for it. See [the roadmap](roadmap.md) for
L1–L4 and how they sequence against the other phases.
