# `read_mediawiki_blocks` — MediaWiki Reader Design

**Status:** proposed
**Roadmap:** phase 5, last of three (`org` landed at `e758eeb`, `rst` after it; `mediawiki`
is the final `planned` format in the registry)
**Predecessors:** `2026-09-01-org-reader-design.md`, `2026-09-01-rst-reader-design.md`

All behaviour below was measured against pandoc 3.1.3 on 2026-09-01. Where this reader
diverges from pandoc, the divergence is stated with its reason and its cost.

## Why this one is worth doing natively

Pandoc reads MediaWiki well when you tell it to. The failure is in *inference*, and it is
the quiet kind:

| file | pandoc's inference | result |
|---|---|---|
| `.wiki` | mediawiki, correctly | byte-identical to explicit `-f mediawiki` |
| `.mediawiki` | "could not deduce format" → **markdown** | `== Heading ==` becomes a `Para` |
| `.mw`, `.wikitext` | same fallback | same |

The warning goes to **stderr and the exit code is 0**, so a pipeline reading stdout gets a
plausible-looking document with every heading demoted to a paragraph and every
`{{template}}` flattened into prose. The most explicit spelling of the format — the one
that literally names it — is the one pandoc cannot deduce.

panduck's registry already claims both `.wiki` and `.mediawiki` for this format, currently
with `status='planned'` and a NULL reader, so they are correctly unroutable today.

## Global constraint: the output must be writable back to pandoc JSON

**This is the binding requirement, and it replaces "match pandoc" as the design rule.**

> Diverge from pandoc's representation wherever the source justifies it. Do not discard
> information. But every construct this reader emits MUST map back to *valid* pandoc JSON.

Valid, not identical. duck_blocks is permitted to be the richer representation — pandoc's
AST is then a lossy export target, and losing an attribute pandoc has no field for is
acceptable. What is not acceptable is emitting a shape the writer cannot render at all.

### This constraint has an unmet prerequisite, measured

panduck has **no write direction registered**. Its only pandoc functions are
`read_pandoc_blocks` and `read_pandoc_blocks_string`.

The write code is already in this repo — `DuckBlocksToPandocAstFun` at
`src/pandoc_block_convert.cpp:2567` and `DuckBlocksToPandocBlocksFun` at `:2601` — and is
unregistered because `Register()` was neutered during the converter handoff to avoid
claiming upstream's names.

The only writer available today is the **installed** `duck_block_utils` v1.4.3, and it
cannot satisfy the constraint:

```
pandoc: Incompatible API versions: encoded with [1,20] but attempted to decode with [1,23,1]
```

It also loses structure on the way out — measured by round-tripping pandoc's own MediaWiki
AST through it:

| | original | after the installed writer |
|---|---|---|
| `DefinitionList` | present | **`BulletList`** |
| `Figure` | present | **`Para`** |
| block count | 18 | 21 |

**The loss is the writer's, not panduck's.** panduck's read of the same input produces
`list` `list_type='definition'` with `role='term'`/`role='definition'`, and
`figure` > `caption` + `image` — both correct. And panduck's own converter emits
`[1,23,1]` (`src/pandoc_block_convert.cpp:2576`), which is the version the local pandoc
accepts. This is the two-clocks split the vocabulary header warns about: the code panduck
owns is current, the installed extension is not, and one says nothing about the other.

**So the prerequisite is: register panduck's write direction before the MediaWiki reader
is verified against this constraint.** Concretely:

- Register `DuckBlocksToPandocAstFun` and `DuckBlocksToPandocBlocksFun` under names panduck
  owns. Upstream still registers `duck_blocks_to_pandoc_ast` and `write_pandoc_ast`, and
  the two-copy window is deliberate (see the converter handoff spec, step 4), so panduck
  takes **`panduck_blocks_to_pandoc_ast(blocks)`** and
  **`panduck_write_pandoc_ast(blocks, path)`** for now, exactly as it took
  `read_pandoc_blocks` rather than upstream's `read_pandoc_ast`. When upstream deletes its
  copy after a released panduck, the canonical names can be added as aliases.
- Add a **write-back arm** to the roundtrip harness: for every fixture, convert the
  reader's blocks to a pandoc AST and feed it to a real `pandoc -f json`. A non-zero exit
  is a failure. This is what makes the constraint a test rather than a promise, and it
  will immediately tell us whether panduck's newer writer shares the installed build's
  `DefinitionList` and `Figure` defects — which is currently **unknown and untestable**.

The write-back arm covers every existing format, not just MediaWiki. MediaWiki is the
occasion for it, not the reason.

## No new vocabulary, and no document metadata

