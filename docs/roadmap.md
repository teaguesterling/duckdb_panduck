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
| **native text serialiser** | `rst`, `org`, `textile`, `mediawiki`, `latex`, `ipynb` | panduck | moderate — no container, and the mapping is the inverse of a reader panduck owns |
| **native container** | `docx`, `odt`, `epub` | panduck + `ZipWriter` | highest — needs zip writing and per-format scaffolding |

Two rules that follow, and matter more than the table:

- **Delegation is the default, not the fallback.** Where a sibling already emits a format, the
  row points at it. Claiming `md` or `html` natively would duplicate `duckdb_markdown` and
  `duckdb_webbed` and create two sources of truth for one format.
- **A row may change kind without changing the surface.** If a library or sibling later emits
  docx, that row becomes delegated and no caller notices. This is why the dispatch layer (3a)
  is worth building before any emitter work.

`rtf` is deliberately unassigned: panduck reads it, but it is a format whose writers are
notoriously divergent, and there is no consumer asking. `pdf` stays read-only — it is already
delegated to the pdf extension and is not a duck_block output target.

#### The container class, in detail

Container output is the expensive kind, but less expensive than it looks:

- **miniz's writer APIs are compiled in** — `MINIZ_NO_ARCHIVE_WRITING_APIS` is commented out in
  the vendored `miniz.hpp`. panduck already links miniz, and `src/zip_container.cpp` uses only
  `mz_zip_reader_*`. A `ZipWriter` sibling to `ZipContainer` is small.
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

**libpanduck** — parsers and emitters usable without DuckDB. This is the strategic endgame,
and panduck's own founding rationale is the argument for it: there is no libpandoc, upstream
has never shipped a C shared library, and jgm/pandoc#6611 has been open since 2020. panduck is
quietly becoming the thing that gap needs.

**Do not start it before Phase 2.** Extracting a library while fidelity policy is still a pile
of per-case rulings bakes those rulings into an API that then has to be supported. Extract
something whose behaviour is a *parameter*.

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
stops future work from being decided case by case. Phase 4 should wait for Phase 2 on purpose.

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
