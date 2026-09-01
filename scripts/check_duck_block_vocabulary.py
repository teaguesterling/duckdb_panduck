#!/usr/bin/env python3
"""Check panduck's vendored duck_block vocabulary against upstream.

WHY THIS EXISTS. src/include/duck_block_vocabulary.hpp is a vendored copy of
duck_block_utils' published header. A copy does not notice when upstream moves --
and neither does a submodule pin, which is also just a copy with a sha attached.
That much is obvious. The part that is NOT obvious is what the C++ constants
protect against:

    TYPE_HEADING  -> TYPE_HEAD          a RENAME: compile error at every use site
    TYPE_PAGE = "page_break" -> "pagebreak"   a VALUE change: compiles CLEAN

A rename cannot survive a build. A value change survives everything -- it
compiles, every test written against its own string literals keeps passing, and
the readers silently stop emitting a type any consumer recognises. Nothing in
C++ catches that. Only this check does, which is why it is not optional
bookkeeping.

WHAT IT REPORTS, and why the three arms are separate:

  DRIFT  a constant renamed, removed, or changed value upstream. BREAKING;
         exits 1.
  NEW    published upstream, absent from our copy. Not breaking -- it means
         re-sync, and possibly new reader work.
  GAPS   published vocabulary that no panduck code branches on, so it can only
         reach a fallthrough. Not breaking, but this is the arm that earns its
         keep: it is what surfaced inline `generic` silently dropping
         source_type in duckdb_markdown and duck_block_utils independently in
         the same week.

COMPARED BY NAME AND VALUE, NEVER BY DIFFING TEXT. This is the design decision
the whole check rests on. Upstream rewrote every idx_t to uint64_t in 3957f36
and later added ~88 lines of vendoring guidance -- hundreds of changed bytes,
not one changed name or value. A text diff screams at that; this stays silent.
A check that cries wolf gets muted within a week, and a muted check catches
nothing on the day it matters.

For the same reason the printed counts are CONTEXT, NOT THE ASSERTION. A pure
rename leaves the count identical (67 vs 67) while breaking every consumer;
duck_block_utils found their spec-alignment check reporting "42 vs 42" one line
above a genuine failure. Tested against that case here -- see
test_count_blindness() below.

Adapted from duckdb_markdown's scripts/check_duck_block_vocabulary.py, which
worked out the DRIFT/NEW/GAPS split and the name+value comparison. Changed for
panduck: there is no local duck_block_utils clone to read (that is the point of
vendoring), so upstream is fetched over HTTPS by default; and the "what do we
branch on" scan reads panduck's readers plus pandoc_ast_map.cpp, which is
already an explicit registry of what panduck maps, plans and drops.

Usage:
    python3 scripts/check_duck_block_vocabulary.py            # fetch over HTTPS
    python3 scripts/check_duck_block_vocabulary.py --upstream ../duck_block_utils
    python3 scripts/check_duck_block_vocabulary.py --strict   # offline is a failure
    python3 scripts/check_duck_block_vocabulary.py --self-test
"""
import argparse
import os
import re
import subprocess
import sys
import urllib.error
import urllib.request

HEADER_REL = "src/include/duck_block_vocabulary.hpp"
UPSTREAM_REPO = "teaguesterling/duckdb_duck_block_utils"
UPSTREAM_API = f"https://api.github.com/repos/{UPSTREAM_REPO}/commits/main"
# NOTE the {ref} slot. Fetching this with ref="main" is WRONG and the reason this
# indirection exists: raw.githubusercontent.com serves BRANCH urls from a cache that
# lags, so a branch fetch can hand back a superseded header and the check then reports
# "in sync" against content upstream has already replaced. Observed live: the API had
# main at 26bfe05 with SPEC_VERSION 2.0 while the branch url was still serving 1.2.
# Resolving main to a sha first and fetching THAT is immune -- a sha url is immutable,
# so it is cached correctly by construction.
UPSTREAM_RAW = "https://raw.githubusercontent.com/" + UPSTREAM_REPO + "/{ref}/" + HEADER_REL

