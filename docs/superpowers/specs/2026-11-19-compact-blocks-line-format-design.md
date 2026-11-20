# Compact duck_blocks line format (V1)

**Status:** implemented in duck_block_utils (teaguesterling/duckdb_duck_block_utils#86) as
`duck_blocks_to_compact`, `duck_blocks_from_compact` and `read_compact_blocks`. Written
2026-11-19. Decisions by Teague; measurements by Claude Opus 5.5.

**The source of the format is now duck_block_utils:** the macro text in its
`src/doc_macros.cpp` holds both code tables, and its `docs/api.md` ("Compact Line Format")
documents them. Where this note and duck_block_utils disagree, duck_block_utils is right.
This note keeps the design reasons and the measurements.

**Goal:** a line format for duck_blocks rows that is small, has a fixed schema, and reads back
into the duck_blocks schema with plain DuckDB SQL and no decoder written in code.

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
exactly through the draft `read_json` reader. A row changed on purpose was caught. The shipped
reader in duck_block_utils round-trips the three fixtures it keeps (section 6).

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
the table cannot be generated from the constants alone. The macro text in duck_block_utils'
`src/doc_macros.cpp` is the source of the table.

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
LOAD duck_block_utils;

-- a list of duck_blocks to compact text, and back
SELECT duck_blocks_to_compact(blocks) FROM doc;
SELECT duck_blocks_from_compact(text) FROM compact_doc;

-- write a file, then read it (or a glob of them) as rows
COPY (SELECT rtrim(duck_blocks_to_compact(blocks), chr(10)) FROM doc)
TO 'doc.compact.jsonl' (FORMAT csv, HEADER false, QUOTE '', ESCAPE '', DELIMITER E'\x01');

FROM read_compact_blocks('doc.compact.jsonl');
```

`read_compact_blocks` returns the seven canonical fields and then `filename` (spec 6.4), ordered
by `filename` and `element_order`.

**What changed from the draft reader in this section.** The draft used `read_json` and
`row_number() OVER ()`, and its line order was inferred, not tested. Line order is the format's
one invariant, so the shipped reader does not depend on how DuckDB schedules a scan. It reads
the file with `read_text`, splits it on newlines, and numbers each line with
`list_transform`'s index. A 4,000-element file read with 8 threads comes back in order
(measured, `test/sql/compact.test` in duck_block_utils).

The reader returns attribute keys sorted, because `a` and `m` are two objects and the format
cannot keep the source's key order across them. Every source map in the corpus was already
sorted by key (178 of 178, measured by the subagent), so for those maps the sort gives back the
source order. DuckDB compares MAPs in key order (measured: `MAP{'b':'1','a':'2'} =
MAP{'a':'2','b':'1'}` is false), so a map that was not sorted reads back equal to the sorted
map, not to itself.

The shipped writer's output is byte-identical to the measured V1 files for the three fixtures
duck_block_utils keeps (1929, 766 and 929 bytes), so the sizes in section 1 apply to it.

## 7. What was tried and rejected

| Variant | Raw vs pandoc | concat zstd vs pandoc | Why rejected |
|---|---|---|---|
| V0: omit only, long keys | 1.09 | 1.02 | Kept as evidence that omission does most of the work |
| V2: V1 + default rules (predicted level, sparse `o`, `h2`, table → `json`) | 0.45 | 0.89 | 2% smaller compressed, needs a decoder in code; larger than V1 on identical content |
| V3: V2 as a positional array | 0.37 | 0.87 | Same, and lines no longer name their fields |

## 8. Side findings in `read_pandoc_blocks` (measured)

- It writes one row per `Str` and per `Space` token. Not filed.
- Each table is stored twice, as rows and in a `pandoc_ast` attribute (6.9 KB across the corpus).
  Not filed.
- It drops the trailing newline from code blocks. Not filed.
- ~~It drops an image's width and height, and a link's classes and key-value attributes.~~
  Fixed in v0.11.2 (#208, #209): inline Link, Image, Span and Code keep their pandoc `Attr`.

## 9. Decided while building it

These were open in the first version of this note. duck_block_utils#86 settled them.

| Question | Decision |
|---|---|
| Where the writer and reader live | Three SQL macros in duck_block_utils, registered at LOAD. No C++, and no `COPY ... (FORMAT ...)` function: the `COPY` recipe in section 6 writes the file. Teague chose duck_block_utils, so every producer in the fleet gets the format |
| A version | None. Adding a code changes the format; until a key or type has a code, it goes in `m` or in `kind:element_type`, and every reader keeps it. The codes freeze at the first duck_block_utils release with the format |
| An unknown `t` code without `:` | Refused with an error |
| A line without `l` | Refused with an error |
| Encodings `t` and `j` | Refused by the writer, because they are codes. Any other encoding is written as it is |
| A NULL attribute value | Refused by the writer, like a NULL `level`, `encoding` or `attributes` map |
| Empty lines | Skipped by the reader, so files concatenate |

---

The encoder scripts and per-file tables from the measurement were in a session scratchpad and
are not kept in the repo. The writer and reader that ship are in duck_block_utils, with their
tests in `test/sql/compact.test`.
