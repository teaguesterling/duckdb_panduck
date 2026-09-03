#!/usr/bin/env python3
"""Every fixture's blocks must write back out as pandoc JSON a REAL pandoc accepts.

panduck's readers are deliberately more faithful than pandoc in places -- richer
attributes, better block types, and metadata pandoc does not extract at all. The standing
rule that keeps that from becoming incompatibility is:

    Diverge from pandoc's representation wherever the source justifies it. Do not discard
    information. But whatever is emitted MUST map back to VALID pandoc JSON.

Valid, not identical. duck_blocks is allowed to be the richer representation and the pandoc
AST is a lossy export target; losing an attribute pandoc has no field for is fine. Emitting
a shape pandoc cannot parse is not.

WHY A REAL PANDOC AND NOT A SCHEMA. The first defect this check would have caught was a
Table whose `c` field held duck_block's own {"headers":...,"rows":...} projection instead of
pandoc's six-element array. It is well-formed JSON, it round-trips through DuckDB perfectly,
every in-process assertion about it passes, and pandoc rejects the entire document:

    When parsing the constructor Table of type Text.Pandoc.Definition.Block
    expected Array but got Object

Nothing short of the real parser sees that. It also went unnoticed for the whole life of the
converter, because nothing wrote blocks back out until the write direction was registered.

THE SELF-TEST IS NOT OPTIONAL. A write-back check that cannot detect a bad AST reports
success by checking nothing, which is how the conformance script once returned SKIP with
exit 0 while every fixture errored. This one proves it can fail before it is trusted to
pass.
"""

import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
FIXTURES = REPO / "test" / "fixtures"
DUCKDB = REPO / "build" / "release" / "duckdb"
EXT = REPO / "build" / "release" / "extension" / "panduck" / "panduck.duckdb_extension"

# Extensions read_panduck_doc routes to a block reader. Data formats and the .zim corpus
# refusal are deliberately absent -- neither has blocks to write back.
DOC_SUFFIXES = (
    ".org",
    ".rst",
    ".tex",
    ".docx",
    ".epub",
    ".ipynb",
    ".odt",
    ".rtf",
    ".wiki",
    ".textile",
)

WRITE_SQL = """
SELECT panduck_write_pandoc_ast('{out}', list(struct_pack(
    kind := kind, element_type := element_type, content := content, level := level,
    encoding := encoding, attributes := attributes, element_order := element_order)
  ORDER BY element_order))
FROM {source};
"""

