# Readers

panduck has eight native readers — RTF, DOCX, ODT, EPUB, LaTeX, Org, RST and ipynb — plus
a reader for pandoc's own JSON AST that reaches every format pandoc can read. All were
written against **real writer output** rather than the format specification, and together
they say something no one of them said alone.

The three plain-text formats each turned on a rule that could not be guessed:

| | the rule, measured |
|---|---|
| **Org** | a leading `*` is a heading at column 0, a bullet when indented, emphasis mid-line — one character, three meanings |
| **RST** | heading level is set by the **order of first appearance** of the adornment character, not by which character it is. A reader hardcoding `= → 1` passes every conventional document |
| **ipynb** | a markdown cell is held **raw**: it is a document, not data, and parsing it here would violate the isolation that keeps `.toml` verbatim |

RST is also the only one of the eight with **no document metadata**: a `:Author:` field
list is a definition list, which is what pandoc makes of it too. The opposite reading is
the obvious one, and nothing in the vocabulary would object to a reader that got it wrong.

## How a format marks a heading predicts how badly writers disagree

Measured on files produced by pandoc and LibreOffice:

| | RTF | DOCX | ODT | EPUB |
|---|---|---|---|---|
| **pandoc** | `\outlinelevel0` | `w:pStyle w:val="Heading1"` | `text:h` `text:outline-level` | `<h1>` |
| **LibreOffice** | `{\stylesheet}` `\sN` | `w:outlineLvl w:val="0"` | `text:h` `text:outline-level` | *nothing* |

In **RTF and DOCX** the two mechanisms are mutually exclusive — a file uses one or the
other, never both — and the writers sit on **opposite sides in the two formats**. pandoc
is outline-based in RTF and style-based in DOCX; LibreOffice is the reverse. *No writer
agrees with itself across formats.* A reader supporting only one mechanism silently loses
every heading from the other.

In **ODT there is no disagreement at all**, and the reason generalises. ODF has a
*dedicated heading element* carrying an explicit level, so there is nothing for two
writers to disagree about. RTF and DOCX disagree precisely because they express a heading
as a **paragraph wearing a hat** — and a property can be attached more than one way.

So the rule is not "always expect two mechanisms". It is:

> Expect competing mechanisms wherever headings are a paragraph carrying a property.
> Expect none where the format has a real heading element.

**EPUB is the third case:** the format has a real heading element, and one writer declines
to use it. LibreOffice's EPUB export emits no semantic markup whatsoever — no `<h1>`, no
`<ul>`, no `<strong>` — only `<p class="paraN"><span class="spanN">`, with all meaning in
a CSS file. pandoc reading such a book finds no headings and no emphasis either.

## `read_rtf_blocks(path)`

RTF is 7-bit ASCII with brace groups and backslash control words, so this reader needs
neither `miniz` nor `pugixml`.

Handles headings, paragraphs, and inline bold / italic / underline / strikethrough;
decodes `\uNNNN` and `\'hh` escapes; skips destinations (`{\fonttbl}`, `{\colortbl}`,
`{\info}`, and any `{\*\…}` ignorable such as bookmarks).

!!! note "The `\ucN` fallback must be skipped at the source, not the output"
    RTF writes a Unicode character as `\uNNNN` followed by an ANSI fallback to discard.
    LibreOffice writes that fallback as `\'hh`, which decodes to *multi-byte* UTF-8 — so
    trimming decoded bytes splits the sequence and produces invalid UTF-8. A `\'hh` counts
    as **one character** against the skip.

**Not yet read:** lists (both writers emit `\bullet` plus a negative indent, which is
heuristic), tables, footnotes, and nested inline formatting (the `duck_block` inline
vocabulary is flat, so bold+italic reports as bold).

## `read_docx_blocks(path)`

DOCX is a ZIP whose body lives in `word/document.xml` — the first reader using both
vendored dependencies: `miniz` to open the archive, `pugixml` to parse the XML.
`word/styles.xml` resolves a `w:pStyle` id to a style name, so a localized "Heading 1"
still resolves.

!!! warning "`parse_ws_pcdata` is required, not a tuning knob"
    pandoc emits inter-word spacing as **separate runs whose only content is a space**
    (`<w:t xml:space="preserve"> </w:t>`). pugixml's default flags discard whitespace-only
    text nodes, so those runs come back empty and get skipped — welding `with` and `bold`
    into `withbold`.

    Nothing in the sqllogictests caught this: they assert headings, inline types and
    Unicode, all of which were already correct. It took comparing against an independent
    reader of the same bytes. See [Validation](validation.md).

