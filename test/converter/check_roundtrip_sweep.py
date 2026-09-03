#!/usr/bin/env python3
"""Sweep EVERY block type through the paths that enumerate types.

Two failure classes, both found repeatedly in one week, both invisible to code
review because the code was CORRECT for every type that existed when it was
written -- the defect is created by a later addition, elsewhere:

  WRITE-ONLY   a type exports fine and cannot be read back, so a round trip
               silently downgrades it. `section` became `div`; `page_break`
               became `div`. Neither was findable by looking at section or
               page_break.

  DROPPED      a container's child walk enumerates type names, so a type it does
               not know vanishes. `table`, `deflist` and `lineblock` were lost
               inside every div, blockquote, figure and caption -- for weeks.

A sweep produces CANDIDATES, not findings. Three candidates here were investigated
and are NOT defects; they are listed in INHERENT with their reasoning so the sweep
cannot re-raise them and nobody re-investigates. Recording the negatives is the
part that keeps a sweep usable -- an unexplained exclusion and a forgotten defect
look identical.

Set DUCK_BLOCK_CHECKS_STRICT=1 to make a skip a failure (see the other checks).
"""

import os
import argparse
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
# test/converter/ -> repo root. This lived at test/ upstream, where one parent was right;
# moving it a directory deeper silently made REPO point at test/ and the binary search
# found nothing, which reported as a SKIP rather than a failure. A path bug that reports
# "nothing to do" is the shape worth naming: the run stayed green while checking nothing.
REPO = HERE.parent.parent

STRUCT = (
    'STRUCT(kind VARCHAR, element_type VARCHAR, "content" VARCHAR, "level" INTEGER, '
    '"encoding" VARCHAR, attributes MAP(VARCHAR,VARCHAR), element_order INTEGER)'
)

# element_type -> (content SQL, encoding, attributes SQL, needs a child block?)
PROBES = {
    "heading": ("'H'", "text", "MAP{'heading_level':'2'}", False),
    "paragraph": ("'x'", "text", "MAP{}", False),
    "plain": ("'x'", "text", "MAP{}", False),
    "code": ("'x'", "text", "MAP{}", False),
    "hr": ("'x'", "text", "MAP{}", False),
    "lineblock": ("'x'", "text", "MAP{}", False),
    "raw": ("'<b>x</b>'", "html", "MAP{'format':'html'}", False),
    "image": ("'alt'", "text", "MAP{'src':'i.png'}", False),
    "section": ("NULL", "text", "MAP{'role':'article'}", True),
    "page_break": ("''", "text", "MAP{'page_number':'3'}", False),
    "generic": (
        r"""'{"t":"Marquee","c":[]}'""",
        "json",
        "MAP{'source_type':'Marquee'}",
        False,
    ),
    "blockquote": ("NULL", "text", "MAP{}", True),
    "div": ("NULL", "text", "MAP{}", True),
    "figure": ("NULL", "text", "MAP{}", True),
    "caption": ("NULL", "text", "MAP{}", True),
    "list": ("NULL", "text", "MAP{'list_type':'bullet'}", True),
    "table": (r"""'{"headers":["h"],"rows":[["c"]]}'""", "json", "MAP{}", False),
    "deflist": ("'x'", "text", "MAP{}", False),
}

# A child that is not a paragraph, where the generic one would be malformed.
CHILD_OVERRIDE = {
    "list": "{'kind':'block','element_type':'list_item','content':'i','level':2,"
    "'encoding':'text','attributes':MAP{},'element_order':1}",
}

# THE ARM'S OWN COVERAGE, which was the hole. This loop probes a hand-written dict
# while the content arm below is driven by duck_block_type_names(). So a type could be
# declared, exported, and covered ONLY by "its text appears somewhere in the AST" --
# and 28 of 43 were. A bare Para/Str fallback satisfies that probe while destroying
# the type completely, which is the same defect duckeye found in a guard asserting a
# cell's text against a raw Table AST that contains the text either way: the check
# measured CONTENT where the property is STRUCTURE.
#
# So every declared type must be probed here or excused here, with the reason.
NOT_A_BLOCK = (
    {
        t: "inline; in block position the exporter correctly wraps it in Para, so a "
        "round trip to itself is not the property. Covered by the NESTED arm and the "
        "inline tests."
        for t in (
            "bold italic underline strikethrough smallcaps superscript subscript span "
            "link cite note quoted math text space softbreak linebreak"
        ).split()
    }
    | {
        t: "kind='value'. Document metadata is not body content -- it lands in the "
        "document's `meta`, never in `blocks`, so a block round trip is meaningless."
        for t in "blocks inlines map string bool version metadata".split()
    }
    | {
        "list_item": "Requires a parent list; standalone it is malformed. Probed with a "
        "real parent in the content arm, which is the shape that occurs.",
    }
)

