#!/usr/bin/env python3
"""Compare panduck's copy of the Pandoc converter against duck_block_utils' copy.

WHY THIS EXISTS, and it is not "two files should match".

src/pandoc_block_convert.cpp lives in BOTH repos. panduck's is the moved copy; upstream
keeps theirs until a released panduck (converter handoff, step 4). Each repo tests its own
copy against its own expectations, and each passes. That is not the same as the copies
agreeing, and the difference is where defects live:

  2026-09-02, found by duckeye with a hand-run version of this check. Coverage of handled
  pandoc constructors was IDENTICAL on both sides. CheckPandocDepth appeared 8 times
  upstream and 7 here. That one number was a recursion bound panduck had lost while fixing
  something else -- ExtractInlinesTextVal segfaulted on a nested AST at depth 50,000 where
  upstream raised a clean error at 128. A Pandoc AST is document-controlled input, so that
  was a crash reachable from a file.

Neither repo's test suite could have found it. panduck's suite asserts what panduck's copy
does; upstream's asserts what theirs does. The defect existed only in the RELATIONSHIP.

THIS PRODUCES A SIGNAL TO INVESTIGATE, NOT A DIAGNOSIS. duckeye's own correction, and the
most important line in this file. The counts flagged 7-versus-8 as worth looking at. What
identified the cause was reading the diff hunks and noticing a parameter had disappeared.
A green run here means "these two coarse invariants agree", nothing more -- and a check
believed to say more than it does is the same failure as a guard that cannot fire.

WHAT IT CANNOT SEE, AND WHY WIDENING IT DOES NOT HELP. Measured, not assumed, so that a
later session does not spend the afternoon I nearly did.

A producer/consumer attribute mismatch is invisible here and is NOT a missing comparator.
duck_block_utils shipped one: its converter writes attrs["list_type"] while its ANSI renderer
reads GetAttribute(block, "ordered"), so ordered lists render as bullets. This check compares
two copies of ONE file; that bug is a disagreement between two DIFFERENT files in a single
repo. Structurally out of scope.

The obvious complement -- attributes written anywhere versus attributes read anywhere -- was
prototyped by duckeye and FAILS IN BOTH DIRECTIONS on its first run:

  SILENT on the real defect. Run against the pre-fix tree, `ordered` does not appear,
  because `ordered` IS written -- by a different producer path than the broken one. The
  property is per-path; a repo-wide set comparison is per-repo, and reports agreement.

  LOUD on correct layering. Its one hit, `page_number`, is read by the renderer and written
  by no producer in that repo -- correctly, because a blocks-in library does not read PDFs;
  an upstream READER supplies it. For such a library "read but never written" is the
  expected state of every attribute a reader provides, so the signal is noise by
  construction.

What would actually catch it is per-producer attribute sets -- for each path constructing a
list block, does it write every name any consumer switches on. That needs knowing which
function is a producer for which path, which is not a grep. Recorded as measured-and-rejected
rather than left as an obvious improvement someone reimplements.

IT ALSO EXPIRES. The window opened the moment there were two copies and closes when step 4
deletes upstream's. It is worth running now precisely because a migration between the
copies is being contemplated, and a migration is when a divergence gets adopted.
"""

import json
import re
import sys
import urllib.error
import urllib.request

SRC_REL = "src/pandoc_block_convert.cpp"
UPSTREAM_REPO = "teaguesterling/duckdb_duck_block_utils"
UPSTREAM_API = f"https://api.github.com/repos/{UPSTREAM_REPO}/commits/main"
# The {ref} slot is resolved to a SHA before fetching. A branch url is served from a cache
# that lags -- check_duck_block_vocabulary.py observed it hand back a superseded file --
# and a stale fetch here would report agreement against content upstream already replaced.
UPSTREAM_RAW = (
    "https://raw.githubusercontent.com/" + UPSTREAM_REPO + "/{ref}/" + SRC_REL
)

