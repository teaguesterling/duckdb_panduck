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
| `cmake -S libpanduck` (standalone project) | the engine **compiles** with no duckdb on the include path, **at C++11** | whether the output is correct |
| `libpanduck/test/` via `ctest` | the engine **works** without DuckDB — 125 checks | behaviour the fixtures do not reach |

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

`Block::content` is a `NullableString` because the row type has to be *able* to
tell an absent field from a present-but-empty one, whichever way the vocabulary
ends up using that. This paragraph used to justify it differently — "an empty
document is a `Doc` row plus a `Text` row whose content is the empty string" —
and that was wrong twice: it was recorded as a ruling when it was a question
Teague asked, and the vocabulary has no `Doc` or `Text` `element_type` to say it
with. The emission helper preserves the older behaviour exactly — `HasContent()`
is "present AND non-empty", so absent and empty both still become SQL NULL.
Changing any reader to emit a real empty string waits on
[duck_block_utils#60](https://github.com/teaguesterling/duckdb_duck_block_utils/issues/60)
and would then be a separate, per-reader change with its own test.

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
| ipynb | 453 | **128** | `PANDUCK_WITH_IPYNB` | off — see the yyjson gap below |
| textile | 708 | **118** | `PANDUCK_WITH_TEXTILE` | on |
| org | 672 | **123** | `PANDUCK_WITH_ORG` | on |
| rst | 996 | **125** | `PANDUCK_WITH_RST` | on |
| mediawiki | 911 | **124** | `PANDUCK_WITH_MEDIAWIKI` | on |
| latex | 1,780 | **151** | `PANDUCK_WITH_LATEX` | on |

Four companion scanners moved whole (textile 290, org 257, rst 232, mediawiki
378 lines), along with latex's tokenizer and macro table and the shared helpers
`block_json.hpp` and `slugify.hpp`.

**That five of six tails landed between 118 and 128 lines is the useful number
here.** Readers written at different times, by different hands, over different
formats each reduce to the same ~120 lines of DuckDB: a bind pair, a scan, a
column list and a registration. The seam is a real structural boundary that was
already there, not a line fitted per reader.

latex's 151 is the exception and it prices one thing: latex reads through
DuckDB's `FileSystem` rather than `std::ifstream`, and that bind stays in the
tail. latex is also the only module that is **three files behind one option** —
the reader cannot work without its tokenizer or its macro table — and the only
one whose companion file had to be *split* rather than moved, because
`panduck_latex_tokens()` is a table function living at the bottom of an otherwise
DuckDB-free tokenizer.

Not yet moved:

- **docx, epub, odt, rtf** — their flatten must first be extracted from their
  `Bind` functions.
- **pandoc** (177, plus a 3,522-line DuckDB-saturated converter) — needs #107
  and the compatibility-mode decision first.

## The standalone test suite

`libpanduck/test/` is a dependency-free, C++11, assert-based runner wired into
`ctest` — no gtest, no Catch2, because adding a framework to prove libpanduck has
almost no dependencies would be self-defeating. **125 checks on the default configure** --
which is exactly what CI runs -- across five format modules plus an always-compiled
`engine` module covering `SpecVersion()` and `HasModule()`. ipynb's own test file
adds 22 more but CI never runs them -- see the yyjson gap below -- so that total is
not quoted here.

Each module asserts: it returns rows at all through the DuckDB-free entry point;
an exact row count for its fixture (so a silently dropped construct fails here
rather than passing a shape check on a shorter list); the vocabulary shape —
`kind` non-empty, **`encoding` non-empty**, `level >= 1`, `element_order`
sequential from 0; a heading's content, level and `heading_level`; an inline
child's content and encoding; the list type; and one format-specific fact. Every
expectation goes through `DuckBlockVocabulary::` constants rather than string
literals, so the tests track the vocabulary.

The runner prints a per-module and total check count and exits non-zero if it ran
*zero* checks. A runner that prints only "OK" is indistinguishable from one that
ran nothing, and this project has had six false greens of exactly that shape.

**The `encoding` check was verified by mutation, not by assertion.** A copy of
the tree outside the repo, with its own build directory, had
`libpanduck/src/org.cpp`'s `child.encoding = ENCODING_TEXT;` deleted — the exact
trap, in the exact row class it favours. Two independent checks fired, naming the
offending row and its contents, and the process exited non-zero.

### Open question: empty input

All six readers return **zero rows** for `""`, and for `"\n\n"`. This subsection
used to call that a *known divergence* from a recorded ruling — that an empty
document is a `Doc` row plus a `Text` row whose content is the empty string, with
NULL reserved for absent. There was no ruling: Teague asked that as a question on
2026-10-05 and it was written into `block.hpp` as his answer, in terms (`Doc`
row, `Text` row) that no `KIND_*` or `TYPE_*` constant in the vocabulary
provides. So there is nothing here to diverge from.

The question is open upstream, where the vocabulary is owned —
[duck_block_utils#60](https://github.com/teaguesterling/duckdb_duck_block_utils/issues/60),
where the empty-document case is question 4, since answering it needs a
document-level concept the vocabulary does not currently have. The tests assert
the *observed* behaviour with a comment, which was the right call for a different
reason than the one given: not "the ruling is not worth a failing test" but
"there is no ruling to assert".

Two things still make this one convention rather than six bugs: it is uniform
six-for-six, so the readers share a behaviour that predates the question; and it
is **user-visible, not an artifact of the new entry points** — the DuckDB tails
pass `ReadX`'s vector straight through, so `read_org_blocks_string('')` returns
no rows today (measured).

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

**5. C++11 aggregates — invisible to the extension build by construction.** In
C++11 a class with a brace-or-equal-initializer for a non-static data member is
**not an aggregate** (`[dcl.init.aggr]/1`; N3653 relaxed this for C++14). latex's
`Token` has `bool display_math = false;`, so all 22 `Token{kind, text, dm}`
brace-initialisation sites were ill-formed the moment the file moved behind a
C++11 seam. The extension compiles at C++17, where it is perfectly legal, so
*nothing in the extension build could ever see it*; the standalone configure
reported 22 errors.

This is a stronger justification for the standalone project than trap 3 was.
`<cstring>` was a header-graph accident; this is a **language-version**
difference, and only a build pinned to the engine's actual standard can find it.
The fix was a defaulted constructor plus a three-argument one reproducing the old
initialisation exactly — not dropping the `= false` to restore aggregate status,
which would have left `display_math` indeterminate at the two
default-construction sites: a real behaviour change dressed up as the smaller
edit.

**This is latent everywhere behind the seam.** `LatexInline`'s `int level = 2`,
`MwInline`'s and `RstBlock`'s defaults make those non-aggregates too; they simply
are not brace-initialised today. The standalone configure therefore has to stay
in CI permanently, not be run once per move — it is the only thing that knows
libpanduck is C++11.

**6. Vocabulary constants cannot be bound to a reference at C++11.**
`duck_block_vocabulary.hpp` declares `static constexpr const char *` members with
no out-of-line definition. Reading one is fine — an lvalue-to-rvalue conversion
in a constant expression does not odr-use the member, which is why every
`row.kind = DuckBlockVocabulary::KIND_BLOCK;` links. Binding one to a
**reference** does odr-use it, and yields `undefined reference to
duckdb::DuckBlockVocabulary::ENCODING_TEXT`. C++17 made such members implicitly
inline, which is why nothing upstream has hit it.

The vendored header must stay byte-exact, so the fix belongs at the call site:
the test harness's `Vocab()` takes `const char *` **by value**. Any standalone
C++11 consumer passing a vocabulary constant to a function taking
`const std::string &` will meet the same link error — worth raising with
duck_block_utils, whose call it is.

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

**2. yyjson — and ipynb cannot build standalone at all today.** DuckDB's
vendored copy is not reusable outside DuckDB: its header includes
`duckdb/common/fast_mem.hpp`, so pointing at it configures fine and then fails to
compile. But the gap is larger than a missing dependency, and three blockers
stack:

- `libpanduck/src/ipynb.cpp` includes `"yyjson.hpp"` — DuckDB's *wrapper* name.
  vcpkg ships `yyjson.h`.
- it does `using namespace duckdb_yyjson`, so it depends on DuckDB's
  **namespaced build** of the library, not merely on yyjson. A system yyjson puts
  those symbols at global scope.
- `PANDUCK_YYJSON_INCLUDE_DIR` adds an include directory but links no library.

So `-DPANDUCK_WITH_IPYNB=ON` does not work standalone, and **CI does not execute
`libpanduck/test/test_ipynb.cpp`** — it was verified through an out-of-repo shim.
Resolving this is a source change to `ipynb.cpp` plus a real dependency decision,
not a flag flip. Separately, the `vcpkg.json` entry costs a dependency on every
platform build (Wasm included) for a target only the standalone configure uses;
either drop it once libpanduck links its own copy another way, or make it
conditional so the extension build keeps using DuckDB's.

## Gaps in the guards themselves — closed

Recorded because a guard everyone trusts is worse than no guard. None of these
was a defect in the engine; each was a way the *checking* was weaker than it
read. All four are now fixed, and one of them was misdiagnosed first — which is
the most useful part of the record.

**`libpanduck/` was outside the format gate.** `extension-ci-tools`' makefile
runs `format.py --all --check --directories src test`, so `make format-check`
returned 0 regardless of what `libpanduck/` looked like — and had since the first
seam move. Two files had quietly accumulated over-120-column lines from their
`DuckBlockVocabulary::` renames, which is exactly what a format gate is for.

Fixed in panduck's own Makefile rather than upstream's: `extension-ci-tools/` is
vendored, and a local edit there drifts silently from upstream and resurfaces as
a mystery on the next sync. `make format-check-libpanduck` and
`format-fix-libpanduck` mirror the upstream invocation through `--directories`.
A separate target, not an override — redefining an included makefile's target is
a last-one-wins game that depends on include order.

CI gets its own `libpanduck formatting` job rather than a step on the seam job,
because that job advertises "no submodules, no extension build, no network" and
that is worth keeping: it is the fastest signal in the pipeline. This check needs
the opposite — `format.py` lives in `duckdb/scripts/` and `.clang-format` is a
**symlink into the submodule**, which dangles without it and makes clang-format
fall back silently to its built-in style.

`make check-libpanduck-seam` was also added: the seam scan had no make target at
all, so a local `make check` never ran it. A guard that exists only in CI is a
guard you find out about after pushing.

**`check_duck_block_vocabulary.py` was shrinking.** Its `SCAN_GLOBS` was `src/`
only, so every reader moved behind the seam took its vocabulary references out of
view — latex's macro table alone removed 36 — while the docstring's "no gap this
scan can SEE" caveat stayed the same sentence. The globs now include
`libpanduck/`, and the pattern matches `DuckBlockVocabulary::` as well as
`DuckBlockTypes::`; matching only the old spelling would have widened the globs
while measuring the same thing, which is worse than not scanning at all.

Measured both ways, because "the report did not change" has two causes. The GAPS
report is byte-identical before and after, so nothing was being hidden yet — and
the new globs really do reach the moved code, 12 files and 50 distinct constants,
so the unchanged report is coverage holding rather than globs matching nothing.

**`panduck::HasModule()` could not answer at all, and the first diagnosis was
wrong.** It was recorded here as "answers false for every module inside the
extension", reasoning that the root `CMakeLists.txt` defines no `PANDUCK_WITH_*`
so every `#ifdef` branch is compiled out. `nm -C` on the built artifact corrected
that: **zero** matches for `panduck::HasModule`, because
`libpanduck/src/panduck.cpp` was never in `EXTENSION_SOURCES`. The function was
absent, so a caller would have hit a *link error*, not a quietly wrong answer.
Two defects stacked, and the plausible explanation hid the simpler one.

Both halves are fixed, and the function now has a caller: 13 checks in an
always-compiled `engine` module in the standalone suite, with **both answers
pinned** per `#ifdef`/`#else` so no configure leaves an arm unexercised. Neither
arm is hypothetical — the default configure CI runs has ipynb off, so CI
exercises the `false` arm for real. Mutation-verified: deleting textile's branch
in an out-of-repo copy fails with `HasModule("textile") should be true`.

It also means the all-modules-off configure is meaningful. Previously it had
nothing to run and exited 2 on the runner's own "zero checks is an error" rule;
now it reports 13 checks and passes.

**`PANDUCK_VOCABULARY_DIR` is PRIVATE on the `panduck` target** while
`panduck/vocabulary.hpp` is public, so a consumer must repeat the path. Left
alone deliberately: that is an L3 interface question — what libpanduck's public
target looks like to someone linking it — not a gap in a guard.

### The pattern worth keeping

Three of these four were invisible because nothing *failed*. A format gate that
checks the wrong directory, a scan whose evidence base shrinks, and a function
with no callers all report success indefinitely. The seam's own history says the
same thing from the other side: of six traps hit while moving readers, three
would have shipped silently. **A green check is a claim about what it measured,
not about what you hoped it measured** — which is why each fix here came with a
measurement that the check now reaches the thing it names.

## Why the repo has not split

**Split-after-stability**, taken from sitting_duck: the split happens only once
the contract has stopped moving (measured as no API-breaking change for a full
milestone). Splitting earlier makes every interface iteration a multi-repo dance
during exactly the phase when the interface churns most. That is milestone L3.

An in-tree seam publishes nothing, which is why L1 and L2 could proceed during
the v2.0 migration instead of waiting for it. See [the roadmap](roadmap.md) for
L1–L4 and how they sequence against the other phases.