# CONSTRUCTS THE FIXTURES DO NOT REACH.
#
# The fixture sweep above is a coverage claim that has to be checked rather than assumed:
# org, rst, latex and docx all have table paths, and NOT ONE FIXTURE in the tree exercises
# them. Exactly one fixture -- spine_order.epub -- contains a table at all, which is how
# the native-table export defect survived: the sweep was green on 16 documents that never
# built a table.
#
# So these go through the reader's *_blocks_string entry point instead. Each names the
# element_type it is supposed to produce, and the check VERIFIES THAT IT DID before
# treating "pandoc accepted it" as meaningful. Without that, a reader that silently
# dropped the construct would report `ok` here forever -- a check on the result that
# cannot see an error in the shape.
CONSTRUCTS = [
    (
        "org table",
        r"read_org_blocks_string(E'| a | b |\n|---+---|\n| 1 | 2 |')",
        "table",
    ),
    (
        "rst grid table",
        r"read_rst_blocks_string(E'+---+---+\n| a | b |\n+===+===+\n| 1 | 2 |\n+---+---+')",
        "table",
    ),
    (
        "rst simple table",
        r"read_rst_blocks_string(E'===  ===\n a    b\n===  ===\n 1    2\n===  ===')",
        "table",
    ),
    (
        "latex tabular",
        r"read_latex_blocks_string(E'\\begin{tabular}{ll}\na & b \\\\\n1 & 2 \\\\\n\\end{tabular}')",
        "table",
    ),
    ("org definition list", r"read_org_blocks_string('- term :: definition')", "list"),
    ("rst field list", r"read_rst_blocks_string(E':Author: A. Writer')", "list"),
    ("org metadata", r"read_org_blocks_string(E'#+TITLE: T\n\nBody.')", "inlines"),
    (
        "mediawiki table",
        r"read_mediawiki_blocks_string(E'{|\n! H !! I\n|-\n| a || b\n|}')",
        "table",
    ),
    ("mediawiki template", r"read_mediawiki_blocks_string(E'{{Infobox|a=1}}')", "raw"),
    (
        "mediawiki definition list",
        r"read_mediawiki_blocks_string(E'; term\n: definition')",
        "list",
    ),
    (
        "mediawiki figure",
        r"read_mediawiki_blocks_string(E'[[File:p.png|thumb|cap]]')",
        "figure",
    ),
    (
        "textile table",
        r"read_textile_blocks_string(E'|_. H |_. I |\n| a | b |')",
        "table",
    ),
    (
        "textile definition list",
        r"read_textile_blocks_string(E'- term := definition')",
        "list",
    ),
    (
        "textile notextile raw",
        r"read_textile_blocks_string(E'notextile. <b>x</b>')",
        "raw",
    ),
    # longtable is what pandoc's LaTeX WRITER emits; it was unmapped while tabular was, so
    # every table in a pandoc-generated .tex was dropped. Neither latex fixture had a table.
    (
        "latex longtable",
        r"read_latex_blocks_string(E'\\begin{longtable}{ll}\na & b \\\\\n\\end{longtable}')",
        "table",
    ),
    # `figure` was DROPPED WHOLE, discarding the \includegraphics inside it.
    (
        "latex figure",
        r"read_latex_blocks_string('\begin{figure}\includegraphics{p.png}\caption{c}\end{figure}')",
        "figure",
    ),
]

COUNT_SQL = "SELECT count(*) FROM {source} WHERE element_type = '{element_type}';"


