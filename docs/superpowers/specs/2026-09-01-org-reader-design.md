# `read_org_blocks` — Org-mode Reader Design

**Status:** proposed
**Roadmap:** phase 5, first of three declared-but-unimplemented formats
(`org`, `rst`, `mediawiki`)

## Why Org first

Measured against pandoc 3.1.3 on 2026-09-01, not assumed:

| format | pandoc metadata | block types produced | principal hazard |
|---|---|---|---|
| **org** | `title`, `author`, `date` | Header, Para, BulletList, OrderedList, DefinitionList, BlockQuote, CodeBlock, Table, HorizontalRule | none |
| rst | none | adds Div, DefinitionList | directives and interpreted-text roles are an open extension surface |
| mediawiki | none | adds RawBlock | `{{templates}}` expand to arbitrary wikitext; pandoc itself emits RawBlock |

Three reasons, in order of weight:

1. **Every block type Org produces is already in panduck's vocabulary and already
   emitted by an existing reader.** No new `element_type` is needed, so this
   reader cannot introduce a vocabulary question — and vocabulary questions have
   been the expensive part of every reader so far.
2. **It is the only one of the three where pandoc extracts metadata.** The
   pending `kind='value'` work is therefore exercised by this reader rather than
   deferred behind it, and the metadata path gets a second consumer at the moment
   it is written instead of a year later.
3. **The grammar is line-oriented and closed.** RST's directives and MediaWiki's
   templates are both *extension mechanisms* — a reader must decide what to do
   with constructs it has never seen, which is the problem the LaTeX reader spent
   its entire design budget on. Org has no such mechanism.

A useful negative result from the same measurement: **RST's `:Author:` field list
does not become metadata.** Pandoc turns it into a `DefinitionList`. Anyone
writing the RST reader on the assumption that a field list is metadata would be
wrong, and nothing in the vocabulary would object.

## Scope

`read_org_blocks(path)` and `read_org_blocks_string(src)`, mirroring the LaTeX
reader's two entry points, plus registry rows flipping `org` from `planned` to
`implemented`.

**Out of scope, deliberately:** TODO keywords, tags, properties drawers,
scheduling/deadline timestamps, footnotes, `#+INCLUDE`, babel evaluation. These
are Org's *agenda and literate-programming* layers. They are real Org, and they
are not document structure; a reader that models them is modelling Emacs. Each
is listed in the allowlist as an intentional gap rather than left silent.

## Architecture

**A line scanner, not a tokenizer.** This is the one substantive departure from
the LaTeX reader, and it is a consequence of the grammar rather than a
preference. LaTeX needs a character-level tokenizer because its constructs
nest arbitrarily and `{`/`}` carry meaning anywhere; Org's block structure is
determined entirely by line prefixes (`*`, `-`, `|`, `#+`), and only *inline*
markup is character-level. So:

```
src ──► LineScanner ──► block structure ──► InlinePass (per line's text)
```

Two passes, each simple, rather than one machine that is neither. The inline
pass is the only character-level code and it is a flat scan — Org's emphasis
markers do not nest in practice and pandoc does not nest them either.

**Files:**

- `src/org_scanner.cpp` / `src/include/org_scanner.hpp` — line classification.
  No duck_block dependency, no DuckDB types. Mirrors `latex_tokenizer.cpp`,
  which is the existing example of a parse layer that would survive extraction
  into `libpanduck`.
- `src/org_reader.cpp` / `src/include/org_reader.hpp` — block assembly,
  metadata, emission, the two table functions.

The DuckDB-facing code stays in the trailing section of `org_reader.cpp`, as in
every other reader: measured 2026-09-01, the first DuckDB reference in each of
the five existing readers falls at 73–88% through the file, and none appear in
the first 60%. That seam is currently accidental; this reader keeps it on
purpose.

## Construct mapping

All measured against pandoc 3.1.3. Where panduck diverges it is stated.

### Block level