# Vendored first: that is panduck's arrangement. The submodule paths stay as
# fallbacks so this script is not the thing that breaks if that is revisited.
LOCAL_CANDIDATES = [
    HEADER_REL,                                     # vendored (panduck today)
    "duck_block_utils/" + HEADER_REL,               # submodule, as panduck had it
    "third_party/duck_block_utils/" + HEADER_REL,   # submodule, markdown's layout
]

CONST_RE = re.compile(
    r'static\s+constexpr\s+[\w:*\s]+?\**(\w+)\s*=\s*(?:"([^"]*)"|([0-9]+))\s*;')

# Vocabulary panduck deliberately does not branch on. Recorded WITH REASONS so an
# intentional gap and an unexplained one never look the same -- an allowlist
# without reasons decays into a mute button.
INTENTIONAL_GAPS = {
    "TYPE_GENERIC": "no reader emits it: panduck's readers map a closed set of "
                    "source constructs, and an unrecognised one is dropped rather "
                    "than wrapped. Emitting `generic` with children is the shape "
                    "that silently dropped source_type in two sibling extensions, "
                    "so adopting it needs a deliberate decision, not a fallthrough",
    "INLINE_GENERIC": "same as TYPE_GENERIC, inline side",
    "KIND_VALUE": "panduck reads documents into blocks and inlines; it never "
                  "produces the value kind, which belongs to metadata encoding",
    "VALUE_STRING": "value kind unused -- see KIND_VALUE",
    "VALUE_BOOL": "value kind unused -- see KIND_VALUE",
    "VALUE_LIST": "value kind unused -- see KIND_VALUE",
    "VALUE_MAP": "value kind unused -- see KIND_VALUE",
    "VALUE_BLOCKS": "value kind unused -- see KIND_VALUE",
    "VALUE_INLINES": "value kind unused -- see KIND_VALUE",
    "VALUE_VERSION": "value kind unused -- see KIND_VALUE",
    "TYPE_METADATA": "no reader extracts document metadata yet; docx/odt/epub all "
                     "have it available and none read it",
    "TYPE_PAGE": "physical pagination, added upstream 2026-08-31. No panduck source "
                 "format exposes page boundaries: docx/odt paginate at layout time, "
                 "epub paginates by spine document",
    "TYPE_DEFLIST": "pandoc_ast_map records this STATUS_PLANNED -- spec'd upstream, "
                    "no code path, currently dropped",
    "TYPE_LINEBLOCK": "pandoc_ast_map records this STATUS_PLANNED -- spec'd upstream, "
                      "no code path, currently dropped",
    "TYPE_FIGURE": "pandoc_ast_map records this STATUS_PLANNED -- pandoc 3.0+, "
                   "no code path, currently dropped",
}

# Files whose contents count as "panduck branches on this".
SCAN_GLOBS = ["src/*.cpp", "src/include/*.hpp"]


def parse_constants(text):
    """name -> value, for every string/int constant in the header."""
    out = {}
    for name, sval, ival in CONST_RE.findall(text):
        out[name] = sval if ival == "" else ival
    return out


def find_local(root):
    for rel in LOCAL_CANDIDATES:
        path = os.path.join(root, rel)
        if os.path.exists(path):
            return path
    return None


def read_upstream_git(repo, ref):
    """Read the header from a local clone at a ref, without checking anything out."""
    try:
        return subprocess.check_output(
            ["git", "-C", repo, "show", f"{ref}:{HEADER_REL}"],
            stderr=subprocess.PIPE, text=True)
    except subprocess.CalledProcessError as exc:
        sys.exit(f"error: cannot read {HEADER_REL} from {repo}@{ref}\n"
                 f"{exc.stderr.strip()}")


def _get(url, timeout):
    req = urllib.request.Request(url)
    # CI hits the API rate limit unauthenticated; a token raises it and is the only
    # thing standing between a shared runner and the unverified fallback path.
    token = os.environ.get("GITHUB_TOKEN")
    if token and "api.github.com" in url:
        req.add_header("Authorization", f"Bearer {token}")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.read().decode("utf-8")
    except (urllib.error.URLError, OSError, TimeoutError):
        return None


