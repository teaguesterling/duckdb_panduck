# Readers

panduck has two native readers. Both were written against **real writer output** rather
than the format specification, and both found the same surprise.

## Headings come from two mechanisms, and writers disagree

Measured on files produced by pandoc and LibreOffice:

| | RTF | DOCX |
|---|---|---|
| **pandoc** | `\outlinelevel0` | `w:pStyle w:val="Heading1"` |
| **LibreOffice** | `{\stylesheet}` `\sN` | `w:outlineLvl w:val="0"` |

The mechanisms are **mutually exclusive** — a file uses one or the other, never both — and
the writers sit on **opposite sides in the two formats**. pandoc is outline-based in RTF
and style-based in DOCX; LibreOffice is the reverse. *No writer agrees with itself across
formats.*

A reader supporting only one mechanism silently loses every heading from the other. Both
readers implement both paths, and the test suite asserts each independently against a
fixture that exercises only that path.

Treat "one heading mechanism" as an unsafe assumption when adding a format, not something
to discover afterwards.

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
