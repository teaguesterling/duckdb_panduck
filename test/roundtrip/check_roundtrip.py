#!/usr/bin/env python3
"""Differential validation: panduck's readers against pandoc, over the same bytes.

WHY DIFFERENTIAL RATHER THAN A SELF ROUND-TRIP.  The obvious test is
X == panduck_read(panduck_write(X)).  It needs a writer panduck does not have yet, and
it is the weaker test regardless: a reader and a writer that share one misunderstanding
round-trip perfectly while both being wrong.  Reading the same bytes with two
INDEPENDENT implementations catches misreads that no self round-trip can.  The writer
round-trip becomes worth adding once writers exist -- it catches a different class of
bug (writer defects), not this one.

WHAT "IDENTITY" MEANS.  Two readers never agree byte-for-byte and mostly should not have
to.  canonical.py defines the normal form; this module declares, per case, how far up
the ladder agreement is required:

    text      all visible text, markers stripped.  Catches data loss.
    skeleton  block types and heading levels.  Catches structural loss.
    marked    skeleton plus canonical inline markup.  Strongest cross-reader level.

THE REFERENCE IS NOT GROUND TRUTH.  pandoc's RTF reader loses the space after an em-dash
where panduck preserves it (verified against the original source document).  So a
divergence is triaged, not assumed to be panduck's fault:

    panduck-wrong     -> FAILS.  panduck misread the document.
    reference-wrong   -> recorded.  pandoc is wrong; panduck is not penalised.
    not-implemented   -> recorded.  panduck does not read this construct yet.
    ambiguous         -> recorded with reasoning.

The ledger is a RATCHET in both directions: an unexpected divergence fails, and so does a
declared divergence that has silently started agreeing -- that one should be promoted
rather than left rotting in the ledger.

Exit codes: 0 = pass (or skipped), 1 = a divergence that matters.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import canonical  # noqa: E402

REPO = Path(__file__).resolve().parent.parent.parent
DEFAULT_DUCKDB = REPO / "build" / "release" / "duckdb"

PASS = "pass"
PANDUCK_WRONG = "panduck-wrong"
REFERENCE_WRONG = "reference-wrong"
NOT_IMPLEMENTED = "not-implemented"
AMBIGUOUS = "ambiguous"

LADDER = ["text", "skeleton", "marked", "meta"]


class Case:
    """One fixture, read both ways, compared at every level of the ladder."""

    def __init__(self, path, fmt, reader, expect, note=""):
        self.path = path
        self.fmt = fmt
        self.reader = reader
        #: level -> PASS, or (verdict, explanation) for a declared divergence
        self.expect = expect
        self.note = note

    @property
    def name(self):
        return f"{Path(self.path).name} [{self.fmt}]"


# ---------------------------------------------------------------------------
# Cases.  A format joins this list when its reader lands; the ladder and the
# ledger do not need to change to accommodate it.
# ---------------------------------------------------------------------------
EMDASH = (
    REFERENCE_WRONG,
    "pandoc's RTF reader drops the space after \\u8212: it yields 'caf\u00e9 \u2014em-dash' where "
    "the source document reads 'caf\u00e9 \u2014 em-dash'. panduck matches the source; pandoc does not.",
)

RTF_LO_HEADINGS = (
    REFERENCE_WRONG,
    "panduck resolves {\\stylesheet} \\sN and reports 'Heading One' as heading/1, while "
    "pandoc reads it as Para[Strong[Span]] and detects NO HEADING AT ALL in a LibreOffice "
    "RTF. panduck is the more faithful reader here, not the divergent one -- the same "
    "situation as w:outlineLvl in a LibreOffice DOCX.\n\n"
    "THIS ENTRY USED TO HAVE A SECOND CAUSE AND IT IS GONE. It read: 'panduck does not read "
    "lists yet, so it emits the RTF \\bullet as paragraph text where pandoc emits "
    "list_item'. Lists are read now, via \\ls and \\ilvl, and {\\listtext ...} -- the "
    "RENDERED bullet glyph, written into the file so a non-list-aware renderer shows "
    "something -- is suppressed as the presentation it is. The text level, which used to "
    "carry that literal bullet character, now AGREES with pandoc exactly.",
)

DOCX_SCOPE = (
    NOT_IMPLEMENTED,
    "panduck reads neither lists nor blockquotes yet, so pandoc's list_item and "
    "blockquote markers have no counterpart and every later position shifts. Text agrees "
    "exactly, so nothing is LOST -- the structure around it is not built yet. Tracked in "
    "the registry notes.",
)

DOCX_LO = (
    REFERENCE_WRONG,
    "two causes, and the dominant one is pandoc's. (1) panduck reads w:outlineLvl and "
    "reports 'Heading One' as heading/1; pandoc ignores outlineLvl entirely and emits "
    "Para[Strong] -- its raw JSON for this file is all Para, no Header, so it detects NO "
    "headings in a LibreOffice DOCX. panduck is the more faithful reader, exactly as with "
    "LibreOffice RTF. (2) panduck does not read blockquotes yet, which is panduck's gap.",
)


RST_ADMONITION = (
    REFERENCE_WRONG,
    "pandoc SYNTHESISES a title paragraph for an admonition; panduck records the directive "
    "name as data. MEASURED: `.. note::` yields, in pandoc, Div[note] > Div[title] > "
    "Para 'Note' alongside the body -- and the string 'Note' appears NOWHERE in the source "
    "document. It is docutils' RENDERING convention materialised into the AST.\n\n"
    "panduck emits `div` with attributes['source_type']='note' and the body beneath it. The "
    "same fact, kept queryable instead of spelled as prose. That is better for this "
    "vocabulary in a way worth stating: a consumer asking WHICH admonition reads an "
    "attribute rather than string-matching an injected paragraph, and pandoc's invented "
    "'Note' would otherwise appear in full-text search as document content the author never "
    "wrote -- the same defect class as the pre-5.0 table tuple polluting search results.\n\n"
    "The directive set is OPEN -- docutils ships dozens, Sphinx hundreds -- so recording the "
    "name rather than minting a role per admonition is also what keeps this reader from "
    "needing a vocabulary change per directive.",
)

PANDOC_ODT_LOOSENESS = (
    REFERENCE_WRONG,
    "pandoc's OWN TWO READERS disagree about this document. lists.odt and lists.docx are "
    "generated from the SAME lists.source.md, and pandoc reads the ordered list's items as\n\n"
    "    odt   ->  Plain, Para,  BulletList, Plain\n"
    "    docx  ->  Plain, Plain, BulletList, Plain\n\n"
    "so its ODT reader calls the second item LOOSE and its DOCX reader calls the same item "
    "TIGHT. One document, two readers, two answers -- which makes this a fact about pandoc "
    "rather than a difference panduck could resolve by choosing better.\n\n"
    "panduck emits `list_item` consistently for every item in both, and duck_block does not "
    "model tightness at the item level at all. NOTHING IS LOST: the text agrees exactly, and "
    "only the Plain-versus-Para marker differs. Same class as LATEX_LOOSE_LISTS, where "
    "pandoc's LaTeX reader calls every list loose regardless of the source.",
)

OFFICE_META = (
    REFERENCE_WRONG,
    "panduck recovers document metadata that pandoc drops. MEASURED: `pandoc file.docx "
    "-t json` and `pandoc file.odt -t json` both return an EMPTY meta, on every fixture "
    "here, even though the files plainly carry dcterms:created, dc:language and "
    "meta:generator. Its DOCX and ODT readers do not populate Meta at all -- unlike its "
    "EPUB, LaTeX and RTF readers, where panduck agrees with it exactly.\n\n"
    "This is a DELIBERATE, APPROVED EXCEPTION and the only place panduck exceeds the "
    "reference on metadata. It is narrow in two ways that matter. Every field emitted "
    "carries attributes['source_type'] with its original spelling (`dcterms:created`, "
    "`meta:generator`), so a consumer can always tell format-derived metadata from "
    "pandoc-derived -- without that marker panduck's output stops being reproducible from "
    "`pandoc -t json` and the next person to diff the two reads recovered data as a bug. "
    "And EMPTY fields are skipped here, unlike in the other three readers: LibreOffice "
    "writes <dc:title/> and <dc:creator/> into every file it saves, so emitting them would "
    "diverge from the reference to convey nothing. Every field listed as divergent below "
    "carries real content.",
)

EPUB_SECTION_DIV = (
    REFERENCE_WRONG,
    "panduck maps <section> to the `section` element_type; pandoc emits a generic Div. "
    'MEASURED, and pandoc\'s own output settles it: for <section id="heading-one" '
    'class="level1"> it emits Div with classes ["section", "level1"] -- it SAYS '
    "section, in a CSS class on a container, rather than in a type. duck_block declares "
    "`section` as an element_type with the HTML5 sectioning set as its role vocabulary "
    "exactly so a consumer does not have to parse class lists to find structure. Same "
    "posture as the LibreOffice fixtures: being more faithful than the reference is not a "
    "divergence to fix.",
)

EPUB_LO_CSS = (
    REFERENCE_WRONG,
    "panduck reports the run formatting; pandoc reports none. LibreOffice's EPUB export "
    "emits NO semantic markup -- no <h1>, no <ul>, no <strong> -- only "
    '<p class="paraN"><span class="spanN"> with the meaning in a CSS file. pandoc does not '
    "resolve those classes, so its output is bare Spans and 'bold' comes back unmarked. "
    "panduck resolves .span2 { font-weight: bold } and reports bold, which is what the "
    "document says. Same posture as LibreOffice RTF and DOCX: being MORE faithful than the "
    "reference is not a divergence to fix. Note what panduck deliberately does NOT do -- "
    "'Heading One' stays a bold paragraph, because .span0 says font-size: 16pt and a font "
    "size is evidence for a heading rather than a statement of one.",
)

TEXTILE_ADJACENT_LISTS = (
    REFERENCE_WRONG,
    "a bullet list ADJACENT to an ordered one stays a list. MEASURED on `* bullet` followed "
    "by `# ordered` with no blank line, against python-textile 4.0.2 -- the format's "
    "reference implementation, installed for this -- which yields a list. pandoc yields "
    '`Para [Str "*", Space, Str "bullet"]` and then an OrderedList: the bullet list is '
    "GONE and its marker is left in the document as a literal asterisk.\n\n"
    "That is the leaked-as-prose failure the Org drawers and MediaWiki's <span> anchors each "
    "produced, and the third format in a row to produce it. panduck emits SIBLING lists "
    "where the reference NESTS the second inside the first -- a deliberate departure, "
    "because the reference's own output there places an <ol> as a direct child of a <ul> "
    "rather than inside an <li>, which is invalid HTML and therefore a quirk of that "
    "implementation rather than a statement about the format.",
)

TEXTILE_NOTEXTILE = (
    REFERENCE_WRONG,
    "`notextile.` means DO NOT PROCESS THIS, and pandoc processes it. MEASURED: "
    "`notextile. <b>raw</b>` gives pandoc a paragraph whose first word is the literal string "
    '"notextile.", with the body parsed as textile regardless -- so it both advertises the '
    "marker to the reader and does the one thing the construct exists to prevent. "
    "python-textile strips the marker and passes the body through, which is what panduck "
    "does: `raw` with format='html'.",
)

MEDIAWIKI_PREFORMATTED = (
    REFERENCE_WRONG,
    "a LEADING SPACE is a code block, and this one is settled by MediaWiki rather than by "
    "argument. MEASURED 2026-09-02 against MediaWiki's own parser (maintenance/parse.php on "
    "a throwaway SQLite wiki): ` line one` renders as `<pre>line one</pre>`.\n\n"
    "pandoc reads it as a PARAGRAPH containing inline Code runs joined by LineBreaks, with "
    "every space replaced by U+00A0. That is the signature of a reader approximating block "
    "structure it has no representation for -- and pandoc's own writer never PRODUCES the "
    "construct, writing code blocks as `<pre>` instead, so its reader and writer disagree "
    "about it.\n\n"
    "This is the only STRUCTURAL divergence in the format -- it changes a block's type, "
    "which is the expensive kind. It shipped for one day with the premise flagged as "
    "unverified, because there was no wikitext renderer on the machine; installing one was "
    "cheaper than the argument.",
)

MEDIAWIKI_BEHAVIOR_SWITCH = (
    REFERENCE_WRONG,
    "`__TOC__` is held RAW; pandoc leaks it into a paragraph as the literal string. "
    "MEASURED against MediaWiki's own parser: the switch is CONSUMED -- `__TOC__\\n\\nSome "
    "text.` renders as `<p>Some text.</p>` and the token appears nowhere. So pandoc's "
    '`Str "__TOC__"` puts text in the document that no reader of the wiki would ever '
    "see.\n\n"
    "panduck emits `raw` with encoding='mediawiki' and source_type='behavior_switch'. Note "
    "this is a CLASSIFICATION divergence, not a retention one: both representations keep "
    "the token, and panduck declines to call it prose. An earlier draft of the design said "
    "DROP it, which would have been retention loss, and the template ruling is what showed "
    "that was wrong -- a behavior switch and a template are the same kind of thing, an "
    "instruction that expands at render time and is unresolvable without the wiki.",
)

LATEX_LOOSE_LISTS = (
    REFERENCE_WRONG,
    "pandoc's LaTeX reader wraps every itemize/enumerate item's content in Para -- a "
    "LOOSE list -- regardless of blank lines or \\tightlist. Verified against a minimal "
    "fixture with no blank lines between \\item and no \\tightlist at all: still Para. "
    "\\tightlist is pandoc's own MARKDOWN WRITER convention, a no-op macro it emits so its "
    "LaTeX output round-trips back into a tight list through pandoc itself; pandoc's LaTeX "
    "READER never consults it, so pandoc.tex containing \\tightlist reads exactly as loose "
    "as handwritten.tex, which has no such macro. panduck folds each \\item's content "
    "directly onto its list_item -- the same tight shape panduck uses for every list it "
    "reads, and the one a human writing \\item bullet one would recognise as their list.",
)

LATEX_HYPERTARGET_DIV = (
    REFERENCE_WRONG,
    "pandoc's own LaTeX writer wraps every heading in "
    "\\hypertarget{id}{%\\n\\section{...}\\label{id}} (see the header comment on "
    "handwritten.tex, which a human never writes). Reading that back, pandoc's LaTeX "
    "reader collapses the wrapper for 'Heading One' into a plain Header but leaves an "
    "extra Div around 'Heading Two' -- an inconsistency inside pandoc's own reader, not a "
    "real structural distinction between the two headings in the source. panduck strips "
    "\\hypertarget uniformly and reads through to the heading both times.",
)

CASES = [
    Case(
        "test/fixtures/pandoc_outlinelevel.rtf",
        "rtf",
        "read_rtf_blocks",
        expect={"text": EMDASH, "marked": EMDASH},  # skeleton must agree exactly
        note="pandoc-generated RTF: headings via \\outlinelevel",
    ),
    Case(
        "test/fixtures/pandoc_pstyle.docx",
        "docx",
        "read_docx_blocks",
        expect={"meta": OFFICE_META},  # text must AGREE
        note="pandoc-generated DOCX: headings via w:pStyle",
    ),
    Case(
        "test/fixtures/libreoffice_outlinelvl.docx",
        "docx",
        "read_docx_blocks",
        expect={
            "skeleton": DOCX_LO,
            "marked": DOCX_LO,
            "meta": OFFICE_META,
        },  # text must AGREE
        note="LibreOffice-generated DOCX: headings via w:outlineLvl",
    ),
    Case(
        "test/fixtures/pandoc.odt",
        "odt",
        "read_odt_blocks",
        expect={"meta": OFFICE_META},  # text must AGREE
        note="pandoc-generated ODT",
    ),
    Case(
        "test/fixtures/libreoffice.odt",
        "odt",
        "read_odt_blocks",
        expect={"meta": OFFICE_META},  # text must AGREE
        note="LibreOffice-generated ODT",
    ),
    Case(
        "test/fixtures/pandoc.epub",
        "epub",
        "read_epub_blocks",
        # THIS WAS THE ONLY FIXTURE THAT AGREED AT EVERY LEVEL, and the empty ledger was
        # the assertion: EPUB content documents are XHTML, so both readers see the same
        # tree and there is nothing left to excuse. "If a divergence ever appears here it
        # is a real defect, because no representational gap remains to blame."
        #
        # THE INVARIANT DID ITS JOB AND IS NOW SPENT. It fired on 272c3e4, which mapped
        # <section> to the `section` element_type -- a deliberate improvement, not a
        # defect, but one that ended this fixture's total agreement. Worth stating plainly
        # that something was lost here: an empty ledger is a much stronger assertion than
        # a declared divergence, and no fixture carries it any more.
        #
        # It also went UNSEEN for a day. This harness was not in any aggregated target, so
        # nothing ran it while every other guard stayed green and was reported as such.
        # `make check` now runs all four and reports all four.
        expect={
            "skeleton": EPUB_SECTION_DIV,
            "marked": EPUB_SECTION_DIV,
        },
        note="pandoc-generated EPUB: semantic XHTML",
    ),
    Case(
        "test/fixtures/libreoffice.epub",
        "epub",
        "read_epub_blocks",
        expect={"marked": EPUB_LO_CSS},  # text and skeleton must AGREE
        note="LibreOffice-generated EPUB: no semantic markup, everything in CSS",
    ),
    Case(
        "test/fixtures/libreoffice_stylesheet.rtf",
        "rtf",
        "read_rtf_blocks",
        expect={"skeleton": RTF_LO_HEADINGS, "marked": RTF_LO_HEADINGS},
        note="LibreOffice-generated RTF: headings via {\\stylesheet} \\sN",
    ),
    Case(
        "test/fixtures/handwritten.rst",
        "rst",
        "read_rst_blocks",
        # DELIBERATELY NON-CONVENTIONAL ADORNMENT ORDER: `~` appears first and `=` second,
        # so `~` is level 1 and `=` is level 2. RST sets a heading's level by WHERE its
        # adornment first appeared, not by which character it is. A conventional document
        # cannot tell the right rule from the wrong one -- a reader hardcoding `= -> 1`
        # passes every fixture a person following convention would write.
        expect={
            "text": RST_ADMONITION,
            "skeleton": RST_ADMONITION,
            "marked": RST_ADMONITION,
        },
        note="hand-written RST: ~ before =, so the adornment ORDER decides the levels",
    ),
    Case(
        "test/fixtures/pandoc.rst",
        "rst",
        "read_rst_blocks",
        # pandoc's own writer NORMALISES adornments to = then -, so this fixture uses
        # different characters for the same levels as its handwritten twin. The pair is
        # what proves the rule is about order rather than about the character.
        expect={
            "text": RST_ADMONITION,
            "skeleton": RST_ADMONITION,
            "marked": RST_ADMONITION,
        },
        note="pandoc-generated RST: adornments normalised to = and -",
    ),
    Case(
        "test/fixtures/handwritten.org",
        "org",
        "read_org_blocks",
        expect={},
        note="hand-written Org: no property drawers -- what a person types",
    ),
    Case(
        "test/fixtures/pandoc.org",
        "org",
        "read_org_blocks",
        # PANDOC'S OWN ORG WRITER emits a :PROPERTIES: drawer under every heading, which a
        # person never writes. That pair caught a real defect the moment it existed: the
        # drawer fell through to TEXT and joined the following paragraph, so the body read
        # ":PROPERTIES: :CUSTOM_ID: heading-one :END: Body text...". The handwritten
        # fixture alone would never have found it.
        expect={},
        note="pandoc-generated Org: :PROPERTIES: drawers under every heading",
    ),
    Case(
        "test/fixtures/lists.docx",
        "docx",
        "read_docx_blocks",
        # The ONLY fixture in the tree with an ordered list or a nested one. Generated from
        # lists.source.md, which is kept beside it so this can be rebuilt rather than
        # trusted. It caught a real defect on its first run: a bullet list following an
        # ordered one at the same depth was swallowed into it, because the open/close logic
        # compared list DEPTH and never list TYPE.
        expect={"meta": OFFICE_META},
        note="ordered + nested lists and a blockquote -- paths no other fixture reached",
    ),
    Case(
        "test/fixtures/lists.odt",
        "odt",
        "read_odt_blocks",
        expect={
            "skeleton": PANDOC_ODT_LOOSENESS,
            "marked": PANDOC_ODT_LOOSENESS,
            "meta": OFFICE_META,
        },
        note="ordered + nested lists and a blockquote, ODF side",
    ),
    Case(
        "test/fixtures/handwritten.textile",
        "textile",
        "read_textile_blocks",
        expect={
            "text": TEXTILE_NOTEXTILE,
            "skeleton": TEXTILE_ADJACENT_LISTS,
            "marked": TEXTILE_ADJACENT_LISTS,
        },
        note="hand-written Textile: adjacent lists and a notextile. block",
    ),
    Case(
        "test/fixtures/pandoc.textile",
        "textile",
        "read_textile_blocks",
        # PANDOC'S OWN TEXTILE WRITER emits `h1(#guide-title).` for every heading and a raw
        # `<dl>` for a definition list, neither of which a person types. That pair caught two
        # real defects the moment it existed: the `(#id)` group was read as a CLASS, and the
        # <dl> leaked into a paragraph as literal markup.
        expect={
            "text": TEXTILE_NOTEXTILE,
            "skeleton": TEXTILE_NOTEXTILE,
            "marked": TEXTILE_NOTEXTILE,
        },
        note="pandoc-generated Textile: (#id) anchors and a raw <dl>",
    ),
    Case(
        "test/fixtures/handwritten.wiki",
        "mediawiki",
        "read_mediawiki_blocks",
        expect={
            "text": MEDIAWIKI_PREFORMATTED,
            "skeleton": MEDIAWIKI_PREFORMATTED,
            "marked": MEDIAWIKI_PREFORMATTED,
        },
        note="hand-written MediaWiki: leading-space preformatted and a behavior switch",
    ),
    Case(
        "test/fixtures/pandoc.wiki",
        "mediawiki",
        "read_mediawiki_blocks",
        # PANDOC'S OWN MEDIAWIKI WRITER emits `<span id="..."></span>` before every heading,
        # which a person never types. That pair caught a real defect the moment it existed:
        # the tags arrived as a PARAGRAPH whose text was the literal markup, which is the
        # leaked-as-prose failure the Org drawers taught. The handwritten fixture alone would
        # never have found it.
        expect={
            "text": MEDIAWIKI_PREFORMATTED,
            "skeleton": MEDIAWIKI_PREFORMATTED,
            "marked": MEDIAWIKI_PREFORMATTED,
        },
        note="pandoc-generated MediaWiki: <span id> anchors before every heading",
    ),
    Case(
        "test/fixtures/handwritten.tex",
        "latex",
        "read_latex_blocks",
        # text must AGREE: only the list's looseness diverges, nothing is lost.
        expect={"skeleton": LATEX_LOOSE_LISTS, "marked": LATEX_LOOSE_LISTS},
        note="hand-written LaTeX: no \\hypertarget, no \\tightlist -- what a person writes",
    ),
    Case(
        "test/fixtures/pandoc.tex",
        "latex",
        "read_latex_blocks",
        # Two independent pandoc-reader quirks stack in this fixture: loose list items
        # (LATEX_LOOSE_LISTS, same as handwritten.tex) AND an inconsistent \hypertarget
        # Div around the second heading only (LATEX_HYPERTARGET_DIV). Both are declared
        # here since either one alone would leave the other an unexplained divergence.
        expect={
            "skeleton": (
                REFERENCE_WRONG,
                LATEX_LOOSE_LISTS[1] + " ALSO: " + LATEX_HYPERTARGET_DIV[1],
            ),
            "marked": (
                REFERENCE_WRONG,
                LATEX_LOOSE_LISTS[1] + " ALSO: " + LATEX_HYPERTARGET_DIV[1],
            ),
        },
        note="pandoc-generated LaTeX: \\hypertarget headings, \\tightlist itemize",
    ),
]


def read_pandoc(path, fmt):
    raw = subprocess.run(
        ["pandoc", "-f", fmt, "-t", "json", str(REPO / path)],
        capture_output=True,
        text=True,
    )
    if raw.returncode != 0:
        raise RuntimeError(f"pandoc failed on {path}:\n{raw.stderr}")
    doc = json.loads(raw.stdout)
    return Doc(
        canonical.pandoc_blocks_to_canonical(doc["blocks"]),
        canonical.pandoc_meta_to_flat(doc.get("meta") or {}),
    )


def read_panduck(path, reader, duckdb_bin, extension=None):
    # attributes is a MAP, and DuckDB renders MAPs as {k=v} -- which is not valid JSON.
    # Project the attributes this comparison needs into plain columns instead.
    #
    # NO [1] ON THESE. map['key'] already yields the VARCHAR; map_extract() is the one that
    # returns a list. These read attributes['heading_level'][1] for three readers, which
    # indexes the STRING and takes its first character -- and passed the whole time,
    # because every heading level is one digit. The first multi-character attribute in the
    # suite (an EPUB link's href) is what exposed it, comparing 'c' against
    # 'ch%202.xhtml#top'. A guard that only ever sees one-character values is not a guard.
    sql = (
        "SELECT kind, element_type, content, "
        "attributes['heading_level'] AS heading_level, "
        "attributes['href'] AS href, "
        "attributes['src'] AS src, "
        "attributes['key'] AS key "
        f"FROM {reader}('{path}') ORDER BY element_order;"
    )
    # CI runs a stock duckdb against the extension artifact the build matrix already
    # produced, rather than rebuilding panduck: -unsigned is required to LOAD a locally
    # built .duckdb_extension. Locally, the statically linked build needs neither.
    cmd = [str(duckdb_bin)]
    if extension:
        cmd.append("-unsigned")
        sql = f"LOAD '{extension}'; " + sql
    cmd += ["-json", "-c", sql]
    raw = subprocess.run(cmd, capture_output=True, text=True, cwd=REPO)
    if raw.returncode != 0:
        raise RuntimeError(f"panduck failed on {path}:\n{raw.stderr}")
    rows = json.loads(raw.stdout or "[]")
    for r in rows:
        attrs = {}
        if r.get("heading_level") is not None:
            attrs["heading_level"] = r["heading_level"]
        if r.get("href") is not None:
            attrs["href"] = r["href"]
        if r.get("src") is not None:
            attrs["src"] = r["src"]
        if r.get("key") is not None:
            attrs["key"] = r["key"]
        r["attributes"] = attrs
    return Doc(canonical.duckblocks_to_canonical(rows), canonical.meta_from_duckblocks(rows))


class Doc:
    """A document's two axes. Metadata is not body content and pandoc does not carry it in
    `blocks`, so folding the two together would compare each against the other's absence.
    """

    def __init__(self, blocks, meta):
        self.blocks = blocks
        self.meta = meta


def compare(level, a, b):
    if level == "meta":
        return a.meta == b.meta, a.meta, b.meta
    fn = canonical.LEVELS[level]
    return fn(a.blocks) == fn(b.blocks), fn(a.blocks), fn(b.blocks)


def render_diff(level, got, ref, limit=6):
    lines = []
    if level == "meta":
        # A DICT, not a list of blocks. Indexing it positionally raised KeyError and took
        # the whole run down BEFORE the two LaTeX cases ran -- so a crash in the diff
        # renderer hid two real metadata failures behind one. Third instance today of an
        # abort making later checks invisible, and this one was mine, written an hour
        # after committing a Makefile comment about exactly this.
        for key in sorted(set(got) | set(ref)):
            g, r = got.get(key), ref.get(key)
            if g != r:
                lines.append(f"      {key}: panduck={g!r} pandoc={r!r}")
        return lines[:limit]
    if level == "text":
        lines.append(f"      panduck: {got[:160]!r}")
        lines.append(f"      pandoc : {ref[:160]!r}")
        return lines
    shown = 0
    for i in range(max(len(got), len(ref))):
        g = got[i] if i < len(got) else None
        r = ref[i] if i < len(ref) else None
        if g != r:
            lines.append(f"      [{i}] panduck={g!r}")
            lines.append(f"          pandoc ={r!r}")
            shown += 1
            if shown >= limit:
                lines.append(f"      ... ({max(len(got), len(ref)) - i - 1} more positions)")
                break
    return lines


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--report", action="store_true", help="show divergences without asserting")
    ap.add_argument(
        "--require",
        action="store_true",
        help="treat a missing prerequisite as a failure instead of a skip. CI passes this: "
        "a job that silently skips reports coverage it is not providing.",
    )
    ap.add_argument(
        "--duckdb",
        default=os.environ.get("PANDUCK_DUCKDB"),
        help="duckdb binary to use",
    )
    ap.add_argument(
        "--extension",
        default=os.environ.get("PANDUCK_EXTENSION"),
        help="panduck.duckdb_extension to LOAD (implies -unsigned); omit when panduck is linked in",
    )
    args = ap.parse_args()
    report_only = args.report

    duckdb_bin = Path(args.duckdb) if args.duckdb else DEFAULT_DUCKDB
    extension = Path(args.extension).resolve() if args.extension else None

    def missing(msg):
        if args.require:
            print(f"FAIL: {msg} (--require)", file=sys.stderr)
            return 1
        print(f"SKIP: {msg}")
        return 0

    if not shutil.which("pandoc"):
        return missing("pandoc not on PATH; cannot run differential validation")
    if not (duckdb_bin.exists() or shutil.which(str(duckdb_bin))):
        return missing(f"duckdb binary not found at {duckdb_bin}; run `make release` first")
    if extension and not extension.exists():
        return missing(f"extension not found at {extension}")

    version = subprocess.run(["pandoc", "--version"], capture_output=True, text=True).stdout.splitlines()[0]
    print(f"Differential validation of panduck's readers against {version}")
    print(f"  duckdb: {duckdb_bin}" + (f"  (LOAD {extension.name})" if extension else "  (linked in)"))
    print()

    failures = 0
    for case in CASES:
        print(f"  {case.name}")
        if case.note:
            print(f"    {case.note}")
        try:
            ref = read_pandoc(case.path, case.fmt)
            got = read_panduck(case.path, case.reader, duckdb_bin, extension)
        except RuntimeError as exc:
            print(f"    FAIL: {exc}")
            failures += 1
            continue

        for level in LADDER:
            agree, g, r = compare(level, got, ref)
            declared = case.expect.get(level, PASS)

            if report_only:
                status = "agree" if agree else "DIVERGE"
                print(f"    {level:9s} {status}")
                if not agree:
                    for line in render_diff(level, g, r):
                        print(line)
                continue

            if declared == PASS:
                if agree:
                    print(f"    {level:9s} agree")
                else:
                    print(f"    {level:9s} FAIL -- expected agreement")
                    for line in render_diff(level, g, r):
                        print(line)
                    failures += 1
            else:
                verdict, why = declared
                if verdict == PANDUCK_WRONG:
                    print(f"    {level:9s} FAIL -- known panduck defect: {why}")
                    failures += 1
                elif agree:
                    # The ratchet: a declared divergence that now agrees is stale.
                    print(f"    {level:9s} FAIL -- declared '{verdict}' but they now AGREE.")
                    print(f"              Promote this level to pass. ({why})")
                    failures += 1
                else:
                    print(f"    {level:9s} diverges as declared [{verdict}] -- {why}")
        print()

    if report_only:
        print("(report mode: nothing asserted)")
        return 0
    if failures:
        print(f"FAIL: {failures} problem(s).")
        return 1
    print("OK: panduck agrees with pandoc everywhere it is declared to, and diverges")
    print("    only where the ledger says it should.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