# Round trips that do NOT preserve the type, investigated and found inherent.
# Each entry is (what it becomes, why it cannot be otherwise).
INHERENT = {
    "image": (
        "paragraph",
        "Pandoc has no block Image constructor. Para[Image] is its only encoding, so a "
        "block image and a paragraph containing one image are the SAME document to it. "
        "Unlike section and page_break -- where the exporter writes a recoverable marker "
        "-- there is nothing to write, so promoting invents a distinction the source "
        "cannot carry. Was 'fixed' once; the existing tests caught that it destroyed the "
        "alt text.",
    ),
    "deflist": (
        "paragraph",
        "SUPERSEDED, and the supported shape works: `list` with list_type='definition' "
        "round-trips to itself with role=term/definition intact (measured). The exporter "
        "has no deflist arm, so a producer still emitting deflist loses the structure -- "
        "which is what the deflist_superseded advisory rule exists to say before it "
        "happens. Not inherent to Pandoc, which has DefinitionList; inherent to the "
        "deprecation.",
    ),
    "caption": (
        "paragraph",
        "Only a STANDALONE caption, which is malformed anyway -- a caption belongs to "
        "the container before it. Inside a figure it round-trips: figure > plain > "
        "caption > plain. The sweep's synthetic probe is unrepresentative here.",
    ),
}

# Constructors that must survive inside a container, with a probe string to find.
NESTED = [
    ("CodeBlock", r'{"t":"CodeBlock","c":[["",[],[]],"NESTPROBE"]}', "NESTPROBE"),
    ("HorizontalRule", r'{"t":"HorizontalRule"}', "HorizontalRule"),
    (
        "LineBlock",
        r'{"t":"LineBlock","c":[[{"t":"Str","c":"NESTPROBE"}]]}',
        "NESTPROBE",
    ),
    (
        "BulletList",
        r'{"t":"BulletList","c":[[{"t":"Plain","c":[{"t":"Str","c":"NESTPROBE"}]}]]}',
        "NESTPROBE",
    ),
    (
        "DefinitionList",
        r'{"t":"DefinitionList","c":[[[{"t":"Str","c":"NESTPROBE"}],[[{"t":"Plain","c":[{"t":"Str","c":"d"}]}]]]]}',
        "NESTPROBE",
    ),
    ("Plain", r'{"t":"Plain","c":[{"t":"Str","c":"NESTPROBE"}]}', "NESTPROBE"),
    (
        "Table",
        r'{"t":"Table","c":[["",[],[]],[null,[]],[],[["",[],[]],[[["",[],[]],[[["",[],[]],'
        r'{"t":"AlignDefault"},1,1,[{"t":"Plain","c":[{"t":"Str","c":"NESTPROBE"}]}]]]]]],[],[["",[],[]],[]]]}',
        "NESTPROBE",
    ),
]


# THIRD ARM. A container carrying its OWN text must not lose it on export.
#
# Neither arm above can see this class. The write-only arm compares element_type, so a
# type that survives with its text gone passes. The containment arm builds its probes
# from Pandoc AST, so it only ever exercises the shapes THIS repo's reader produces --
# and a reader and an exporter written together share their misunderstandings. The
# defect that prompted this arm was exactly that: the reader wrote an image's alt text
# into BOTH `content` and `attributes['alt']`, the exporter read only the attribute, and
# every round trip through this repo looked clean while any other producer -- one
# following the vocabulary's content rule, which says the text goes in `content` -- lost
# the alt silently.
#
# So these probes are HAND-BUILT rather than read from AST. That is the whole point:
# they are the only thing here that does not go through the reader first.
CONTENT_PROBE = "SWEEPTEXTZ"

