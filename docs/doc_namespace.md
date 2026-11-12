# The `doc_` namespace

**`doc_*` takes a path. `db_*` takes blocks you already hold.**

That split is why this namespace belongs to panduck: taking a path is the IO engine's job,
while operating on blocks is `duck_block_utils`' job.

```sql
SELECT level, title, indent FROM doc_toc('report.docx');
SELECT doc_render('report.docx', 'md');
```

## `doc_toc(src, format := 'auto')`

Returns `level, title, id, indent, element_order` — the same table
`duck_blocks_toc(panduck_read_blocks(src))` produces, which the suite asserts directly rather
than by eye.

The same document read from two formats yields the same table of contents:

```
doc_toc('sample.docx')  ->  1  Heading One  indent 0
                            2  Heading Two  indent 1
doc_toc('sample.rtf')   ->  identical
```

Two formats, two readers, two different heading mechanisms, one vocabulary, one answer.

## `doc_render(src, output_format, format := 'auto')`

Renders a document to a **format**: `md`, `html`, or `text`.

```sql
SELECT doc_render('notes.rtf', 'md');
--  # Heading One
--  A paragraph with **bold**, *italic*, and ~~strike~~ text.
```

An unsupported output format is **named**, never silently defaulted:

```sql
SELECT doc_render('notes.rtf', 'nonsense');
--  panduck: doc_render supports md, html and text; nonsense is unsupported
--  or its extension is not installed
```

`duck_block_utils` deleted its own `doc_render` when it stopped depending on format
extensions. Rendering to md/html *is* format IO, which is panduck's job; ANSI and plain
text are vocabulary-level outputs and stay in `duck_block_utils`.

## On-demand loading

These load `duck_block_utils` when called, exactly as reading `.md` loads
`duckdb_markdown` and `.html` loads `duckdb_webbed`. panduck's core never needs it — every
reader, the registry and both dispatchers work with `duck_block_utils` absent — and when it
is missing the error names it:

```
panduck: doc_toc needs the duck_block_utils extension (INSTALL duck_block_utils FROM community)
```

That asymmetry is deliberate. When `duck_block_utils` owned path dispatch, its `doc_toc`
**could not work** without a reader. panduck's `doc_*` is sugar over a composition that
remains available directly:

```sql
SELECT duck_blocks_toc(panduck_read_blocks('report.docx'));
```

## `doc_section(src, section, format := 'auto', match := 'exact')`

The blocks under one heading, still as `duck_blocks`. Matches a heading by its **text or
its `id`** — a heading's text is what a person names, its `id` is what a link names, and
an HTML document has both:

```sql
SELECT * FROM doc_section('report.docx', 'Methods');
SELECT * FROM doc_section('page.html', 'methods');   -- by id
```

The boundary is **the next heading at the same level or higher**, not the next heading — a
section contains its subsections, which is what makes `doc_section('Chapter 2')` mean what
a reader expects. A section that is not present returns no rows rather than an error.

### `match := 'contains'` — when the caller has a fragment, not a title

`match := 'exact'` (the default) is `content = section OR id = section`.
`match := 'contains'` is `content ILIKE '%section%' ESCAPE '\' OR id = section`.

```sql
SELECT count(*) FROM doc_section('README.md', 'Install');                     -- 0
SELECT count(*) FROM doc_section('README.md', 'Install', match := 'contains'); -- 21
```

A person searching a document types a fragment, not a heading verbatim; that first query
returning nothing is the shape a consumer hits first. Substring matching is **opt-in
rather than the default** because `ILIKE '%x%'` matches everything `= x` does and more, so
flipping the default could only widen existing verbatim lookups, silently.

Three consequences worth knowing:

- **The title arm is case-insensitive; the id arm stays whole-string.** An `id` is a slug
  the caller either has or does not, and a substring of one is not a meaningful request —
  `'alpha'` should not pull in the section whose id is `alpha-one`.
- **`%` and `_` in the pattern are live wildcards**, with `ESCAPE '\'` to reach a literal
  one. This is `ILIKE`'s own contract, spelled the same way as duckeye's `-S` so that
  delegating to this function is not a behaviour change for its users.
- **A span contained by another matched span is dropped.** A fragment can match both a
  chapter and a subsection inside it; without the dedup the child prints twice. Two
  matches at the same level are disjoint and both survive — the rule is "contained by",
  not "overlaps".

> **`doc_section`, `doc_search_sections` and `doc_container` take a SINGLE document.** They
> slice the block stream by `element_order` alone, and `element_order` restarts per document
> (see "`element_order` is per document" below), so a glob would interleave two documents at
> the same values and every span would be computed across the seam. All three now refuse one
> by name:
>
> ```sql
> SELECT count(*) FROM doc_section('test/fixtures/sections.html', 'Alpha');  --  4
> SELECT count(*) FROM doc_section('test/fixtures/*.html', 'Alpha');
> --  Invalid Input Error: panduck: doc_section takes a single document; element_order
> --  restarts per document, so a glob or list interleaves them. Use
> --  read_panduck_doc(src, filename := true) for multiple documents
> ```
>
> The multi-document path is `read_panduck_doc(…, filename := true)`, which keeps
> provenance.
>
> **The test is how many documents the source resolves to, not what shape it is written
> in.** A glob matching exactly one file, a one-element list, and a plain path are all one
> document and all work. `doc_toc` and `doc_render` refuse a plural source the same way and
> with the same wording.
>
> *(An earlier revision of this page said a glob was "not rejected — it is read, and the
> rows interleave silently", and showed 12 rows. That was true when written and is not
> true now; the guard landed. Corrected rather than deleted, because the interleaving is
> still the reason the guard exists.)*