**Not yet read:** tables, lists, images, footnotes.

## `read_odt_blocks(path)`

ODT is a ZIP whose body lives in `content.xml`, so it shares `ZipContainer` with DOCX and
`pugixml` with both. Headings come from `<text:h text:outline-level="N">` with no
ambiguity, per the table above.

Inline formatting resolves **indirectly**: a run names a style
(`<text:span text:style-name="T1">`) and `<office:automatic-styles>` carries the
properties (`fo:font-weight="bold"`). That is the ODF analogue of DOCX's `styles.xml`,
except the styles that matter are generated per document and live in `content.xml` itself.

!!! warning "Skipping a container can lose the words, not just the structure"
    ODF nests list content as `text:list > text:list-item > text:p`. An early version of
    this reader skipped `<text:list>` wholesale, the way it still skips tables — which
    lost the **text**, not merely the list structure. Lists are now flattened to
    paragraphs: the structure stays unmodelled, every word survives.

    Losing structure is a gap; losing text is a bug. Only the differential validator's
    *text* level distinguishes them, and it is what caught this.

**Not yet read:** list structure, tables, images, footnotes.

## `read_epub_blocks(path)`

A book is a ZIP with three levels of indirection before any text:

```
META-INF/container.xml            the only path the format fixes
  -> <rootfile full-path=…/>      the OPF package document
    -> <manifest><item id href/>  id -> file
    -> <spine><itemref idref/>    READING ORDER
```

**The spine is why this cannot be "read every `.xhtml` member".** ZIP member order is
arbitrary and the manifest is a set; the spine is the only statement of the order a human
reads the book in. Neither pandoc's nor LibreOffice's output can test that — both emit
single-chapter books whose member order happens to match their spine — so
`test/fixtures/spine_order.epub` is built by hand to store its chapters backwards.

!!! success "EPUB needs no HTML parser, and that is what made it cheap"
    The open question when EPUB was scheduled was how much to delegate to `duckdb_webbed`,
    since EPUB content documents are "HTML". They are not: the specification requires
    **XHTML**, which is well-formed XML by definition, so the same `pugixml` that reads
    DOCX and ODT reads a book directly. Delegating would have bought nothing and added a
    load-time dependency for a format that does not need one.

    An arbitrary `.html` file still routes to `duckdb_webbed`, because arbitrary HTML is
    not XML. An EPUB's content documents always are.

EPUB is the first format panduck models **structurally** — XHTML says `<ul><li>` and
`<blockquote>` outright, where the other three only had paragraphs wearing hats — so it
emits `list_item`, `blockquote` and `div` blocks alongside headings and paragraphs.

### Where CSS is read, and where it is not

LibreOffice's export leaves no alternative: without resolving CSS classes, its books come
back as undifferentiated paragraphs. The line is drawn at whether a declaration **names**
the formatting or merely **implies** it:

| Declaration | | Why |
|---|---|---|
| `font-weight: bold` (or `≥ 600`) | **read** | "bold" is the answer, not evidence for it |
| `font-style: italic` | **read** | likewise |
| `text-decoration: line-through` / `underline` | **read** | likewise |
| `font-size: 16pt` | **ignored** | evidence that this *might* be a heading — and one emphasised paragraph in a document with no headings looks identical |

So a LibreOffice book comes back with its **formatting** recovered (which pandoc does not
do) and its **headings** absent (which pandoc also reports). The heading information is
genuinely not in the document; only a font size is.

Only single-class selectors (`.name`, `p.name`) are read. Descendant combinators, ids and
`@media` need a real cascade to be correct, and being half-right about a cascade is worse
than ignoring the rule.

### Links, images, and paths

`<img src>` is resolved to an **archive member** — a consumer extracting the image needs
that, and `../images/cover.png` is meaningless without knowing which chapter said it.
`<a href>` is deliberately left **as authored**: a link may point outside the book, and
carries a `#fragment` that resolution would drop.

!!! note "panduck reads a legal EPUB that pandoc rejects"
    Manifest hrefs are relative to the package document, so a book whose `.opf` sits in a
    subdirectory climbs out with `../`. pandoc concatenates without normalising and fails
    outright: `No entry on path: OEBPS/pkg/../text/ch1.xhtml`. This is why
    `spine_order.epub` is a panduck-only unit fixture rather than a differential one.

