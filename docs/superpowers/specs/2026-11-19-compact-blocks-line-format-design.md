# Compact duck_blocks line format (V1)

**Status:** design, not implemented. Written 2026-11-19. Decisions by Teague; measurements by
Claude Opus 5.5.

**Goal:** a line format for duck_blocks rows that is small, has a fixed schema, and reads back
into the duck_blocks schema with plain DuckDB `read_json` and no decoder written in code.

Teague's brief: "compressed, omit null fields, line delimited, fixed schema, one letter labels
and types. Order implicit and preserved."

---

## 1. The result, measured

Corpus: 16 fixtures from `test/fixtures/` in 9 formats, read by the native panduck readers.
"concat zstd" is all 16 files joined and compressed as one stream with `zstd -19`.

| Format | Raw bytes | vs pandoc | concat zstd | vs pandoc |
|---|---|---|---|---|
| pandoc 3.1.3 JSON (`pandoc -t json`) | 41,103 | 1.00 | 2,961 | 1.00 |
| duck_blocks JSONL today (`COPY ... (FORMAT json)`) | 68,926 | 1.68 | 4,048 | 1.37 |
| **V1 (this design)** | **21,435** | **0.52** | **2,694** | **0.91** |

The corpus is small (about 41 KB of pandoc JSON), so treat the compressed ratio as a direction.

Most of the lead over pandoc JSON comes from the panduck readers, not from the format. On
identical content (rows that `read_pandoc_blocks` made from pandoc's own JSON), V1 is 0.98 of
pandoc JSON raw and 1.02 with compression: a tie.

Leaving fields out does most of the work. Omission alone (long keys kept) takes today's JSONL
from 1.68 to 1.09 raw. The short codes take it to 0.52.

**Round trip (measured):** all 1,475 rows in 32 files (16 fixtures, two readers each) read back
exactly through the SQL in section 6. A row changed on purpose was caught.

## 2. The line

One JSON object per line, one line per row, in `element_order` order. There is no header
line, so each line is a complete row.

| Field | duck_blocks column | When it is written |
|---|---|---|
| `t` | `kind` + `element_type` | Always. A type code from section 3 |
| `c` | `content` | When not NULL. `""` is written as `""`, so NULL and empty stay distinct |
| `l` | `level` | Always |
| `e` | `encoding` | When not `text`. `json` is written as `j` |
| `a` | `attributes`, known keys | When the row has a known key. A STRUCT, codes in section 4 |
| `m` | `attributes`, other keys | When the row has a key not in section 4. A MAP, keys verbatim |

`element_order` is not written. The line number, from 0, is the order.

```
{"t":"h","c":"Heading Two","l":1,"a":{"h":"2","i":"heading-two"}}
{"t":"p","c":"","l":1}
{"t":"x","c":"plain","l":2}
{"t":"g","l":3,"a":{"u":"cat.png"},"m":{"align":"center","width":"200px"}}
```

**V1 does not carry NULL `level`, `encoding` or `attributes`.** A writer that meets one
must stop with an error, not write a default. In the corpus, none of the 1,475 rows has one
(measured). If a producer needs them later, that is a format change.

## 3. Type codes

Type codes are values, not column names, so case is significant: `C` is a code block and
`c` is inline code.

| Code | Type | Code | Type | Code | Type |
|---|---|---|---|---|---|
| `h` | block heading | `x` | inline text | `v` | inline subscript |
| `p` | block paragraph | `S` | inline space | `u` | inline superscript |
| `i` | block list_item | `b` | inline bold | `z` | inline cite |
| `l` | block list | `e` | inline italic | `w` | inline raw |
| `q` | block blockquote | `c` | inline code | `o` | inline caption |
| `C` | block code | `k` | inline strikethrough | `y` | inline span |
| `d` | block div | `a` | inline link | `B` | inline linebreak |
| `T` | block table | `N` | inline note | `j` | inline softbreak |
| `R` | block raw | `g` | inline image | `V` | value inlines |
| `r` | block hr | `s` | block section | `L` | value list |
| `f` | block figure | `P` | block plain | `Z` | value string |
| `n` | block caption | | | `M` | value map |

A type that is not in the table is written as `kind:element_type`, for example
`"t":"block:deflist"`. No code contains `:`, so the reader can tell the two apart. The
corpus did not need this escape (measured).

## 4. Attribute codes, and why there are two attribute fields

`a` is a STRUCT with one optional field per known key. `m` is a `MAP(VARCHAR, VARCHAR)` that
holds every other key unchanged. A row can have both.

The set of attribute keys is open. Since #195, an RST directive option becomes an attribute
under its own name (`:width:` becomes `width`). Separate fields mean an unknown key can never
collide with a short code, so no escape rule is needed. Teague proposed the split.

| Code | Key | Code | Key | Code | Key |
|---|---|---|---|---|---|
| `c` | class | `k` | key | `n` | start |
| `i` | id | `r` | role | `m` | template_name |
| `s` | source_type | `w` | href | `a` | alt |
| `h` | heading_level | `f` | format | `l` | link_type |
| `t` | list_type | `u` | src | `p` | pandoc_ast |
| `o` | ordered | `g` | language | `x` | execution_count |
| `d` | number_delim | `y` | number_style | `e` | title |

These 21 keys are every key in the corpus (measured). The STRUCT codes are separate from the
row field names, so `a.m` (template_name) does not clash with the row field `m`.

**Codes must be unique when case is ignored (measured).** DuckDB treats identifiers without
regard to case. `read_json` refuses `a` and `A` as two columns ("Duplicate named argument").
Inside a STRUCT, `H` and `h` also become one field. So the unknown-key field is `m`, not `A`.
Three keys got new letters from the subagent's first table (the choice of letters is Claude's
call): `heading_level` `H` → `h`, `href` `h` → `w`, `title` `T` → `e`.

