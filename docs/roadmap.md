# Roadmap

> Written 2026-10-05, against v0.5.6. Every claim about current state here was measured,
> not recalled; where something could not be established it says so.
>
> Revised the same day: Phase 3 originally asked whether the write direction should stay
> interchange-only and recommended that it should. That question has been answered the other
> way — writers are first-class — and the phase is rewritten around a `COPY` surface. The
> earlier recommendation was made before the container-writing cost had been measured.

## Where panduck actually is

**The breadth phase is finished.** The roadmap used to live in the dispatch table as rows with
`status='planned'`, and a regression test pinned "a planned format must not be routable"
against a concrete extension for the project's whole life — `.docx`, then `.odt`, `.epub`,
`.tex`, `.org`, `.rst`, `.wiki` — each promotion turning the test red and demanding the format
be finished rather than the test patched. MediaWiki was the last. **That list is now empty**,
and the test states the invariant directly instead of naming an example.

So "add more formats" is no longer a plan, and the issue tracker will not supply one either:
five issues are open, three of which are one cluster.

What remains is depth, and the direction comes from the sentence panduck already lives by:

> panduck is compatible with pandoc's **data model**, not its **ABI** — and that claim is
> tested, not asserted.

Everything below serves that sentence. Anything that does not is out of scope, however
appealing.

## The governing principle: a unified API over other libraries

**panduck's product is the API, not the parsing.** One vocabulary (`duck_block`), one dispatch
surface, one fidelity policy — across many formats, in both directions. Everything underneath
that is an implementation detail and should be *somebody else's library* wherever a capable one
exists.

This is already how much of panduck works, and the pattern should be stated rather than
rediscovered per format:

| concern | delegated to |
|---|---|
| XML parsing | pugixml |
| zip containers | miniz |
| markdown / HTML / plain output | `duckdb_markdown`, `duckdb_webbed`, `duck_block_utils` |
| PDF | the `pdf` extension |
| the vocabulary itself | `duck_block_utils`, vendored |

**The eleven hand-written readers are not a counter-example, they are the exception the rule
predicts.** They exist because no in-process library covered those formats with
pandoc-compatible semantics — the same absence that justifies panduck at all. They are
replaceable: if a usable library appears for a format, that reader becomes an adapter, and the
differential validator is the safety net that makes the swap checkable rather than scary.

Three consequences worth stating, because they constrain every phase below:

- **Delegation is the default; hand-writing is what you do when nothing exists.** Not the
  reverse.
- **A wrapper inherits its library's fidelity**, which may not be panduck's. That is an
  argument for Phase 2 (compat mode) *preceding* heavy delegation: without a fidelity
  parameter, each wrapped library silently imposes its own answer.
- **Delegation must degrade, not break.** `doc_render` already errors with "needs the markdown
  extension" rather than failing obscurely, and the architecture doc notes panduck works with
  `duck_block_utils` absent except for convenience wrappers. Optional dependencies stay
  optional — and in a fleet where the DuckDB ABI is exact-version, every added sibling
  dependency is also a version-skew surface.

## The two directions are not symmetric

This is the structural fact that shapes most of what follows.

| | reading | writing |
|---|---|---|
| surface | 11 native readers + a Pandoc AST reader reaching every format pandoc reads | one native emitter (Pandoc JSON) |
| dispatch | registry table, path/extension dispatch, **runtime reader registration** | a hardcoded three-arm `CASE` in `doc_render` |
| extensibility | flip a row from `planned` to `implemented`; register a reader at runtime | edit C++ and rebuild |

`doc_render(src, output_format, format := 'auto')` offers `md`, `html` and `text`, but panduck
emits **none** of them: each arm delegates to a sibling extension and is gated on that
extension being installed.

| target | delegates to | requires |
|---|---|---|
| `md` | `duck_blocks_to_md` | `markdown` |
| `html` | `duck_blocks_to_html` | `webbed` |
| `text` | `duck_blocks_to_text` | `duck_block_utils` |
| `ansi` | — | declared, not wired up |

That was a defensible design — Pandoc JSON is the universal interchange, and the README's
`| pandoc -f json -t markdown` pipe covers everything else — but it was never actually
decided, only arrived at.