**Not yet read:** tables, footnotes, nested list depth, `toc.ncx` / `nav.xhtml` metadata.

## `read_latex_blocks(path)`

LaTeX has no document model to parse against — it is a macro language, and two writers
producing "the same document" agree on almost none of the bytes. So this reader is not a
grammar over a fixed set of constructs; it is a **tokenizer** (control words, control
symbols, brace groups, math shifts, comments, verbatim as a lexical mode) plus a **macro
DISPOSITION table** that says, for each macro or environment name, what its bytes mean:

| Disposition | Meaning |
|---|---|
| `SEMANTIC` | emits a `duck_block` element — `\textbf` -> bold, `\section` -> heading |
| `TRANSPARENT` | the macro vanishes, its content argument is read as if the macro were never there |
| `DROPPED` | the macro AND its arguments vanish — `\maketitle`, `\label`, presentational noise |
| `TEXT` | expands to literal characters — `\LaTeX`, `\ldots` |

!!! success "TRANSPARENT is what makes a pandoc file and a handwritten one read alike"
    pandoc buries every heading two brace levels deep:
    `\hypertarget{id}{\section{Heading One}\label{id}}`. A person just writes `\section{Heading
    One}`. A reader with a pandoc-specific branch would learn nothing general; reading
    `\hypertarget` as TRANSPARENT — drop the macro, descend into its content argument —
    reads both, because that is what `\hypertarget` *means*, not what pandoc happens to
    emit. `test/fixtures/handwritten.tex` and `test/fixtures/pandoc.tex` are the same
    document written twice, and the sqllogictest asserts they yield identical headings.

Sectioning is ranked against `\documentclass`: `\section` is heading level 1 in `article`
but level 2 in `book`/`report`, where `\chapter` takes level 1. Lists are `bullet`
(`itemize`) or `ordered` (`enumerate`), with the tight/loose distinction resolved at
`\end{...}` — tight iff no blank line ever separated two items, per Pandoc's own rule.
Inline formatting nests genuinely (`\textbf{\emph{x}}` is bold containing italic, not a
single flattened run), unlike RTF's flat inline vocabulary.

TeX's five **ligatures** are resolved in the tokenizer: `---` and `--` to em and en dashes,
` `` ` and `''` to curly double quotes, and `~` to a no-break space. This is not cosmetic —
pandoc spells every quotation mark and every unbreakable space that way, so without it the
same sentence read from `.tex` and from `.docx` would differ in its punctuation. A lone
`` ` `` or `'` is left alone, because `'` is also how English spells an apostrophe. None of
the five run inside math or verbatim, whose bodies are cut out as raw bytes before the
ligature rules ever see them.

**Math is read opaque.** `$..$`, `\(..\)` (inline) and `$$..$$`, `\[..\]` (display) all
become a `math` inline with `attributes['display']` set accordingly, but the TeX between
the shifts is carried verbatim as `content` — never parsed, never macro-expanded. There is
no `duck_block` shape for a formula's internal structure, so claiming to read one would be
a falsehood about what the reader actually does with it; the alternative (dropping math
silently) would lose a citation-bearing equation's text outright, which parsing a heading
wrong does not.

!!! note "Malformed input degrades, it does not throw"
    An unclosed brace closes at end of input; an unterminated environment resolves the
    same way a properly closed one does; a stray `\end` closes nothing unless it names
    `document`, which ends the document from wherever it is found. A reader that refuses a
    slightly-broken source is worse than one that returns most of it, and real-world TeX is
    frequently slightly broken. `test/fixtures/edge_cases.tex` carries these cases —
    together with math and verbatim — because they cannot be added to the matched
    handwritten/pandoc pair without breaking the equivalence assertion.

**Not yet read:** tables, `\newcommand` expansion (a user macro is read as an unclaimed
name — dropped, its argument left as text — never expanded against its definition).

## `read_org_blocks(path)`

`#+TITLE:`, `#+AUTHOR:` and `#+DATE:` become `kind='value'` rows appended after the blocks.
Keys are **pandoc's** namespace — `title`, `author`, `date` — not the source spelling, the
same rule that turns `dc:title` into `title` for EPUB.

**Repeated `#+AUTHOR:` lines concatenate into ONE space-joined value.** Measured: pandoc
emits a single `MetaInlines`, not a `MetaList`. LaTeX's `\author` *does* yield a `MetaList`,
so the same logical field has two shapes in two formats and both are pandoc's. Generalising
from one to the other is wrong in a way no fixture written by one author would reveal.

