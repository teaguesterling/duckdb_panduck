#!/usr/bin/env python3
"""Name every test file the suite SKIPS, and fail on a skip nobody declared.

WHY THIS EXISTS
===============

sqllogictest skips a statement whose error message matches a pattern -- panduck's runs
match `HTTP` -- and it skips a whole file whose `require` or `require-env` is unmet. Both
are reported the same way, as a COUNT BY REASON:

    All tests passed (9 skipped tests, 2694 assertions in 25 test cases)
    Skipped tests for the following reasons:
    skip on error_message matching 'HTTP': 5

That line says five files lost their assertions. It does not say WHICH. Measured on a
clean extension directory, which is what every CI runner has: 25 files ran and 2694
assertions passed, against 30 files and 2932 assertions locally -- so five whole files
and 238 assertions vanished under a summary reading "All tests passed" (#34, #59).

THOSE FIVE NOW RUN (#34). A bare `INSTALL x;` resolves against the CORE repository, so for
a community extension it is an HTTP 404 -- and the very statement written to make a file
self-sufficient was what removed its assertions, invisibly. The spelling that works is
`INSTALL x FROM community;`, and the eight such statements now use it, so the HTTP
allowlist below is empty.

THIS SCRIPT DID NOT BECOME REDUNDANT. It is what made the loss legible in the first place,
and the scan for a bare community INSTALL is what stops one being reintroduced: the next
person to write `INSTALL webbed;` gets a red check instead of 40 assertions quietly
disappearing. An allowlist emptying out is the fix landing, not the guard retiring.

WHAT THIS DOES
==============

Runs each file on its own, reads the per-file summary, and prints one line per file:

    doc_namespace.test    SKIPPED  skip on error_message matching 'HTTP': 1

A skip whose (file, reason) pair is in DECLARED below is expected and passes. Anything
else fails the check, so a file that starts skipping is a red build rather than a quieter
summary. `--list` prints the report and always exits 0, for looking rather than gating.

THE ALLOWLIST IS PAIRS, WITH REASONS, NOT A COUNT
=================================================

A count ("5 HTTP skips are fine") passes when a DIFFERENT file starts skipping, which is
the failure this check exists to catch. Each entry names the file, the reason, and why it
is acceptable -- the same posture as check_word_loss.py's EXPECTED: a measured exception,
never a tolerance.
"""

import argparse
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
TEST_DIR = ROOT / "test" / "sql"

HTTP = "skip on error_message matching 'HTTP'"

# Extensions the community repository serves. A BARE `INSTALL x;` resolves against the CORE
# repository, so for these it is an HTTP 404 -- which sqllogictest matches and turns into a
# skip. `INSTALL x FROM community;` is the spelling that works, and the one the runtime mode
# sees the consequences of.
COMMUNITY = ("webbed", "duck_block_utils", "pdf", "markdown")

BARE_INSTALL_RE = re.compile(r"^INSTALL\s+(\w+)\s*;", re.M)
REQUIRE_ENV_RE = re.compile(r"^require-env\s+(\S+)", re.M)

# (file, reason prefix) -> why this skip is expected.
DECLARED = {
    # THE FIVE HTTP ENTRIES ARE GONE (#34), and their absence is the fix landing. They
    # declared doc_namespace, html_attributes, html_reader, multidoc and register_reader --
    # the five files whose assertions vanished on a clean runner because a bare `INSTALL` of
    # a community extension is a 404, and sqllogictest skips any statement whose error
    # matches `HTTP`. Those statements now say `FROM community`, so the files RUN instead of
    # skipping and there is nothing left to declare.
    #
    # THE DETECTION ABOVE STAYS. COMMUNITY and BARE_INSTALL_RE still scan for a bare
    # community INSTALL, so reintroducing one fails this check rather than quietly removing
    # assertions again. An allowlist emptying out is the goal; the scanner is not.
    ("doc_body_parity.test", "require-env PANDUCK_BODY_PARITY_EXT"):
        "Compares panduck's native body walk against duck_block_utils' duck_blocks_body. "
        "Needs a duck_block_utils BUILD, so it is gated deliberately (#70).",
    ("expand_embedded_markdown.test", "require-env PANDUCK_TEST_EXPAND"):
        "expand_embedded delegates to the markdown extension, which a runner cannot "
        "download. The default-behaviour half runs everywhere in expand_embedded.test.",
    ("pdf_reader.test", "require-env PANDUCK_TEST_PDF"):
        "Needs the pdf community extension.",
    ("reader_policy_mutation.test", "require-env PANDUCK_TEST_POLICY_MUTATION"):
        "Mutates reader policy globally; run on its own so it cannot disturb other files.",
}

SKIPPED_RE = re.compile(r"All tests were skipped")


