# read_latex_blocks — design

**Status:** IMPLEMENTED and merged to main. Built across eight reviewed tasks; see
`docs/superpowers/plans/2026-08-31-latex-reader.md`. Where this document and the code
disagree, the CODE is authoritative — a whole-branch review once cited a stale line here
as grounds against correct code.
**Roadmap:** phase 4. `supported_extensions.cpp` already declares `latex` as
`STATUS_PLANNED` with the note "streaming tokenizer for macros, environments, math".

## Goal

Read `.tex` / `.latex` into duck_blocks, so that a LaTeX document and the same document
in DOCX, ODT, EPUB or RTF produce the same table of contents and the same prose.

## The problem the fixtures encode

`test/fixtures/handwritten.tex` and `test/fixtures/pandoc.tex` are the *same document
written twice* — once as a person writes LaTeX, once as pandoc emits it. This is the same
two-writer split panduck already handles in DOCX (`w:outlineLvl` vs a `w:pStyle` resolved
through `styles.xml`) and RTF (`\outlinelevel` vs a `{\stylesheet}` reference), except
here the second writer is a program and the first is a person.

A person writes:

```latex
\section{Heading One}
\label{sec:one}
```

pandoc writes:

```latex
\hypertarget{heading-one}{%
\section{Heading One}\label{heading-one}}
```

`\section` is two brace-levels deep, behind a `%` comment token. Both must yield
`heading`, `heading_level=1`, content `Heading One`.

Three further constraints the fixtures pin down, each of which breaks a naive reader:

1. **`%` eats the newline.** pandoc's trailing `%` in `\hypertarget{heading-one}{%` exists
   precisely to suppress the space that the line break would otherwise produce. A reader
   that strips comments but keeps the newline injects a leading space into every heading.
2. **`\maketitle` must emit nothing.** `handwritten.tex` has `\title`/`\author`/
   `\maketitle`; `pandoc.tex` has neither in its body. The two documents are only
   equivalent if `\maketitle` produces no block. This is consistent with every other
   panduck reader: none extracts document metadata yet.
