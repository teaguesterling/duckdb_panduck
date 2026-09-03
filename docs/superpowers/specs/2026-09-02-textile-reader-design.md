# `read_textile_blocks` — Textile Reader Design

**Status:** proposed
**Why now:** one of the two formats between panduck and duckeye's stated v1 endstate,
*"panduck replaces the pandoc dependency."* Nine of twelve removes no dependency — it makes
the same dependency cover less — so textile and `man` are the last two steps of a goal
rather than extras. Textile first: it is a closed lightweight-markup grammar of the shape
Org, RST and MediaWiki have already worn smooth, where `man` is roff, a macro language
closer to the LaTeX reader's problem.

Measured against pandoc 3.1.3 and against **python3-textile 4.0.2**, the format's reference
implementation, on 2026-09-02. Where the two disagree, that is said explicitly and the
reference wins.

## No new vocabulary, and no document metadata

Textile has an unusually wide inline set, and **every member of it already exists**:

| textile | duck_block |
|---|---|
| `*strong*`, `**bold**` | `bold` — both, see below |
| `_em_`, `__italic__` | `italic` — both |
| `@code@` | `code` |
| `-del-` | `strikethrough` |
| `+ins+` | `underline` |
| `^sup^` / `~sub~` | `superscript` / `subscript` |
| `??cite??` | `cite` |
| `%span%` | `span` |
| `"text":url` | `link` |
| `!img.png!` | `image` |

No vocabulary question is opened. That was the argument for doing Org first and it has now
held for four formats running.

**`*` and `**` both become `bold`; `_` and `__` both become `italic`.** Measured: pandoc
yields `Strong` for both and `Emph` for both. Textile's distinction is HTML's `<strong>`
versus `<b>` — presentational emphasis versus semantic — and duck_block has one `bold`. The
collapse is declared in the roundtrip ledger rather than modelled, exactly as Org's
`=code=`/`~verbatim~` pair was.

**`meta` is empty on every input tried.** Textile is the **third** format after RST and
MediaWiki with no document metadata, so `read_textile_blocks` emits no `kind='value'` rows.

## Construct mapping

### Block level

Textile blocks are `marker.` at line start, optionally carrying attributes:
`p{color:red}.`, `h2(class).`, `bq[fr].`, plus alignment `<`, `>`, `=`, `<>`.

| textile | duck_block |
|---|---|
| `h1.` … `h6.` | `heading` + `heading_level`, plus a slugified `id` — pandoc emits `top-heading`, and `doc_toc` needs it |
| `p.` or a bare paragraph | `paragraph` |
| `bq.` | `blockquote` |
| `pre.` and `bc.` | `code` — measured: pandoc yields `CodeBlock` for both, with no language |
| `* x`, `** x` | `list` `list_type='bullet'`, nesting by marker RUN LENGTH, as MediaWiki |
| `# x`, `## x` | `list` `list_type='ordered'` + `start`/`number_style`/`number_delim` |
| `- term := definition` | `list` `list_type='definition'` > `list_item` `role='term'`/`'definition'` |
| `\|_. H \|` / `\| a \|` | `table`, native `{headers, rows}`, `encoding='json'`; `_.` marks a header cell |
| `###. comment` | dropped — a comment is not content |
| `notextile.` | `raw`, `format='html'` — **divergence, below** |
| block attributes (`{style}`, `(class)`) | `attributes['style']` / `attributes['class']`; pandoc wraps in a `Div` and panduck keeps the block's own type |

**A leading space is NOT preformatted**, which is worth stating because MediaWiki's is and
these two readers sit next to each other. Measured against python-textile: ` indented line`
renders `<p>indented line</p>`. Nothing special.

## Where this diverges from pandoc

The same rule that decided MediaWiki's: **mirror pandoc's deliberate choices; diverge where
pandoc leaked non-prose into prose.** Both rows below are the second kind, and both are
settled against the reference implementation rather than argued.

### 1. A list adjacent to a different list type is still a list

```textile
* bullet
# ordered
```

| | |
|---|---|
| python-textile | `<ul><li>bullet</li>` … `<ol><li>ordered</li></ol>` — a list |
| pandoc | `Para [Str "*", Space, Str "bullet"]` then `OrderedList` — **the bullet list is gone**, its marker left as a literal asterisk |
| panduck | two sibling lists |

Pandoc's reading loses a list and puts a stray `*` in the document's prose. That is the
leaked-as-prose failure, and the same one the Org drawers and MediaWiki's `<span>` anchors
each produced.

**panduck emits SIBLING lists where the reference NESTS the second inside the first.** This
is a deliberate departure from the reference, on the grounds that its own output here is
invalid HTML — it places the `<ol>` as a direct child of the `<ul>` rather than inside an
`<li>` — so it is a quirk of that implementation rather than a statement about the format.
Two sibling lists lose nothing and describe what the author wrote.

### 2. `notextile.` means "do not process this", and pandoc processes it

| | |
|---|---|
| python-textile | marker stripped, body passed through verbatim |
| pandoc | `Para [Str "notextile.", …]` — the marker kept as **prose**, and the body parsed as textile anyway |
| panduck | `raw`, `format='html'`, marker consumed |

Measured: `notextile. <b>raw</b>` gives pandoc a paragraph whose first word is the literal
string "notextile." followed by `RawInline` html for the tags — so pandoc both fails to
honour the block and advertises the marker to the reader. The whole purpose of the
construct is to not be processed.

**The ledger therefore has two rows**, both "reference-wrong", both measured against
python-textile. `%span%` is a third candidate — pandoc drops it to plain text where the
reference emits `<span>` and duck_block has `INLINE_SPAN` — but it is *additive*: emitting
`span` costs pandoc nothing it had, so it earns no row.

## Architecture

`src/textile_scanner.{cpp,hpp}` and `src/textile_reader.{cpp,hpp}`, mirroring Org, RST and
MediaWiki — including the seam: **no DuckDB types outside the trailing emission section**,
verified rather than intended. All three existing scanners have zero DuckDB references.

Textile needs **no brace balancing**, which makes it simpler than MediaWiki: there is no
construct whose interior changes how a later line is classified. It is a pure line scanner
with one wrinkle — a block marker's `attributes` (`{...}`, `(...)`, `[...]`) sit between the
marker and its `.`, so the scanner parses the marker head rather than matching a fixed
prefix.

## Testing

- `test/sql/textile_reader.test`, structured like the other three: most assertions through
  `read_textile_blocks_string`, plus a fixture for path and encoding behaviour.
- **A fixture pair**, hand-written and `pandoc -f textile -t textile`. The generated one has
  earned its place three times now — `:PROPERTIES:` drawers in Org, adornment normalisation
  in RST, `<span id>` anchors in MediaWiki — each a construct a person never types and a
  writer always emits.
- Both fixtures join `make check-conformance` (glob by extension) and
  `check-writeback`; the write-back arm gains textile's table and definition list, since
  the fixture sweep is a coverage claim that has to be checked rather than assumed.
- Added to `test/roundtrip/check_roundtrip.py` with the two declared divergences and no
  others.

## Out of scope, stated rather than silent

Footnote *bodies* (`fn1.`) are read as ordinary paragraphs — pandoc resolves a `[1]`
reference into an inline `Note` and panduck emits the `note` inline for the reference, but
the reference-to-body linkage is a cross-block mechanism this reader does not build.
Also out: `==escaping==`, acronym expansion `ABC(Always Be Closing)`, `|=. caption` table
captions, alignment modifiers, and `p<>.` justification.

Every one is **dropped or held raw, never leaked as prose** — the rule the Org reader paid
for and the one both divergences above exist to honour.
