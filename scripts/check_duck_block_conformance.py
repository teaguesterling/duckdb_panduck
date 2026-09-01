#!/usr/bin/env python3
"""Check every panduck reader's output against the vendored duck_block conformance macros.

WHY THIS EXISTS. Until this script, panduck's 940 assertions checked panduck's own
behaviour and NOTHING about duck_block conformance. Every conformance claim made about
this repo came from a human loading duck_block_utils by hand and running it once.
Nothing ran in CI, so nothing would notice a regression.

WHY VENDORED MACROS RATHER THAN THE REAL VALIDATOR. panduck's duckdb submodule tracks
the BRANCH v1.5-variegata, not a tag. It currently sits on the v1.5.5 release commit, so
duck_block_utils loads today -- measured, not assumed. One `git submodule update
--remote` moves it off-release, DuckDB's exact-version ABI match fails, and the
validator becomes unloadable by INSTALL, by LOAD '<path>', by any route. The vendored
SQL keeps working across that change. It also carries two checks the real validator's
per-element macro cannot express at all: duplicate element_order, and level jumping by
more than one.

THE SELF-TEST IS NOT OPTIONAL. A conformance check that cannot detect a defect reports
the same clean output as one that can, and this project has now shipped that exact
shape twice in one day -- a guard pointed at an input that could not exercise it, and a
lint whose element_type arm accepted any string for three major versions. So the script
first feeds itself known-bad documents and FAILS if they are not caught, and one
known-good document and FAILS if it is. A green run means the check fired correctly on
five constructed defects before it ever looked at a fixture.

Usage:
    python3 scripts/check_duck_block_conformance.py
    python3 scripts/check_duck_block_conformance.py --strict   # a skip is a failure
"""

import argparse
import glob
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DUCKDB = os.path.join(ROOT, "build", "release", "duckdb")
EXTENSION = os.path.join(ROOT, "build", "release", "extension", "panduck",
                         "panduck.duckdb_extension")
# NOT VENDORED. The conformance macros are duck_block_utils' artifact and are read from
# where that repo prepares them, so there is no second copy in this tree to drift. A
# copy is only safe when something compares it, and upstream already runs that
# comparison (check_conformance_macro.py) against its own extension -- a copy here would
# be a third party to a two-party agreement, checked by nobody.
DEFAULT_UPSTREAM = os.path.join(os.path.dirname(ROOT), "duckdb_duck_block_utils")
MACROS_RELPATH = os.path.join("vendor", "duck_block_conformance.sql")
# .toml/.yaml are here deliberately: those two registry branches synthesise a block by
# hand instead of passing one up from a reader, which is exactly the kind of element a
# reader test never covers. They shipped a NULL level because of it.
FIXTURE_GLOBS = ("*.rtf", "*.docx", "*.odt", "*.epub", "*.tex", "*.toml", "*.yaml", "*.org")

SEP = "|"

# A duck_block struct literal, so the self-test can build documents no reader would
# produce. Field order matches the vocabulary; `kind` defaults to 'block'.
def blk(element_type, level, element_order, content="NULL", kind="block"):
    c = content if content == "NULL" else "'%s'" % content
    return ("{'kind':'%s','element_type':'%s','content':%s,'level':%d::INTEGER,"
            "'encoding':NULL,'attributes':MAP{},'element_order':%d::INTEGER}"
            % (kind, element_type, c, level, element_order))


def doc(*blocks):
    return "[" + ", ".join(blocks) + "]"


# (name, document, expect_valid, expect_undeclared)
#
# The conforming case is load-bearing: without it a macro that always returned false
# would pass every other row here, and the script would report a clean run while
# detecting nothing.
SELF_TESTS = [
    ("conforming document (negative control)",
     doc(blk("paragraph", 1, 0, "hello")), True, []),
    ("duplicate element_order",
     doc(blk("paragraph", 1, 0, "a"), blk("paragraph", 1, 0, "b")), False, []),
    ("level jumps by more than one",
     doc(blk("paragraph", 1, 0), blk("paragraph", 3, 1, "orphan")), False, []),
    ("level below 1",
     doc(blk("metadata", 0, 0, "title")), False, []),
    ("element_type outside the vocabulary",
     doc(blk("zzzz_not_a_type", 1, 0, "x")), True, ["zzzz_not_a_type"]),
]