def verdict(breaking, added, verified):
    """Pure decision: (exit_code, headline, detail). Separated from I/O so the
    self-test can pin it -- notably that an UNVERIFIED read never reports OK.

    "No drift seen" from a copy you could not date is not a clean bill of health.
    Saying OK there is the same false negative the check exists to prevent, which is
    how the stale-cache bug survived its first hour.
    """
    if breaking:
        return 1, "FAILED", ("vocabulary drift is breaking. Re-sync the copy and "
                             "update the references.")
    if not verified:
        return 0, "UNVERIFIED", (
            "compared against a branch url that may be served from a stale cache.\n"
            "         No difference was seen, but the copy was NOT dated -- this is "
            "not a clean\n         bill of health. Re-run with the API reachable, or "
            "--upstream <clone>.")
    if added:
        return 0, "OK with news", ("upstream added vocabulary. Re-sync and review "
                                   "whether panduck should handle it.")
    return 0, "OK", ""


def resolve_main_sha(timeout):
    """Resolve upstream main to a commit sha, so the fetch can bypass the branch cache."""
    body = _get(UPSTREAM_API, timeout)
    if not body:
        return None
    m = re.search(r'"sha"\s*:\s*"([0-9a-f]{40})"', body)
    return m.group(1) if m else None


def read_upstream_https(timeout):
    """Fetch the published header at a resolved sha.

    Returns (text, ref_label) or (None, reason). Falls back to the branch url when the
    API is unreachable, and SAYS SO -- a branch fetch may be stale, and a check that
    quietly compares against cached content is worse than one that admits it could not
    reach the authority.
    """
    sha = resolve_main_sha(timeout)
    if sha:
        text = _get(UPSTREAM_RAW.format(ref=sha), timeout)
        if text:
            return text, f"main @ {sha[:7]}", True
        return None, "resolved main but could not fetch the header at that sha", False
    text = _get(UPSTREAM_RAW.format(ref="main"), timeout)
    if text:
        # Reachable but UNDATED. Deliberately flagged unverified: this path once
        # returned a header two spec versions behind while reporting no drift.
        return text, "main (BRANCH URL -- may be cached/stale, sha unresolved)", False
    return None, "upstream unreachable", False


def branched_on(root):
    """Constant names and literal values panduck actually references.

    Two forms count. `DuckBlockTypes::TYPE_HEADING` is the preferred one -- a
    rename is then a compile error. Bare literals ("heading") count too, because
    pandoc_ast_map.cpp is a declarative table of Pandoc constructor -> duck_block
    type and legitimately spells the types as strings.
    """
    import glob
    named, literal = set(), set()
    for pattern in SCAN_GLOBS:
        for path in glob.glob(os.path.join(root, pattern)):
            with open(path, encoding="utf-8") as fh:
                text = fh.read()
            named |= set(re.findall(r'DuckBlockTypes::([A-Z_][A-Z0-9_]*)', text))
            literal |= set(re.findall(r'"([a-z][a-z0-9_:]*)"', text))
    return named, literal


