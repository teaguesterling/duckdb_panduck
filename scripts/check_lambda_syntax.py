#!/usr/bin/env python3
"""No SQL string in src/ may use DuckDB's single-arrow lambda (`x -> ...`).

WHY (#65). DuckDB 1.5.5 accepts `x -> ...` under the DEFAULT lambda_syntax but prints a
deprecation WARNING on stdout, which landed inside consumers' output from every
expand_embedded call. From 2.0 the default REJECTS it: the binder's condition changed from
`setting == DISABLE_SINGLE_ARROW` to `setting != ENABLE_SINGLE_ARROW`. The replacement is
`lambda x: ...`.

WHY A STATIC SCAN AND NOT ONLY A TEST. A sqllogictest can only check a lambda it can bind,
and panduck's expand macro references parse_markdown_to_duck_blocks, so without the markdown
extension it fails with a Catalog Error before its lambdas are ever syntax-checked. CI has no
markdown. The scan reaches every SQL string whether or not it can be bound here.

HOW IT READS THE SOURCE. It tokenizes C++ rather than grepping lines, because a lambda is
routinely split across ADJACENT string literals -- `list_transform(parse(x), ` on one line
and `pd_sub -> {...}` on the next -- and the compiler joins adjacent literals into one
string. Comments and char literals are skipped, so a `->` in C++ code or prose outside a
string is never read. Inside a string, a lambda is recognised by POSITION: its parameter
list follows `(` or `,`. That keeps prose ("container.xml -> the .opf") and the JSON
extraction operators (`j->'$.a'`, `j->>'$.a'`) out.

IT TESTS ITSELF FIRST, on every run. A scan that silently lost its ability to find a lambda
would report zero forever and look like a clean tree -- the failure this repo keeps finding
in its own checks. If the self-test fails, the scan does not run and the exit code is 2.

Usage:  python3 scripts/check_lambda_syntax.py [--self-test]
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src"

IDENT = r"[A-Za-z_][A-Za-z0-9_]*"
PARAMS = r"(?:\(\s*" + IDENT + r"(?:\s*,\s*" + IDENT + r")*\s*\)|" + IDENT + r")"
# `->` not followed by `>` (JSON ->>) and not followed by a quoted path (JSON ->'$.a').
LAMBDA = re.compile(r"[(,]\s*(" + PARAMS + r")\s*->(?!>)(?!\s*')")
RAW_OPEN = re.compile(r'R"([^(\s\\)]{0,16})\(')


def _strip_comments(s):
    s = re.sub(r"/\*.*?\*/", " ", s, flags=re.S)
    return re.sub(r"//[^\n]*", " ", s)


def literal_groups(text):
    """Return runs of ADJACENT string literals as [(line, content), ...] lists.

    Adjacent means separated only by whitespace and comments, which is exactly when the
    compiler concatenates them."""
    groups, group, last_end = [], [], None
    i, n = 0, len(text)

    def add(start, end, content):
        nonlocal group, last_end
        line = text.count("\n", 0, start) + 1
        if group and last_end is not None and not _strip_comments(text[last_end:start]).strip():
            group.append((line, content))
        else:
            if group:
                groups.append(group)
            group = [(line, content)]
        last_end = end

    while i < n:
        if text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
            continue
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        c = text[i]
        if c == "'":
            j = i + 1
            while j < n and text[j] != "'":
                j += 2 if text[j] == "\\" else 1
            i = j + 1
            continue
        m = RAW_OPEN.match(text, i)
        if m and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_")):
            close = ")" + m.group(1) + '"'
            j = text.find(close, m.end())
            j = n if j < 0 else j
            add(i, min(n, j + len(close)), text[m.end():j])
            i = min(n, j + len(close))
            continue
        if c == '"':
            j, buf = i + 1, []
            while j < n and text[j] != '"':
                if text[j] == "\\" and j + 1 < n:
                    buf.append(text[j:j + 2])
                    j += 2
                else:
                    buf.append(text[j])
                    j += 1
            add(i, j + 1, "".join(buf))
            i = j + 1
            continue
        i += 1
    if group:
        groups.append(group)
    return groups


def findings(text):
    """[(line, params, snippet)] for every single-arrow lambda inside a SQL string."""
    out = []
    for group in literal_groups(text):
        joined, starts = "", []
        for line, content in group:
            starts.append((len(joined), line))
            joined += content
        for m in LAMBDA.finditer(joined):
            pos = m.start(1)
            # The literal containing the match, then newlines INSIDE it before the match: a
            # multi-line raw R"SQL(...)" string opens many lines above the lambda.
            off, ln = max(((o, l) for o, l in starts if o <= pos), default=(0, group[0][0]))
            line = ln + joined.count("\n", off, pos)
            snippet = " ".join(joined[max(0, pos - 30):pos + 50].split())
            out.append((line, m.group(1), snippet))
    return out


SELF_TEST = [
    # (description, C++ source, expected number of findings)
    ("two-parameter lambda", 'x = "list_transform(bs, (pd_e, pd_i) -> {kind: pd_e.kind})";', 1),
    ("one-parameter lambda", 'x = "list_transform(blocks, pd_blk -> pd_blk.x)";', 1),
    ("lambda split across adjacent literals",
     'x = "list_transform(parse(x), "\n      "   pd_sub -> {a: 1})";', 1),
    ("no spaces around the arrow", 'x = "list_transform(l,x->x+1)";', 1),
    ("comment between adjacent literals", 'x = "f(l, " /* why */ "y -> y)";', 1),
    ("raw string literal", 'x = R"sql(SELECT list_transform(l, x -> x))sql";', 1),
    ("new lambda syntax is clean", 'x = "list_transform(l, lambda x: x + 1)";', 0),
    ("prose arrow is not a lambda", 'x = "container.xml -> the .opf package document";', 0),
    ("JSON -> extraction is not a lambda", 'x = "SELECT (j->\'$.a\') FROM t";', 0),
    ("JSON ->> extraction is not a lambda", 'x = "SELECT f(j ->> \'$.a\')";', 0),
    ("arrow in C++ code is not SQL", "auto v = node->next; f(a, b -> c);", 0),
    ("arrow in a comment is not SQL", '// "list_transform(l, x -> x)"\nint y;', 0),
    ("char literal quote does not open a string", "if (c == '\"') { s = \"prose a -> b\"; }", 0),
    ("literals separated by code are not joined", 'f("g(l, "); h(); k("x -> x)");', 0),
]

# (description, C++ source, expected line of the single finding) -- the reported line is what
# a developer opens, so it is tested, not assumed.
LINE_TEST = [
    ("lambda on a later line of a multi-line raw string",
     'int a;\nx = R"SQL(\nSELECT 1;\nSELECT list_transform(l, y -> y);\n)SQL";', 4),
    ("lambda in the second of two adjacent literals",
     'x = "list_transform(parse(x), "\n    "pd_sub -> {a: 1})";', 2),
]


def self_test():
    ok = True
    for desc, src, want in SELF_TEST:
        got = len(findings(src))
        if got != want:
            ok = False
            print(f"  SELF-TEST FAIL  {desc}: expected {want}, found {got}")
    for desc, src, want in LINE_TEST:
        got = [ln for ln, _, _ in findings(src)]
        if got != [want]:
            ok = False
            print(f"  SELF-TEST FAIL  {desc}: expected line {want}, got {got}")
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--self-test", action="store_true", help="run only the self-test")
    args = ap.parse_args()

    if not self_test():
        print("\nThe scan cannot be trusted, so it did not run.")
        return 2
    print(f"self-test: {len(SELF_TEST) + len(LINE_TEST)} cases pass")
    if args.self_test:
        return 0

    files = sorted(p for p in SRC.rglob("*") if p.suffix in (".cpp", ".hpp", ".h", ".cc"))
    total = 0
    for path in files:
        for line, params, snippet in findings(path.read_text(encoding="utf-8", errors="replace")):
            total += 1
            print(f"{path.relative_to(ROOT)}:{line}: single-arrow lambda ({params} ->) in SQL: ...{snippet}...")
    print(f"scanned {len(files)} files under src/")
    if total:
        print(f"\n{total} single-arrow lambda(s). DuckDB 2.0 rejects these under the default "
              f"lambda_syntax; rewrite `x -> ...` as `lambda x: ...` and `(a, b) -> ...` as "
              f"`lambda a, b: ...` (#65).")
        return 1
    print("No SQL string uses a single-arrow lambda.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
