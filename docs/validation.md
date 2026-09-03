# Validation

panduck claims to be pandoc-compatible without pandoc. Two harnesses test that claim rather
than asserting it, and both run a **real pandoc binary**.

## `make test_pandoc_alignment` — the vocabulary contract

`src/pandoc_ast_map.cpp` records all 35 `pandoc-types` 1.23 Block and Inline constructors
with the `duck_block` `element_type` each maps to, queryable as
`panduck_pandoc_ast_map()`.

The harness runs pandoc over a kitchen-sink fixture exercising **34 of the 35** (only
`Null` is unreachable — no pandoc reader emits it) and fails on three kinds of drift:

1. pandoc emits a constructor absent from the mapping table
2. `pandoc-api-version` no longer matches what the mapping targets
3. the set of unimplemented constructors changed without the ledger being updated

All three failure modes were negative-tested. It **skips cleanly** (exit 0) when pandoc is
absent so it never blocks a build; CI installs pandoc explicitly so it actually runs.

## `make test_roundtrip` — differential validation

Reads every fixture **twice** — once with panduck, once with pandoc — and compares.

```
  pandoc_pstyle.docx [docx]
    text      agree
    skeleton  diverges as declared [not-implemented] -- lists and blockquotes
    marked    diverges as declared [not-implemented]
```

### Why not a self round-trip

The obvious test is `X == panduck_read(panduck_write(X))`. It needs a writer panduck
doesn't have, and it is the weaker test regardless: **a reader and a writer sharing one
misunderstanding round-trip perfectly while both being wrong.** Two *independent* readers of
the same bytes catch misreads no self round-trip can.

That is not hypothetical. It caught a real bug on the first new reader: DOCX text came back
as `A paragraph withbold,italic, andstriketext.` — every inter-word space lost, because
pugixml discards whitespace-only text nodes and pandoc emits spacing as separate
space-only runs. **Nothing in the sqllogictests would have found it** — they assert
headings, inline types and Unicode, all of which were already correct.

### What counts as identity

Two readers never agree byte-for-byte and mostly shouldn't have to. `canonical.py`
normalises away differences that carry no information:

- pandoc emits explicit `Space` inlines where panduck folds spaces into runs
- pandoc splits text per word
- pandoc nests `Strong [Str "bold"]` where panduck puts content on the `bold` inline
- **a tight list item's text belongs to the item.** pandoc splits `<li>bullet one</li>`
  into an empty `list_item` plus a `Plain`; panduck puts the text on the item. The fold
  reads *pandoc's own signal* — it uses `Plain` rather than `Para` for exactly this case —
  rather than guessing.
- **an empty paragraph is not a block.** pandoc's EPUB reader injects one per spine
  document as a cross-document link target. The rule is applied to **both sides**, or it
  would just be excusing one reader; container blocks (`div`, `blockquote`, `list_item`,
  `hr`, …) are kept, because their identity is structural rather than textual.

Both of the last two were found by EPUB and both shifted every later position, which reads
exactly like a reader defect and is not one. Each case then declares how far up the ladder
agreement is required:

| Level | Compares | Catches |
|---|---|---|
| `text` | all visible text, markers stripped | data loss |
| `skeleton` | block types + heading levels | structural loss, misclassification |
| `marked` | skeleton + canonical inline markup | the above plus formatting |

### The reference is not ground truth

pandoc is the reference, and pandoc is not always right:

- On `pandoc_outlinelevel.rtf`, **pandoc's own RTF reader** yields `café —em-dash` where
  the source document reads `café — em-dash`. panduck matches the source.
- On LibreOffice files, pandoc detects **no headings at all** — its raw JSON for
  `libreoffice_outlinelvl.docx` is `[Para, Para, Para, …]`, no `Header` — because it ignores
  `w:outlineLvl`. panduck reads them correctly.
- On `libreoffice.epub`, pandoc detects no **formatting** either: LibreOffice's EPUB export
  puts every bold and italic in a CSS class, which pandoc does not resolve. panduck does.
- On a book whose `.opf` sits in a subdirectory, pandoc **cannot open the file at all**:
  it concatenates manifest hrefs without normalising `../`.

So divergences are **triaged**, not assumed to be panduck's fault:

| Verdict | Effect |
|---|---|
| `panduck-wrong` | **fails** |
| `reference-wrong` | recorded; pandoc is wrong |
| `not-implemented` | recorded; panduck doesn't read this construct yet |
| `ambiguous` | recorded with reasoning |

The ledger **ratchets both ways**: an undeclared divergence fails, and so does a declared
divergence that has silently started agreeing — that one should be promoted rather than left
rotting. Both directions are negative-tested.