It slices panduck's own block stream rather than wrapping `duck_block_utils`, and that was
a measurement rather than a preference. On build `3f2a0f0`, `duck_blocks_get_section`
returns `VARCHAR` — rendered text — for *every* `output_format` including `'blocks'`, and
`duck_blocks_sections_like` returns `(section, start_order, content)`, also rendered. Both
are useful; neither hands back `duck_blocks`. Wrapping either would make `doc_section` the
only `doc_*` that takes a path and returns text instead of a queryable table. Slicing
locally also needs no `json` extension, which those functions do.

**One of those two reasons has since expired upstream, and the other has not.** As of
`duck_block_utils` `f047b3e`/`0eb9e47` — on their `main`, *not* in the published build
measured above — `output_format` is gone entirely and the functions return blocks:
`duck_blocks_get_section(blocks, pattern)` and `duck_blocks_get_pages(blocks, first, last)`
return `LIST(duck_block)`, and `duck_blocks_sections_like(blocks, query)` returns
`(section, start_order, blocks)`. Their own reasoning matches the note above: one return
type per macro means every `output_format` branch collapsed to VARCHAR anyway.

So the "returns rendered text" reason no longer applies to their `main`. The `json`
dependency reason still does, and `doc_section` is unchanged on that basis. Recorded here
rather than silently left standing, because a justification that has half-expired reads as
though it were still whole.

`doc_sections_like` is still absent under that name, and the reason it was absent has now
been met rather than waived. It was declined because it is *"a **search** returning
rendered text — a different shape and a different job"*: the objection was to the **shape**,
not to searching. `doc_search_sections` below returns `duck_blocks` — the same seven columns
as `doc_section`, in the same order — so it is the same shape and composes with everything
`doc_section` composes with. `duck_block_utils`' `duck_blocks_sections_like` remains a
different thing: it returns one row per section as `(section, start_order, blocks)`, which
is the right shape for enumerating matches and the wrong one for slicing a document.

### `match := 'glob'` — duckeye's pattern language