Every construct maps to an `element_type` that already exists and is already emitted by
another reader. No vocabulary question is opened. That was the argument for doing Org
first and it was not guaranteed to hold here.

Measured: `pandoc -f mediawiki` returns **`meta: {}`** for every input tried. MediaWiki is
the **second** format after RST with no document metadata, so `read_mediawiki_blocks`
emits no `kind='value'` rows at all. Article titles live in the page's name or in an XML
dump's `<title>`, neither of which is in a `.wiki` file.

## Construct mapping

### Block level

| MediaWiki | duck_block | notes |
|---|---|---|
| `= x =` … `====== x ======` | `heading` + `attributes['heading_level']` | plus a slugified `id`, which is what makes `doc_toc` work on this format |
| blank-line-separated text | `paragraph` | |
| `* x`, `** x` | `list` `list_type='bullet'` > `list_item` | nesting by marker run length, not indentation |
| `# x`, `#* x` | `list` `list_type='ordered'` + `start`/`number_style`/`number_delim` | mixed runs (`#*`) nest a bullet list inside an ordered one |
| `; term` / `: definition` | `list` `list_type='definition'` > `list_item` `role='term'`/`'definition'` | the spec 5.0 shape, identical to Org's `::` and RST's field list |
| `{\| … \|}` | `table`, native `{headers, rows}`, `encoding='json'` | `!` marks header cells, `\|-` a row break, `\|\|` a cell break |
| `<blockquote>` | `blockquote` | |
| `<syntaxhighlight lang="x">` | `code` + `attributes['language']` | |
| `<pre>` | `code`, no language | pandoc writes code blocks as `<pre>` and reads them back as `CodeBlock` — a clean round trip |
| `----` | `hr` | |
| leading-space lines | `code` | **divergence, see below** |
| `<!-- -->` | dropped | pandoc emits nothing; a comment is not content |

### Inline

| MediaWiki | duck_block |
|---|---|
| `'''bold'''` | `bold` |
| `''italic''` | `italic` |
| `'''''both'''''` | `bold` > `italic` (measured: pandoc nests `Strong[Emph]`, in that order) |
| `<code>x</code>` | `code` |
| `<nowiki>x</nowiki>` | plain text — `nowiki` suppresses markup, it does not mark content |
| `[[Article]]`, `[[Article\|label]]` | `link`, `href='Article'`, `attributes['link_type']='wikilink'` |
| `[http://x label]`, `[http://x]` | `link`; the bare form auto-numbers its text, as pandoc does |
| `[[File:p.png\|thumb\|caption]]` | `figure` > `image` (`src`) + `caption` |
| `<ref>…</ref>` | inline `note` |

**Internal links carry their marker in an odd place, and it is worth recording.** Pandoc
emits `Link [attr] [inlines] ["Article", "wikilink"]` — the second element of the target
tuple is normally a link *title*, and pandoc overloads it as a type marker. A reader
copying that field into a `title` attribute would produce a document full of links titled
"wikilink". panduck reads it as what it is and emits `link_type='wikilink'` instead.

## Templates

**`{{Infobox}}` and friends are held raw, with the template name surfaced as an
attribute.** Ruled 2026-09-01.

```
{{Infobox person
| name = X
}}

  kind          block                 (inline position -> kind='inline')
  element_type  raw
  content       "{{Infobox person\n| name = X\n}}"
  encoding      mediawiki
  attributes    source_type   = template
                template_name = Infobox person
```

Templates are MediaWiki's open extension surface, and they are **unresolvable by
construction**: `{{convert|5|km}}` expands only against the wiki's template namespace,
which is not in the file. This is the same situation as a LaTeX `\newcommand` used without
its definition, which panduck's LaTeX reader reads as an unclaimed name rather than
expanding.

**MediaWiki itself confirms the unresolvability, measured 2026-09-02.** Given a template it
does not have, its own parser produces no content — only a red link saying the template
does not exist:

```
$ printf '{{convert|5|km}}\n' | php maintenance/parse.php
<p><a href="...action=edit&redlink=1" class="new"
      title="Template:Convert (page does not exist)">Template:Convert</a>
</p>
```

That is the strongest available argument for holding the call verbatim. A reader with no
template namespace has strictly less information than MediaWiki did, and MediaWiki produced
nothing; inventing an expansion would be fabrication, and dropping the call would discard
the one thing the file actually says.

Three measured facts shape the implementation:

1. **Position decides block or inline.** A template alone in a paragraph becomes a
   `RawBlock` in pandoc; one mid-sentence becomes a `RawInline`. panduck mirrors this with
   `kind`.
