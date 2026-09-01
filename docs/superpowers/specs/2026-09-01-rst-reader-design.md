# `read_rst_blocks` — reStructuredText Reader Design

**Status:** proposed
**Roadmap:** phase 5, second of three (`org` landed at `e758eeb`; `mediawiki` remains)
**Predecessor:** `docs/superpowers/specs/2026-09-01-org-reader-design.md`

## What makes RST different from Org

Org was chosen first because it had no extension mechanism. RST has two, and
they are the whole of this design's difficulty. Everything else follows the Org
reader's shape: a line scanner, an inline pass, `kind='value'` metadata — except
RST has no metadata at all, which is the first surprise below.

All measured against pandoc 3.1.3 on 2026-09-01.

### 1. Heading level is set by ORDER OF FIRST APPEARANCE, not by the character

This is the single most important fact in the format and the one most likely to
be got wrong by assuming. Measured:

```rst
First          Second        Third
~~~~~          ======        ------
```

yields `Header level=1`, `level=2`, `level=3` — **in that order**. `~` is level 1
here purely because it appeared first. In a document that opened with `=====`,
`=` would be level 1 and `~` something else.

So the reader carries state: a list of adornment characters in the order they
were first seen, and a heading's level is that character's index. A reader that
hardcodes `= → 1, - → 2` is right on the conventional document and wrong on a
valid one, and every fixture written by a person who follows convention will
pass.

**The fixture pair must therefore include a document that does NOT follow the
convention**, or this rule is untested. That is the same lesson the Org
`:PROPERTIES:` drawer taught: the handwritten fixture agrees with the author's
habits, and only a second witness disagrees.

### 2. A field list is NOT metadata

```rst
:Author: A. Writer
:Version: 1.0
```

Measured: pandoc emits a **`DefinitionList`** and an **empty `meta`**. RST is the
only one of panduck's seven formats with no document metadata at all.

This is worth stating loudly because the opposite is the obvious reading — the
syntax exists precisely to record document fields, docutils itself promotes them
into the document's metadata, and every other format's author field lives in
metadata. A reader written on that assumption produces `kind='value'` elements
pandoc does not have, and **nothing in the vocabulary would object**: the shape
is valid, the keys are plausible, and only a differential against pandoc catches
it.

So: field lists become `list` with `list_type='definition'`, exactly as
`term :: definition` does in Org and `<dl>` does in EPUB. `read_rst_blocks`
emits no `kind='value'` rows.

### 3. Directives are an open set, and that is the design problem

`.. name:: argument` followed by an indented body. Anyone can define one; docutils
ships dozens; Sphinx adds hundreds. A reader cannot enumerate them.

Measured mappings for the ones pandoc special-cases:

| directive | pandoc | panduck |
|---|---|---|
| `.. code-block:: python` | `CodeBlock` class `["python"]` | `code`, `attributes['language']` |
| `.. note::` | `Div` class `["note"]` | `div`, `attributes['source_type']='note'` |
| any other | `Div` with the name as a class | `div`, `attributes['source_type']=<name>` |

`source_type` carries the directive name rather than a minted role. The spec's own
instruction for an unrecognised name is to keep the original in
`attributes['source_type']` so it is **visible as a gap rather than silently
private**, and that is exactly this case. No new role values are requested.

**The body is DESCENDED INTO, never dropped.** This follows the LaTeX reader's
rule for an unknown environment: a macro is usually presentational and wraps a
fragment, but an environment usually wraps prose, and dropping it loses
paragraphs. A directive body is prose. `code-block` is the exception — its body is
its content, taken verbatim.

## Construct mapping

Everything not listed under the three sections above:

| RST | duck_block |
|---|---|
| paragraph | `paragraph` |
| `- x`, `* x`, `+ x` | `list` `list_type='bullet'` > `list_item` |
| `1. x`, `#. x` | `list` `list_type='ordered'` + `start`/`number_style`/`number_delim` |
| `term` + indented body | `list` `list_type='definition'` > `list_item` `role='term'`/`'definition'` |
| `::` + indented block | `code`, no language |
| grid and simple tables | `table`, native `{headers, rows}`, `encoding='json'` |
| `----` transition | `hr` |
| `.. comment` (no `::`) | dropped |
| `*emph*` | `italic` |
| `**strong**` | `bold` |
| ` ``literal`` ` | `code` |
| `` `text <url>`_ `` | `link`, `href` = url |

Both table grammars produce `Table` in pandoc and both must produce the native
schema here. The grid form (`+---+`) and the simple form (`===  ===`) are
different parsers; the simple form's column boundaries come from the rule row's
run lengths, which is the one place cell extraction is positional rather than
delimited.

## Architecture

`src/rst_scanner.{cpp,hpp}` and `src/rst_reader.{cpp,hpp}`, mirroring the Org
reader — including the seam: **no DuckDB types outside the trailing emission
section**, verified rather than intended. `src/org_scanner.cpp` has zero DuckDB
references and this one must too.

RST is indentation-significant in a way Org is not: a directive body, a
definition, a literal block and a nested list are all "the indented lines that
follow". The scanner therefore reports each line's indent and the reader groups
by it, rather than the scanner trying to decide what an indented run belongs to.

## Testing

`test/sql/rst_reader.test`, and a fixture pair — one hand-written, one
pandoc-generated (`pandoc -f rst -t rst`), added to the roundtrip harness with an
**empty** expected-divergence ledger. The Org reader achieved that and it is the
target here; any divergence is either a defect or an argument to have, not a row
to add quietly.

**The adornment-order fixture is required, not optional.** One of the two must
use a non-conventional adornment order, because a conventional document cannot
distinguish the right rule from the wrong one.

## Out of scope, stated rather than silent

Substitutions (`|name|`), citations and footnotes, `.. include::`, roles
(`` :ref:`x` ``), doctest blocks, line blocks, option lists. Each is real RST.
Each is also either a cross-document mechanism or a docutils-specific feature
with no duck_block equivalent, and inventing one is what the vocabulary's
`generic` + `source_type` idiom exists to avoid.

Every one of these must be **DROPPED, not LEAKED**. The Org reader shipped a
defect where scoped-out `:PROPERTIES:` drawers fell through to TEXT and joined
the following paragraph; scoping a construct out has to mean dropped. Losing
structure is a gap; emitting non-content as prose is a bug.
