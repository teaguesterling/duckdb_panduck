# Readers

panduck has four native readers — RTF, DOCX, ODT and EPUB. All four were written against
**real writer output** rather than the format specification, and the four together say
something the first two alone did not.

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