2. **Templates nest, and the scanner must balance braces rather than match them.**
   `{{nested|{{inner|x}}|y}}` is measured as ONE raw unit including the inner call.
3. **Contents are not parsed.** `{{Infobox | a = [[Link]] and '''bold''' }}` keeps its
   wikitext verbatim; the `[[Link]]` inside is not a link.

Surfacing `template_name` is **additive**: the block type and content match pandoc exactly,
so it earns no divergence-ledger row, and it turns "which articles carry an Infobox
person?" into a `WHERE` clause instead of a regex over `content`. Writing back to pandoc
JSON yields `RawBlock ["mediawiki", …]`, which is what pandoc itself produces — the
constraint is satisfied and only the attribute, which pandoc has no field for, is dropped.

Template *expansion* is out of scope, permanently, not pending.

## Where this reader diverges from pandoc

One rule decides these: **where pandoc made a deliberate choice, mirror it; where pandoc
leaked non-prose into prose, that is the defect.**

### 1. Leading-space preformatted becomes a `code` block

| | |
|---|---|
| pandoc | `Para [Code "line one", LineBreak, Code "line two"]`, with every space replaced by **U+00A0** |
| panduck | `code` block |

The evidence is an asymmetry inside pandoc itself. Pandoc **writes** a code block as
`<pre>…</pre>` and reads that back as `CodeBlock`. The leading-space form is one pandoc
**never writes**, and reads as a paragraph. A reader/writer pair that disagrees about a
construct is evidence the reader is approximating, and substituting non-breaking spaces is
the signature of code preserving indentation visually because it has no block-level
representation to put it in.

**VERIFIED against MediaWiki itself, 2026-09-02.** An earlier draft of this spec shipped
this premise flagged as unverified — asserted from knowledge of the format, with no
wikitext renderer on the machine to check it against. It has since been checked against
MediaWiki's own parser (see *Verifying against the real parser* below):

```
$ printf ' line one\n line two\n' | php maintenance/parse.php
<pre>line one
line two
</pre>
```

Leading space **is** `<pre>`. So pandoc's `Para[Code, LineBreak, Code]` misrepresents block
structure, panduck's `code` is the faithful reading, and this row is a declared divergence
on measured grounds rather than an argument from pandoc's internal asymmetry.

This is the only *structural* divergence in the design — it changes a block's type, which
is the expensive kind. Writing back yields `CodeBlock`, which is valid pandoc JSON.

### 2. Behavior switches are held raw rather than leaked as text

| | |
|---|---|
| pandoc | `Str "__TOC__"` — the literal token, inside a paragraph, as if an author had typed it as prose |
| panduck | `raw`, `encoding='mediawiki'`, `attributes['source_type']='behavior_switch'` |

`__TOC__`, `__NOTOC__` and `__FORCETOC__` are instructions that expand to content at render
time. **Measured against MediaWiki's own parser, 2026-09-02** — the token is CONSUMED and
never reaches the reader:

```
$ printf '__TOC__\n\nSome text.\n' | php maintenance/parse.php
<p>Some text.
</p>
```

So pandoc emitting `Str "__TOC__"` into a paragraph is not a representational choice with a
defensible reading behind it; it puts a string in the document that MediaWiki guarantees no
reader ever sees.

**An earlier draft of this design said "dropped", and that was wrong.** It is the same kind
of thing as a template — a render-time instruction, unresolvable without the wiki — and
holding one raw while discarding the other rests on no principle. Raw also satisfies "do
not discard information", which dropping does not. The divergence from pandoc is now about
*classification only*: both representations keep the token, and panduck declines to call it
prose. Writing back yields `RawBlock ["mediawiki", "__TOC__"]`.

`<references/>` is **not** in this category and stays `raw`/`html`: pandoc emits
`RawBlock ["html", "<references/>"]`, which is a deliberate choice, so it is mirrored.
Unknown HTML generally follows the same rule — `raw` with `encoding='html'`, which is what
the pandoc-generated fixture will contain, since the mediawiki *writer* injects
`<span id="level_1"></span>` before every heading.

### 3. A named reference reuse keeps its name — free, not a divergence

Measured: `<ref name="a"/>` becomes an **empty** `Note []` in pandoc. The name is in the
source and pandoc discards it.

panduck emits `note` with `attributes['name']='a'`, on both the definition
(`<ref name="a">…</ref>`) and the reuse, so a consumer can join one to the other. Type
matches, content matches, one attribute added — the same additive shape as `template_name`,
so it earns no ledger row. Writing back yields `Note`, and the name is dropped because
pandoc's `Note` has nowhere to put it. That is the export target being lossy, which the
global constraint explicitly permits.