def report(local, upstream, root, show_gaps=True, verified=True, strict=False):
    """Compare two constant maps. Returns (exit_code, headline)."""
    removed = sorted(set(local) - set(upstream))
    added = sorted(set(upstream) - set(local))
    changed = sorted(k for k in set(local) & set(upstream) if local[k] != upstream[k])

    breaking = False
    if removed:
        breaking = True
        print("DRIFT  gone upstream (our references would no longer compile):")
        for k in removed:
            print(f"         {k} = {local[k]!r}")
    # SPEC_VERSION is not vocabulary, it is a statement ABOUT the vocabulary, so it gets
    # its own arm. A bump can mean the SHAPE rules changed while every name and value
    # stayed put -- 2.0 ("one shape per element_type") moved list_item from carrying
    # content to owning a paragraph child, and no constant moved at all. Reporting that
    # as "our output silently stops matching" would point the reader at the type names,
    # which are fine; the thing to go read is the spec.
    spec_moved = "SPEC_VERSION" in changed
    changed = [k for k in changed if k != "SPEC_VERSION"]
    if changed:
        breaking = True
        print("DRIFT  value changed upstream (our output silently stops matching):")
        for k in changed:
            print(f"         {k}: {local[k]!r} -> {upstream[k]!r}")
    if spec_moved:
        breaking = True
        print(f"SPEC   version moved: {local['SPEC_VERSION']!r} -> "
              f"{upstream['SPEC_VERSION']!r}")
        print("       Names and values may be untouched while the SHAPE rules changed.")
        print("       Read docs/duck_blocks_spec.md upstream before re-syncing; this is")
        print("       the only signal a structural change gives you.")
    if added:
        print("NEW    published upstream, not in our copy:")
        for k in added:
            print(f"         {k} = {upstream[k]!r}")
    if not (removed or changed or added):
        if spec_moved:
            # Both true at once, and the pairing is the whole point: names can be
            # perfectly in sync while the structure they describe has changed under you.
            print("       (every name and value IS in sync -- that is exactly why a")
            print("        shape change needs its own signal.)")
        else:
            print("vocabulary is in sync (compared by name and value)")

    if show_gaps:
        named, literal = branched_on(root)
        # Prefix alone is too broad: KIND_IDX is a struct field offset, not a
        # vocabulary name, and reporting it as an unhandled type is the kind of
        # false positive that trains people to ignore this arm. Vocabulary values
        # are lowercase tokens ("heading", "page_break"); offsets are numeric.
        vocab = {k: v for k, v in upstream.items()
                 if k.startswith(("TYPE_", "INLINE_", "VALUE_", "KIND_"))
                 and not v.isdigit()}
        gaps = sorted(k for k, v in vocab.items()
                      if k not in named and v not in literal
                      and k not in INTENTIONAL_GAPS)
        if gaps:
            print()
            print("GAPS   published but nothing branches on it (reaches a fallthrough):")
            for k in gaps:
                print(f"         {k} = {vocab[k]!r}")
            print("       If a gap is deliberate, add it to INTENTIONAL_GAPS with a reason.")

    print()
    code, headline, detail = verdict(breaking, bool(added), verified)
    print(f"{headline}: {detail}" if detail else headline)
    if not verified and strict:
        print("       --strict: refusing to pass on an undated comparison.")
        code = 1
    return code, headline