`'glob'` is `'contains'` with a different pattern language (#163). It is the language of
duckeye's `-S` and `-s`:

| pattern | means |
|---|---|
| `*` | any run of characters |
| `?` | one character |
| `%`, `_`, `\` | themselves |

The match is case-insensitive and substring, as `'contains'` is. The id arm is whole and
raw, as in every mode: `'a_b-and-a-b'` finds the heading whose id is exactly that.

The difference from `'contains'` shows on exactly the characters a heading often holds:

```sql
-- headings: "50% done", "500 done", "a_b and a-b", "A backslash", "back\slash"
doc_section(f, '50%',        match := 'contains')  -- "50% done" AND "500 done"
doc_section(f, '50%',        match := 'glob')      -- "50% done"
doc_section(f, 'a_b',        match := 'contains')  -- "a_b and a-b" AND "A backslash"
doc_section(f, 'a_b',        match := 'glob')      -- "a_b and a-b"
doc_section(f, 'back\slash', match := 'contains')  -- "A backslash" (\s escapes s)
doc_section(f, 'back\slash', match := 'glob')      -- "back\slash"
```

`'contains'` keeps live `%` and `_`. That is its documented contract, so it does not change.

The translation is a function of its own, `panduck_glob_like(pattern)`, which returns the
ILIKE pattern. duckeye needs the translation outside these functions too, so it is named
rather than hidden in a mode. The caller adds the `'%'` on each side and `ESCAPE '\'`:

```sql
SELECT panduck_glob_like('\%_*?');   -- \\\%\_%_
SELECT title FROM t WHERE title ILIKE '%' || panduck_glob_like('Al*a') || '%' ESCAPE '\';
```

**The order of the replacements is the contract**: backslash first, so the escapes added
after it are not doubled. It is SQL in the extension, not C++ behind the libpanduck seam,
because its output is an ILIKE pattern and means nothing outside SQL. A matcher for a
libpanduck consumer is #164.

### The id arm, and what a miss returns (#166)

**A heading's id is matched whole and raw, in every mode.** `section` is compared with
`attributes['id']` as written: no wildcard, no escape, case-sensitive. A slug is a value the
caller has or does not have, so a part of one is not a request.

The id is the one the reader set. `doc_section` derives no slug for a heading that has none.
A reader sets pandoc's id: the author's explicit id where the format has one (org
`:CUSTOM_ID:`, latex `\label`, textile `h1(#id).`), else pandoc's slug of the title.
Measured on `test/fixtures/` on 2026-11-11, against pandoc 3.1.3:

| readers | headings carry `attributes['id']` |
|---|---|
| markdown, html, rst, textile, mediawiki, docx, odt, org, latex | yes |
| rtf, epub | no, and pandoc gives none either |

Two titles that slug alike get the SAME id. pandoc writes `a` and then `a-1`. Teague ruled on
2026-11-11 that the suffix is a separate change (#85).

For the second group, the id arm never matches, and only the title can. duckeye's own heading
list (`duck_blocks_headings_structs`) also gives an empty id for those headings, so the two
tools agree there.

**duckeye's `-S` compares the id with the ESCAPED pattern**, `id = duckeye_like(arg)`, so
`-S my_id` compares the id with `my\_id`. duckeye's `docs/dev/sql-core.md` records this as
probably a bug. panduck does not copy it. On duckeye's parity fixtures the two compares give
the same result for every pattern. No pattern that holds `\ % _ * ?` equals a fixture id, and
the patterns with none of those characters escape to themselves.

**A miss returns zero rows**, from all four functions, and never an error or a NULL row. A NULL
appears only when the caller aggregates zero rows, for example
`SELECT list(b) FROM doc_section(...) b`. That NULL is the aggregate's, so test it with
`IS NULL`, or count the rows first.

## `expand_embedded := true` — parse a document held inside another

A `.ipynb` markdown cell is held as one `raw` block, deliberately: panduck's delegation
lives in the SQL layer, so a C++ reader that parsed markdown would make its output depend
on which extensions happen to be installed. The consequence is that a notebook has **no
headings** and nothing to navigate.

```sql
SELECT count(*) FROM doc_toc('notebook.ipynb');                        -- 0
SELECT level, title FROM doc_toc('notebook.ipynb', expand_embedded := true);
-- 1 | Notebook Title
```

Accepted by `read_panduck_doc`, `doc_toc`, `doc_section`, `doc_search_sections` and
`doc_container`. `panduck_expand_embedded(blocks)` is the same operation for a caller who
already holds blocks — the parameter is that helper applied for you, not a second
implementation that could drift from it.

**Opt-in, and the default is the point.** An unchanged call reads identically with or
without the `markdown` extension present; asking for expansion is asking for the
dependency, knowingly. A document with nothing embedded is returned unchanged, so the
parameter is safe to pass blindly.

**Without `markdown`, the query fails — it does not quietly return the unexpanded
document.** Measured against an empty `extension_directory`:

```
Invalid Input Error: panduck: expand_embedded needs the markdown extension
                     (INSTALL markdown FROM community)
```

This is worth stating because the opposite is the plausible guess, and it is the guess
that costs you: a notebook reader that silently returned zero headings on a machine
missing one extension would look like a document with no headings. The guard is
`panduck_ensure_extension('markdown')`, and it exists so the caller does not instead get
a Catalog Error naming `parse_markdown_to_duck_blocks` — a function they never typed
(#25's shape).

Two details worth knowing:

- **Levels are offset, not copied.** Parsed blocks arrive at their own depth — 1 for a
  heading — while the raw block they replace sits inside the host document. A heading from
  a notebook cell lands at depth 2, *under* the cell's `div`, rather than level-with it.
- **`element_order` keeps its origin.** It is renumbered contiguously, but a reader that
  starts at 0 still starts at 0. Normalising here would silently rewrite one reader's
  numbering to match another's.

## `doc_search_sections(src, pattern, format := 'auto', match := 'contains')`

**Find the section by its content**, where `doc_section` finds it by its heading. The
difference is the bound:

| | boundary | answers |
|---|---|---|
| `doc_section` | next heading of the **same or higher** level | "give me this chapter, subsections included" |
| `doc_search_sections` | next heading of **any** level | "give me the innermost section that mentions this" |

```sql
SELECT count(*) FROM doc_section('README.md', 'Formats');                       -- 27
SELECT count(*) FROM doc_search_sections('README.md', 'Every format pandoc reads'); -- 25
```

The first carries the subsection along; the second returns just the subsection that holds
the phrase. That is what *innermost* means — a match inside a subsection returns the
subsection, not its parent and all its siblings.

The pattern is `ILIKE '%pattern%' ESCAPE '\'` against the section's **flattened text** —
every block's content joined in document order, including the heading's own — so a phrase
that straddles two blocks still selects the section. No match returns no rows rather than
an error.

`match := 'glob'` takes duckeye's pattern language instead, as for `doc_section` above.
`'exact'` is refused: the pattern is compared with a section's whole text, so an exact
compare would not be a search.

> **The separator is a single space, and a block with no content contributes no
> separator.** Blocks whose content is `NULL` or `''` are skipped entirely rather than
> flattened to empty:
>
> ```
> # Heading / alpha / --- / beta   ->  "Heading alpha beta"
>                                 not  "Heading alpha  beta"
> ```
>
> An `hr` carries no content, and joining it as `''` would still earn it a separator — so
> `doc_search_sections(doc, 'alpha beta')` found nothing, on a doubled space that exists
> nowhere in the document. `README.md` alone has 71 contentless blocks out of 476 (`hr`,
> `list`, `list_item`, `link`, `bold`, `paragraph`), so this is the ordinary shape of a
> document rather than an edge case.
>
> The flattened text contains what the document contains. The alternative reading — that a
> semantic break *should* stop a phrase matching across it — is defensible, but then it
> wants a real separator with a defined meaning, not a doubled space that appears only when
> a block happens to be empty.
>
> This is part of the contract, not an implementation detail: it decides whether a pattern
> spanning a block boundary matches. A tool comparing its own section search against this
> one should check its flattening before concluding the two disagree — a different join
> rule is a difference in the *flattening*, not in the matching, and neither side is
> wrong. This paragraph exists because exactly that happened: twelve patterns on a real
> 696-block document agreed exactly, and a constructed four-block case did not.

Two cases fall out of the same rule rather than needing their own:

- **Content before the first heading is a section.** Otherwise a preamble belongs to no
  heading and no query can reach it.
- **A document with no headings at all is one section.** A matching document returns its
  content rather than nothing.

Both hold because a block's section is keyed by the nearest heading at or before it, and
a block with no preceding heading simply has none.

The segments **partition the document**: an empty pattern matches every segment and returns
the whole document exactly once, with no block missed and none repeated.

## `doc_section_blocks(blocks, section, match := 'exact')` and `doc_search_sections_blocks(blocks, pattern, match := 'contains')`

**The same two narrows, over a `duck_blocks` list instead of a path** (#162). Use them for
blocks that never were a file: a zim entry, stdin, a Pandoc AST, a code AST, or the output
of another narrow.

```sql
WITH d AS (SELECT list(b ORDER BY b.element_order) AS blocks
           FROM read_panduck_doc('report.docx') b)
SELECT * FROM doc_section_blocks((SELECT blocks FROM d), 'Meth', match := 'contains');

SELECT duck_blocks_to_md(list(s ORDER BY s.element_order))
FROM doc_search_sections_blocks(panduck_pandoc_ast_to_blocks('<ast json>'), 'p < 0.05') s;
```

Each returns what its path form returns over the same blocks: the same rows, in the same
order. The path form and the list form share one span implementation, and
`test/sql/doc_blocks.test` asserts the parity.

**They are table macros, not scalars.** A scalar macro over a 72,000-block list ran out of
memory at 6.1 GiB (measured by duckeye, 2026-11-10). A table macro with the same body
matched duckeye's own SQL in time and output.

**A list is one document.** The span is keyed on **list position**, not on
`element_order`. `element_order` restarts per document, so a list that joins two documents
repeats it, and keying on it would interleave their spans. Each result row keeps the
`element_order` the block arrived with:

```sql
-- blocks[5:9] is blocks 4..8; 'Beta One' is at position 3 of the slice
SELECT element_order, content FROM doc_section_blocks((SELECT blocks[5:9] FROM d), 'Beta One');
-- 7  Beta One
-- 8  beta one body
```

An empty or `NULL` list returns no rows. A miss returns no rows, as for the path forms.

## `doc_reader(src, format := 'auto')` and `doc_reader_blocks(blocks)`

**Reading mode: the page without its named chrome** (duckeye ask 5). It is taken from
duckeye's `duckeye_reader` (`feat/reader-mode` `d959ad3`), so duckeye, its Android app and
panduck run one copy.

```sql
SELECT * FROM doc_reader('page.html');

WITH d AS (SELECT list(b ORDER BY b.element_order) AS blocks FROM read_html_blocks('page.html') b)
SELECT duck_blocks_to_md(list(r ORDER BY r.element_order))
FROM doc_reader_blocks((SELECT blocks FROM d)) r;
```

**The rule is cordexa's: remove named structure only, never content judged to be junk.**
webbed gives `<header>`, `<nav>`, `<main>`, `<article>`, `<aside>` and `<footer>` as
`section` blocks with `attributes['role']`.

- Kept: the `<main>`, else every `<article>`, else the whole document.
- Dropped from what is kept: `nav` and `aside`, and a `header` or `footer` unless it is
  inside an article. Inside an article, it is the article's own title block or byline.
- Metadata (`kind = 'value'`) keeps `title`, `author` and `description`. The rest of
  `<meta>` is presentation: viewport, `twitter:*`, `react-*`.

A section's subtree is the blocks after it, up to the next block at its level or above.
Each row keeps its own `element_order`, in list order.

**The shape is the performance.** duckeye measured four shapes of this logic on a
7,335-block Wikipedia page. An `EXISTS` or self-join over the unnested blocks took 43 s.
`blocks[i]` in a per-block lambda took 23 s, and spans passed as macro arguments took 34 s.
Spans computed once, then passed as values, took 0.3 s. So the spans, `keep` and `chrome`
are columns of `MATERIALIZED` CTEs, and the per-block lambda mentions only values. Both
macros are table macros, for the reason given for `doc_section_blocks`.

**Why `doc_reader_blocks` is in panduck, not in `duck_block_utils`.** The rule at the top
of this page puts blocks-only functions in `duck_block_utils`. This one bends it, as
`doc_section_blocks` does, for three reasons:

1. duckeye calls it on blocks that never were a file: a zim entry or stdin. Its SQL core
   then needs one extension for every narrow, not two.
2. `doc_reader` and `doc_reader_blocks` share one body, so the path form and the list form
   cannot drift apart.
3. The rule reads a reader's vocabulary: the section roles that webbed emits. It is not a
   generic operation on the duck_block format.

If `duck_block_utils` takes it later, it can take the body unchanged.

## `read_panduck_doc(src, …)` reads more than one document

```
read_panduck_doc(src, format := 'auto', pages := '', filename := false,
                      attributes := 'default', reader_params := MAP {})
```

`src` takes three forms. Every example below was run against this repo's fixtures.

| form | example | resolution |
|---|---|---|
| a plain path | `'test/fixtures/constructs.odt'` | dispatched as it always was |
| a glob — `*`, `?` or `[` | `'test/fixtures/*.odt'` | expanded, sorted, one arm per match |
| a `VARCHAR[]` | `['a.docx','b.odt']` | used in argument order; an element may itself be a glob |

```sql
SELECT count(*) FROM read_panduck_doc('test/fixtures/constructs.odt');   -- 54
SELECT count(*) FROM read_panduck_doc('test/fixtures/*.odt');            -- 180
```

**A list may mix formats, and that is the interesting case.** Each path resolves through
the registry independently, so one call reads a DOCX and an ODT into one block stream:

```sql
SELECT count(*) FROM read_panduck_doc(['test/fixtures/constructs.docx',
                                       'test/fixtures/constructs.odt']);
--  106      -- 52 from the DOCX + 54 from the ODT
```

A glob is sorted before the arms are built, so the result is deterministic across runs and
platforms rather than inheriting the filesystem's enumeration order.

### `filename := true` — off by default, for every source form

Off for a plain path, off for a glob, off for a list. The column is named `filename`, is
**trailing**, and holds the path panduck dispatched on — as written for a plain path or a
list element, as expanded for a glob:

```sql
SELECT filename, count(*)
FROM read_panduck_doc(['test/fixtures/constructs.docx',
                       'test/fixtures/constructs.odt'], filename := true)
GROUP BY 1 ORDER BY 1;
--  test/fixtures/constructs.docx  52
--  test/fixtures/constructs.odt   54
```

**Off by default is not conservatism, it is what the released fleet can consume.** Measured
against the installed `duck_block_utils` **3f2a0f0 (spec 6.3)**, an eight-field struct does
not bind at all:

```
SELECT duck_blocks_to_text(
    (SELECT list(b) FROM read_panduck_doc('test/fixtures/lists.odt', filename := true) b));

Binder Error: No function matches the given name and argument types
'duck_blocks_to_text(STRUCT(kind VARCHAR, element_type VARCHAR, "content" VARCHAR,
"level" INTEGER, "encoding" VARCHAR, attributes MAP(VARCHAR, VARCHAR),
element_order INTEGER, filename VARCHAR)[])'.
```

The same query with provenance off returns text. So panduck's **default** output stays the
canonical seven columns and works against 6.3 and 6.4 alike; a caller who writes
`filename := true` has, by opting in, taken on the requirement that their consumer accepts
eight fields. On 3f2a0f0 an explicit `::duck_block[]` cast *does* succeed and silently
**drops** the eighth field — `duck_block` on that build is exactly seven fields — so the
cast is not a way round the binder.

Provenance is **projected, not requested** from the reader. `SELECT *, '<path>' AS filename`
puts the column after the canonical seven by construction, which is what both shipped
sibling producers got wrong by emitting it first (webbed `a865d37`, markdown `340c0cd`).
It also means panduck needs nothing from a sibling to have provenance: the path is already
in hand at dispatch time.

**Core's `filename := 'custom_name'` rename form is deliberately unsupported.** panduck's
`filename` is a `BOOLEAN`, so the string form is refused by the cast:

```sql
SELECT * FROM read_csv('x.csv', filename := 'src');           -- core: renames the column
--  1|2|x.csv

SELECT count(*) FROM read_panduck_doc('test/fixtures/constructs.odt', filename := 'doc');
--  Conversion Error: Could not convert string 'doc' to BOOL
```

Spec 6.4 keys eight-field acceptance on the exact type with `filename` **last**, so a struct
whose eighth field is named anything else does not bind as a `duck_block` — honouring the
rename would hand back rows no consumer accepts. That measurement was taken against a 6.4
build (see the design note, `docs/superpowers/specs/2026-09-05-multi-document-reads-design.md`
§3.4); it is *not* reproducible against 3f2a0f0, where no eight-field struct binds under any
spelling. This is a conscious narrowing of core's contract rather than an oversight.

### `attributes := 'default' | 'all'` — an intent, not a parameter name

The caller names **panduck's** vocabulary. The sibling reader's actual spelling for it lives
in registry data, so a sibling renaming its parameter is fixed by re-registering at runtime
rather than by rebuilding panduck:

```sql
CALL panduck_register_doc_reader('webbed', 'read_html_blocks', ['.htmltest'],
     options := [{intent: 'attributes', value: 'all',
                  param: 'capture_attributes', arg: '*', arg_type: 'VARCHAR'}]);

SELECT panduck_read_arms_opt(['x.htmltest'], false, 'attributes', 'all');
--  SELECT * FROM read_html_blocks('x.htmltest', capture_attributes := '*')
```

That is the generated SQL, and it also runs: against **webbed v2.9.0 (`73189d2`)**, which
is what the community registry serves, `capture_attributes` exists and the read returns the
document with the option threaded.

That sentence was false for most of this feature's life and is worth the caution: until
webbed published, the same call raised `Binder Error: Invalid named parameter
"capture_attributes"` from webbed, and saying otherwise would have described a feature that
worked nowhere.

`'default'` is a sentinel that renders to nothing **without consulting the registry**, so an
unchanged call generates byte-identical SQL to what it generated before options existed.

**An intent the reader has no mapping for raises rather than being dropped**, which is the
whole point — a reader that cannot honour what was asked for must say so instead of
returning a document quietly missing it:

```sql
SELECT count(*) FROM read_panduck_doc('test/fixtures/constructs.odt', attributes := 'all');
--  Invalid Input Error: panduck: the reader for test/fixtures/constructs.odt
--  has no mapping for attributes = 'all'
```

**`.html` and `.htm` ship a built-in mapping**: `attributes := 'all'` renders
`capture_attributes := '*'`, webbed's spelling for *every* source attribute. No registration
needed.

**`'*'` and not `'classes'`**, which this example got wrong until webbed published the
grammar. webbed accepts `'default' | 'classes' | '*' | true | false | ['id', …]`, where
`'default'` is `['id','name','href','src']` and `'classes'` is that **plus** `class`. Only
`'*'` means all of them. Mapping `'all'` onto `'classes'` would hand back five attributes to
a caller who asked for everything — silently, with no error.

**Every other reader still has no mapping**, and an intent a reader cannot honour raises
rather than being dropped. That is the point of the error above: a reader that cannot do
what was asked must say so instead of returning a document quietly missing it.

### `reader_params := MAP{'name':'value'}` — the escape hatch

For any reader parameter panduck has no intent for. Keys are validated as identifiers,
values are always rendered as quoted `VARCHAR` literals and the reader's own cast does the
rest — there is no bare-typed arm here, because that is precisely the arm measured to be a
live injection under an unchecked value.

```sql
SELECT panduck_read_arms_opt(['a.odt'], false, 'attributes', 'default',
                             reader_params := MAP{'ignore_errors': 'true'});
--  SELECT * FROM read_odt_blocks('a.odt', ignore_errors := 'true')

SELECT count(*) FROM read_panduck_doc('test/fixtures/constructs.html',
                                      reader_params := MAP{'ignore_errors': 'true'});
--  15
```

panduck does not interpret the value; `ignore_errors` above is `webbed` **093856b**'s own
parameter, and what it means is that reader's business. A key that is not an identifier is
refused before anything is rendered:

```sql
SELECT count(*) FROM read_panduck_doc('test/fixtures/constructs.html',
    reader_params := MAP{'ignore_errors) UNION SELECT 1 --': 'true'});
--  Invalid Input Error: panduck: reader_params key must be an identifier,
--  got 'ignore_errors) UNION SELECT 1 --'
```

A NULL value is not a value, and neither is a NULL map:

```sql
--  reader_params := MAP{'ignore_errors': NULL}
--  Invalid Input Error: panduck: reader_params['ignore_errors'] must name a value; NULL is not one
--  reader_params := NULL
--  Invalid Input Error: panduck: cannot use NULL as argument for "reader_params"
```

### Every named parameter refuses NULL

DuckDB core is the standard this was aligned to, on the very parameter whose name and
column were both settled by "same as core":

```sql
SELECT * FROM read_csv('x.csv', filename := NULL);
--  Invalid Input Error: Cannot use NULL as argument for "filename"

SELECT count(*) FROM read_panduck_doc('test/fixtures/constructs.odt', filename := NULL);
--  Invalid Input Error: panduck: cannot use NULL as argument for "filename"
```

`format`, `pages`, `attributes` and `reader_params` all answer the same way. This mattered
because a NULL took the same path the *default* takes, so the caller could not tell that
what they asked for did not happen — measured before the fix: `filename := NULL` returned
seven columns with provenance silently off, and `pages := NULL` returned all 54 rows of an
ODT and all 180 of a glob.

### Options are refused, not ignored, where they cannot be honoured

The special-cased branches — pdf, zim, toml/yaml, the text fallbacks and the code fallback —
build their own SQL rather than going through the arm builder, so they cannot thread a
reader argument. They say so by name:

```sql
SELECT count(*) FROM read_panduck_doc('test/fixtures/two_pages.pdf', attributes := 'all');
--  Invalid Input Error: panduck: attributes is not supported for pdf

SELECT count(*) FROM read_panduck_doc('test/fixtures/two_pages.pdf',
                                      reader_params := MAP{'ignore_errors': 'true'});
--  Invalid Input Error: panduck: reader_params is not supported for pdf

SELECT count(*) FROM read_panduck_doc('test/fixtures/config.toml', filename := true);
--  Invalid Input Error: panduck: filename is not supported for toml

SELECT count(*) FROM read_panduck_doc('src/reader_registry.cpp', filename := true);
--  Invalid Input Error: panduck: filename is not supported for this source
```

PDF is the one special-cased branch that *does* project `filename`, so it is excluded from
that refusal by name. "this source" is the code fallback, which has no registry format to
name.

### `element_order` is per document

**The contract, stated rather than left incidental.** `element_order` is dense from 0 over
the list a reader **emits**, *including any synthetic elements the reader inserts* — the
`page_break` markers `read_pdf_blocks` adds are numbered in the same sequence as the content
around them, not on a separate axis. The reader's output is the source document as far as
any consumer is concerned; there is no earlier ordering a consumer can see.

That distinction decides what functions may do with it. Functions that **project** — a table
of contents, a heading list, a section slice — carry `element_order` through *unrenumbered*,
gaps and all, so it stays a join key back to the emitted list. Functions that **construct** a
standalone document — assemble, merge, reorder — renumber, and say so.

This is duck_block_utils' ruling as vocabulary owner, confirmed by Teague. `read_pdf_blocks`
conforms today and `test/sql/pdf_reader.test` pins it: dense `0..44` with no gaps or repeats,
markers *inside* that run rather than appended, verified by an `EXCEPT` against `range(0,45)`
that forces exactly the two marker positions to be the holes. The alternative reading —
unrenumbered relative to whatever function produced the rows — is not one the spec can hold,
because a producer inserting markers could then never conform and `page_break` would be
unrepresentable.

It is the position of a block within **its own** document. Making it global would break
every consumer that uses it to reconstruct one document's order — `doc_section`,
`doc_container` and the pandoc writer among them:

```sql
SELECT element_order, count(*) AS docs
FROM read_panduck_doc(['test/fixtures/constructs.docx','test/fixtures/constructs.odt'])
GROUP BY 1 ORDER BY 1 LIMIT 4;
--  0  2
--  1  2
--  2  2
--  3  2
```

A global order is `ORDER BY filename, element_order`, which is also why the two features
belong in the same section:

```sql
SELECT filename, element_order, element_type
FROM read_panduck_doc(['test/fixtures/constructs.docx',
                       'test/fixtures/constructs.odt'], filename := true)
ORDER BY filename, element_order LIMIT 4;
--  test/fixtures/constructs.docx  0  heading
--  test/fixtures/constructs.docx  1  paragraph
--  test/fixtures/constructs.docx  2  text
--  test/fixtures/constructs.docx  3  bold
```

### Pages are carried by markers, not by every block

`read_pdf_blocks` emits one `page_break` block per page, carrying
`attributes['page_number']`. **Content blocks do not carry it.** They did once, as a panduck
convention, so that `WHERE attributes['page_number'] = '2'` worked without a join; that was
ruled redundant emission, since the marker is what `duck_blocks_page_rows` and
`duck_blocks_get_pages` actually read.

Select pages at the reader — `read_pdf_blocks(src, first_page, last_page)` — which never
parses the pages you did not ask for. Filtering blocks by page after the fact would mean
parsing the whole document to discard most of it.

A document with **no** markers reports **zero** pages, not one. "No page information" and
"one page" are different facts, and every non-paginated format — markdown, HTML, docx — must
answer zero, or it acquires a phantom page.


## Policy settings

Three settings gate what panduck will do at runtime. **Every default preserves the
behaviour described everywhere else in this document** — registration allowed, every reader
on — so a deployment that sets nothing sees no change.

| setting | type | default | effect |
|---|---|---|---|
| `panduck_allow_registration` | BOOLEAN | `true` | when false, `panduck_register_doc_reader` and `panduck_register_table_reader` refuse |
| `panduck_enabled_readers` | VARCHAR | `'*'` | allowlist of reader **format** names; `'*'` means all |
| `panduck_disabled_readers` | VARCHAR | `''` | denylist of format names, applied **after** the allowlist |

```sql
SET panduck_disabled_readers = 'pdf,zim';   -- everything except those two
SET panduck_enabled_readers  = 'markdown';  -- only markdown
SET panduck_allow_registration = false;     -- no new readers at runtime
```

Lists are comma-separated and tolerate whitespace and case — `' ODT , rtf '` works, because a
hand-typed `SET` is the whole interface. A name panduck does not know is inert rather than
an error. A format in **both** lists is refused: the denylist applies last, so a reader
cannot smuggle itself back on.

**This is defence in depth, not a privilege boundary.** Reaching registration already
requires the ability to run `CALL panduck_register_doc_reader` — that is, arbitrary SQL — and
the `function` name is validated as an identifier, so a registration cannot inject SQL. These
settings exist for the deployment that does not want runtime registration *at all*, or wants
a particular reader off: an embedder exposing panduck to untrusted SQL, where "you already
need arbitrary SQL to reach it" is not the reassurance it is for a local session.

**A disabled reader raises. It does not fall through.** The `code` fallback answers any
source no other reader claimed, and it answers with a syntax-highlighted parse tree — so a
reader that were merely *skipped* when disabled would hand back a tree of the document's own
text with no error at all. `code` is itself a nameable format, which makes
`SET panduck_disabled_readers = 'code'` the difference between *"read documents, or tell me
you cannot"* and *"read documents, or hand back a parse tree"*.

**What the gate covers, and what it does not.** It covers every *dispatching* entry point —
`read_panduck_doc`, `read_panduck_table`, `read_pdf_blocks`, `panduck_read_blocks` and the
whole `doc_*` family — on single, glob and list sources alike. It is applied **per path**, and
a plural source is refused rather than filtered: a mixed list is not silently reduced to its
allowed members, because the caller asked for every document in it.

**It covers panduck's own format readers called directly too** — `read_odt_blocks`,
`read_rtf_blocks`, `read_docx_blocks` and the rest of that family, including their `_string`
variants, which take no path at all. Each is gated in its **bind**, so a disabled reader
fails before any file is opened and fails with panduck's named error rather than the
reader's own IO error.

**How a source is named for policy.** In order: an explicit `format :=` argument, then the
registry's format for it, then the registry key with its dot stripped, then `code`. The
third link exists so a reader registered with an *empty* `reader_ext` — which has no format —
is still nameable, rather than being mistaken for the `code` fallback and refused with it.

**A registration cannot rename a source out of a denylist.** Registration *replaces* a
registry row rather than shadowing it, so the name policy uses is taken from a map of what
panduck **natively** calls each extension, frozen when the registry is constructed and
unreachable from any statement a user can write. Re-registering `.md` still answers
`markdown`, even though the live row no longer carries a format at all.

Built-in beats live, deliberately: pointing `.md` at some other reader does not escape a
`markdown` denylist, because the operator disabled *reading markdown files*. An extension
panduck does not ship a reader for has no native name, so it keeps its own — its
`reader_ext`, or failing that its registry key.

### A glob matching nothing raises

Zero rows would be indistinguishable from a corpus that happens to be empty, and core does
not do that either — `read_csv` errors identically for a pattern that matches nothing and
for a named file that is absent (`IO Error: No files found that match the pattern "…"`).

```sql
SELECT count(*) FROM read_panduck_doc('test/fixtures/*.nosuch');
--  Invalid Input Error: panduck: no files matched test/fixtures/*.nosuch
```

**Each element of a list is checked as it expands**, before the results are flattened,
because flattening throws away which element produced which paths — after it, a source that
named itself in the call would vanish silently:

```sql
SELECT count(*) FROM read_panduck_doc(['test/fixtures/constructs.odt',
                                       'test/fixtures/*.nosuch']);
--  Invalid Input Error: panduck: no files matched test/fixtures/*.nosuch
```

A literal empty list is caught at dispatch, where there is no element to name:

```sql
SELECT count(*) FROM read_panduck_doc([]);
--  Invalid Input Error: panduck: no files matched []
```

A **plain path** deliberately does not go through the filesystem, so a caller naming one
missing file still gets the reader's own error rather than a glob-shaped one from panduck:
`read_panduck_doc('test/fixtures/nope.odt')` raises `IO Error: read_odt_blocks: not a
readable ZIP archive: test/fixtures/nope.odt`.

## `panduck_register_doc_reader(ext, function, extensions, options := …)`

`options` is **structured data, not a stored SQL fragment**:
`LIST(STRUCT(intent, value, param, arg, arg_type))`, all five fields required, all validated
at registration — so an ill-formed row can never be stored, let alone rendered.

```sql
CALL panduck_register_doc_reader('markdown', 'read_markdown_blocks', ['.mdx'],
     options := [{intent: 'attributes', value: 'all',
                  param: 'extract_metadata', arg: 'true', arg_type: 'BOOLEAN'}]);

SELECT panduck_read_arms_opt(['notes.mdx'], false, 'attributes', 'all');
--  SELECT * FROM read_markdown_blocks('notes.mdx', extract_metadata := true)
```

| field | meaning |
|---|---|
| `intent` | panduck's vocabulary — `'attributes'` |
| `value` | the intent's value — `'all'` |
| `param` | this reader's spelling, validated as `^[A-Za-z_][A-Za-z0-9_]*$` |
| `arg` | the literal to pass |
| `arg_type` | `VARCHAR`, `BOOLEAN` or `INTEGER` |

`VARCHAR` renders quoted with `''` doubling; `BOOLEAN` and `INTEGER` render **bare** and are
therefore checked against their declared type, which is the entire reason `arg_type` exists:

```sql
--  param: 'x := 1) UNION SELECT'
--  Invalid Input Error: panduck: param must be an identifier, got 'x := 1) UNION SELECT'
--  arg: 'true) UNION SELECT 1 --', arg_type: 'BOOLEAN'
--  Invalid Input Error: panduck: arg 'true) UNION SELECT 1 --' is not a BOOLEAN
--  arg_type: 'DOUBLE'
--  Invalid Input Error: panduck: unknown arg_type 'DOUBLE' (VARCHAR, BOOLEAN or INTEGER)
--  a row with no `arg` at all
--  Invalid Input Error: panduck: an option needs a non-NULL 'arg'
```

The struct casts **by name**, so field order does not matter and a *misspelled* field
arrives as NULL rather than as a bind error — which is why every field being required and
NULL being refused is the check that actually stands there. Rendering re-checks `param` and
`arg` a second time, because that is the only place registration data becomes SQL text and
it does not delegate its own safety to a check that ran somewhere else.

`panduck_register_table_reader` takes the same `options` parameter.

## `read_pdf_blocks(src, pages := '')`

PDF into `duck_blocks`, with page selection that works:

```sql
LOAD pdf;   -- calling the reader directly does not autoload it; dispatch does
SELECT count(*) FROM read_pdf_blocks('test/fixtures/two_pages.pdf');              -- 45
SELECT count(*) FROM read_pdf_blocks('test/fixtures/two_pages.pdf', pages := '1'); -- 28
SELECT count(*) FROM read_pdf_blocks('test/fixtures/two_pages.pdf', pages := '2'); -- 17
```

`pages` accepts `N` or `N-M` and **rejects anything else** rather than ignoring it. It was
previously declared on `read_panduck_doc` and never referenced in the macro body, so
`pages := 'utter nonsense'` returned the whole document. It is now real on both surfaces,
and dispatch refuses in `read_pdf_blocks`' own words so the two paths cannot disagree about
the same argument:

```sql
SELECT count(*) FROM read_panduck_doc('test/fixtures/two_pages.pdf', pages := '1');
--  28

SELECT count(*) FROM read_panduck_doc('test/fixtures/two_pages.pdf', pages := 'utter nonsense');
--  Invalid Input Error: panduck: pages must be N or N-M, got utter nonsense

SELECT count(*) FROM read_panduck_doc('test/fixtures/two_pages.pdf', pages := NULL);
--  Invalid Input Error: panduck: pages must be N or N-M, got <NULL>
```

Formats that have no pages say so, and so does a source naming more than one document:

```sql
SELECT count(*) FROM read_panduck_doc('test/fixtures/constructs.odt', pages := '2');
--  Invalid Input Error: panduck: pages applies only to paginated formats (pdf); odt has no pages

SELECT count(*) FROM read_panduck_doc('test/fixtures/*.odt', pages := '2');
--  Invalid Input Error: panduck: pages applies to a single document;
--  test/fixtures/*.odt names more than one
```

That plural guard runs **before** the format is resolved, so it holds for `'*.pdf'` too —
which the format guard alone would let through, since a glob of PDFs still resolves to
`pdf`. Forward-safety rather than redundancy: the day any format grows a plural arm, that
combination would otherwise start silently dropping `pages` again.

This goes through the PDF element grain: `read_pdf(..., grain := 'element')`
on the consolidated API, or `read_pdf_elements(...)` on older releases. It avoids
`pdf_to_markdown`, because that scalar
takes a path and nothing else and therefore cannot select pages at all. The trade is
measured and real: the elements route **keeps list structure** that the markdown route
flattens into one paragraph, and **loses inline emphasis** that the markdown route keeps.
Neither dominates. Heading levels are ranked over the whole document before the content is
sliced, so a section cut from the middle keeps the depth it has in the full document
instead of being promoted to level 1 — which costs a full-document scan, so `pages` selects
content rather than saving work.

Requires the `pdf` extension, which declares excluded platforms (wasm, windows
mingw/rtools/arm64). `test/sql/pdf_reader.test` is gated on `PANDUCK_TEST_PDF` for that
reason and runs via `make check-pdf`; until it is wired into a platform-restricted CI job
the pdf path is verified locally and nowhere else.

## What is missing, and why

`doc_render` has no `ansi` arm. `duck_block_utils` build `3f2a0f0` does register
`duck_blocks_render_ansi` as a scalar at LOAD, so the old pragma blocker is gone and this
is now only unwired — adding it means checking that signature against what `doc_render`
needs.

`doc_toc` is built on the **scalar** `duck_blocks_toc`, not on a table macro.

`test/sql/doc_namespace.test` asserts which spelling the installed build has, so the day
that changes the suite says so by name rather than failing three assertions later with
`function ... does not exist`. It did exactly that on 2026-09-04.