**A heading's rank stays an attribute.** `level` is depth in the tree. `heading_level` is the
rank (H1 to H6). The two differ in 18 of 31 headings (measured). Most readers put every heading
at level 1. EPUB nests headings in sections, so `Heading One` (H1) is at level 2. Neither value
can be derived from the other, so V1 writes both. Folding the rank into the type code (`h2`)
was rejected: it saves about 8 bytes per heading and adds a special rule to the reader.

Adding a key to section 4 changes the format. A file written with a longer table puts that
key in `a`, where an older reader does not look for it. Until then, a new key goes in `m`, and
every reader keeps it.

Not every key has a `DuckBlockTypes::ATTR_*` constant (`class`, `href` and `src` do not). So
the table cannot be generated from the constants alone. This document is the source of the
table.

## 5. Order: the line number

The reader numbers rows 0, 1, 2 ... in line order. The writer sorts rows by `element_order`,
and the numbers it had are not kept.

That is lossless only if `element_order` is dense. The native readers are dense on all 16
fixtures (measured). `read_pandoc_blocks` is not: 47 numbers are missing across 11 files.
The cause is that `CollapseLonePlainIntoParent` deletes a folded `plain` row without
renumbering (`src/include/duck_block_normalize.hpp:142`). That is a bug, filed as #206. It is
not information the format must keep.

## 6. Reading it in DuckDB