# Divergences that are DELIBERATE, recorded with reasons. An allowlist without reasons
# decays into a mute button, and this one gates a precondition for handoff step 4.
EXPECTED = {
    "BlockTypes": "panduck renames BlockTypes:: to DuckBlockTypes:: throughout -- 264 "
    "sites, mechanical, and the reason a raw text diff of these files is "
    "useless for this purpose",
    # Flagged by this check on its first run here, then INVESTIGATED rather than muted --
    # which is the workflow it is for.
    #
    # Both copies recover formatted text from a Link or Image in a table cell. They do it
    # differently. panduck names the constructors and takes c[1], the inline array, so the
    # target never enters. Upstream names nothing and descends into every nested array,
    # relying on a Link's URL being a BARE STRING rather than a {"t":"Str"} object -- bare
    # strings inside arrays reach process_item, which requires an object and returns, so
    # attr and target contribute nothing.
    #
    # EQUIVALENCE CHECKED BY READING BOTH, not assumed: for Link and Image the reachable
    # Str objects are identical under either strategy, so the flattened text agrees. Theirs
    # is the smaller idea -- it needs no per-constructor arm -- and panduck carries two arms
    # it could drop. That is redundancy, not a defect, and not worth churn in a file whose
    # two copies are being compared.
    "Link": "different strategy for the same requirement -- see the note in EXPECTED",
    "Image": "different strategy for the same requirement -- see the note in EXPECTED",
}


def strip_comments(text: str) -> str:
    """Remove // and /* */ comments.

    NOT COSMETIC. The guard count is the invariant this check exists for, and counting raw
    string occurrences gets it wrong the moment someone WRITES ABOUT the guard: after the
    depth fix, panduck's file mentions CheckPandocDepth twice in a doc comment, so a naive
    grep reports 9 against upstream's 8 and flags a divergence that does not exist. A check
    whose own subject matter appears in prose has to read code, not text.
    """
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def handled_constructors(code: str) -> set:
    """Pandoc constructor names the file branches on, e.g. strcmp(t, "Para")."""
    return set(re.findall(r'strcmp\(\s*\w+\s*,\s*"([A-Za-z]+)"\s*\)', code))


def guard_calls(code: str) -> int:
    """CheckPandocDepth CALL sites -- `CheckPandocDepth(<expr>);` -- not mentions."""
    return len(re.findall(r"\bCheckPandocDepth\s*\([^)]*\)\s*;", code))


def defined_functions(code: str) -> set:
    """Names of functions defined in the file, as a coarse structural fingerprint."""
    return set(
        re.findall(
            r"^(?:static\s+)?[\w:<>,\s*&]+?\b(\w+)\s*\([^;]*?\)\s*\{", code, flags=re.M
        )
    )


def fetch_upstream() -> tuple:
    """Return (source_text, sha) or raise urllib.error.URLError."""
    req = urllib.request.Request(
        UPSTREAM_API, headers={"User-Agent": "panduck-divergence-check"}
    )
    with urllib.request.urlopen(req, timeout=20) as resp:
        sha = json.load(resp)["sha"]
    url = UPSTREAM_RAW.format(ref=sha)
    req = urllib.request.Request(
        url, headers={"User-Agent": "panduck-divergence-check"}
    )
    with urllib.request.urlopen(req, timeout=20) as resp:
        return resp.read().decode("utf-8"), sha


def compare(local: str, upstream: str) -> list:
    """Every divergence found, as human-readable lines."""
    out = []
    lc, uc = strip_comments(local), strip_comments(upstream)

    l_types, u_types = handled_constructors(lc), handled_constructors(uc)
    for t in sorted(u_types - l_types):
        out.append(f"constructor handled UPSTREAM but not here: {t}")
    for t in sorted(l_types - u_types):
        out.append(f"constructor handled HERE but not upstream: {t}")

    l_guard, u_guard = guard_calls(lc), guard_calls(uc)
    if l_guard != u_guard:
        out.append(
            f"CheckPandocDepth call sites differ: here={l_guard} upstream={u_guard} "
            f"-- a recursion bound may have been lost or added"
        )

    l_fns, u_fns = defined_functions(lc), defined_functions(uc)
    missing = sorted(u_fns - l_fns)
    if missing:
        out.append(
            f"functions defined upstream but not here: {', '.join(missing[:8])}"
            + (f" (+{len(missing) - 8} more)" if len(missing) > 8 else "")
        )
    return out