**It is decided now (2026-10-05): writers are first-class.** The target is a `COPY` surface,
and Phase 3 below is no longer a question but a project.

---

## Phase 0 — survive the platform shift

**The only deadline panduck does not control.**

DuckDB v2.0 deleted `SimpleNamedParameterFunction` under us on 2026-09-25 with no notice, and
because `duckdb_version: v2.0-cyanoptera` is a **floating branch ref**, the same commit built
green on 09-22 and red on 09-30. It read exactly like a self-inflicted break and was not one.

- **#102 — `DataChunk::SetValue` deprecated**, 136 sites. Mechanical and **needs no compat
  shim**: `Vector::SetValue` exists undeprecated on both lines, so
  `output.SetValue(c, r, v)` → `output.data[c].SetValue(r, v)` is version-safe everywhere.
- **The registry descriptor PR** is the real proving ground. `community-extensions`'
  `build_next.yml` builds *every* registry pull request against `v2.0-cyanoptera`, so panduck
  cannot submit one until the source compiles there. v0.5.6 was cut for exactly this reason.
- **Keep the early-warning habit.** What is `[[deprecated]]` on `main` today is removed on the
  next line. The deprecation list in `duckdb_markdown/docs/duckdb_v2_migration.md` is the
  cheapest forecast available.

**Operational rule learned the hard way, worth keeping:** when an advisory leg reddens on a
branch that touches no C++, re-run the last green run on its own unchanged commit *before*
attributing the failure. A matching failure exonerates the branch in one measurement. And the
`HEAD is now at …` line inside the docker build belongs to **vcpkg**, not DuckDB.

## Phase 1 — close the fidelity gaps already on the board

Three of the five open issues are one cluster, and they have a natural order:

1. **#97 — the pandoc reader does not recurse into `Note`/`Cite`**, so a footnote's body is
   lost on import. **Newly unblocked**: this was waiting on an id-minting ruling, which landed
   in duck_block_utils v3.5.0 as `ATTR_ID` / `ATTR_NAME` — `id` is the fragment locator
   (minted or supplied), `name` is what the source called the thing.
2. **#85 — RST footnote markers stay literal**, and headings/`doc_render` anchors depend on the
   same decision. Follows #97.
3. **#98 — `pandoc_ast_map` cannot express a half-mapping.** `STATUS_MAPPED` is one flag over a
   *bidirectional* correspondence.

**#98 is the important one even though it looks like the small one.** It is why the `[note]`
bug shipped: the guard could only check that a constructor pandoc emits appears in the map,
never that panduck can write one back. Fixing the instance without fixing the guard buys one
bug; fixing the guard buys the class.

Its sibling is **#100** — the parsed-fixture drift check reads a persisted extension cache and
can therefore pass against stale data. Same family: **a guard that cannot fail is not a
guard.** Worth doing both under one theme rather than separately.

**Prerequisite picked up for free:** Phase 1 forces a re-vendor of the duck_block vocabulary
header (currently `BEHIND` v3.5.0 — passing, stamp consistent, just missing `ATTR_ID`,
`ATTR_NAME`, `TYPE_DOCUMENT`). That also retires the local `TYPE_DOCUMENT_PENDING_VENDOR`
workaround in `pandoc_block_convert.cpp`.

## Phase 2 — make fidelity a choice instead of a ruling

**The highest-leverage product move, and the one this document argues hardest for.**

panduck keeps being *more faithful than pandoc*: docutils adornment thresholds, transitions
after bullet lists, textile delimiter word boundaries, richer attributes. The governing rule
permits it — diverge where the source justifies it, discard nothing, never emit JSON pandoc
rejects — but it means **every divergence costs a human ruling**, one at a time, forever.

A `compat := 'pandoc' | 'faithful'` parameter converts a recurring argument into a switch:

- consumers who want pandoc's exact output get it, and can say so in a query rather than a bug
  report;
- the differential validator can assert **both** behaviours instead of encoding one opinion;
- every past ruling becomes a test case rather than a precedent someone has to rediscover.