```sql
CREATE OR REPLACE TEMP MACRO read_compact_blocks(f) AS TABLE
WITH src AS (
  SELECT (row_number() OVER () - 1)::INTEGER AS element_order, *,
         coalesce(MAP {'h': 'block:heading', 'p': 'block:paragraph', 'i': 'block:list_item',
           'l': 'block:list', 'q': 'block:blockquote', 'C': 'block:code', 'd': 'block:div',
           'T': 'block:table', 'R': 'block:raw', 'r': 'block:hr', 'f': 'block:figure',
           's': 'block:section', 'P': 'block:plain', 'n': 'block:caption',
           'x': 'inline:text', 'S': 'inline:space', 'b': 'inline:bold', 'e': 'inline:italic',
           'c': 'inline:code', 'k': 'inline:strikethrough', 'a': 'inline:link',
           'N': 'inline:note', 'g': 'inline:image', 'v': 'inline:subscript',
           'u': 'inline:superscript', 'z': 'inline:cite', 'w': 'inline:raw',
           'o': 'inline:caption', 'y': 'inline:span', 'B': 'inline:linebreak',
           'j': 'inline:softbreak', 'V': 'value:inlines', 'L': 'value:list',
           'Z': 'value:string', 'M': 'value:map'}[t], t) AS kt
  FROM read_json(f, format := 'newline_delimited', columns := {
    t: 'VARCHAR', c: 'VARCHAR', l: 'INTEGER', e: 'VARCHAR',
    a: 'STRUCT(c VARCHAR, i VARCHAR, s VARCHAR, h VARCHAR, t VARCHAR, o VARCHAR,
               k VARCHAR, r VARCHAR, w VARCHAR, f VARCHAR, u VARCHAR, g VARCHAR,
               d VARCHAR, y VARCHAR, n VARCHAR, m VARCHAR, a VARCHAR, l VARCHAR,
               p VARCHAR, x VARCHAR, e VARCHAR)',
    m: 'MAP(VARCHAR, VARCHAR)'})
)
SELECT split_part(kt, ':', 1) AS kind,
       split_part(kt, ':', 2) AS element_type,
       c AS content,
       l AS level,
       CASE coalesce(e, 't') WHEN 't' THEN 'text' WHEN 'j' THEN 'json' ELSE e END AS encoding,
       map_from_entries(list_sort(map_entries(map_concat(
         coalesce(m, MAP {}::MAP(VARCHAR, VARCHAR)),
         map_from_entries(list_filter([
           {k: 'class', v: a.c}, {k: 'id', v: a.i}, {k: 'source_type', v: a.s},
           {k: 'heading_level', v: a.h}, {k: 'list_type', v: a.t}, {k: 'ordered', v: a.o},
           {k: 'key', v: a.k}, {k: 'role', v: a.r}, {k: 'href', v: a.w},
           {k: 'format', v: a.f}, {k: 'src', v: a.u}, {k: 'language', v: a.g},
           {k: 'number_delim', v: a.d}, {k: 'number_style', v: a.y}, {k: 'start', v: a.n},
           {k: 'template_name', v: a.m}, {k: 'alt', v: a.a}, {k: 'link_type', v: a.l},
           {k: 'pandoc_ast', v: a.p}, {k: 'execution_count', v: a.x}, {k: 'title', v: a.e}],
           lambda x: x.v IS NOT NULL)))))) AS attributes,
       element_order
FROM src ORDER BY element_order;

FROM read_compact_blocks('doc.v1.jsonl');
```

The reader sorts attribute keys, because `map_concat` does not keep the source order. Every
source map in the corpus was already sorted by key (measured by the subagent, 178 of 178), so
the sort gives back the source order.

`row_number() OVER ()` takes the order in which `read_json` returns lines. That order is the
file order with `preserve_insertion_order` on, which is DuckDB's default. This is inferred
from the round trip, not from a test with many threads or a large file.

## 7. What was tried and rejected

| Variant | Raw vs pandoc | concat zstd vs pandoc | Why rejected |
|---|---|---|---|
| V0: omit only, long keys | 1.09 | 1.02 | Kept as evidence that omission does most of the work |
| V2: V1 + default rules (predicted level, sparse `o`, `h2`, table → `json`) | 0.45 | 0.89 | 2% smaller compressed, needs a decoder in code; larger than V1 on identical content |
| V3: V2 as a positional array | 0.37 | 0.87 | Same, and lines no longer name their fields |

## 8. Side findings in `read_pandoc_blocks` (measured, not filed)

- It writes one row per `Str` and per `Space` token.
- Each table is stored twice, as rows and in a `pandoc_ast` attribute (6.9 KB across the corpus).
- It drops an image's width and height.
- It drops the trailing newline from code blocks.
- It drops a link's classes and key-value attributes: `[link](u){.big data-foo=bar}` gives only
  `{href=u}`.

## 9. Left to the builder

- Where the writer and reader live: a `COPY ... (FORMAT compact_blocks)` function, two macros,
  or a duck_block_utils function shared by the fleet.
- Whether the format carries a version, and where, given that there is no header line.
- Behaviour on an unknown `t` code without `:`. The SQL above returns it as `kind`, with an
  empty `element_type`. A real reader should refuse it.
- Encodings other than `text` and `json` are written as they are. `t` and `j` are then
  reserved and cannot be used as encoding names.

---

The encoder scripts and per-file tables from the measurement were in a session scratchpad and
are not kept in the repo. Section 2 specifies the writer completely enough to rebuild them.
Section 6 is the exact reader used for the round trip.