def self_test() -> list:
    """Prove each comparator can FIRE, using the real defect's shape.

    A divergence check that cannot detect a divergence reports agreement by comparing
    nothing, which is how this family's conformance script once returned SKIP with exit 0
    while every fixture errored.
    """
    failures = []
    base = 'void f() { CheckPandocDepth(d); if (strcmp(t, "Para") == 0) {} }\n'

    # The ACTUAL 2026-09-02 defect: a guard call removed.
    if not any(
        "CheckPandocDepth call sites differ" in m
        for m in compare(base.replace("CheckPandocDepth(d); ", ""), base)
    ):
        failures.append("SELF-TEST: a missing guard call was not detected")

    # A constructor handled on one side only.
    dropped = base.replace('strcmp(t, "Para")', 'strcmp(t, "Plain")')
    if not any("constructor handled" in m for m in compare(dropped, base)):
        failures.append("SELF-TEST: a diverging constructor set was not detected")

    # A whole function present on one side only.
    if not any(
        "functions defined upstream" in m
        for m in compare(base, base + "void g() { }\n")
    ):
        failures.append("SELF-TEST: a function missing on this side was not detected")

    # THE COMMENT TRAP, which is the one a naive implementation fails. Prose mentioning the
    # guard must not count as a call -- panduck's file now discusses CheckPandocDepth twice.
    prose = base.replace(
        "void f()", "// CheckPandocDepth and CheckPandocDepth again\nvoid f()"
    )
    if compare(prose, base):
        failures.append(
            "SELF-TEST: a comment mentioning CheckPandocDepth was counted as a call"
        )

    # A CONFORMING control: identical files must report nothing.
    if compare(base, base):
        failures.append("SELF-TEST: identical sources reported a divergence")
    return failures


def main() -> int:
    require = "--require" in sys.argv

    try:
        local = open(SRC_REL, encoding="utf-8").read()
    except OSError as e:
        print(f"  FAIL: cannot read {SRC_REL}: {e}")
        return 1

    failures = self_test()
    if failures:
        for f in failures:
            print(f"  {f}")
        print(
            "\nFAILED: the checker cannot detect a divergence, so its verdict means nothing."
        )
        return 1

    try:
        upstream, sha = fetch_upstream()
    except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError, OSError) as e:
        # Upstream unreachable is a SKIP, matching check-vocabulary: this compares against a
        # network resource and an offline build should not fail for it. --require makes it a
        # failure, which is what CI wants.
        print(f"  {'FAIL' if require else 'SKIP'}: cannot reach upstream ({e})")
        return 1 if require else 0

    print(f"  upstream {UPSTREAM_REPO} @ {sha[:10]}")
    divergences = [
        d for d in compare(local, upstream) if not any(k in d for k in EXPECTED)
    ]
    if not divergences:
        print(
            "  no divergence in handled constructors, guard calls, or defined functions."
        )
        print()
        print(
            "  A SIGNAL, NOT A DIAGNOSIS: this compares three coarse invariants. It does"
        )
        print("  not establish that the two copies behave identically.")
        return 0

    for d in divergences:
        print(f"  DIVERGENCE: {d}")
    print()
    print("These are SIGNALS TO INVESTIGATE, not diagnoses. Read the diff:")
    print(
        f"  git show {sha}:{SRC_REL} > /tmp/upstream.cpp && diff /tmp/upstream.cpp {SRC_REL}"
    )
    print(
        "A divergence may be deliberate -- if so, record it in EXPECTED with its reason."
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