# Types needing an attribute before the probe means anything.
CONTENT_ATTRS = {
    "image": "MAP{'src':'i.png'}",
    "raw": "MAP{'format':'html'}",
    "heading": "MAP{'heading_level':'2'}",
    "code": "MAP{'language':'sql'}",
}

# Types whose text legitimately has nowhere to go, with the reason. As in INHERENT, the
# negatives are the part worth writing down: an unexplained exemption and a forgotten
# defect look identical six months later.
CONTENT_EXEMPT = {
    "hr": "HorizontalRule has no text position at all -- Pandoc's constructor takes no arguments.",
    "page_break": "A marker. It exports as an empty classed Div by design; it owns no blocks.",
    "list": "A list's text lives in its ITEMS. A list carrying content directly is malformed, "
    "and list_item is probed with a real parent below, which is the shape that matters.",
    "table": "content is the native {headers,rows} JSON, so a bare word is not a table. Real "
    "tables round-trip through the preserved pandoc_ast tuple and are tested in "
    "pandoc_blocks_v2.test.",
    "metadata": "kind='value', not a block -- it lands in the document's `meta`, not in `blocks`.",
}


# FOURTH ARM. The same text, through the RENDER path rather than the export path.
#
# Added on a tip from duckdb_markdown, who ran the content arm above against their own
# code and found FOUR instances -- div, section, figure, caption -- of a rule they had
# already fixed for list_item that same evening. Given a rule and one symptom they
# repaired the symptom and left the class intact in four more places, which is what a
# sweep converts into a list of sites and re-reading the rule does not.
#
# Their `caption` is the case that argues for this arm specifically: a structural branch
# consumed a childless caption and emitted nothing, which SHADOWED the leaf renderer --
# so their first fix was live, correct, and unreachable. A fix that cannot be reached and
# a fix that does not work produce identical output.
#
# The export arm above cannot see any of that: a type can export its text perfectly and
# still render as nothing, and this repo shipped exactly that combination earlier the same
# day (figure, caption and list carrying content rendered as nothing while to_text
# returned it). Two paths, two arms.
RENDER_EXEMPT = {
    "hr": "A rule. No text position in either path -- same reason as the export arm.",
    "page_break": "A marker. It renders as a break; text on it has no meaning.",
    "table": "content is the native {headers,rows} JSON, so a bare word is not a table "
    "and the renderer has nothing to project. Real tables are covered in "
    "render_ansi.test.",
    "metadata": "kind='value'. Document metadata is not body content, so the renderer "
    "correctly declines to draw it; the probe builds it as a block.",
    "raw": "DELIBERATE, and only in to_text: raw markup is omitted so that searching for "
    "`script` does not match `<script>`. Investigated when the agreement guard "
    "first flagged it; it renders fine, which is the half that matters on screen.",
}


def skip(reason: str) -> int:
    if os.environ.get("DUCK_BLOCK_CHECKS_STRICT") == "1":
        print(f"FAIL: {reason}")
        print("      DUCK_BLOCK_CHECKS_STRICT=1 is set, so a skipped check is a failed check.")
        return 1
    print(f"SKIP: {reason}")
    return 0


#: Set by main() from --load, and prepended to every statement. Empty once the converter
#: lives here and is linked into panduck's own build.
LOAD_PREFIX = ""


def duckdb_bin():
    for candidate in ("build/release/duckdb", "build/debug/duckdb"):
        path = REPO / candidate
        if path.exists():
            return path
    return None