3. **Two spellings of strike.** `\sout` (ulem, handwritten) and `\st` (pandoc's output).
   Both map to `strikethrough`.

## Scope

**In:** a fixed, documented, auditable set of document-level constructs — sectioning,
paragraphs, lists, block quotes, verbatim, basic inline formatting, links, images,
footnotes, and math kept opaque.

**Out:** `\newcommand` expansion, `\input`/`\include` following, package emulation,
bibliography resolution, and any attempt to *evaluate* TeX. LaTeX is Turing-complete;
"read LaTeX" has no natural boundary, so the boundary is drawn explicitly and made
introspectable rather than left implicit.

**Unrecognised input is dropped, never wrapped in `generic`.** A blanket `generic`
backstop is right for a *closed, fully semantic* constructor set like Pandoc's AST, and
inverts for an *open-ended, mostly presentational* one like TeX macros — it would emit a
block per layout macro and flood real documents. (This failure was observed directly in
`duckdb_webbed`'s HTML reader.) Emitting `generic` with children is additionally the exact
shape that silently dropped `source_type` in `duckdb_markdown` and `duck_block_utils` in
the same week, so adopting it would need a deliberate decision, not a fallthrough.

## Architecture

Three units, split so the tokenizer knows nothing about duck_blocks and the table stays
data:

| File | Responsibility | Depends on |
|---|---|---|
| `src/latex_tokenizer.cpp` / `.hpp` | bytes → token stream | nothing |
| `src/latex_macros.cpp` / `.hpp` | the disposition table (static data) | nothing |
| `src/latex_reader.cpp` / `.hpp` | scope stack, emission, table function | both, + `duck_block_types.hpp` |

`supported_extensions.cpp` flips `latex` from `STATUS_PLANNED` to `STATUS_IMPLEMENTED`
with `read_latex_blocks`; dispatch and `panduck_reader_registry()` pick it up with no
other change.

The tokenizer having no duck_block dependency is deliberate: it makes the fiddly parts
(comment handling, control-word whitespace, verbatim) testable without constructing a
single block, and keeps `latex_reader.cpp` about meaning rather than bytes.

## Tokenizer

Token kinds: `TEXT`, `CONTROL_WORD(name)`, `CONTROL_SYMBOL(ch)`, `BEGIN_GROUP`,
`END_GROUP`, `PAR_BREAK`, `MATH_SHIFT(inline|display)`, `END`.

Rules, in the order they bite:

- **Comments.** `%` discards to end of line, **then the newline itself, then the next
  line's leading whitespace** — TeX's actual rule. `\%` is a literal percent.
- **Control words.** `\` followed by one or more letters. **Trailing whitespace is
  consumed** (`\LaTeX foo` is one control word then `foo`, not `\LaTeX` + `" foo"`).
  A starred form (`\section*`) is looked up as the unstarred name.
- **Control symbols.** `\` followed by exactly one non-letter. `\\` is a line break.
- **Paragraph breaks.** A blank line (two newlines separated only by whitespace) emits
  `PAR_BREAK`. This is LaTeX's only paragraph signal — single newlines are just spaces.
- **Ligatures**, resolved during text scanning: `---` → `—`, `--` → `–`, ` `` ` → `"`,
  `''` → `"`, `~` → non-breaking space.
- **Verbatim suspends tokenizing.** Inside `verbatim` / `lstlisting` / `\verb`, bytes are
  taken raw until the matching terminator. Nothing is expanded, no comment is stripped.

Argument scanning is driven by the macro table's declared arity: optional `[...]` groups
then required `{...}` groups.

## The macro table

Four dispositions. This is the core of the design and the answer to "how do the two
fixtures agree without a pandoc special case".

| Disposition | Behaviour | Why |
|---|---|---|
| `SEMANTIC` | maps to a duck_block type | the reader's actual job |
| `TRANSPARENT` | drop the macro, **descend into its content argument** | wrappers that carry prose |
| `DROPPED` | drop the macro **and its arguments** | presentational or metadata |
| `TEXT` | expands to literal characters | escapes, symbols, dashes |

`TRANSPARENT` is what makes `pandoc.tex` work with no pandoc-specific rule:
`\hypertarget` is not a document construct, but we descend into its second argument, so
`\section` is found at whatever depth it happens to sit. Any future wrapper macro behaves
the same way for free.

### Sectioning

`\documentclass` is the one thing read out of the otherwise-discarded preamble, because
it decides the sectioning base:

| Class | Levels |
|---|---|
| `article` (and default, and fragments with no preamble) | `section`=1, `subsection`=2, `subsubsection`=3, `paragraph`=4, `subparagraph`=5 |
| `book`, `report` | `chapter`=1, `section`=2, `subsection`=3, `subsubsection`=4, `paragraph`=5, `subparagraph`=6 |

`\part` maps to 1 in both. It collides with the next level down when a document uses both,
which is rare and documented rather than solved — `heading_level` is capped at 6 by the
duck_block spec regardless.

### Initial table

**SEMANTIC — blocks/environments**

| LaTeX | duck_block |
|---|---|
| `itemize` | `list`, `list_type=bullet` |
| `enumerate` | `list`, `list_type=ordered` (plus `start`, `number_style`, `number_delim`) |
| `description` | **deferred** — see below |
| `\item` | `list_item` |
| `quote`, `quotation` | `blockquote` |
| `verbatim`, `lstlisting` | `code` (raw content) |

**These are containers and carry no content of their own** (duck_block 4.0). Their text
lives in a child at `level + 1`, and the container ends at the first element back at its
own level. WHICH child depends on how the source wrote it:

```
list          attrs list_type='bullet', ordered='false'
  list_item                              <- level+1, NO content
    plain      "bullet one"              <- level+2, a BARE run: \item text
  list_item
    paragraph  "para item"               <- level+2, the item held a real paragraph
blockquote
  paragraph    "A block quote."          <- level+1
```

`plain` is a block-level text run with **no paragraph semantics** — Pandoc's `Plain`
constructor. A bare `\item text` is exactly that, so it yields `plain`; an item whose
body is a genuine paragraph yields `paragraph`. The distinction is tight-versus-loose and
it is a property of the RUN, not of whether the item has block children: an item can hold
a nested list and still be tight. Collapsing the two loses information the source carried,
and panduck's EPUB reader learned this the expensive way.

Writing `list_item` with its text in `content` is a pre-4.0 shape and must not be emitted.

**`description` is DEFERRED, no longer held.** When this was written duck_block had no shape
for a definition list, so emitting `list_type='description'` would have invented a value no
consumer could read. That question is now SETTLED upstream:

```
list         list_type='definition'
  list_item  role='term'          > plain "term"
  list_item  role='definition'    > plain "the definition"
```

Zero new types — the extensibility argument for making `list_type` canonical over a boolean
`ordered`, made before anything needed it, and `dl` arrived the same day. So the shape exists
and this reader simply has not implemented it: `description` stays TRANSPARENT, its text
survives, and the work is scheduled rather than blocked. **Deferred and held are different
states and this document said the wrong one** — a reader would have concluded the question
was open.

These rules are scoped to **block** element_types; inline wrappers remain a documented gap
upstream, so the inline rules below are unaffected.

**SEMANTIC — inline**

| LaTeX | duck_block |
|---|---|
| `\textbf`, `\bf`, `\strong` | `bold` |
| `\emph`, `\textit`, `\it` | `italic` |
| `\underline`, `\uline` | `underline` |
| `\sout`, `\st` | `strikethrough` |
| `\texttt`, `\verb` | `code` |
| `\href{url}{text}`, `\url` | `link`, `href` attribute |
| `\includegraphics` | `image`, `src` attribute |
| `\footnote` | `note` |
| `\cite` | `cite` |
| `\textsc` | `smallcaps` |
| `\textsuperscript` / `\textsubscript` | `superscript` / `subscript` |
| `\\` | `linebreak` |

**TRANSPARENT:** `\hypertarget` (descend arg 2), `\texorpdfstring` (descend arg 1),
`\textnormal`, `\mbox`, `\text`, `\protect`, and the `center` and `abstract` environments.

**DROPPED:** `\label`, `\tightlist`, `\maketitle`, `\title`, `\author`, `\date`,
`\vspace`, `\hspace`, `\newpage`, `\clearpage`, `\noindent`, `\centering`, `\pagestyle`,
`\setlength`, `\index`, `\tableofcontents`, `\newcommand`, `\renewcommand`,
`\bibliography`, `\bibliographystyle`, plus everything in the preamble.

**DROPPED environments** (see asymmetry below): `tabular`, `tikzpicture`, `figure`,
`table`, `equation`, `align`, `displaymath`.

**TEXT:** `\ldots`/`\dots` → `…`; `\%`, `\$`, `\&`, `\#`, `\_`, `\{`, `\}` → the literal
character; `\textbackslash` → `\`; `\LaTeX` → `LaTeX`; `\TeX` → `TeX`.

### The unknown-input asymmetry

- **Unknown macro → `DROPPED` with its arguments.** Most unrecognised macros are
  presentational (`\vspace{2em}`), and descending would flood the output.
- **Unknown environment → `TRANSPARENT`.** Most unrecognised environments still wrap
  ordinary prose (`\begin{center}`), and dropping them would lose real content.

The explicit DROPPED-environment list above exists because that default is wrong for
exactly one family: descending into `tabular` or `tikzpicture` emits mangled cell and
coordinate text as prose. Tables are unread by every panduck reader today, so declining to
read them here is consistent rather than a new gap.

### Introspection — not exposed

An earlier draft exposed the table as `panduck_latex_macros()`, mirroring
`panduck_pandoc_ast_map()`, on the grounds that an allowlist nobody can enumerate is a
backstop with extra steps.

That argument does not survive scrutiny. A static table in a source file IS enumerable by
reading it; every entry that matters is exercised by the reader's own tests; and a table
function is a permanent public commitment no consumer has asked for. The comparison to
`panduck_pandoc_ast_map()` is also weaker than it looks — that table describes what
panduck maps for *other extensions* to consume, while this one is internal to one reader.

`panduck_latex_tokens()` IS exposed, for a reason that does not transfer: comment-eats-
newline, control-word whitespace and verbatim cannot be observed any other way, whereas a
wrong disposition shows up directly as a wrong block.

## Emission and nesting

Inlines start at `level = 1` and increment with each open formatting scope, matching
`epub_reader.cpp:651`.

Blocks and inlines are **separate scales**, and this is the trap worth stating outright: a
block's NULL `level` is not a depth that inlines count from. An inline at `level = 1` is a
top-level inline *inside* the block it follows, not a sibling of it. A consumer that
absorbs children by "level greater than mine" reads the two scales as one, and the symptom
is a spurious blank paragraph plus the inline run rendering as its own block —
`duckdb_markdown` hit exactly this and fixed it by having a block take the contiguous
inline run that follows it, using level only *among* inlines where it genuinely is the
nesting.

Top-level blocks carry **no** `level` (NULL), as every existing reader emits them.
Container blocks — `list`, `list_item`, `blockquote`, `div`, `section`, `figure`,
`caption` — own their children at `level + 1`, and the container ends at the first element
back at its own level. A container carries `content` only when its single child is a plain
text run; `list` never can, because its children are always `list_item`s. So a flat document is indistinguishable from what the other readers
produce, and depth appears only when the source actually has it — the same rule as for
inlines.

**Flat when flat, nested when nested.** The rule is already the duck_block convention,
stated in `duck_block_types.hpp`: *"Empty content => NULL (a formatting container that
recurses into structured child inlines carries no literal content of its own)."*

```
\textbf{x}                    \textbf{\emph{x}}

kind   type  content level    kind   type      content level
------ ----- ------- -----    ------ --------- ------- -----
inline bold  x         1      inline bold                1
                              inline italic              2
                              inline text      x         3
```

So the ordinary case is byte-identical to what the existing readers already produce, and
LaTeX differs only where those readers are lossy. `rtf_reader.cpp:77` states that
limitation outright — *"a run carrying several attributes is reported by its strongest
one … nested inline structure is a later refinement"* — because RTF and DOCX carry
formatting as a flag set. LaTeX's source is a tree, so flattening would discard the one
thing LaTeX states unambiguously.

**Consequence, accepted deliberately:** panduck becomes the first producer emitting
inlines at depth > 1. That is the shape which rendered as `****Doc` in `duckdb_markdown`'s
writer — a wrapper with empty content followed by its child, walked flat. That bug is real
and already known; never emitting the shape leaves it latent rather than absent.

**Math** is opaque: `$…$` and `\(…\)` → `math` with `attributes['display'] = 'inline'`;
`$$…$$` and `\[…\]` → `display = 'block'`. Content is the verbatim TeX. No math parsing.

This line previously said `mode=inline` / `mode=display`, which duck_block does not define —
its own spec gives `math` the attribute `display`, valued `inline` or `block`. A whole-branch
review flagged the shipped code as inventing an attribute, citing THIS document; the code was
right and this document was the stale one. Recorded because a spec that outlives its subject
is read as authority.

**Math is a TOKENIZER construct, not a macro** — it never reaches the disposition table.
That matters for `\(`, `\)`, `\[` and `\]`, which lex as control symbols: without an
explicit branch they fall through to the generic control-symbol case and the math
silently vanishes. pandoc PREFERS those spellings over `$`, so a reader handling only `$`
loses math on exactly the documents most likely to contain it.

**Preamble** is discarded up to `\begin{document}`, except `\documentclass`. With no
`\begin{document}`, the whole file is treated as body — handwritten fragments are common
and erroring on them would be unhelpful.

## Error handling

Malformed input degrades; it never throws.

- Unbalanced `{`: all open scopes close at EOF.
- Unterminated `\begin{…}`: the environment closes at EOF.
- Unterminated `verbatim` or `$`: closes at EOF, content kept.
- A `\end{x}` with no matching `\begin{x}`: ignored.

A reader that refuses a slightly-broken document is worse than one that returns most of
it, and TeX in the wild is frequently slightly broken.

## Testing

`test/sql/latex_reader.test`, following the shape of `epub_reader.test`.

**The central assertion is two-writer equivalence:** `handwritten.tex` and `pandoc.tex`
must yield the same logical document — same headings at the same levels, same prose, same
inline formatting. This is what the fixtures were built for, and it is what proves
`TRANSPARENT` works without a pandoc special case. It mirrors `doc_namespace.test`, which
already asserts that two formats agree on one table of contents.

Targeted tests for the things that fail silently:

| Test | Guards against |
|---|---|
| comment eats the newline | a leading space in every pandoc heading |
| `\maketitle` emits nothing | the two fixtures failing to match |
| preamble dropped entirely | `\usepackage` lines becoming paragraphs |
| `\textbf{\emph{x}}` depth | the nesting decision silently regressing to flat |
| `\textbf{x}` stays one flat run | over-nesting the common case |
| `---` → em-dash, UTF-8 `café` | encoding damage |
| unknown macro dropped, unknown environment descended | the asymmetry inverting |
| `tabular` dropped whole | mangled cell text as prose |
| unbalanced braces | a throw on real-world input |
| `\item text` yields `list_item` + `plain` at `level+2`, NOT `paragraph` | collapsing tight and loose |
| an item holding a real paragraph yields `paragraph`, and the two DIFFER | a rule keyed on "has block children", which makes them identical |
| `description` emits no `list_type` | inventing an unspecified attribute value |
| `blockquote` owns its `paragraph` at `level+1` | the container rule silently not applied |

A third fixture is needed for math, verbatim and malformed input: the existing two are a
*matched pair* and must not be edited, or the equivalence test stops meaning anything.

`make check-vocabulary`'s GAPS arm should be re-run after implementation — this reader
adds branches on `math`, `note`, `cite` and others currently listed as unhandled.

## Non-goals and follow-ups

1. **The other four readers are not retrofitted to nest.** LaTeX will emit depth they
   never produce. The inconsistency is real and left visible rather than expanding this
   task fourfold.
2. **One deliberately nested fixture, run through each consumer's writer.** Since panduck
   becomes the first producer of depth > 1, this is cheap insurance against the `****Doc`
   class of bug across the portfolio. Separate task.
3. **Accents** (`\'{e}` → `é`) are out. Both fixtures use literal UTF-8. A minimal accent
   table is a later addition.
4. **Tables** stay unread, consistent with every other panduck reader.
