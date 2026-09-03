#!/usr/bin/env python3
"""Which words does pandoc see in a fixture that panduck does not?

THE STRONGEST FORMAT-AGNOSTIC AUDIT AVAILABLE. It needs no knowledge of any format's
constructs, and it is exactly the invariant the DOCX hyperlink bug violated: words
present in the document, absent from the output. A missing element_type is a fidelity
gap that a consumer can see and work around; a missing WORD is data loss they cannot.

It found, in one pass across five readers:
  - RST simple tables sliced at drifting offsets ("Value" -> "alue", data cells empty)
  - textile `bc.` blocks truncated to their first line, the rest leaking out as prose

Not a substitute for the per-construct tests: it cannot see a heading read as a
paragraph, because the words are all still there. The two checks fail differently on
purpose.

Usage:  python3 scripts/check_word_loss.py [--duckdb PATH] [--strict]
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FIXTURES = ROOT / "test" / "fixtures"

# reader, pandoc format name, fixture
TARGETS = [
    ("read_docx_blocks", "docx", "constructs.docx"),
    ("read_odt_blocks", "odt", "constructs.odt"),
    ("read_rst_blocks", "rst", "constructs.rst"),
    ("read_textile_blocks", "textile", "constructs.textile"),
]

# Words pandoc reports that panduck legitimately does not put in `content`.
# Each entry is a MEASURED, reasoned exception -- not a tolerance.
EXPECTED = {
    # pandoc's textile reader does not understand `bc(class).` and leaves the marker in
    # the document as prose, so its plain output contains the literal "bc" and "python".
    # python-textile 4.0.2 settles it the other way: <pre class="python"><code ...>.
    # panduck parses the block and records the class as attributes['language'], which is
    # not `content` -- so the words are absent from this comparison by being handled
    # BETTER, not worse. Reference-wrong, same posture as the roundtrip ledger.
    ("textile", "bc"),
    ("textile", "python"),
}


def words(text):
    return set(re.findall(r"[A-Za-z][A-Za-z0-9_]{1,}", text))


def panduck_words(duckdb, reader, path):
    seen = set()
    # Both joins: a space-join splits H<sub>2</sub>O into "H 2 O", an empty join runs
    # neighbouring elements together. A word need only survive one of them.
    for sep in (" ", ""):
        sql = f"SELECT coalesce(string_agg(content, '{sep}'), '') FROM {reader}('{path}');"
        out = subprocess.run([duckdb, "-noheader", "-list", "-c", sql],
                             capture_output=True, text=True)
        if out.returncode != 0:
            raise RuntimeError(out.stdout.strip() + out.stderr.strip())
        seen |= words(out.stdout)
    return seen


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--duckdb", default=str(ROOT / "build" / "release" / "duckdb"))
    ap.add_argument("--strict", action="store_true",
                    help="fail when a prerequisite is missing instead of skipping")
    args = ap.parse_args()

    if not Path(args.duckdb).exists():
        print(f"no duckdb at {args.duckdb}; build first")
        return 1 if args.strict else 0
    if subprocess.run(["which", "pandoc"], capture_output=True).returncode != 0:
        print("pandoc not installed")
        return 1 if args.strict else 0

    failures = 0
    for reader, fmt, name in TARGETS:
        path = FIXTURES / name
        if not path.exists():
            print(f"  {fmt:<9} SKIP    missing fixture {name}")
            failures += args.strict
            continue
        ref = subprocess.run(["pandoc", "-f", fmt, "-t", "plain", str(path)],
                             capture_output=True, text=True)
        if ref.returncode != 0:
            print(f"  {fmt:<9} SKIP    pandoc could not read it")
            failures += args.strict
            continue
        missing = words(ref.stdout) - panduck_words(args.duckdb, reader, str(path))
        unexpected = sorted(w for w in missing if (fmt, w) not in EXPECTED)
        declared = sorted(w for w in missing if (fmt, w) in EXPECTED)
        note = f"  [{len(declared)} declared]" if declared else ""
        if unexpected:
            print(f"  {fmt:<9} LOST    {' '.join(unexpected)}{note}")
            failures += 1
        else:
            print(f"  {fmt:<9} ok      no words lost{note}")

    if failures:
        print(f"\n{failures} reader(s) drop words present in the document.")
        return 1
    print("\nNo reader drops a word pandoc can see.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