The existing divergences are the initial test matrix. They should be enumerated while the
reasoning behind them is still recoverable.

## Phase 3 — writers are first-class: a `COPY` surface

The goal, both forms:

```sql
COPY (FROM read_panduck_doc('in.odt')) TO 'foo.docx' (FORMAT panduck, DOC_FORMAT docx);
COPY (FROM read_panduck_doc('in.odt')) TO 'doo.docx';   -- just works
```

### How DuckDB makes the second form possible

Measured in `bind_copy.cpp` on the v1.5.6 pin, not assumed:

```cpp
if (info.is_format_auto_detected && info.format.empty()) {
    info.format = ExtractFormat(info.file_path);   // the file's suffix
}
// then: catalog lookup of {COPY_FUNCTION_ENTRY, info.format}
```

**`FORMAT` defaults to the file's suffix, and a copy function of that _name_ is looked up in
the catalog.** `ExtractFormat` strips a compression suffix first, so `foo.docx.gz` resolves to
`docx` for free. When the lookup misses, `IsFormatExtensionKnown` scans `EXTENSION_FILE_POSTFIXES`
to say which extension would have provided it.

So both forms are reachable today with no upstream change:

| written | resolves to | needs |
|---|---|---|
| `TO 'foo.docx' (FORMAT panduck, DOC_FORMAT docx)` | format `panduck` | `CopyFunction("panduck")` + a `DOC_FORMAT` option |
| `TO 'doo.docx'` | format `docx` | `CopyFunction("docx")` |

`duckdb_yaml` already ships this pattern — `CopyFunction("yaml")`, `copy_function.extension =
"yaml"`, a `plan` function `CopyToYAMLPlan`, registered from `RegisterYAMLCopyFunctions`. It is
a working template rather than a design to derive. **No name collisions exist**: `markdown`,
`webbed` and `duck_block_utils` register no copy functions at all, and neither does panduck.

The per-suffix registrations share one implementation and differ only in the `DOC_FORMAT` they
preset — so **the list of suffixes is data**, which is the writer registry this document asked
for, mirroring the reader side's table.

### 3a — the COPY surface, on emitters that already exist

| target | emitter | status |
|---|---|---|
| pandoc json | panduck, native | ready |
| `md` | `duck_blocks_to_md` (markdown) | ready, delegated |
| `html` | `duck_blocks_to_html` (webbed) | ready, delegated |
| `text` | `duck_blocks_to_text` (duck_block_utils) | ready, delegated |
| `docx` / `odt` | — | **nothing exists in the fleet** |

Emitters may be **black boxes** — sibling extensions or linked libraries — exactly as
`doc_render` already treats them. panduck owns dispatch, not necessarily serialisation.

**Ship 3a first.** It makes `TO 'foo.md'`, `TO 'foo.html'` and `TO 'foo.json'` work
immediately, proves the dispatch and the option plumbing, and leaves the expensive piece
separable instead of blocking.

### 3b — the emitter matrix

**The emitter is a property of the registry row, not a single strategy.** panduck owns
*dispatch*; it does not have to own *serialisation*, and for several formats it should not.
Adding a writer is adding a row — the same shape the reader side already has.

Four kinds of row, against the 11 formats panduck reads (`docx, epub, ipynb, latex, mediawiki,
odt, org, pandoc, rst, rtf, textile`) plus the render targets:

| kind | targets | provider | cost |
|---|---|---|---|
| **native, built** | pandoc json | panduck | done |
| **delegated** | `md`, `html`, `text` | markdown / webbed / duck_block_utils | done — reuse, do not reimplement |
| **native text serialiser** | `rst`, `org`, `textile`, `mediawiki`, `latex`, `ipynb` | panduck *only if no library exists* | moderate — no container, and the mapping is the inverse of a reader panduck owns |
| **native container** | `docx`, `odt`, `epub` | panduck + `ZipWriter` | highest — needs zip writing and per-format scaffolding |

Three rules that follow, and matter more than the table:

- **Delegation is the default, not the fallback** — this is the governing principle applied to
  the write direction. Where a sibling already emits a format, the row points at it. Claiming
  `md` or `html` natively would duplicate `duckdb_markdown` and `duckdb_webbed` and create two
  sources of truth for one format.