`pandoc.epub` is the **only fixture with no ledger entry at all** — it agrees at every
level, and the empty entry is itself the assertion. EPUB content documents are XHTML, so
both readers see the same tree and no representational gap is left to blame; a divergence
appearing there is a real defect by construction.

`--report` shows raw divergences without asserting, which is how every ledger entry was
derived. Entries written from expectation rather than measurement are how a validator ends
up encoding its own bugs — two harness bugs were caught exactly that way.

## `make check-writeback` — the mapping back must stay total

The other checks ask whether panduck READS correctly. This one asks whether what it
produces can be written back out as pandoc JSON that a **real pandoc** accepts.

It needs the real parser because this defect class is invisible from inside DuckDB. The
first thing it caught was a `Table` whose `c` field held duck_block's own
`{"headers":…,"rows":…}` projection where pandoc's grammar demands a six-element array.
That is well-formed JSON, it round-trips through DuckDB perfectly, every in-process
assertion about it passed — and pandoc rejected the entire document:

```
When parsing the constructor Table of type Text.Pandoc.Definition.Block
expected Array but got Object
```

Every table from every native reader was unexportable, and the compatibility claim held
only for tables that had come from pandoc to begin with.

**It hid behind a coverage gap that looked like coverage.** The fixture sweep was green on
17 documents. Org, RST, LaTeX and DOCX all have table paths and *not one fixture exercises
them* — exactly one fixture in the tree contains a table at all. So the check now carries a
**constructs arm** driving each reader's `*_blocks_string` entry point, and each case names
the `element_type` it must produce and verifies it did *before* treating "pandoc accepted
it" as evidence. Without that, a reader that silently dropped the construct would report
`ok` forever: a check on the result that cannot see an error in the shape.

## The other four checks

`make check` runs seven things. The three above are the ones with prose; these four are
each a narrow guard that earned its place by catching something.

| Target | What it compares | What it caught |
|---|---|---|
| `check-vocabulary` | the vendored `duck_block_types.hpp` against upstream, by name **and value** | a constant that matched by name while its value had moved |
| `check-conformance` | every fixture's output against upstream's own conformance macros | element types outside the closed vocabulary — twice, `encoding='mediawiki'` and `encoding='org'` |
| `check-converter` | the relocated Pandoc converter against its own regression sweep | that moving a file between repos silently drops the tests that lived beside it |
| `check-divergence` | panduck's copy of the converter against upstream's, at a resolved SHA | a missing recursion bound that **segfaulted** on a deeply nested AST |

`check-divergence` reports a *signal*, not a diagnosis. Its `EXPECTED` list records the
divergences that are deliberate — a different Link/Image strategy, and helper names that
differ because the same bug was fixed independently in both repos. A new divergence means
*look*, not *fix*: the two copies drifting apart is sometimes upstream moving.

`check-conformance` covers 31 fixtures; `check-writeback` covers 29 plus 16 constructs.
The two numbers differ on purpose — the deliberately malformed fixtures in
`test/fixtures/malformed/` must be *read* without crashing, but there is nothing valid to
write back.

## In CI

The roundtrip job does **not** rebuild panduck. The distribution matrix already produces
the `linux_amd64` artifact, so the job downloads that and `LOAD`s it into a stock DuckDB
CLI of the same version — about two minutes instead of forty.

It passes `--require`, which turns a missing prerequisite into a failure. Without it the
check skips when pandoc or the artifact is absent, and **a job that silently skips reports
coverage it is not providing.**

### The wasm symbol check

The loadable `.wasm` is linked by a **separate** `emcc -sSIDE_MODULE=2` step that reads
only `DUCKDB_EXTENSION_PANDUCK_LINKED_LIBS`. `target_link_libraries()` is ignored there,
so a dependency missing from that list is left as an unresolved import: the module builds,
uploads, and goes green, then throws on the first call in the user's browser.

**CI builds the wasm but never instantiates it**, so nothing else in the pipeline can see
the difference. `wasm-symbol-check` downloads the artifact and statically parses it —
no duckdb-wasm runtime, no version match — so a failure is unambiguously the link bug
rather than an ABI mismatch. See `test/wasm/README.md`.

Three sibling extensions shipped this exact bug before panduck nearly did.

## A note on writing assertions

Three failure modes surfaced building this, each one passing while the property it named
was violated:

- a check on **shape** cannot see an error in **mapping**
- a check on **result** cannot see an error in **shape**
- a **negative** check cannot see **absence** — `EXCEPT` returning 0 is satisfied by two
  readers that both return nothing
- a check on **symbols** cannot see the **artifact** — a debug archive resolves every
  symbol exactly as well as a release one, so the wasm check is blind to which of the two
  got linked. The only thing that can see it is the resolved path printed to the log.

All three were found by *watching the guard fail*, not by watching it pass. A guard only
ever observed passing is indistinguishable from one that cannot fail.