def duckdb_write(source: str, out: Path) -> str | None:
    """Write a source expression's blocks to out as a pandoc AST. Error string, or None."""
    proc = subprocess.run(
        [
            str(DUCKDB),
            "-unsigned",
            "-c",
            f"LOAD '{EXT}';",
            "-c",
            WRITE_SQL.format(out=out, source=source),
        ],
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        return f"panduck failed to read/write: {proc.stderr.strip()[:200]}"
    if not out.exists():
        return "panduck_write_pandoc_ast produced no file"
    return None


def element_count(source: str, element_type: str) -> int:
    """How many blocks of element_type the source produced. -1 if the query failed."""
    proc = subprocess.run(
        [
            str(DUCKDB),
            "-unsigned",
            "-noheader",
            "-list",
            "-c",
            f"LOAD '{EXT}';",
            "-c",
            COUNT_SQL.format(source=source, element_type=element_type),
        ],
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        return -1
    try:
        return int(proc.stdout.strip().splitlines()[-1])
    except (ValueError, IndexError):
        return -1


def pandoc_accepts(path: Path) -> str | None:
    """Feed the AST to a real pandoc. Returns pandoc's complaint, or None if accepted."""
    proc = subprocess.run(
        ["pandoc", "-f", "json", "-t", "plain", str(path)],
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        return proc.stderr.strip().splitlines()[0][:200] if proc.stderr.strip() else "pandoc rejected it"
    return None


def self_test(tmp: Path) -> list[str]:
    """Prove this checker can FAIL, using ASTs whose defects it must catch.

    Each case is a real shape a broken writer has produced or plausibly would, not a
    syntactically mangled file -- a check that only detects corrupt JSON would pass all of
    these while missing every defect that matters.
    """
    api = '"pandoc-api-version":[1,23,1]'
    cases = {
        # THE ACTUAL DEFECT this check was written for.
        "native table object as Table's c": (
            '{%s,"meta":{},"blocks":[{"t":"Table","c":{"headers":["A"],"rows":[["x"]]}}]}' % api
        ),
        # The version skew that made duck_block_utils v1.4.3's exports unreadable.
        "stale api version": (
            '{"pandoc-api-version":[1,20],"meta":{},"blocks":[{"t":"Para","c":[{"t":"Str","c":"x"}]}]}'
        ),
        # A constructor that does not exist -- what emitting an invented element_type does.
        "unknown constructor": ('{%s,"meta":{},"blocks":[{"t":"NotAThing","c":[]}]}' % api),
        # Right constructor, wrong arity: pandoc validates tuple shape, not just names.
        "table with too few fields": ('{%s,"meta":{},"blocks":[{"t":"Table","c":[["",[],[]],[null,[]]]}]}' % api),
    }
    failures = []
    for name, body in cases.items():
        p = tmp / "selftest.json"
        p.write_text(body)
        if pandoc_accepts(p) is None:
            failures.append(f"SELF-TEST: pandoc accepted a document it must reject -- {name}")

    # ...and a CONFORMING control, so the checker is not merely rejecting everything.
    p = tmp / "selftest_ok.json"
    p.write_text('{%s,"meta":{},"blocks":[{"t":"Para","c":[{"t":"Str","c":"ok"}]}]}' % api)
    if pandoc_accepts(p) is not None:
        failures.append("SELF-TEST: pandoc rejected a conforming document -- the check is broken")
    return failures


def main() -> int:
    require = "--require" in sys.argv

    missing = []
    if shutil.which("pandoc") is None:
        missing.append("pandoc is not on PATH")
    if not DUCKDB.exists():
        missing.append(f"no duckdb binary at {DUCKDB}")
    if not EXT.exists():
        missing.append(f"no panduck extension at {EXT}")
    if missing:
        # A check that silently skips reports coverage it is not providing. --require turns
        # a missing prerequisite into the failure it is in CI.
        for m in missing:
            print(f"  {'FAIL' if require else 'SKIP'}: {m}")
        return 1 if require else 0

    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)

        failures = self_test(tmp)
        if failures:
            for f in failures:
                print(f"  {f}")
            print("\nFAILED: the checker cannot detect a bad AST, so its verdict means nothing.")
            return 1

        fixtures = sorted(p for p in FIXTURES.iterdir() if p.suffix.lower() in DOC_SUFFIXES)
        if not fixtures:
            # Decided BEFORE the nothing-to-do path: zero fixtures is a broken checkout, not
            # a pass. The conformance script shipped this bug once already.
            print(f"  FAIL: no document fixtures found under {FIXTURES}")
            return 1

        bad = 0
        for src in fixtures:
            out = tmp / "ast.json"
            if out.exists():
                out.unlink()
            err = duckdb_write(f"read_panduck_doc('{src}')", out) or pandoc_accepts(out)
            if err:
                bad += 1
                print(f"  REJECTED  {src.name}: {err}")
            else:
                print(f"  ok        {src.name}")

        print()
        for name, source, element_type in CONSTRUCTS:
            out = tmp / "ast.json"
            if out.exists():
                out.unlink()
            n = element_count(source, element_type)
            if n <= 0:
                # The construct did not survive the READ, so writing it back proves nothing.
                # This arm exists precisely to stop a silent drop from reading as success.
                bad += 1
                print(
                    f"  NOT PRODUCED  {name}: reader emitted no '{element_type}' block "
                    f"({'query failed' if n < 0 else 'count 0'})"
                )
                continue
            err = duckdb_write(source, out) or pandoc_accepts(out)
            if err:
                bad += 1
                print(f"  REJECTED      {name}: {err}")
            else:
                print(f"  ok            {name} ({n} {element_type})")

        print()
        total = len(fixtures) + len(CONSTRUCTS)
        if bad:
            print(f"FAILED: {bad} of {total} cases do not write back as valid pandoc JSON.")
            return 1
        print(
            f"All {len(fixtures)} fixtures and {len(CONSTRUCTS)} constructs write back as "
            f"pandoc JSON a real pandoc accepts."
        )
        return 0


if __name__ == "__main__":
    sys.exit(main())
