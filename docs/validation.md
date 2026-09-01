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
normalises away three differences that carry no information — pandoc emits explicit `Space`
inlines where panduck folds spaces into runs; pandoc splits text per word; pandoc nests
`Strong [Str "bold"]` where panduck puts content on the `bold` inline. Each case then
declares how far up the ladder agreement is required:

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

`--report` shows raw divergences without asserting, which is how every ledger entry was
derived. Entries written from expectation rather than measurement are how a validator ends
up encoding its own bugs — two harness bugs were caught exactly that way.

## In CI

The roundtrip job does **not** rebuild panduck. The distribution matrix already produces
the `linux_amd64` artifact, so the job downloads that and `LOAD`s it into a stock DuckDB
CLI of the same version — about two minutes instead of forty.

It passes `--require`, which turns a missing prerequisite into a failure. Without it the
check skips when pandoc or the artifact is absent, and **a job that silently skips reports
coverage it is not providing.**

## A note on writing assertions

Three failure modes surfaced building this, each one passing while the property it named
was violated:

- a check on **shape** cannot see an error in **mapping**
- a check on **result** cannot see an error in **shape**
- a **negative** check cannot see **absence** — `EXCEPT` returning 0 is satisfied by two
  readers that both return nothing

All three were found by *watching the guard fail*, not by watching it pass. A guard only
ever observed passing is indistinguishable from one that cannot fail.
