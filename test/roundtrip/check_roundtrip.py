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

LADDER = ["text", "skeleton", "marked"]


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

RTF_LISTS = (
    NOT_IMPLEMENTED,
    "two causes. (1) panduck does not read lists yet, so it emits the RTF \\bullet as "
    "paragraph text where pandoc emits list_item -- panduck's gap, tracked in the "
    "registry notes. (2) panduck resolves {\\stylesheet} \\sN and reports 'Heading One' "
    "as heading/1, while pandoc reads it as Para[Strong[Span]] and detects no heading "
    "at all -- panduck is the more faithful reader here, not the divergent one.",
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

ODT_SCOPE = (
    NOT_IMPLEMENTED,
    "panduck flattens list items to paragraphs and does not read blockquotes, so pandoc's "
    "list_item and blockquote markers have no counterpart and later positions shift. TEXT "
    "AGREES EXACTLY on both fixtures -- nothing is lost, the structure around it is not "
    "modelled. That distinction is why this is not-implemented rather than a defect: an "
    "earlier version skipped text:list wholesale and lost the words themselves, which the "
    "text level caught.",
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
        expect={"skeleton": DOCX_SCOPE, "marked": DOCX_SCOPE},  # text must AGREE
        note="pandoc-generated DOCX: headings via w:pStyle",
    ),
    Case(
        "test/fixtures/libreoffice_outlinelvl.docx",
        "docx",
        "read_docx_blocks",
        expect={"skeleton": DOCX_LO, "marked": DOCX_LO},  # text must AGREE
        note="LibreOffice-generated DOCX: headings via w:outlineLvl",
    ),
    Case(
        "test/fixtures/pandoc.odt",
        "odt",
        "read_odt_blocks",
        expect={"skeleton": ODT_SCOPE, "marked": ODT_SCOPE},  # text must AGREE
        note="pandoc-generated ODT",
    ),
    Case(
        "test/fixtures/libreoffice.odt",
        "odt",
        "read_odt_blocks",
        expect={"skeleton": ODT_SCOPE, "marked": ODT_SCOPE},  # text must AGREE
        note="LibreOffice-generated ODT",
    ),
    Case(
        "test/fixtures/libreoffice_stylesheet.rtf",
        "rtf",
        "read_rtf_blocks",
        expect={"text": RTF_LISTS, "skeleton": RTF_LISTS, "marked": RTF_LISTS},
        note="LibreOffice-generated RTF: headings via {\\stylesheet} \\sN",
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
    return canonical.pandoc_blocks_to_canonical(json.loads(raw.stdout)["blocks"])


def read_panduck(path, reader, duckdb_bin, extension=None):
    # attributes is a MAP, and DuckDB renders MAPs as {k=v} -- which is not valid JSON.
    # Project the attributes this comparison needs into plain columns instead.
    sql = (
        "SELECT kind, element_type, content, "
        "attributes['heading_level'][1] AS heading_level, "
        "attributes['href'][1] AS href, "
        "attributes['src'][1] AS src "
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
        r["attributes"] = attrs
    return canonical.duckblocks_to_canonical(rows)


def compare(level, a, b):
    fn = canonical.LEVELS[level]
    return fn(a) == fn(b), fn(a), fn(b)


def render_diff(level, got, ref, limit=6):
    lines = []
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
    ap.add_argument("--duckdb", default=os.environ.get("PANDUCK_DUCKDB"), help="duckdb binary to use")
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