- **"Native" in the third row means _not yet delegated_.** Before writing a serialiser for any
  of those formats, survey for a usable library; write one only where nothing exists, exactly
  as the eleven readers came about. The row records today's answer, not a commitment.
- **A row may change kind without changing the surface.** If a library or sibling later emits
  docx, that row becomes delegated and no caller notices. This is why the dispatch layer (3a)
  is worth building before any emitter work.

`rtf` is deliberately unassigned: panduck reads it, but it is a format whose writers are
notoriously divergent, and there is no consumer asking. `pdf` stays read-only — it is already
delegated to the pdf extension and is not a duck_block output target.

#### The container class, in detail

Container output is the expensive kind, but less expensive than it looks:

- **miniz's writer APIs are compiled in** — `MINIZ_NO_ARCHIVE_WRITING_APIS` is commented out in
  the vendored `miniz.hpp`. panduck already links miniz, and
  `libpanduck/src/zip_container.cpp` uses only `mz_zip_reader_*`. A `ZipWriter` sibling to
  `ZipContainer` is small, and would belong behind the seam beside it (issue #104, L2).
- **pugixml can serialise** (`xml_document::save`), though panduck has **zero** `.save(` calls
  today — it has only ever parsed.
- **The mapping is an inverse of one panduck already owns.** It reads docx structurally, not
  through pandoc: `word/document.xml`, `styles.xml`, `numbering.xml`, `footnotes.xml`,
  `docProps/core.xml`.
- **`ZipContainer` is shared by docx, odt and epub**, so a writer serves ODF too. The second
  container format is much cheaper than the first.

**The asymmetry that makes this tractable:** `docx_reader.cpp` is 900 lines because it must
accept whatever Word, LibreOffice and Google Docs each emit — heading detection supports two
mechanisms, and checks style *ids* as well as names because one app writes `"Overskrift 1"` and
another `"heading_20_3"`. **A writer emits one canonical shape.** A reader is defensive; a
writer is not.

What is genuinely new is the OOXML scaffolding — `[Content_Types].xml`, `_rels/.rels`,
`word/_rels/document.xml.rels`, a minimal `styles.xml`. Honest caveat: *"a minimal valid docx
that Word opens without complaint"* has a long tail that only testing against real Word will
surface. Budget for that tail, and treat `check-writeback`'s discipline — feed the output to a
real consumer — as the model for how it gets verified.

### Decisions still open in Phase 3

1. **Input contract.** Require the duck_block 7-column shape and error clearly otherwise
   (recommended), or accept arbitrary relations and render them as a table document?
2. **Which suffixes panduck claims.** `docx`, `odt`, `rst`, `org` are unambiguously panduck's.
   `md` and `html` are **not** — claiming them globally pre-empts `duckdb_markdown` and
   `duckdb_webbed`. Recommended: claim the document formats, leave `md`/`html` to their owners,
   reachable through `FORMAT panduck, DOC_FORMAT md`.
3. **docx emitter**: link a third-party library as a black box, or hand-roll minimal OOXML on
   miniz plus pugixml?

`copy_from_bind` exists on the same API, so `COPY tbl FROM 'x.docx'` is available later for
symmetry — not part of this phase.

## Phase 4 — the longer bets

**Embedded-document post-parsing.** An embedded `DOCUMENT` held raw is a *deferral*, unlike a
toml/yaml blob which is terminal. A post-parse helper completes the model and makes the
distinction visible rather than implicit.

**libpanduck** — the same unified API, usable without DuckDB. panduck's founding rationale is
the argument for it: there is no libpandoc, upstream has never shipped a C shared library, and
jgm/pandoc#6611 has been open since 2020.

**What it is, per the governing principle above: a unified API _over format libraries_ — not
an extraction of panduck's own parsers.** The valuable, portable part is the vocabulary, the
dispatch, and the fidelity policy. The parsers are the part most likely to be replaced by
somebody else's library, one format at a time, and a library whose identity is "panduck's
parsers" would make each such replacement a breaking change instead of an internal one.

Read that way, libpanduck is less an extraction than a *re-homing*: the layer that already sits
above pugixml, miniz, the sibling extensions and the hand-written readers, with the DuckDB
binding becoming one consumer of it rather than its container.

### The shape: a sub-extension API and a results format

Adopting the model in `sitting_duck/docs/planning/v2-architecture.md` (RFC, tracking its #87),
which refactors a monolithic extension into an engine (`sitting_duckling`), a module source
tree, and *compositions*. Distilled, it is two deliverables — **a sub-extension API** and **a
results format** — and panduck starts ahead on the second:

| deliverable | sitting_duck | panduck |
|---|---|---|
| results format | taxonomy spec, still to be built as a versioned artifact | **already exists**: `duck_block`, versioned by `duck_block_utils` (`SPEC_VERSION`), vendored byte-exact |
| sub-extension API | module ABI + conformance kit | the new work |

| identity | ships |
|---|---|
| **libpanduck** | the engine: vocabulary types, dispatch, fidelity policy — plus the format modules whose parsers depend only on its own dependencies |
| **panduck** | the batteries-included composition for DuckDB: table functions, the `doc_*` macros, the `COPY` surface — delegating to sibling extensions wherever they are the better answer |

**Module placement rule** (mirror of sitting_duck's, which is a rule rather than a judgment
call): a format module lives in libpanduck **iff its parser depends only on the host engine's
own dependencies** — no DuckDB, no sibling extension.

So `markdown` (cmark), `xml` (pugixml, already linked) and `pdf` can live in libpanduck while
the panduck extension still delegates those to `duckdb_markdown`, `duckdb_webbed` and the `pdf`
extension — because inside a DuckDB session those are the better answer, and outside one they
are unavailable. **The provider is a property of the composition, not of the format.** That is
what makes "libpanduck supports more formats than panduck exposes" coherent rather than
duplicative, and it is the reason format support belongs behind **build-time flags**
(`option(LIBPANDUCK_WITH_MARKDOWN …)`) rather than in separate repos — sitting_duck's "packs
are build outputs, not repos".

**Two of the four doors already exist in embryo**, which is why this is less new than it reads:

| door | panduck status |
|---|---|
| compiled-in (build flag per module) | the new work |
| runtime registration | **shipped** — `panduck_register_doc_reader` / `_register_table_reader` + the registry table |
| pack extension (`INSTALL panduck_<format>`) | later, only if demand pulls it |

**Conformance.** sitting_duck's rule is that a module either passes the conformance kit or it
is not a module. panduck has one in embryo already — the differential validator against a real
pandoc, `check-writeback`, `check-wordloss` — and formalising it is what makes swapping a
hand-written reader for a wrapped library checkable instead of scary.

### Milestones, and a correction

**Split-after-stability**, taken verbatim from sitting_duck: the repo split happens only once
the contract has stopped moving (measured: no API-breaking change for a full milestone).
Splitting earlier makes every interface iteration a multi-repo dance during exactly the phase
when the interface churns most.

- **L1 — contract in-tree. No files move.** A DuckDB-free row struct as the engine's currency
  (today eleven per-format structs convert to rows in the DuckDB layer), the module interface,
  and the conformance kit assembled from the validators that already exist.
- **L2 — in-tree layout.** A `libpanduck/` target that **does not include DuckDB headers**, so
  the boundary is compiler-enforced rather than conventional. Move parse cores one at a time.
  This is cheaper than it sounds: measured across the eleven readers, ~10,500 lines contain
  **~195 that touch DuckDB** — each reader is already a pure `Parse*` core returning
  `std::vector<XBlock>` plus a table-function tail. `duck_block_types.hpp` needs splitting
  along the same line, since it mixes portable constants with `Value`-returning helpers.

  **Status:** ten readers are behind the seam — docx, epub, ipynb, latex, mediawiki, odt,
  org, rst, rtf and textile — each reduced to a DuckDB tail of 113–144 lines, plus four
  companion scanners, latex's tokenizer and macro table, the shared ZIP container, and the
  shared `panduck/block_json.hpp` / `panduck/slugify.hpp` helpers. The
  `duck_block_types.hpp` split turned out not to be needed: `DuckBlockTypes` declares
  exactly one constant of its own (`FRONTMATTER_MIME_TYPE`) and inherits the rest from
  `DuckBlockVocabulary`, so aliasing the vocabulary once in `panduck/vocabulary.hpp` was
  sufficient and the `Value`-returning helpers simply stay on the DuckDB side. See
  [libpanduck](libpanduck.md) for the module contract, the enforcement mechanisms and the
  traps a move has to clear.
- **L3 — repo split**, only after L1's contract has stopped moving.
- **L4 — new format modules behind build flags**: markdown, xml, pdf — supported by libpanduck,
  not necessarily exposed by panduck.

**Correcting an earlier draft of this document:** it said "do not start libpanduck before
Phase 2". That is wrong as stated. The fidelity objection applies to **releasing a stable API**,
not to establishing the boundary — an in-tree seam publishes nothing. L1 and L2 can start now,
and there is a positive reason to: every DuckDB breakage this project has absorbed
(`named_parameters`, `SetValue`, and `ExecuteWithNulls` in a sibling) lived in that 2%. Code
behind the seam cannot break on a DuckDB API change, so the seam is a *mitigation* for the v2.0
migration rather than a competitor to it.

Phase 2 still gates **L3 and L4**: a wrapped library imposes its own fidelity, and that has to
be a documented parameter rather than an accident of which library got linked.

---

## Sequencing

```
Phase 0  ──►  Phase 1  ──►  Phase 2  ──────────►  Phase 4 (libpanduck)
(forced)      (#97 → #85,    (compat mode)          ▲
              #98 + #100)                           │
                                                    │
              Phase 3a ──►  Phase 3b ───────────────┘
              (COPY surface,  (ZipWriter,
               existing        docx + odt)
               emitters)
```

Phase 0 is forced and timed externally. Phase 1 is unblocked today. Phase 2 is the one that
stops future work from being decided case by case.

**Phase 4 splits across that line rather than sitting behind it.** Its first two milestones —
the in-tree contract (L1) and the compiler-enforced `libpanduck/` boundary (L2) — can start
immediately and *reduce* Phase 0's exposure, since every DuckDB break this project has absorbed
lived in the 2% of reader code that touches DuckDB. Only L3 (the repo split) and L4 (new format
modules behind build flags) wait for Phase 2, because a wrapped library's fidelity has to be a
parameter before it is a dependency.

**Phase 3 runs in parallel rather than in sequence.** 3a touches the registration and dispatch
layer, not the reader or fidelity code, so it does not contend with Phases 1 and 2. 3b is the
one real dependency worth respecting: a writer is where fidelity policy becomes visible to
users, so **3b benefits from Phase 2 landing first** — otherwise "what does panduck emit for
this block" becomes another pile of per-case rulings, this time baked into files other
applications have to open.

## What this roadmap deliberately excludes

- **More input formats.** The `planned` list is empty and reaching every format pandoc reads
  is already covered by the Pandoc AST reader.
- **Calling pandoc in-process.** The README's reasoning has not changed: no maintained C
  bindings, no upstream shared library, and linking the GHC runtime into a `dlopen`'d
  extension inside DuckDB's process would be a bad neighbour and unshippable as a community
  extension.
- **Pandoc's full writer matrix.** Phase 3 covers the formats panduck already reads plus the
  interchange target; it is not a commitment to emit everything pandoc can. `| pandoc -f json`
  remains the answer for the long tail.

## Open questions

1. ~~Is Pandoc JSON the intended write contract, or a waypoint?~~ **Answered 2026-10-05: a
   waypoint. Writers are first-class — see Phase 3.** The three sub-questions it leaves are
   listed there.
2. Should `compat` be per-call, a setting, or both? Phase 3b makes this sharper, since a
   written file carries the choice out of the database.
3. `ansi` in `doc_render`: wire it, or remove it? It is currently declared and unreachable.
   Phase 3a is the natural moment to settle it, since it is the same dispatch question.
4. How often should the vendored duck_block header be re-pulled — on need, or on a cadence?
   Today it is "on need", which is why it sits `BEHIND` v3.5.0.