def run(duckdb, sql: str) -> str:
    proc = subprocess.run(
        [str(duckdb), "-unsigned", "-noheader", "-list", "-c", LOAD_PREFIX + sql],
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        return "<ERROR> " + proc.stderr.strip().splitlines()[0] if proc.stderr.strip() else "<ERROR>"
    return proc.stdout.strip().splitlines()[0] if proc.stdout.strip() else ""


def run_all(duckdb, sql: str) -> str:
    """Whole output, not the first line.

    `run` above returns line ONE, which is right for a scalar and wrong for anything
    rendered: a code block puts its language on line 1 and its text on line 2, so the
    render arm reported `code` as dropping text that was plainly on screen. A harness
    that truncates its own evidence produces a confident false result in whichever
    direction the truncation happens to fall -- here a false positive; duckdb_markdown
    hit the same class as a false NEGATIVE the same evening, when their lint bridge
    swallowed stderr and a failing query came back as "no findings".

    Same lesson either way: before trusting a new bridge, run something through it whose
    answer you already know.
    """
    proc = subprocess.run(
        [str(duckdb), "-unsigned", "-noheader", "-list", "-c", LOAD_PREFIX + sql],
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        return "<ERROR> " + proc.stderr.strip()
    return proc.stdout


def main() -> int:
    # DURING THE HANDOFF this runs against the converter still living in
    # duckdb_duck_block_utils, loaded into panduck's binary. That is the point of the
    # sequencing: the net is green HERE before the code moves under it, so a regression
    # after the move is unambiguously caused by the move.
    #
    # Once the converter is linked into panduck, --load is simply omitted and the same
    # assertions run against the local build with no other change.
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument(
        "--load",
        default=os.environ.get("PANDUCK_CONVERTER_EXT", ""),
        help="path to an extension providing the converter; omit once it is local",
    )
    args = ap.parse_args()
    global LOAD_PREFIX
    if args.load:
        if not os.path.exists(args.load):
            return skip(f"converter extension not found at {args.load}")
        LOAD_PREFIX = f"LOAD '{args.load}'; "

    duckdb = duckdb_bin()
    if duckdb is None:
        return skip("no built duckdb binary (run `make` first)")

    failed = False

    print("Round-trip sweep: export then read back, every block type")
    for ty, (content, enc, attrs, needs_child) in sorted(PROBES.items()):
        child = ""
        if needs_child:
            child = (
                ", "
                + (
                    CHILD_OVERRIDE.get(ty)
                    or f"{{'kind':'block','element_type':'paragraph','content':'inner','level':2,"
                    f"'encoding':'text','attributes':MAP{{}},'element_order':1}}"
                )
                + f"::{STRUCT}"
            )
        sql = (
            f"SELECT coalesce((SELECT b.element_type FROM (SELECT unnest(pandoc_ast_to_blocks("
            f"duck_blocks_to_pandoc_blocks([{{'kind':'block','element_type':'{ty}','content':{content},"
            f"'level':1,'encoding':'{enc}','attributes':{attrs},'element_order':0}}::{STRUCT}{child}]"
            f")::VARCHAR)) AS b) WHERE b.kind='block' LIMIT 1), '<NOTHING>');"
        )
        got = run(duckdb, sql)
        if got == ty:
            continue
        if ty in INHERENT and got == INHERENT[ty][0]:
            continue
        failed = True
        print(f"\nFAIL: `{ty}` does not survive a round trip -- it reads back as `{got}`.")
        if ty in INHERENT:
            print(f"      Expected `{INHERENT[ty][0]}` by the recorded exception, got `{got}`.")
        else:
            print("      Either the reader cannot read what the exporter writes (add the read side),")
            print("      or the encoding genuinely cannot carry the distinction -- in which case add")
            print("      it to INHERENT with the reasoning rather than 'fixing' it.")
    if not failed:
        n_inherent = len(INHERENT)
        print(
            f"  {len(PROBES) - n_inherent} types round-trip to themselves; "
            f"{n_inherent} recorded as inherent ({', '.join(sorted(INHERENT))})"
        )

    # COVERAGE OF THIS ARM, driven from the build rather than from the dict above.
    # A hand-written probe list silently stops covering a type the moment one is added,
    # and the summary line still says "every block type". Adding a type is precisely the
    # change whose author is thinking about the new type rather than about the loops
    # that enumerate them.
    declared = set(
        run(
            duckdb,
            "SELECT string_agg(t, ' ') FROM (SELECT unnest(duck_block_type_names()) AS t);",
        ).split()
    )
    unaccounted = sorted(declared - set(PROBES) - set(NOT_A_BLOCK))
    if unaccounted:
        failed = True
        print(f"\nFAIL: {len(unaccounted)} declared type(s) are neither probed nor excused here: {unaccounted}")
        print("      Add a PROBES entry, or a NOT_A_BLOCK entry saying why a block round trip")
        print("      is not the property. Leaving it out is not a third option: the content arm")
        print("      below still passes it, and 'its text appears somewhere in the AST' is")
        print("      satisfied by a fallback that destroys the type.")
    stale_keys = sorted((set(PROBES) | set(NOT_A_BLOCK)) - declared)
    if stale_keys:
        failed = True
        print(f"\nFAIL: {stale_keys} named here but no longer declared by the build.")
        print("      A probe for a vanished type fails and looks exactly like a defect; an")
        print("      exemption for one goes on excusing nothing, forever.")
    # And the excuses expire the same way the others do: a type recorded as "not a block"
    # that DOES round-trip to itself in block position has become block-capable, and the
    # entry is now hiding a type nobody probes.
    revived = []
    for ty in sorted(NOT_A_BLOCK):
        sql = (
            f"SELECT coalesce((SELECT b.element_type FROM (SELECT unnest(pandoc_ast_to_blocks("
            f"duck_blocks_to_pandoc_blocks([{{'kind':'block','element_type':'{ty}','content':'x',"
            f"'level':1,'encoding':'text','attributes':MAP{{}},'element_order':0}}::{STRUCT}]"
            f")::VARCHAR)) AS b) WHERE b.kind='block' LIMIT 1), '<NOTHING>');"
        )
        if run(duckdb, sql) == ty:
            revived.append(ty)
    if revived:
        failed = True
        print(f"\nFAIL: {revived} round-trip to themselves as blocks, so the excuse has expired.")
        print("      Move them to PROBES -- an expired exemption hides the next regression")
        print("      behind an explanation nobody rechecks.")
    if not failed:
        print(f"    coverage: all {len(declared)} declared types are probed here or excused with a reason")

    # Three containers, not one. This arm probed only a Div until 2026-09-01, and that
    # is exactly why a definition list silently dropped every block after the first for
    # as long as `list_type='definition'` had existed: the defect was one level down a
    # container this never opened. A list item and a definition each have their OWN walk
    # over the same ListItem struct, and the definition walk read one field of it.
    #
    # `%s` is where the probe's constructor goes.
    #
    # EVERY shell carries a LEAD PARAGRAPH before the probe, so each container is
    # exercised with TWO block children. A walk that stops after the first child
    # passes a single-child probe, which is how a definition dropped everything after
    # the first for as long as list_type='definition' had existed.
    #
    # BlockQuote and Figure were added 2026-09-01 after duckdb_markdown found a
    # multi-block blockquote flattened into one run-together string in THEIR reader --
    # `> quoted para\n>\n> second para` came back as "quoted parasecond para". Their
    # diagnosis is the part worth copying: what hid it was not a weak assertion but a
    # missing DIRECTION. Their check ran someone else's blocks through their writer,
    # a path that was always correct; reader-through-writer for a multi-block
    # container had no coverage at all. This arm is that direction, and it did not
    # open a quote either. The behaviour here was correct -- verified before adding
    # the shells -- which is exactly when a coverage gap is cheapest to close and
    # least likely to be noticed.
    CONTAINERS = [
        (
            "Div",
            '{"t":"Div","c":[["",[],[]],[{"t":"Para","c":[{"t":"Str","c":"lead"}]},%s]]}',
        ),
        (
            "a blockquote",
            '{"t":"BlockQuote","c":[{"t":"Para","c":[{"t":"Str","c":"lead"}]},%s]}',
        ),
        (
            "a figure",
            '{"t":"Figure","c":[["",[],[]],[null,[]],' '[{"t":"Para","c":[{"t":"Str","c":"lead"}]},%s]]}',
        ),
        (
            "a list item",
            '{"t":"BulletList","c":[[{"t":"Para","c":[{"t":"Str","c":"lead"}]},%s]]}',
        ),
        (
            "a definition",
            '{"t":"DefinitionList","c":[[[{"t":"Str","c":"term"}],'
            '[[{"t":"Para","c":[{"t":"Str","c":"lead"}]},%s]]]]}',
        ),
    ]

    print("Containment sweep: every constructor inside each container that has its own walk")
    for cname, shell in CONTAINERS:
        for name, inner, probe in NESTED:
            doc = '{"pandoc-api-version":[1,23,1],"meta":{},"blocks":[' + (shell % inner) + "]}"
            got = run(
                duckdb,
                f"SELECT duck_blocks_to_pandoc_blocks(pandoc_ast_to_blocks('{doc}'))::VARCHAR;",
            )
            if probe in got:
                continue
            failed = True
            # "not present" covers two different findings and they need different fixes:
            # the element may be gone, or it may have DEGRADED into something else (a Div
            # classed with its type name -- the shared fallback's output). Saying DROPPED
            # for both sends the reader looking for a deletion that is not there.
            shape = "DEGRADED to a classed Div" if '"Div"' in got else "DROPPED"
            print(f"\nFAIL: `{name}` is {shape} inside {cname}.")
            print("      A container's child walk must not decide whether a child EXISTS by")
            print("      enumerating type names, and must not stop after the FIRST child.")
            print("      Every container with its own walk needs the same terminal arm; where")
            print("      two walks read one structure, they must read all of it.")
    if not failed:
        print(f"  all {len(NESTED)} constructors survive inside each of {len(CONTAINERS)} containers")

    # FIFTH ARM, from duckdb_markdown, who ran it over their own 43 types after their
    # reader flattened a multi-block blockquote into one run-together string. The
    # containment arm above is pandoc-AST-driven and probes five containers; this one is
    # BUILD-driven and probes every declared type in container position, so it cannot
    # shrink away from its own claim the way a hand-written list does.
    #
    # Two failure modes, because they need different fixes and one assertion sees only
    # one of them: JOINED (ALPHA + BETA come back as ALPHABETA -- a text extractor ran
    # where a block walk belonged) and DROPPED (a child is simply gone).
    print("Container sweep: every declared type holding TWO block children")
    kids = (
        f"{{'kind':'block','element_type':'paragraph','content':'ALPHA','level':2,"
        f"'encoding':'text','attributes':MAP{{}},'element_order':1}}::{STRUCT},"
        f"{{'kind':'block','element_type':'paragraph','content':'BETA','level':2,"
        f"'encoding':'text','attributes':MAP{{}},'element_order':2}}::{STRUCT}"
    )
    sweep_sql = (
        "WITH t AS (SELECT unnest(duck_block_type_names()) AS ty),"
        "     d AS (SELECT ty, duck_blocks_to_pandoc_blocks(["
        f"{{'kind':'block','element_type':ty,'content':NULL,'level':1,'encoding':'text',"
        f"'attributes':MAP{{}},'element_order':0}}::{STRUCT},{kids}])::VARCHAR AS ast FROM t)"
        " SELECT count(*) || ' ' || count(*) FILTER (WHERE ast LIKE '%ALPHABETA%')"
        " || ' ' || count(*) FILTER (WHERE ast NOT LIKE '%ALPHA%')"
        " || ' ' || count(*) FILTER (WHERE ast NOT LIKE '%BETA%') FROM d;"
    )
    swept, joined, alpha_gone, beta_gone = (int(x) for x in run(duckdb, sweep_sql).split())
    n_declared = len(declared)
    if (joined, alpha_gone, beta_gone) != (0, 0, 0):
        failed = True
        print(
            f"\nFAIL: containers that JOINED their children: {joined}; that dropped one: " f"{alpha_gone + beta_gone}."
        )
        print("      A container with block children must emit them as blocks. Joining is the")
        print("      worse half: the words survive, so any check asking whether the text is")
        print("      still there passes on the destroyed output.")
    # THE COUNT IS PART OF THE ASSERTION, not decoration. It swept 47 rows against 43
    # declared types the first time it ran, because duck_block_type_names() enumerated
    # CONSTANTS and four names live on two axes -- code, image and raw as block and
    # inline, list as block and value. Every check here built a set() from that function,
    # which is exactly the measurement that hides multiplicity.
    if swept != n_declared:
        failed = True
        print(f"\nFAIL: swept {swept} rows for {n_declared} declared types.")
        print("      duck_block_type_names() is returning duplicates, so a consumer counting")
        print("      it gets the wrong vocabulary size and any join against it double-counts.")
    if not failed:
        print(f"  all {swept} types emit two block children as two blocks, neither joined nor dropped")
        print("    (detector control: 'ALPHABETA' matches the JOINED probe, 'ALPHA' matches DROPPED)")

    print("Content sweep: every block type, hand-built, must not lose its own text")
    types = run(
        duckdb,
        "SELECT string_agg(t, ' ') FROM (SELECT unnest(duck_block_type_names()) AS t);",
    ).split()
    checked = 0
    for ty in sorted(set(types)):
        if ty in CONTENT_EXEMPT:
            continue
        attrs = CONTENT_ATTRS.get(ty, "MAP{}")
        blk = (
            f"{{'kind':'block','element_type':'{ty}','content':'{CONTENT_PROBE}','level':%d,"
            f"'encoding':'text','attributes':{attrs},'element_order':%d}}::{STRUCT}"
        )
        # list_item gets a real parent: standalone it is malformed, and probing a
        # malformed shape would report a defect the vocabulary does not have.
        if ty == "list_item":
            parent = (
                f"{{'kind':'block','element_type':'list','content':NULL,'level':1,'encoding':'text',"
                f"'attributes':MAP{{'list_type':'bullet'}},'element_order':0}}::{STRUCT}"
            )
            doc_sql = f"[{parent}, {blk % (2, 1)}]"
        else:
            doc_sql = f"[{blk % (1, 0)}]"
        checked += 1
        got = run(duckdb, f"SELECT duck_blocks_to_pandoc_blocks({doc_sql})::VARCHAR;")
        if CONTENT_PROBE in got:
            continue
        failed = True
        print(f"\nFAIL: `{ty}` carries text in `content` and the exporter DROPS it.")
        print(f"      got: {got[:160]}")
        print("      The vocabulary's content rule says a single text child lives in `content`,")
        print("      so an exporter reading only an attribute loses it for every producer but")
        print("      one that happens to write both. Read `content` as the fallback, or add the")
        print("      type to CONTENT_EXEMPT with the reason its text has nowhere to go.")
    if not failed:
        print(
            f"  {checked} types keep their text; {len(CONTENT_EXEMPT)} exempt " f"({', '.join(sorted(CONTENT_EXEMPT))})"
        )

    print("Render sweep: the same text, through render_ansi and to_text")
    rendered = 0
    for ty in sorted(set(types)):
        if ty in RENDER_EXEMPT:
            continue
        attrs = CONTENT_ATTRS.get(ty, "MAP{}")
        blk = (
            f"[{{'kind':'block','element_type':'{ty}','content':'{CONTENT_PROBE}','level':1,"
            f"'encoding':'text','attributes':{attrs},'element_order':0}}::{STRUCT}]"
        )
        rendered += 1
        shown = run_all(
            duckdb,
            "SELECT regexp_replace(duck_blocks_render_ansi(" + blk + ", 40), '\x1b\\[[0-9;]*m', '', 'g');",
        )
        text = run_all(duckdb, f"SELECT duck_blocks_to_text({blk});")
        missing = [n for n, v in (("render_ansi", shown), ("to_text", text)) if CONTENT_PROBE not in v]
        if not missing:
            continue
        failed = True
        print(f"\nFAIL: `{ty}` carries text in `content` and {' and '.join(missing)} shows NOTHING.")
        print("      A type can export its text perfectly and still render as nothing -- this repo")
        print("      shipped exactly that for figure, caption and list. Check for a structural branch")
        print("      that consumes the element and emits nothing, which SHADOWS the leaf renderer and")
        print("      makes a correct fix unreachable. Or add it to RENDER_EXEMPT with the reason.")
    if not failed:
        print(
            f"  {rendered} types show their text in both; {len(RENDER_EXEMPT)} exempt "
            f"({', '.join(sorted(RENDER_EXEMPT))})"
        )

    # EXCLUSIONS EXPIRE, and a stale one is worse than none: it goes on excusing a
    # case that no longer needs excusing, and hides the next regression behind an
    # explanation nobody rechecks. Recording the REASON is what lets you notice an
    # entry has gone stale; this is the step past that, because the instinct on
    # reading a stale reason is to fix the WORDING rather than delete the entry.
    #
    # duckdb_markdown's, after their line-block exclusion stopped holding within the
    # hour: an entry whose condition no longer applies must be REMOVED, not annotated.
    #
    # So every exemption is re-checked against the property it excuses. An exempt type
    # that now PASSES is reported as expired -- the sweep will not quietly keep
    # forgiving it.
    print("Exemption audit: is every recorded exclusion still load-bearing?")
    stale = []
    for ty, reason in sorted(CONTENT_EXEMPT.items()):
        attrs = CONTENT_ATTRS.get(ty, "MAP{}")
        blk = (
            f"[{{'kind':'block','element_type':'{ty}','content':'{CONTENT_PROBE}','level':1,"
            f"'encoding':'text','attributes':{attrs},'element_order':0}}::{STRUCT}]"
        )
        if CONTENT_PROBE in run(duckdb, f"SELECT duck_blocks_to_pandoc_blocks({blk})::VARCHAR;"):
            stale.append(("CONTENT_EXEMPT", ty, "it PASSES now", reason))
    for ty, reason in sorted(RENDER_EXEMPT.items()):
        attrs = CONTENT_ATTRS.get(ty, "MAP{}")
        blk = (
            f"[{{'kind':'block','element_type':'{ty}','content':'{CONTENT_PROBE}','level':1,"
            f"'encoding':'text','attributes':{attrs},'element_order':0}}::{STRUCT}]"
        )
        shown = run_all(
            duckdb,
            "SELECT regexp_replace(duck_blocks_render_ansi(" + blk + ", 40), '\x1b\\[[0-9;]*m', '', 'g');",
        )
        text = run_all(duckdb, f"SELECT duck_blocks_to_text({blk});")
        if CONTENT_PROBE in shown and CONTENT_PROBE in text:
            stale.append(("RENDER_EXEMPT", ty, "it PASSES now", reason))
    for ty, (becomes, reason) in sorted(INHERENT.items()):
        content, enc, attrs, needs_child = PROBES[ty]
        child = ""
        if needs_child:
            child = (
                f", {{'kind':'block','element_type':'paragraph','content':'inner','level':2,"
                f"'encoding':'text','attributes':MAP{{}},'element_order':1}}::{STRUCT}"
            )
        sql = (
            f"SELECT coalesce((SELECT b.element_type FROM (SELECT unnest(pandoc_ast_to_blocks("
            f"duck_blocks_to_pandoc_blocks([{{'kind':'block','element_type':'{ty}','content':{content},"
            f"'level':1,'encoding':'{enc}','attributes':{attrs},'element_order':0}}::{STRUCT}{child}]"
            f")::VARCHAR)) AS b) WHERE b.kind='block' LIMIT 1), '<NOTHING>');"
        )
        if run(duckdb, sql) == ty:
            stale.append(("INHERENT", ty, "it round-trips to itself now", reason))
    # An entry whose KEY names a type that no longer exists. The probes above cannot
    # catch this -- a vanished type simply fails its probe and looks like an exemption
    # still doing its job, so a rename or a removal leaves the entry excusing nothing,
    # forever. Added after duckdb_markdown caught the same second mode in theirs.
    known = set(types)
    for where, registry in (
        ("INHERENT", INHERENT),
        ("CONTENT_EXEMPT", CONTENT_EXEMPT),
        ("RENDER_EXEMPT", RENDER_EXEMPT),
    ):
        for ty in sorted(registry):
            if ty not in known:
                reason = registry[ty][1] if where == "INHERENT" else registry[ty]
                stale.append((where, ty, "no such element_type any more", reason))

    if stale:
        failed = True
        for where, ty, why, reason in stale:
            print(f"\nFAIL: `{ty}` no longer needs its {where} entry -- {why}.")
            print("      DELETE the entry rather than rewording it. A stale exclusion goes on")
            print("      excusing a case that no longer needs excusing, and the next real")
            print("      regression in that type hides behind an explanation nobody rechecks.")
            # The recorded reason, so the deletion is informed rather than obedient --
            # if it still reads as true, the property it describes moved, and THAT is
            # worth understanding before the entry goes.
            print(f"      recorded reason: {reason}")
    else:
        print(f"  all {len(CONTENT_EXEMPT) + len(RENDER_EXEMPT) + len(INHERENT)} exclusions still hold")

    if failed:
        return 1
    print("OK: no write-only types, nothing dropped in a container, no text lost on export or render.")
    # WHAT THIS DOES NOT COVER, stated so a green run is not read as more than it is.
    # duckdb_markdown found the same limit in their own arm by perturbing their writer
    # to emit a hard break as a soft one and watching it PASS: a consistently wrong
    # conversion is self-consistent on the second pass. These arms find output that is
    # LOST or that MEANS something different when read back. They do not find output
    # that is simply wrong in both directions -- that is the unit suite's job, and
    # neither subsumes the other.
    print("    (round-trip stability cannot see a consistently wrong conversion -- see test/sql/)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