`=code=` and `~verbatim~` both map to `code`, and the direction is worth recording because
it is the opposite of what the names suggest: `=code=` yields pandoc `Code` with class
`["verbatim"]`, `~verbatim~` yields `Code` with none. duck_block's `code` has no class
field and the distinction is a spelling difference, so the collapse is declared in the
roundtrip ledger rather than modelled.

`:PROPERTIES:` drawers, TODO keywords and tags are Org's *agenda* layer, not document
structure, and are **dropped**. That word is load-bearing: the reader shipped a defect
where scoped-out drawers fell through to plain text and joined the following paragraph.
Losing structure is a gap; emitting non-content as prose is a bug.

## `read_rst_blocks(path)`

**Heading level is set by the ORDER of first appearance of the adornment character, not by
which character it is.** A document opening with `~~~~~` has `~` as level 1. A reader
hardcoding `= → 1, - → 2` is right on every conventional document and wrong on a valid one,
so the fixture pair includes one that does not follow the convention — without it the rule
is untested and the wrong reader passes.

A field list is **not metadata**:

```rst
:Author: A. Writer
```

becomes a definition list, which is what pandoc makes of it too. RST is the only one of the
eight formats with no document metadata at all.

Directives are an open set — docutils ships dozens, Sphinx hundreds — so the reader cannot
enumerate them. `.. code-block:: python` becomes `code` with `language`; everything else
becomes `div` carrying the directive name in `attributes['source_type']`, where it is
visible as a gap rather than silently private. **The body is descended into, never dropped**:
a directive body is prose, and the one exception is `code-block`, whose body is its content.

## `read_ipynb_blocks(path)`

Every cell is a `div` carrying `attributes['source_type']` = its `cell_type`. Cell
boundaries are structure a consumer needs — "which cell produced this" is the question
notebooks exist to answer — so they are not flattened away.

**A code cell's outputs are content.** A notebook read without its outputs is a script.
`stream` output takes `text`; `execute_result` and `display_data` take `text/plain` from the
MIME bundle, the one every producer writes and the only one that is text rather than an
encoded image.

**A markdown cell is held RAW** (`raw` + `encoding='markdown'`), and that is a deferral, not
a resting place. A markdown cell contains a *document* — it would be duck_blocks — which
makes it a different case from a whole-file `.toml` blob, where verbatim is the correct and
final answer. It is raw here because delegating would make this reader's output depend on
which extensions happen to be installed: panduck's delegation lives in the SQL dispatch
layer, and a C++ reader cannot reach those functions. One consistent behaviour beats two
that vary by environment. A consumer wanting blocks calls `md_to_blocks()` on the content
today; a post-parse helper for embedded formats discharges it later.

Notebook metadata **exceeds pandoc deliberately**: pandoc puts the whole thing into one
opaque `jupyter` MetaMap, so asking "who wrote this" means walking a blob. Each recovered
field carries `attributes['source_type']` with its original path, keeping a format-derived
field distinguishable from a pandoc-derived one.

## `read_pandoc_blocks(path)` — every format pandoc reads

`json` is one of pandoc's own formats and it *is* the Pandoc AST, so this is not a ninth
format alongside the eight:

```bash
pandoc -f org -t json input.org > ast.json
```
```sql
SELECT * FROM read_pandoc_blocks('ast.json');
SELECT * FROM read_pandoc_blocks_string(?);
```

All **43** of pandoc's input formats become reachable with no per-format code, for anyone
who has pandoc installed, while the native readers keep serving people who do not. That is
what "compatible with pandoc's data model, not its ABI" cashes out to.

It does **not** make pandoc a dependency, and `.json` does not auto-route here — see
[Dispatch](dispatch.md).

## Output shape

Every reader emits the canonical `duck_block` schema, in document order:

```
kind  element_type  content  level  encoding  attributes  element_order
```

- Heading level lives in `attributes['heading_level']`, **not** in `level` — `level` is
  hierarchy depth. This matches `pandoc_ast_to_blocks` exactly, which is what
  `duck_block_utils`' section slicing is built against.
- A **text-only** run flattens into `content` with no inline children; a run carrying
  formatting emits `content = NULL` plus `kind='inline'` children.
- A **heading always flattens**, even though both writers bold heading text — the bold
  belongs to the heading style, and a NULL there would empty any table of contents built
  from `content`.

Because the shape is canonical, `read_panduck_doc` passes native readers straight through
with `SELECT *` — no column list to transpose, no struct round trip.
