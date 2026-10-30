#!/usr/bin/env python3
"""Does the Pandoc converter keep every word on a pandoc -> duck_blocks -> pandoc round trip?

THE WRITE-DIRECTION TWIN OF scripts/check_word_loss.py. That check compares READERS
against pandoc and never looks at the converter's export half; check-writeback asserts only
that a real pandoc ACCEPTS what comes out, which an empty `Caption []` or `Para []` does.
Neither can see a word that goes in and does not come out. This can, and it needs no
knowledge of any construct to see it.

Every fixture under test/ that pandoc can read is converted to a pandoc AST BY PANDOC, plus
every test/**/*.pandoc.json. Each AST goes through

    panduck_blocks_to_pandoc_blocks(panduck_pandoc_ast_to_blocks(ast))

and the words of the blocks out must include, with multiplicity, the words of the blocks in.
A word is a whitespace-separated token of the text pandoc carries in Str, Code, Math,
RawInline, CodeBlock and RawBlock. Metadata is not counted: it travels in `meta`, not
`blocks`. Formatting may change; words may not go missing.

It found, on its first run (2026-10-30, as a scratch script in the step-4 differential),
four causes behind 14 inputs: figure captions, inline image alt, list items with formatted
text, and the last paragraph of a container in a document with metadata. See
test/sql/pandoc_roundtrip_fidelity.test, which reproduces each in a few lines.

EXEMPT is for a loss that was investigated and is NOT a converter defect. Each entry is
keyed on (case, word) and carries the measured reason. It is not a tolerance: an entry that
stops being needed is reported, so the list cannot rot into a blanket excuse.

FAILS LOUDLY. Exit 1, never a skip, when pandoc is missing, the build is missing, no case
was run, or duckdb errors on any case. A guard that prints "no words lost" after measuring
nothing is the failure this repo's other checks were written to prevent.

IF IT CRASHED, it does not print the success line. A duckdb error on a case prints
`ERROR <case>: <first stderr line>` and counts as a failure; an exception in this script
prints a Python traceback and exits non-zero. The last line of a passing run is always
`N ASTs round-tripped; no converter loses a word.` -- anything else is not a pass.

Usage: python3 test/converter/check_converter_wordloss.py [--duckdb PATH]
"""

import argparse
import collections
import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent

# Source formats pandoc reads, by extension. Same map as check_converter_differential.py,
# which measured these first.
FMT = {
    "md": "markdown",
    "rst": "rst",
    "org": "org",
    "textile": "textile",
    "wiki": "mediawiki",
    "tex": "latex",
    "docx": "docx",
    "odt": "odt",
    "epub": "epub",
    "html": "html",
    "rtf": "rtf",
    "ipynb": "ipynb",
}

# (case name, word) -> measured reason. Empty: every loss found so far was fixed.
EXEMPT = {}

TEXT_CONSTRUCTORS = ("Str", "Code", "Math", "RawInline", "CodeBlock", "RawBlock")


def words(node, acc):
    """Count the words pandoc carries as text, anywhere under `node`."""
    if isinstance(node, dict):
        if node.get("t") in TEXT_CONSTRUCTORS:
            c = node.get("c")
            if isinstance(c, str):
                text = c
            elif isinstance(c, list) and c and isinstance(c[-1], str):
                text = c[-1]
            else:
                text = ""
            for w in text.split():
                acc[w] += 1
        for v in node.values():
            words(v, acc)
    elif isinstance(node, list):
        for v in node:
            words(v, acc)
    return acc


def corpus(tmp):
    """(name, path to a pandoc JSON AST) for every case, and the fixtures pandoc refused."""
    cases, refused = [], []
    for p in sorted(ROOT.joinpath("test").rglob("*")):
        ext = p.suffix.lstrip(".")
        if ext not in FMT or not p.is_file():
            continue
        rel = str(p.relative_to(ROOT))
        r = subprocess.run(["pandoc", "-f", FMT[ext], "-t", "json", str(p)], capture_output=True)
        if r.returncode != 0:
            refused.append((rel, r.stderr.decode(errors="replace").strip().splitlines()[:1]))
            continue
        out = Path(tmp) / (rel.replace("/", "__") + ".json")
        out.write_bytes(r.stdout)
        cases.append((rel, out))
    for p in sorted(ROOT.joinpath("test").rglob("*.pandoc.json")):
        cases.append((str(p.relative_to(ROOT)), p))
    return cases, refused


def round_trip(duckdb, path):
    sql = (
        "SELECT panduck_blocks_to_pandoc_blocks(panduck_pandoc_ast_to_blocks("
        f"(SELECT content FROM read_text('{path}'))));"
    )
    r = subprocess.run([duckdb, "-noheader", "-list", "-c", sql], capture_output=True, text=True)
    if r.returncode != 0:
        lines = (r.stderr.strip() or r.stdout.strip() or f"exit {r.returncode}").splitlines()
        raise RuntimeError(lines[0])
    return json.loads(r.stdout)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--duckdb", default=str(ROOT / "build" / "release" / "duckdb"))
    args = ap.parse_args()

    if not Path(args.duckdb).exists():
        print(f"FAIL: no duckdb at {args.duckdb}; build first (make release).")
        return 1
    try:
        ver = subprocess.run(["pandoc", "--version"], capture_output=True, text=True)
    except FileNotFoundError:
        ver = None
    if ver is None or ver.returncode != 0:
        print("FAIL: pandoc is not installed. This guard needs it to build its corpus.")
        return 1
    print(f"{ver.stdout.splitlines()[0]}; converter from {args.duckdb}")

    failures = 0
    exempt_used = set()
    with tempfile.TemporaryDirectory() as tmp:
        cases, refused = corpus(tmp)
        for rel, why in refused:
            print(f"  not a case  {rel}: pandoc could not read it ({' '.join(why)})")
        if not cases:
            print("FAIL: no cases ran. A guard that measured nothing has not passed.")
            return 1
        for rel, path in cases:
            blocks_in = json.loads(Path(path).read_text())
            if isinstance(blocks_in, dict):
                blocks_in = blocks_in.get("blocks", [])
            try:
                blocks_out = round_trip(args.duckdb, path)
            except (RuntimeError, json.JSONDecodeError) as e:
                print(f"  ERROR {rel}: {e}")
                failures += 1
                continue
            lost = words(blocks_in, collections.Counter()) - words(blocks_out, collections.Counter())
            unexplained = {w: n for w, n in lost.items() if (rel, w) not in EXEMPT}
            exempt_used |= {(rel, w) for w in lost if (rel, w) in EXEMPT}
            if unexplained:
                failures += 1
                shown = " ".join(f"{w}" + (f"x{n}" if n > 1 else "") for w, n in sorted(unexplained.items()))
                print(f"  LOST  {rel}: {shown}")

    stale = sorted(set(EXEMPT) - exempt_used)
    for case, word in stale:
        print(f"  STALE EXEMPT ({case!r}, {word!r}): that word is no longer lost. Delete the entry.")
    failures += len(stale)

    if failures:
        print(f"\nFAIL: {failures} problem(s) across {len(cases)} ASTs. A word went in and did not come out.")
        return 1
    print(f"\n{len(cases)} ASTs round-tripped; no converter loses a word.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