**The divergence ledger for this format therefore has exactly two rows**: preformatted
(structural) and behavior switches (classification only). Both are now backed by
MediaWiki's own parser rather than by argument. Org shipped with an empty ledger; saying
plainly that this one does not is better than letting it look like an oversight.

## Verifying against the real parser

Everything above was measured against pandoc. The three rulings that turn on *what
MediaWiki means* — not what pandoc does with it — were checked against MediaWiki's own
parser, and the recipe is recorded here because the alternative is trusting this document:

```bash
sudo apt-get install -y mediawiki php-sqlite3      # php-sqlite3 is NOT pulled in by mediawiki
php /usr/share/mediawiki/maintenance/install.php \
    --dbtype=sqlite --dbpath=/tmp/wiki --confpath=/tmp/wiki \
    --scriptpath="" --lang=en --pass=<pw> ProbeWiki Admin

# parse.php writes a localisation cache; without these it dies on /var/cache permissions
cat >> /tmp/wiki/LocalSettings.php <<'EOF'
$wgCacheDirectory = "/tmp/wiki/cache";
$wgLocalisationCacheConf['storeDirectory'] = "/tmp/wiki/cache";
EOF

printf ' leading space\n' | php /usr/share/mediawiki/maintenance/parse.php --conf /tmp/wiki/LocalSettings.php
```

This is a throwaway wiki with a SQLite backend and no web server; it exists only to reach
`Parser`. Nothing else in the repo depends on it, and it is not part of `make check` — the
questions it answers are about the *format*, which does not change between runs, so the
answers belong in this document rather than in a job that reinstalls MediaWiki forever.

Both rows are `pandoc_compat` candidates. With these two the ledger across all formats is
large enough that the mode is worth designing rather than speculating about — but that is a
separate spec, and this one does not build it.

## Architecture

`src/mediawiki_scanner.{cpp,hpp}` and `src/mediawiki_reader.{cpp,hpp}`, mirroring Org and
RST — including the seam: **no DuckDB types outside the trailing emission section**,
verified rather than intended. `src/org_scanner.cpp` and `src/rst_scanner.cpp` each have
zero DuckDB references and this one must too. These scanners are the layer that would
survive extraction into `libpanduck`.

**One real departure from Org's pure line scanner.** A `|` inside `{{…}}` is an argument
separator; a `|` at the start of a line is a table row. So a line cannot be classified by
its prefix alone. The scanner carries `{{`/`}}` and `{|`/`|}` nesting depth as it walks,
and a line inside either region is classified as body rather than by its prefix. Because
templates nest, this is brace *balancing*, not matching — measured, not assumed.

That is one added state on top of Org's design, not a different design. Everything else is
line-prefix driven: `=`, `*`, `#`, `;`, `:`, `!`, and leading space.

## Testing

- `test/sql/mediawiki_reader.test`, structured like `org_reader.test` and
  `rst_reader.test`: most assertions through `read_mediawiki_blocks_string` so a construct
  needs no fixture, plus a fixture for path and encoding behaviour.
- **A fixture pair**, hand-written and `pandoc -f mediawiki -t mediawiki`. The generated
  one earns its place here: the writer injects `<span id="level_1"></span>` before every
  heading, which is exactly the kind of thing a hand-written fixture never contains and a
  reader must not choke on.
- Both fixtures join `make check-conformance` automatically — its glob is by extension.
- Added to `test/roundtrip/check_roundtrip.py` with the **two** declared divergences above
  and no others. Any third divergence is a defect or an argument to have, not a row to add
  quietly.
- **The write-back arm** described under the global constraint, which is the test that makes
  "must write valid pandoc JSON" real.

The template-nesting case, the `|`-in-template case, and the three meanings of a
line-initial `*`-versus-`#` run must be asserted in one place, because a scanner that gets
two of them right looks correct on most articles.

## Out of scope, stated rather than silent

Template expansion; MediaWiki **XML dumps** (a `.xml` dump is a corpus of thousands of
articles, which is the `.zim` situation — "read it as a document" has no answer, and the
right response is a refusal that names a better tool, not a concatenation); `<gallery>`;
interwiki and category link resolution; `__TOC__` actually generating a table of contents;
and the `.mw` / `.wikitext` extension spellings, which pandoc also fails to deduce and
which can be added to the registry in one line if anyone wants them.

Every one of these must be **dropped or held raw, never leaked as prose**. That is the
lesson the Org reader paid for: scoped-out `:PROPERTIES:` drawers fell through to text and
joined the following paragraph. Losing structure is a gap; emitting non-content as prose is
a bug.