def run_sql(sql):
    proc = subprocess.run([DUCKDB, "-unsigned", "-list", "-noheader"],
                          input=sql, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError("duckdb failed:\n%s" % (proc.stderr or proc.stdout))
    lines = [ln for ln in proc.stdout.splitlines()
             if ln.strip() and not ln.startswith("WARNING") and SEP in ln]
    return lines


def resolve_macros(upstream):
    """Locate the conformance SQL in the upstream repo. Returns a path or None."""
    candidate = os.path.join(upstream, MACROS_RELPATH)
    return candidate if os.path.exists(candidate) else None


def preamble(macros):
    return ("LOAD '%s';\n.read %s\n" % (EXTENSION, macros))


def self_test(macros):
    """Prove the macros can distinguish conforming from non-conforming input."""
    sql = [preamble(macros)]
    for i, (_name, document, _v, _u) in enumerate(SELF_TESTS):
        sql.append("SELECT 'st%d' || '%s' || duck_blocks_are_valid(%s)::VARCHAR || '%s' || "
                   "coalesce(list_aggregate(duck_blocks_undeclared_types(%s), "
                   "'string_agg', ','), '');"
                   % (i, SEP, document, SEP, document))
    got = {}
    for line in run_sql("\n".join(sql)):
        parts = line.split(SEP)
        if len(parts) >= 3 and parts[0].startswith("st"):
            got[int(parts[0][2:])] = (parts[1].strip(), parts[2].strip())

    failures = []
    for i, (name, _d, want_valid, want_undecl) in enumerate(SELF_TESTS):
        if i not in got:
            failures.append("%s: produced no result" % name)
            continue
        valid_s, undecl_s = got[i]
        valid = valid_s.lower() == "true"
        undecl = [u for u in undecl_s.split(",") if u]
        if valid != want_valid:
            failures.append("%s: duck_blocks_are_valid returned %s, expected %s"
                            % (name, valid, want_valid))
        if undecl != want_undecl:
            failures.append("%s: undeclared_types returned %r, expected %r"
                            % (name, undecl, want_undecl))
    return failures


def check_fixtures(macros):
    fixtures = []
    for pattern in FIXTURE_GLOBS:
        fixtures.extend(sorted(glob.glob(os.path.join(ROOT, "test", "fixtures", pattern))))
    if not fixtures:
        return [], []

    # One process PER FIXTURE. A shared process aborts the whole run on the first
    # error, and .toml/.yaml depend on optional community extensions that may not be
    # installed -- that must degrade to a per-fixture note, not take the gate down.
    #
    # Materialise before the macro sees it: the macro body does `FROM unnest(<arg>)`,
    # and DuckDB rejects a FUNCTION CALL there with "Table function cannot contain
    # subqueries", a message naming a construct the caller never wrote. This applies to
    # panduck_read_blocks, which is a SCALAR, so the restriction is not specific to
    # table functions; measured both ways 2026-09-01.
    results, failures, skipped = [], [], []
    for f in fixtures:
        rel = os.path.relpath(f, ROOT)
        sql = (preamble(macros) +
               "CREATE TEMP TABLE fx AS SELECT panduck_read_blocks('%s') AS blk;\n"
               "SELECT '%s' || '%s' || duck_blocks_are_valid(blk)::VARCHAR || '%s' || "
               "coalesce(list_aggregate(duck_blocks_undeclared_types(blk), "
               "'string_agg', ','), '') FROM fx;\n"
               "SELECT 'ERR' || '%s' || 'order ' || e.element_order::VARCHAR || "
               "' ' || e.field || ': ' || e.message "
               "FROM fx, duck_blocks_errors(fx.blk) e;\n"
               % (rel, rel, SEP, SEP, SEP))
        try:
            lines = run_sql(sql)
        except RuntimeError as exc:
            detail = str(exc)
            if "needs the" in detail and "extension" in detail:
                skipped.append((rel, "optional extension not installed"))
            else:
                failures.append("%s: %s" % (rel, detail.strip().splitlines()[-1]))
            continue
        errs = [ln.split(SEP, 1)[1].strip() for ln in lines
                if ln.startswith("ERR" + SEP)]
        for line in lines:
            if line.startswith("ERR" + SEP):
                continue
            parts = line.split(SEP)
            if len(parts) < 3:
                continue
            valid = parts[1].strip().lower() == "true"
            undecl = [u for u in parts[2].strip().split(",") if u]
            results.append((rel, valid, undecl))
            if not valid:
                failures.append("%s: %s" % (rel, "; ".join(errs) if errs
                                            else "duck_blocks_are_valid returned false"))
            if undecl:
                failures.append("%s: element types outside the vocabulary: %s"
                                % (rel, ", ".join(undecl)))
    return results, failures, skipped


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--strict", action="store_true",
                    help="treat a skipped run (no build, no macros) as a failure")
    ap.add_argument("--upstream", default=DEFAULT_UPSTREAM,
                    help="duck_block_utils checkout providing %s (default: %s)"
                         % (MACROS_RELPATH, DEFAULT_UPSTREAM))
    args = ap.parse_args()

    macros = resolve_macros(args.upstream)
    if macros is None:
        msg = ("SKIP: conformance macros not found at %s\n"
               "      Pass --upstream <duck_block_utils checkout>."
               % os.path.join(args.upstream, MACROS_RELPATH))
        if args.strict:
            print(msg.replace("SKIP", "FAIL") +
                  "\n      --strict: refusing to pass without checking.")
            return 1
        print(msg)
        return 0

    for label, path in (("duckdb binary", DUCKDB), ("panduck extension", EXTENSION)):
        if not os.path.exists(path):
            msg = "SKIP: %s not found at %s (run `make release`)" % (label, path)
            if args.strict:
                print(msg.replace("SKIP", "FAIL") +
                      "\n       --strict: refusing to pass without checking.")
                return 1
            print(msg)
            return 0

    print("Macros: %s" % macros)
    print("Self-test: proving the macros can detect a defect...")
    failures = self_test(macros)
    if failures:
        print("\nFAIL: the conformance macros did not behave as expected on constructed")
        print("      input. The check cannot be trusted against real fixtures until this")
        print("      is resolved -- a check that cannot fire reports the same clean")
        print("      output as one that can.\n")
        for f in failures:
            print("  - %s" % f)
        return 1
    print("  %d/%d constructed cases classified correctly.\n"
          % (len(SELF_TESTS), len(SELF_TESTS)))

    results, failures, skipped = check_fixtures(macros)
    if failures and not results:
        print("\nFAIL: every fixture errored -- the check ran nothing:\n")
        for f in failures:
            print("  - %s" % f)
        return 1

    if not results and not skipped:
        msg = "SKIP: no fixtures found under test/fixtures/"
        if args.strict:
            print(msg.replace("SKIP", "FAIL"))
            return 1
        print(msg)
        return 0

    width = max([len(r[0]) for r in results] + [len(s[0]) for s in skipped])
    for rel, valid, undecl in results:
        note = "conformant" if valid and not undecl else "NOT CONFORMANT"
        if undecl:
            note += " (undeclared: %s)" % ", ".join(undecl)
        print("  %-*s  %s" % (width, rel, note))
    for rel, why in skipped:
        print("  %-*s  skipped -- %s" % (width, rel, why))

    if failures:
        print("\nFAIL: %d conformance problem(s):\n" % len(failures))
        for f in failures:
            print("  - %s" % f)
        return 1

    tail = "" if not skipped else " (%d skipped for missing optional extensions)" % len(skipped)
    print("\nOK: %d fixtures, all conformant (shape and vocabulary).%s" % (len(results), tail))
    return 0


if __name__ == "__main__":
    sys.exit(main())