def predict(path):
    """Reasons this file WILL be skipped, read from the file itself.

    The runtime mode measures what happened; this predicts it from the causes, so it needs
    no build and runs on any runner. It cannot see a skip whose cause is not one of these
    two -- which is why it reports what it scanned for, and why the runtime mode exists.
    """
    text = path.read_text()
    # A require-env GATE FIRES FIRST and skips the whole file, so nothing below it ever
    # runs -- including a bare INSTALL that would otherwise 404. Predicting both would
    # report a skip that cannot happen, and the runtime mode proves it: pdf_reader.test
    # is reported skipped for its env var alone, never for HTTP. One file, one reason.
    envs = REQUIRE_ENV_RE.findall(text)
    if envs:
        return [(f"require-env {var}", f"require-env {var} gates the whole file") for var in envs]
    reasons = []
    for ext in BARE_INSTALL_RE.findall(text):
        if ext in COMMUNITY:
            reasons.append((HTTP, f"bare `INSTALL {ext};` -- community, so a 404 on a clean runner"))
    return reasons


def run_one(unittest, path, env_home=None):
    """(skipped, reason) for one test file. reason is '' when it ran."""
    env = None
    if env_home:
        import os
        env = dict(os.environ, HOME=env_home)
    r = subprocess.run([unittest, str(path)], capture_output=True, text=True, env=env)
    out = r.stdout + r.stderr
    if not SKIPPED_RE.search(out):
        return False, ""
    tail = out.split("Skipped tests for the following reasons:", 1)
    reason = tail[1].strip().splitlines()[0].strip() if len(tail) > 1 else "(unreported)"
    return True, reason


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--unittest", default=str(ROOT / "build" / "release" / "test" / "unittest"),
                    help="path to the unittest binary (default: build/release/test/unittest)")
    ap.add_argument("--home", default=None,
                    help="run with this HOME, e.g. an empty directory to reproduce a CI runner")
    ap.add_argument("--list", action="store_true", help="report only; never fail")
    ap.add_argument("--static", action="store_true",
                    help="predict skips by reading the test files; needs no build, for CI")
    args = ap.parse_args()

    if args.static:
        files = sorted(TEST_DIR.glob("*.test"))
        undeclared, predicted = [], 0
        for path in files:
            for reason, cause in predict(path):
                predicted += 1
                key = next((k for k in DECLARED if k[0] == path.name and reason.startswith(k[1])), None)
                mark = "" if key else "   <-- NOT DECLARED"
                print(f"  {path.name:<34} {reason:<44} {cause}{mark}")
                if key is None:
                    undeclared.append((path.name, reason))
        print(f"\n  scanned {len(files)} files for two causes: a bare INSTALL of a community")
        print(f"  extension, and require-env. {predicted} predicted skip(s), "
              f"{predicted - len(undeclared)} declared.")
        if undeclared and not args.list:
            print("\nFAILED: a test file will be skipped and nothing declares it.")
            print("Use `INSTALL x FROM community;`, or add the pair to DECLARED with a reason.")
            return 1
        print("\nOK: every predicted skip is declared. (This mode reads the files; the")
        print("    default mode runs them and needs a build.)")
        return 0

    if not pathlib.Path(args.unittest).exists():
        print(f"no unittest binary at {args.unittest}; build first", file=sys.stderr)
        return 2

    files = sorted(TEST_DIR.glob("*.test"))
    if not files:
        print(f"no test files under {TEST_DIR}", file=sys.stderr)
        return 2

    ran, skipped, undeclared, stale = 0, [], [], []
    for path in files:
        is_skipped, reason = run_one(args.unittest, path, args.home)
        if not is_skipped:
            ran += 1
            continue
        skipped.append((path.name, reason))
        key = next((k for k in DECLARED if k[0] == path.name and reason.startswith(k[1])), None)
        if key is None:
            undeclared.append((path.name, reason))

    for name, reason in skipped:
        declared = not any(n == name and r == reason for n, r in undeclared)
        print(f"  {name:<34} {'SKIPPED':<8} {reason}" + ("" if declared else "   <-- NOT DECLARED"))
    print(f"\n  {ran} files ran, {len(skipped)} skipped "
          f"({len(skipped) - len(undeclared)} declared, {len(undeclared)} not)")

    # A DECLARED SKIP THAT NO LONGER HAPPENS is also reported: the allowlist has outlived
    # the reason for the entry, and an allowlist nobody prunes becomes a mute button.
    for (name, prefix) in DECLARED:
        if not any(n == name and r.startswith(prefix) for n, r in skipped):
            stale.append((name, prefix))
    if stale:
        # ADVISORY, NOT A FAILURE, because this list is environment-dependent: on a machine
        # with the community extensions installed, every HTTP entry is legitimately absent.
        # It is printed so an entry that has outlived its cause can be pruned -- an
        # allowlist nobody prunes becomes a mute button -- not because it is wrong here.
        print("\n  declared, but not skipping in THIS environment (expected where the")
        print("  extension is installed; prune only if it can no longer skip anywhere):")
        for name, prefix in stale:
            print(f"    {name}: {prefix}")

    if args.list:
        return 0
    if undeclared:
        print("\nFAILED: a test file is being skipped and nothing says why.")
        print("Either fix the cause, or add the (file, reason) pair to DECLARED with the")
        print("reason it is acceptable. A file whose assertions vanish must not be silent.")
        return 1
    print("\nOK: every skipped file is declared, with a reason.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
