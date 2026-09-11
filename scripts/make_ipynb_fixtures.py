#!/usr/bin/env python3
"""Minimal .ipynb fixtures for the markdown-cell reader (panduck#39).

No dependencies: a notebook is JSON. Each case isolates one property a fix could
pass while still leaving cell-internal structure invisible.

  nested_headings   THREE headings at THREE levels in ONE cell, prose after each.
                    A fix that extracts a leading H1 and stops passes a
                    single-heading fixture and still leaves -S broken. This is the
                    case that discriminates; the others are controls.
  empty_markdown    a markdown cell with no content, and one with only whitespace.
  split_headings    headings in SEPARATE cells, to show ordering across the seam
                    once cell-internal parsing lands.
  code_adjacent     markdown beside code, so the already-correct path stays covered.

Contributed by the duckeye session, which reported #39 and pointed out -- before
any fix existed -- that panduck's own notebook.ipynb has ONE heading at the START
of its only markdown cell, so a leading-H1-only fix scores 1 of 1 against it and
looks finished. A fixture where the wrong implementation scores full marks is not
testing the thing it names.

GENERATED RATHER THAN COMMITTED BY HAND, because empty_markdown's whitespace-only
cell is the case nobody maintains correctly in a checked-in JSON blob -- and it is
the one that tends to produce a stray block or a crash.

Regenerate with:  python3 scripts/make_ipynb_fixtures.py test/fixtures
"""
import json, pathlib, sys

def nb(*cells):
    return {"cells": list(cells), "metadata": {},
            "nbformat": 4, "nbformat_minor": 5}

def md(*lines):
    return {"cell_type": "markdown", "metadata": {},
            "source": [l + "\n" for l in lines]}

def code(*lines):
    return {"cell_type": "code", "execution_count": None, "metadata": {},
            "outputs": [], "source": [l + "\n" for l in lines]}

CASES = {
    "nested_headings": nb(md("# Top Title", "", "Prose under top.", "",
                             "## Sub Section", "", "Prose under sub.", "",
                             "### Deeper Still", "", "Prose under deeper.")),
    "empty_markdown":  nb(md(""), md("   "), md("# After The Empties")),
    "split_headings":  nb(md("# First Cell Heading"), code("x = 1"),
                          md("## Second Cell Heading", "", "prose")),
    "code_adjacent":   nb(md("# Doc Title", "", "prose"), code("print(1)"),
                          md("## Later"), code("print(2)")),
}

out = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".")
out.mkdir(parents=True, exist_ok=True)
for name, doc in CASES.items():
    p = out / f"{name}.ipynb"
    p.write_text(json.dumps(doc, indent=1) + "\n")
    print(f"{p}  ({p.stat().st_size} bytes)")