def test_count_blindness():
    """The property that makes this check worth having: it must not be count-based.

    A pure rename and a pure value change both leave the constant COUNT identical.
    A check that asserts on counts passes both. Verified rather than asserted --
    duck_block_utils shipped a check that printed "42 vs 42" directly above a real
    failure.
    """
    base = ('static constexpr const char *TYPE_PAGE = "page_break";\n'
            'static constexpr const char *TYPE_HEADING = "heading";\n')
    renamed = base.replace("TYPE_PAGE", "TYPE_PAGEBREAK")
    revalued = base.replace('"page_break"', '"pagebreak"')

    a, b, c = (parse_constants(base), parse_constants(renamed),
               parse_constants(revalued))
    failures = []

    if not (len(a) == len(b) == len(c) == 2):
        failures.append(f"setup: counts should all be 2, got {len(a)}/{len(b)}/{len(c)}")

    # A rename: same count, must still be caught (as a removal + an addition).
    if not (set(a) - set(b)):
        failures.append("a rename was not detected as a removal")
    # A value change: same count, same names, must be caught.
    changed = [k for k in set(a) & set(c) if a[k] != c[k]]
    if changed != ["TYPE_PAGE"]:
        failures.append(f"a value change was not detected; got {changed}")
    # Cosmetic churn must be silent -- this is what keeps the check credible.
    cosmetic = base.replace("const char *", "const char* ").replace(";\n", ";  // note\n")
    if parse_constants(cosmetic) != a:
        failures.append("cosmetic churn changed the parsed vocabulary")

    # Field offsets are not vocabulary. KIND_IDX is a struct index, and reporting it
    # as an unhandled type is the false positive that trains people to ignore GAPS.
    offsets = parse_constants('static constexpr uint64_t KIND_IDX = 0;\n')
    if not all(v.isdigit() for v in offsets.values()):
        failures.append("numeric field offsets are not distinguishable from vocabulary")

    # WHAT THE CHECK READS, not how it classifies. Both false negatives found in this
    # checker's first day were about its input -- a stale cached header, and a gap
    # masked by an incidental reference -- and neither would have been caught by the
    # classification tests above. An undated read must never report OK: "no drift
    # seen" from a copy you could not date is not a clean bill of health.
    if verdict(False, False, verified=False)[1] == "OK":
        failures.append("an UNVERIFIED read reported OK")
    if verdict(False, True, verified=False)[1] != "UNVERIFIED":
        failures.append("an UNVERIFIED read with additions did not report UNVERIFIED")
    if verdict(True, False, verified=False)[0] != 1:
        failures.append("real drift stopped failing when the read was unverified")
    if verdict(False, False, verified=True)[1] != "OK":
        failures.append("a verified clean read did not report OK")

    for f in failures:
        print(f"SELF-TEST FAILED: {f}")
    if failures:
        return 1
    print("self-test OK: rename, value change and cosmetic churn classified correctly "
          "with the count held constant;")
    print("              field offsets excluded; an undated read never reports OK")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--upstream", default=None,
                    help="path to a local duck_block_utils clone "
                         "(default: fetch the published header over HTTPS)")
    ap.add_argument("--ref", default="origin/main",
                    help="upstream ref, only with --upstream (default: origin/main)")
    ap.add_argument("--fetch", action="store_true",
                    help="git fetch first, only with --upstream")
    ap.add_argument("--timeout", type=float, default=15.0,
                    help="HTTPS timeout in seconds (default: 15)")
    ap.add_argument("--strict", action="store_true",
                    help="treat an unreachable upstream as a failure rather than a skip")
    ap.add_argument("--self-test", action="store_true",
                    help="verify the checker's own classification, then exit")
    args = ap.parse_args()

    if args.self_test:
        return test_count_blindness()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    local_path = find_local(root)
    if not local_path:
        sys.exit("error: no duck_block_vocabulary.hpp found in "
                 + " or ".join(LOCAL_CANDIDATES))
    local = parse_constants(open(local_path, encoding="utf-8").read())

    if args.upstream:
        if not os.path.exists(os.path.join(args.upstream, ".git")):
            sys.exit(f"error: {args.upstream} is not a git clone")
        if args.fetch:
            subprocess.run(["git", "-C", args.upstream, "fetch", "--quiet", "origin"],
                           check=False)
        text = read_upstream_git(args.upstream, args.ref)
        source = f"{args.upstream}@{args.ref}"
        verified = True
    else:
        text, ref_label, verified = read_upstream_https(args.timeout)
        source = f"{UPSTREAM_REPO} {ref_label}"
        if text is None:
            # Skipping loudly beats failing a build over a flaky network, but
            # --strict exists so CI can refuse to skip.
            msg = (f"SKIPPED: cannot reach upstream ({ref_label}).\n"
                   f"         The vendored copy was NOT verified against anything.")
            if args.strict:
                print(msg.replace("SKIPPED", "FAILED"))
                return 1
            print(msg)
            print("         Re-run with network, or --upstream <clone>, to check it.")
            return 0

    upstream = parse_constants(text)
    if not upstream:
        sys.exit("error: parsed no constants from upstream -- has the header's "
                 "shape changed?")

    print(f"local    {os.path.relpath(local_path, root)}  ({len(local)} constants)")
    print(f"upstream {source}  ({len(upstream)} constants)")
    print(f"spec     local {local.get('SPEC_VERSION', '?')}  "
          f"upstream {upstream.get('SPEC_VERSION', '?')}")
    print("         (counts are context, not the assertion -- a rename leaves them equal)")
    print()

    code, _ = report(local, upstream, root, verified=verified, strict=args.strict)
    return code


if __name__ == "__main__":
    sys.exit(main())