| Org | duck_block | notes |
|---|---|---|
| `* text` … `***** text` | `heading`, `attributes['heading_level']` = star count | level capped at 6, as elsewhere |
| blank-line-separated text | `paragraph` | |
| `- item`, `+ item`, `* item` at indent | `list` `list_type='bullet'` > `list_item` | leading `*` is a heading only at column 0 |
| `1. item`, `1) item` | `list` `list_type='ordered'` + `start`/`number_style`/`number_delim` | always emitted, matching the other readers |
| `- term :: definition` | `list` `list_type='definition'` > `list_item` `role='term'` + `list_item` `role='definition'` | spec 5.0 shape, already implemented for `<dl>` and `\begin{description}` |
| `#+BEGIN_QUOTE` … `#+END_QUOTE` | `blockquote` | |
| `#+BEGIN_SRC lang` … `#+END_SRC` | `code`, `attributes['language']` | pandoc puts the language in the class list |
| `#+BEGIN_EXAMPLE` … `#+END_EXAMPLE` | `code`, no language | pandoc emits CodeBlock with class `example`; panduck emits no language, because `example` is not one |
| `| a | b |` with a `|---|` rule | `table`, native `{headers, rows}`, `encoding='json'` | spec 5.0 shape; header iff a rule row follows the first row, the same rule the LaTeX reader uses |
| `-----` (5+ dashes) | `hr` | |
| `# comment` | dropped | pandoc emits nothing; a comment is not content |

### Inline

| Org | duck_block |
|---|---|
| `*bold*` | `bold` |
| `/italic/` | `italic` |
| `_underline_` | `underline` |
| `=code=` | `code` |
| `~verbatim~` | `code` |
| `+strike+` | `strikethrough` |
| `[[url][label]]` | `link`, `href` = url, label as child text |
| `[[url]]` | `link`, `href` = url, url as text |

**`=code=` and `~verbatim~` are not quite the same to pandoc, and panduck
collapses them deliberately.** Measured: `=code=` yields `Code` with class
`["verbatim"]`; `~verbatim~` yields `Code` with no class — the opposite way round
from what the names suggest. duck_block's `code` inline has no class field, and
the distinction is a spelling difference rather than a semantic one, so both map
to `code` and the divergence is declared in the roundtrip ledger rather than
modelled. Recording it because the surprising direction is exactly the kind of
detail a later reader would "correct" the wrong way.

### Metadata

`#+TITLE:`, `#+AUTHOR:`, `#+DATE:` become `kind='value'` elements at level 1,
appended **after** the blocks (spec 6.2 makes the ordering a contract).

Keys are **pandoc's namespace** — `title`, `author`, `date` — not the source
spelling. This was measured across five formats and is the rule that would
otherwise have been got wrong: `dc:title` becomes `title`, RTF's `\info`
generator becomes `generator`.

**Repeated `#+AUTHOR:` lines concatenate into ONE value, space-joined.** Measured:
two `#+AUTHOR:` lines yield a single `MetaInlines` reading `A. Writer B. Second`,
*not* a `MetaList`. This is worth stating because LaTeX's `\author` *does* yield
a `MetaList`, so the same logical field has two different shapes in two formats
and both are pandoc's. A reader that generalises from LaTeX to Org here is wrong.

Shape follows `MetaInlines`: `value` / `inlines` at level 1 with
`attributes['key']`, empty content, and the text as an inline child at level 2.

## Testing

- `test/sql/org_reader.test`, structured like `latex_reader.test`: most
  assertions through `read_org_blocks_string` so a construct can be tested
  without a fixture, and a fixture for the file path and encoding behaviour.
- `test/fixtures/` gains a hand-written `.org` and a pandoc-generated one, the
  same two-witness arrangement every other format has — a real writer's output
  and what a person actually types are different documents, and each has caught
  defects the other did not.
- The fixture joins `make check-conformance` automatically (its glob is by
  extension) and must be added to `test/roundtrip/check_roundtrip.py`. Any
  divergence from pandoc is declared with a reason or fixed; no fixture ships
  with an undeclared divergence.

## Risks

**The leading-`*` ambiguity is the one real parsing hazard.** `* text` at column
0 is a heading; `  * text` indented is a bullet; `*bold*` mid-line is emphasis.
All three are the same character. Column position and a following space
disambiguate all three, and the test file must assert all three in one place,
because a reader that gets two right and the third wrong looks correct on most
documents.

**Table header detection** was re-measured for Org rather than carried over from
LaTeX, since the two formats are unrelated and pandoc's readers do not share
logic. It happens to agree: `| a | b |` with no rule row yields an empty header
and two body rows; a `|---+---|` rule after the first row promotes that row. So
the LaTeX rule holds here, as a measurement rather than an assumption.
