# read_latex_blocks — design

**Status:** approved, not yet implemented
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
| `enumerate` | `list`, `list_type=ordered` |
| `description` | `list`, `list_type=description` |
| `\item` | `list_item` |
| `quote`, `quotation` | `blockquote` |
| `verbatim`, `lstlisting` | `code` (raw content) |

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

### Introspection

The table is exposed as `panduck_latex_macros()`, mirroring `panduck_pandoc_ast_map()`.
An allowlist's advantage over a backstop is that it can be audited; this codebase already
makes its coverage introspectable, and an allowlist nobody can enumerate is just a
backstop with extra steps.

## Emission and nesting

Inlines start at `level = 1` and increment with each open formatting scope, matching
`epub_reader.cpp:651`.

Blocks at the top level carry **no** `level` (NULL), as every existing reader emits them.
Blocks that genuinely nest — a `list` inside a `list_item`, a `quote` inside a `quote` —
carry their depth in `level`, which is both the duck_block container convention and what
`pandoc_ast_map.cpp:22` already records for `BlockQuote` ("nested blocks flattened, depth
carried in level"). So a flat document is indistinguishable from what the other readers
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

**Math** is opaque: `$…$` and `\(…\)` → `math` with `mode=inline`; `$$…$$` and `\[…\]` →
`math` with `mode=display`. Content is the verbatim TeX. No math parsing.

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
