#!/usr/bin/env python3
"""Verify panduck's Pandoc AST mapping against a real pandoc binary.

Panduck does not link pandoc (see src/include/pandoc_ast_map.hpp for why), so nothing
in the build would notice if pandoc changed its AST underneath us. This harness closes
that gap: it runs the installed pandoc over a kitchen-sink fixture, collects every
Block/Inline constructor pandoc actually emits, and asserts each one is accounted for in
src/pandoc_ast_map.cpp.

It reads the C++ table as source-of-truth text rather than querying a built extension,
so it runs without a compiled binary. The SQL tests (test/sql/pandoc_ast_map.test) close
the other half by asserting the built extension agrees with that same table.

Exit codes: 0 = pass (or skipped, pandoc absent), 1 = drift detected.
"""

import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
MAP_CPP = REPO / "src" / "pandoc_ast_map.cpp"
MAP_HPP = REPO / "src" / "include" / "pandoc_ast_map.hpp"
FIXTURE = REPO / "test" / "pandoc" / "fixtures" / "kitchen_sink.md"

# Constructors of pandoc-types that are NOT Block or Inline: the enum-like payloads of
# Alignment, ColWidth, ListNumberStyle, ListNumberDelim, QuoteType, MathType and
# CitationMode. They appear as {"t": ...} in the JSON but are not document elements, so
# they are outside this mapping's scope.
AUXILIARY = {
    "AlignDefault", "AlignLeft", "AlignRight", "AlignCenter",
    "ColWidth", "ColWidthDefault",
    "DefaultStyle", "Example", "Decimal", "LowerRoman", "UpperRoman", "LowerAlpha", "UpperAlpha",
    "DefaultDelim", "Period", "OneParen", "TwoParens",
    "SingleQuote", "DoubleQuote",
    "DisplayMath", "InlineMath",
    "AuthorInText", "SuppressAuthor", "NormalCitation",
}

# Constructors pandoc emits that the duck_block round-trip does not yet implement.
# Every entry here must be status=planned in the C++ table. This list is a ratchet: a
# newly-unhandled constructor fails the run until it is either implemented or
# deliberately recorded here.
KNOWN_GAPS = {"LineBlock", "DefinitionList", "Figure", "Underline"}

# Emitted by no pandoc reader in practice, so the fixture cannot exercise it.
NOT_EXERCISABLE = {"Null"}


def fail(msg):
    print(f"FAIL: {msg}", file=sys.stderr)
    return 1


def parse_cpp_table():
    """Extract (pandoc_type -> (kind, element_type, status)) from the C++ table."""
    text = MAP_CPP.read_text()
    entry = re.compile(
        r'\{"([A-Za-z]+)",\s*"(block|inline)",\s*(?:"([^"]*)"|(nullptr)),\s*STATUS_([A-Z]+)'
    )
    table = {}
    for m in entry.finditer(text):
        name, kind, element_type, is_null, status = m.groups()
        table[name] = (kind, None if is_null else element_type, status.lower())
    return table


def parse_target_api_version():
    text = MAP_HPP.read_text()
    major = re.search(r"API_VERSION_MAJOR\s*=\s*(\d+)", text)
    minor = re.search(r"API_VERSION_MINOR\s*=\s*(\d+)", text)
    if not (major and minor):
        raise SystemExit(fail("could not read API_VERSION_* from pandoc_ast_map.hpp"))
    return int(major.group(1)), int(minor.group(1))


def collect_constructors(ast):
    """Every {"t": ...} value reachable in the document body."""
    seen = set()

    def walk(node):
        if isinstance(node, dict):
            t = node.get("t")
            if isinstance(t, str):
                seen.add(t)
            for value in node.values():
                walk(value)
        elif isinstance(node, list):
            for value in node:
                walk(value)

    walk(ast["blocks"])
    return seen


def main():
    pandoc = shutil.which("pandoc")
    if not pandoc:
        print("SKIP: pandoc not on PATH; cannot verify AST alignment.")
        print("      Install pandoc to run this check (CI installs it explicitly).")
        return 0

    version = subprocess.run([pandoc, "--version"], capture_output=True, text=True).stdout.splitlines()[0]
    print(f"Checking panduck's AST mapping against {version}")

    raw = subprocess.run(
        [pandoc, "-f", "markdown", "-t", "json", str(FIXTURE)],
        capture_output=True, text=True,
    )
    if raw.returncode != 0:
        return fail(f"pandoc failed to convert the fixture:\n{raw.stderr}")

    ast = json.loads(raw.stdout)
    table = parse_cpp_table()
    errors = 0

    # 1. AST API version must match what the mapping targets.
    target = parse_target_api_version()
    actual = tuple(ast["pandoc-api-version"][:2])
    if actual != target:
        errors += fail(
            f"pandoc-api-version is {'.'.join(map(str, ast['pandoc-api-version']))} but the "
            f"mapping targets {target[0]}.{target[1]}. pandoc-types changed the AST; review "
            f"src/pandoc_ast_map.cpp before bumping API_VERSION_* in pandoc_ast_map.hpp."
        )
    else:
        print(f"  api-version {actual[0]}.{actual[1]} matches target")

    # 2. Every Block/Inline constructor pandoc emits must appear in the table.
    emitted = {c for c in collect_constructors(ast) if c not in AUXILIARY}
    unmapped = sorted(emitted - set(table))
    if unmapped:
        errors += fail(
            f"pandoc emits {len(unmapped)} constructor(s) absent from src/pandoc_ast_map.cpp: "
            f"{', '.join(unmapped)}"
        )
    else:
        print(f"  all {len(emitted)} emitted constructors are present in the mapping")

    # 3. The set of unimplemented constructors must not grow silently.
    planned = {name for name, (_, _, status) in table.items() if status == "planned"}
    if planned != KNOWN_GAPS:
        added = sorted(planned - KNOWN_GAPS)
        removed = sorted(KNOWN_GAPS - planned)
        detail = []
        if added:
            detail.append(f"newly unimplemented: {', '.join(added)}")
        if removed:
            detail.append(f"now implemented (drop from KNOWN_GAPS): {', '.join(removed)}")
        errors += fail("the set of unimplemented constructors changed -- " + "; ".join(detail))
    else:
        print(f"  {len(planned)} known gap(s) unchanged: {', '.join(sorted(planned))}")

    # 4. Coverage report -- informational, not a failure.
    unexercised = sorted(set(table) - emitted - NOT_EXERCISABLE)
    covered = len(emitted & set(table))
    print(f"  fixture exercises {covered}/{len(table)} mapped constructors")
    if unexercised:
        print(f"  NOTE: not exercised by the fixture: {', '.join(unexercised)}")

    if errors:
        return 1
    print("OK: panduck's Pandoc AST mapping is aligned with the installed pandoc.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
