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

WHAT IT COMPARES AGAINST: THE LATEST RELEASE, NOT MAIN. Since spec 1.4 the contract
with duck_block_utils is release-based (settled with their consumer check, #38).
Spec releases are batched, so main can carry constants no release has yet, and
comparing against main reddened an up-to-date copy mid-batch -- which is what turned
every minor into a re-vendor. Re-vendor only on a SPEC_VERSION MAJOR change, or when
panduck needs something a later minor added (a constant, or a predicate whose answers
changed). The next forced sync is a 2.0.

WHAT IT REPORTS, and why the arms are separate:

  DRIFT  a shared constant's value differs from the release -- strings AND
         integers, so a moved *_IDX offset counts. FAILS.
  EXTRA  in our copy, not in the release: a local edit, or vendored from an
         unreleased main. FAILS.
  SPEC   a MAJOR mismatch, or a copy claiming a minor AHEAD of the release.
         FAILS. A copy behind on minor is aligned.
  PROVENANCE  the stamp is missing, malformed, disagrees with the file or with
         its release label, or the header at the stamped sha differs from the
         copy by anything but one inserted comment block. FAILS.
  BEHIND in the release, not in our copy. PASSES -- re-vendor only if needed.
  GAPS   published vocabulary that no panduck code branches on, so it can only
         reach a fallthrough. Not breaking, but this is the arm that earns its
         keep: it is what surfaced inline `generic` silently dropping
         source_type in duckdb_markdown and duck_block_utils independently in
         the same week.

GAPS HAS A BLIND SPOT OF ITS OWN, DOCUMENTED RATHER THAN FIXED. "Branched on"
counts a constant's VALUE appearing as a bare string literal ANYWHERE in the
scanned sources, not just where it is compared against duck_block vocabulary --
so a short, generic value collides with unrelated code and reads as "handled"
when nothing branches on it as vocabulary at all. Measured on panduck:
TYPE_FIGURE ('figure') collides with the LaTeX environment name in
latex_macros.cpp; TYPE_PAGE ('page_break') collides with a provenance comment
in duck_block_types.hpp; VALUE_LIST ('list') collides with TYPE_LIST, the
block type -- duck_block's own spec warns "list" is spelled as both a block
type and a value type. All three stay in INTENTIONAL_GAPS with reasons, not
because the scan proved them unhandled, but because the scan CANNOT see past
the collision to tell. An empty GAPS section therefore means "no gap this scan
can SEE" -- not "no gap." Scoping the literal match to duck_block call sites
would close this, but that is real work, deliberately deferred.

COMPARED BY NAME AND VALUE, NEVER BY DIFFING TEXT. This is the design decision
the whole check rests on. Upstream rewrote every idx_t to uint64_t in 3957f36
and later added ~88 lines of vendoring guidance -- hundreds of changed bytes,
not one changed name or value. A text diff screams at that; this stays silent.
A check that cries wolf gets muted within a week, and a muted check catches
nothing on the day it matters. The PROVENANCE arm does diff text, but only against
the header at the sha the copy itself names -- the file it claims to be -- so
upstream churn cannot reach it.

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
    python3 scripts/check_duck_block_vocabulary.py            # latest release, over HTTPS
    python3 scripts/check_duck_block_vocabulary.py --upstream ../duckdb_duck_block_utils --fetch
    python3 scripts/check_duck_block_vocabulary.py --ref main  # preview what the next release brings
    python3 scripts/check_duck_block_vocabulary.py --strict   # offline is a failure
    python3 scripts/check_duck_block_vocabulary.py --self-test
"""

import argparse
import difflib
import os
import re
import subprocess
import sys
import urllib.error
import urllib.request

HEADER_REL = "src/include/duck_block_vocabulary.hpp"
UPSTREAM_REPO = "teaguesterling/duckdb_duck_block_utils"
UPSTREAM_API = f"https://api.github.com/repos/{UPSTREAM_REPO}"
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
    HEADER_REL,  # vendored (panduck today)
    "duck_block_utils/" + HEADER_REL,  # submodule, as panduck had it
    "third_party/duck_block_utils/" + HEADER_REL,  # submodule, markdown's layout
]

CONST_RE = re.compile(r'static\s+constexpr\s+[\w:*\s]+?\**(\w+)\s*=\s*(?:"([^"]*)"|([0-9]+))\s*;')

# The vendored copy's provenance stamp, e.g.
#   // Vendored at upstream commit: 95a84e6 (SPEC_VERSION 1.4)  [duck_block_utils v3.3.0]
# The release label is optional; a sha and a spec are not.
STAMP_PREFIX = "// Vendored at upstream commit:"
STAMP_RE = re.compile(
    r"^// Vendored at upstream commit: ([0-9a-f]{7,40}) \(SPEC_VERSION ([0-9]+\.[0-9]+)\)"
    r"(?:\s+\[duck_block_utils (v[0-9]+\.[0-9]+\.[0-9]+)\])?\s*$"
)
RELEASE_TAG_RE = re.compile(r"^v([0-9]+)\.([0-9]+)\.([0-9]+)$")

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
    "VALUE_LIST": "value kind unused -- see KIND_VALUE. NOTE: this scan cannot actually "
    "verify that -- 'list' also collides with TYPE_LIST's own value, so the "
    "GAPS scan would report this as 'branched on' even if the reason above "
    "stopped being true. Documented, not fixed -- see the docstring's "
    "GAPS BLIND SPOT section",
    "VALUE_MAP": "value kind unused -- see KIND_VALUE",
    "VALUE_BLOCKS": "value kind unused -- see KIND_VALUE",
    "VALUE_INLINES": "value kind unused -- see KIND_VALUE",
    "VALUE_VERSION": "value kind unused -- see KIND_VALUE",
    "TYPE_METADATA": "no reader extracts document metadata yet; docx/odt/epub all have it "
    "available and none read it. NOT A NEUTRAL GAP -- this is a DISCARD. "
    "Every document's title, author and date is dropped, and LaTeX drops "
    "\\title/\\author/\\maketitle explicitly. A leak into the body would "
    "be visible and correctable; a discard is unrecoverable and looks like "
    "a document that never had a title. Filed behind fidelity work for a "
    "day, which was too generous -- by this project's own gap-versus-"
    "discard rule it is on the wrong side. Closing it needs the `value` "
    "kind as well as this type",
    "TYPE_PAGE": "IMPLEMENTED 2026-09-01 for EPUB -- this entry now records what is still "
    'missing rather than a gap. <span epub:type="pagebreak" title="42"/> '
    "becomes page_break with attributes['page_number'], which is EPUB 3's "
    "print-equivalent pagination and what citation workflows need. STILL "
    "ABSENT: docx section breaks and odt soft page breaks, both of which are "
    "layout-time in those formats rather than authored markers, so whether "
    "they are the same construct is a real question and not an oversight. The "
    "earlier reason here was WRONG, not merely stale: it said no panduck "
    "source exposes page boundaries because epub paginates by spine document "
    "-- spine items are chapter boundaries and correctly are not pages, while "
    "the construct that IS a page went unread. NOTE: 'page_break' also "
    "collides with a provenance comment in duck_block_types.hpp, so the GAPS "
    "scan cannot verify this reason either -- see the docstring's GAPS BLIND "
    "SPOT section",
    "TYPE_DEFLIST": "pandoc_ast_map records this STATUS_PLANNED -- spec'd upstream, " "no code path, currently dropped",
    "TYPE_LINEBLOCK": "pandoc_ast_map records this STATUS_PLANNED -- spec'd upstream, "
    "no code path, currently dropped",
    "TYPE_FIGURE": "pandoc_ast_map records this STATUS_PLANNED -- pandoc 3.0+, "
    "no code path, currently dropped. NOTE: 'figure' also collides with "
    "the LaTeX environment NAME in latex_macros.cpp, so the GAPS scan "
    "cannot verify this reason either -- see the docstring's GAPS BLIND "
    "SPOT section",
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


def git_out(repo, *args):
    """stdout of a git command in a clone, or None if it failed."""
    try:
        return subprocess.check_output(["git", "-C", repo, *args], stderr=subprocess.PIPE, text=True)
    except subprocess.CalledProcessError:
        return None


def read_upstream_git(repo, ref):
    """Read the header from a local clone at a ref, without checking anything out. None if absent."""
    return git_out(repo, "show", f"{ref}:{HEADER_REL}")


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


def parse_spec(v):
    """(major, minor) from a SPEC_VERSION string, or None if it is not one.

    Module level rather than nested, because spec_superseded needs the same parse and two
    copies of a version parser is exactly how two rules drift apart.
    """
    try:
        major, _, minor = str(v).partition(".")
        return int(major), int(minor or 0)
    except (TypeError, ValueError):
        return None


def spec_compatible(local_v, upstream_v):
    """Is upstream's SPEC_VERSION compatible with ours, per duck_block_utils' contract?

    The contract (defined upstream at b40bcab): MAJOR for a breaking shape or
    vocabulary change, MINOR for an additive one. The assertion to write is therefore
    MAJOR EQUALITY PLUS A MINOR FLOOR -- not equality on the string. Equality would go
    red on releases that cannot affect us, and a check that cries wolf gets muted,
    which is the failure this whole script exists to avoid.

    HISTORICAL HAZARD, recorded because it makes this function look wrong when it is
    not: the 1.1 -> 1.2 bump was MIS-NUMBERED. list and blockquote became structural,
    which broke duckdb_markdown's writer in three places, and it shipped as a minor.
    So a consumer applying this rule across that specific bump gets a pass where it
    should have failed. The contract is only sound going forward; 1.x history is not.
    Upstream records the mis-numbering rather than quietly renumbering, which is the
    only reason this is knowable at all.
    """

    lo, up = parse_spec(local_v), parse_spec(upstream_v)
    if lo is None or up is None:
        return False  # unparseable: refuse to call it compatible
    if lo[0] != up[0]:
        return False  # major differs -- breaking by the contract
    return up[1] >= lo[1]  # minor floor; upstream ahead on minor is additive


def spec_superseded(local_v, upstream_v, supersedes):
    """Is the major difference a RENUMBERING rather than a break?

    THE RULE ABOVE CANNOT TELL THE TWO APART, and that is not a flaw in it -- a major
    change IS breaking by the contract, and a renumber changes the major. Measured on
    the same rule: 6.6 -> 1.2 reads as breaking, identically to a real 2.0.

    So the renumber carries evidence a check can see. duck_block_utils publishes
    SPEC_VERSION_SUPERSEDES alongside SPEC_VERSION for the one release it takes
    consumers to re-vendor: 1.2 supersedes 6.6, same shape, no constant moved.

    ACCEPTED ONLY WHEN THE SUPERSEDED VALUE IS AT OR AHEAD OF OURS. A consumer pinned
    at 6.6 is looking at the same vocabulary renamed and is fine; one pinned at 6.7 --
    were there such a thing -- would be looking at something the renumber did not
    cover, and gets the breaking verdict it deserves. The escape hatch is for the
    renumbering, not for any major mismatch that happens to ship one.

    Recorded rather than made silent, per duck_block_utils' own precedent: they
    documented the mis-numbered 1.1 -> 1.2 rather than quietly renumbering, which is
    the only reason it is knowable. A renumber a consumer cannot see is worse than an
    inflated number.
    """
    lo, sup = parse_spec(local_v), parse_spec(supersedes)
    up = parse_spec(upstream_v)
    if lo is None or up is None or sup is None:
        return False
    if lo[0] == up[0]:
        return False  # not a renumber; the ordinary rule applies
    if lo[0] != sup[0]:
        return False  # we are not on the line being retired
    return sup[1] >= lo[1]


def classify(local, upstream):
    """Pure alignment decision against a RELEASE: status OK / BEHIND / FAILED, with reasons.

    Reasons: EXTRA (ours, not the release's), CHANGED (a shared value differs -- integers
    included, so a moved *_IDX offset fails), MISSING (the release's, not ours), AHEAD (the
    copy claims a minor no release has), MAJOR (spec major differs, or is unparseable),
    RENUMBER (the major differs but the release supersedes this copy's line -- see
    spec_superseded). EXTRA, CHANGED, AHEAD and MAJOR fail; MISSING and an older minor are
    BEHIND and pass.

    SPEC_VERSION is compared as a version, never as a value: equal-or-older minor is the
    contract, and a value comparison would call every minor bump CHANGED.
    """
    lo_v, up_v = local.get("SPEC_VERSION"), upstream.get("SPEC_VERSION")
    lo, up = parse_spec(lo_v), parse_spec(up_v)
    extra = sorted(set(local) - set(upstream))
    missing = sorted(set(upstream) - set(local))
    changed = sorted(k for k in (set(local) & set(upstream)) - {"SPEC_VERSION"} if local[k] != upstream[k])

    if lo is None or up is None:
        spec = "major"
    elif lo[0] != up[0]:
        spec = "renumber" if spec_superseded(lo_v, up_v, upstream.get("SPEC_VERSION_SUPERSEDES")) else "major"
    elif not spec_compatible(lo_v, up_v):
        spec = "ahead"
    elif lo[1] < up[1]:
        spec = "behind"
    else:
        spec = "same"

    reasons = [spec.upper()] if spec in ("major", "ahead", "renumber") else []
    if extra:
        reasons.append("EXTRA")
    if changed:
        reasons.append("CHANGED")
    if missing:
        reasons.append("MISSING")

    if {"EXTRA", "CHANGED", "AHEAD", "MAJOR"} & set(reasons):
        status = "FAILED"
    elif missing or spec in ("behind", "renumber"):
        status = "BEHIND"
    else:
        status = "OK"
    return {"status": status, "reasons": reasons, "spec": spec, "extra": extra, "missing": missing, "changed": changed}


def _stamp_line(text):
    """The provenance stamp line, if one is present near the top; None otherwise."""
    for line in text.splitlines()[:12]:
        if line.startswith(STAMP_PREFIX):
            return line
    return None


def parse_stamp(text):
    """{"sha", "spec", "tag"} from the vendored copy's stamp, or None if absent or malformed."""
    line = _stamp_line(text)
    m = STAMP_RE.match(line) if line else None
    return {"sha": m.group(1), "spec": m.group(2), "tag": m.group(3)} if m else None


def provenance_problems(local_text, at_stamp_text, label_sha):
    """Why the copy is not demonstrably the header it claims to be; [] when it is.

    at_stamp_text is upstream's header at the stamped sha (None: could not be read).
    label_sha is the commit the stamp's release label resolves to (None: no label, or not
    resolved -- the caller reports an unresolvable label itself).

    THE ONE ALLOWED DIFFERENCE is a single inserted block of comment or blank lines that
    carries the stamp: the note saying where the copy came from. Anything else -- a changed
    line, code inside the block, a second insertion -- is a local edit. A local edit that
    happens to match a NEWER release is invisible to the name+value comparison, which is
    why provenance is checked against the sha the copy names rather than the release.
    """
    line = _stamp_line(local_text)
    if line is None:
        return [f"stamp missing: no '{STAMP_PREFIX} <sha> (SPEC_VERSION x.y)' line near the top"]
    stamp = parse_stamp(local_text)
    if stamp is None:
        return [f"stamp malformed: {line.strip()!r}"]
    problems = []
    declared = parse_constants(local_text).get("SPEC_VERSION")
    if stamp["spec"] != declared:
        problems.append(f"stamp claims SPEC_VERSION {stamp['spec']}, the file declares {declared}")
    if stamp["tag"] and label_sha and not label_sha.startswith(stamp["sha"]):
        problems.append(f"stamp labels {stamp['tag']} at {stamp['sha']}, but {stamp['tag']} is {label_sha[:7]}")
    if at_stamp_text is None:
        problems.append(f"the header at the stamped sha {stamp['sha']} could not be read")
        return problems
    up_lines, lo_lines = at_stamp_text.splitlines(), local_text.splitlines()
    hunks = [
        op for op in difflib.SequenceMatcher(None, up_lines, lo_lines, autojunk=False).get_opcodes() if op[0] != "equal"
    ]
    inserted = lo_lines[hunks[0][3] : hunks[0][4]] if len(hunks) == 1 and hunks[0][0] == "insert" else None
    if inserted is None or line not in inserted or any(l.strip() and not l.lstrip().startswith("//") for l in inserted):
        where = ", ".join(f"{op} at copy lines {j1 + 1}-{j2}" for op, _, _, j1, j2 in hunks[:3]) or "no difference"
        problems.append(
            f"the copy differs from the header at {stamp['sha']} by more than one inserted comment block ({where})"
        )
    return problems


def pick_release_tag(tags):
    """The newest plain vX.Y.Z among tag names, compared numerically; None if there is none.

    A clone knows tags, not GitHub's release flags, so a pre-release is recognised by its
    suffix and excluded -- the same set releases/latest excludes over HTTPS.
    """
    found = [(tuple(int(g) for g in m.groups()), t) for t in tags for m in [RELEASE_TAG_RE.match(t)] if m]
    return max(found)[1] if found else None


def verdict(breaking, behind, verified):
    """Pure decision: (exit_code, headline, detail). Separated from I/O so the
    self-test can pin it -- notably that an UNVERIFIED read never reports OK.

    "No drift seen" from a copy you could not date is not a clean bill of health.
    Saying OK there is the same false negative the check exists to prevent, which is
    how the stale-cache bug survived its first hour.

    BEHIND PASSES. The contract is release-based: a copy on an older minor of the same
    major, or missing constants a later minor added, is aligned. Re-vendoring for it is
    driven by need, which is what stops the churn.
    """
    if breaking:
        return (1, "FAILED", "the copy is not aligned with the release -- see the arms above.")
    if not verified:
        return (
            0,
            "UNVERIFIED",
            (
                "compared against a branch url that may be served from a stale cache.\n"
                "         No difference was seen, but the copy was NOT dated -- this is "
                "not a clean\n         bill of health. Re-run with the API reachable, or "
                "--upstream <clone>."
            ),
        )
    if behind:
        return (
            0,
            "BEHIND",
            (
                "the release has vocabulary or a minor this copy lacks. Aligned by the contract:\n"
                "         re-vendor only if panduck needs a constant or predicate answer that was added."
            ),
        )
    return 0, "OK", ""


def resolve_commit(ref, timeout):
    """Full commit sha for a ref (tag, branch or short sha) via the API; None if unreachable."""
    body = _get(f"{UPSTREAM_API}/commits/{ref}", timeout)
    m = re.search(r'"sha"\s*:\s*"([0-9a-f]{40})"', body) if body else None
    return m.group(1) if m else None


def resolve_release(timeout):
    """(tag, sha) of upstream's latest published release; None if unreachable.

    releases/latest excludes drafts and pre-releases by GitHub's own definition, which is
    the set the contract means by "release".
    """
    body = _get(f"{UPSTREAM_API}/releases/latest", timeout)
    m = re.search(r'"tag_name"\s*:\s*"([^"]+)"', body) if body else None
    if not m:
        return None
    sha = resolve_commit(m.group(1), timeout)
    return (m.group(1), sha) if sha else None


def fetch_header(sha, timeout):
    """The header at an immutable sha url -- never a branch url (see UPSTREAM_RAW)."""
    return _get(UPSTREAM_RAW.format(ref=sha), timeout)


def branched_on(root):
    """Constant names and literal values panduck actually references.

    Two forms count. `DuckBlockTypes::TYPE_HEADING` is the preferred one -- a
    rename is then a compile error. Bare literals ("heading") count too, because
    pandoc_ast_map.cpp is a declarative table of Pandoc constructor -> duck_block
    type and legitimately spells the types as strings.
    """
    import glob

    named, literal = set(), set()
    # Excluded BY NAME, not by narrowing SCAN_GLOBS: the vendored header is what
    # DEFINES the vocabulary, so scanning it reads every constant's own definition
    # line back as if it were consumer usage -- every value it defines then trivially
    # satisfies "value not in literal" and GAPS can never report anything. A narrower
    # glob would hide this again the moment someone relocates the header; naming the
    # file explicitly means relocating it breaks this exclusion loudly instead of
    # silently.
    excluded = os.path.normpath(os.path.join(root, HEADER_REL))
    for pattern in SCAN_GLOBS:
        for path in glob.glob(os.path.join(root, pattern)):
            if os.path.normpath(path) == excluded:
                continue
            with open(path, encoding="utf-8") as fh:
                text = fh.read()
            named |= set(re.findall(r"DuckBlockTypes::([A-Z_][A-Z0-9_]*)", text))
            literal |= set(re.findall(r'"([a-z][a-z0-9_:]*)"', text))
    return named, literal


def report(local, upstream, root, show_gaps=True, verified=True, strict=False, provenance=None):
    """Compare a copy's constants against a RELEASE's. Returns (exit_code, headline).

    The decision is classify()'s; this only prints it, so the self-test can pin the decision
    without capturing output. provenance is provenance_problems()' list, or None if unchecked.
    """
    c = classify(local, upstream)
    lo_v, up_v = local.get("SPEC_VERSION"), upstream.get("SPEC_VERSION")

    if c["extra"]:
        print("EXTRA  in our copy, not in the release (a local edit, or vendored from an unreleased main):")
        for k in c["extra"]:
            print(f"         {k} = {local[k]!r}")
    if c["changed"]:
        print("DRIFT  value differs from the release (our output silently stops matching):")
        for k in c["changed"]:
            print(f"         {k}: {local[k]!r} -> {upstream[k]!r}")
    # SPEC_VERSION is not vocabulary, it is a statement ABOUT the vocabulary, so it gets
    # its own arm. A bump can mean the SHAPE rules changed while every name and value
    # stayed put -- 2.0 ("one shape per element_type") moved list_item from carrying
    # content to owning a paragraph child, and no constant moved at all.
    if c["spec"] == "major":
        print(f"SPEC   MAJOR mismatch: copy {lo_v!r}, release {up_v!r}")
        print("       A breaking shape or vocabulary change, and the one re-vendor the contract")
        print("       forces. Names and values may be untouched while the SHAPE rules changed --")
        print("       read docs/duck_blocks_spec.md upstream before re-syncing.")
    elif c["spec"] == "ahead":
        print(f"SPEC   copy claims {lo_v!r}, AHEAD of the latest release {up_v!r}")
        print("       No release has that minor: vendored from main mid-batch, or edited locally.")
        print("       Vendor from a release tag.")
    elif c["spec"] == "renumber":
        print(
            f"SPEC   {lo_v} -> {up_v} is a RENUMBERING, not a break:\n"
            f"       the release records SPEC_VERSION_SUPERSEDES = {upstream.get('SPEC_VERSION_SUPERSEDES')},"
            f" the line this copy is on.\n"
            f"       Same shape, no constant moved. Re-vendor the header and set the local major to"
            f" {str(up_v).split('.')[0]}."
        )
    elif c["spec"] == "behind":
        print(f"SPEC   copy {lo_v!r} is behind the release {up_v!r} on the same major.")
        print("       Aligned by the contract: re-vendor only if panduck needs a constant, or a")
        print("       predicate answer, that a later minor added.")
    if c["missing"]:
        print("BEHIND in the release, not in our copy (passes; re-vendor only if panduck needs one):")
        for k in c["missing"]:
            print(f"         {k} = {upstream[k]!r}")
    if not (c["extra"] or c["changed"] or c["missing"]):
        if c["spec"] in ("major", "ahead"):
            # Both true at once, and the pairing is the whole point: names can be
            # perfectly in sync while the structure they describe has changed under you.
            print("       (every name and value IS in sync -- that is exactly why the version")
            print("        needs its own signal.)")
        else:
            print("vocabulary is in sync with the release (compared by name and value)")
    if provenance:
        print("PROVENANCE  the copy is not demonstrably the header it claims to be:")
        for p in provenance:
            print(f"         {p}")
    elif provenance is not None:
        print("provenance: stamp consistent; the copy is the header at the stamped sha plus its note")

    if show_gaps:
        named, literal = branched_on(root)
        # Prefix alone is too broad: KIND_IDX is a struct field offset, not a
        # vocabulary name, and reporting it as an unhandled type is the kind of
        # false positive that trains people to ignore this arm. Vocabulary values
        # are lowercase tokens ("heading", "page_break"); offsets are numeric.
        vocab = {
            k: v
            for k, v in upstream.items()
            if k.startswith(("TYPE_", "INLINE_", "VALUE_", "KIND_")) and not v.isdigit()
        }
        gaps = sorted(k for k, v in vocab.items() if k not in named and v not in literal and k not in INTENTIONAL_GAPS)
        if gaps:
            print()
            print("GAPS   published but nothing branches on it (reaches a fallthrough):")
            for k in gaps:
                print(f"         {k} = {vocab[k]!r}")
            print("       If a gap is deliberate, add it to INTENTIONAL_GAPS with a reason.")

    print()
    code, headline, detail = verdict(c["status"] == "FAILED" or bool(provenance), c["status"] == "BEHIND", verified)
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
    base = (
        'static constexpr const char *TYPE_PAGE = "page_break";\n'
        'static constexpr const char *TYPE_HEADING = "heading";\n'
    )
    renamed = base.replace("TYPE_PAGE", "TYPE_PAGEBREAK")
    revalued = base.replace('"page_break"', '"pagebreak"')

    a, b, c = (
        parse_constants(base),
        parse_constants(renamed),
        parse_constants(revalued),
    )
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
    offsets = parse_constants("static constexpr uint64_t KIND_IDX = 0;\n")
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

    # The version contract: MAJOR equality plus a MINOR floor, not string equality.
    for lo, up, want, why in [
        ("1.1", "2.0", False, "a major bump must be breaking"),
        ("1.1", "1.2", True, "a minor bump must not fail"),
        ("2.0", "2.0", True, "equal versions are compatible"),
        ("2.1", "2.0", False, "upstream behind on minor is not a floor match"),
        (
            "1.1",
            "nonsense",
            False,
            "an unparseable version must not read as compatible",
        ),
    ]:
        if spec_compatible(lo, up) != want:
            failures.append(f"spec_compatible({lo!r}, {up!r}) -- {why}")

    # THE RENUMBERING ESCAPE HATCH, pinned in both directions. Four of these seven must be
    # REFUSED -- an escape hatch that accepts everything is not a hatch, it is a hole, and
    # this one exists precisely because the ordinary rule cannot tell a renumber from a
    # break. duck_block_utils renumbered 6.6 -> 1.2 with no shape change (their PR #30,
    # c233f18) and published SPEC_VERSION_SUPERSEDES so a check could see the difference.
    for lo, up, sup, want, why in [
        ("6.5", "1.2", "6.6", True, "the renumber itself: this copy is on the retired 6.x line"),
        ("6.6", "1.2", "6.6", True, "pinned exactly at the superseded value"),
        ("6.5", "2.0", None, False, "a real major break, with no supersedes published"),
        ("6.5", "1.2", None, False, "a renumber CLAIMED but not evidenced"),
        ("6.7", "1.2", "6.6", False, "ahead of what the renumber covered"),
        ("6.5", "1.2", "5.0", False, "supersedes a line this copy is not on"),
        ("1.2", "1.3", "6.6", False, "majors already equal -- the ordinary rule applies"),
    ]:
        if spec_superseded(lo, up, sup) != want:
            failures.append(f"spec_superseded({lo!r}, {up!r}, {sup!r}) -- {why}")

    failures += release_contract_failures()

    for f in failures:
        print(f"SELF-TEST FAILED: {f}")
    if failures:
        return 1
    print("self-test OK: rename, value change and cosmetic churn classified correctly " "with the count held constant;")
    print("              field offsets excluded; an undated read never reports OK;")
    print("              a renumbering is accepted only on the line it retires;")
    print("              BEHIND passes, EXTRA/CHANGED/AHEAD/MAJOR and provenance problems fail")
    return 0


def release_contract_failures():
    """The post-1.4 contract, settled with duck_block_utils (their consumer check #38).

    Compare against the latest duck_block_utils RELEASE, not main: spec releases are
    batched, so main can carry constants no release has, and comparing against it reddens an
    up-to-date copy mid-batch -- the churn this contract exists to stop. A copy on an older
    minor of the same major is ALIGNED. So:

      FAILED  MAJOR mismatch, a CHANGED value (strings AND integers, *_IDX included), an
              EXTRA constant the release lacks, a copy claiming a minor AHEAD of the release,
              or a PROVENANCE problem.
      BEHIND  constants missing only, at the same or an older minor. Passes.
    """
    failures = []
    U = {"SPEC_VERSION": "1.4", "TYPE_A": "a", "KIND_IDX": "0", "PREDICATE_REVISION": "1.3"}

    def with_(d, **kw):
        out = dict(d)
        for k, v in kw.items():
            if v is None:
                out.pop(k, None)
            else:
                out[k] = v
        return out

    for local, upstream, status, reasons, why in [
        (U, U, "OK", set(), "identical to the release"),
        (with_(U, PREDICATE_REVISION=None), U, "BEHIND", {"MISSING"}, "missing a constant at the same minor"),
        (
            with_(U, SPEC_VERSION="1.3", PREDICATE_REVISION=None),
            U,
            "BEHIND",
            {"MISSING"},
            "an older minor missing what the newer one added is aligned",
        ),
        (with_(U, SPEC_VERSION="1.3"), U, "BEHIND", set(), "an older minor with nothing missing still says BEHIND"),
        (with_(U, EXTRA_X="x"), U, "FAILED", {"EXTRA"}, "a constant the release does not have"),
        (
            with_(U, EXTRA_X="x", PREDICATE_REVISION=None),
            U,
            "FAILED",
            {"EXTRA", "MISSING"},
            "EXTRA is not excused by also being behind",
        ),
        (with_(U, TYPE_A="b"), U, "FAILED", {"CHANGED"}, "a changed string value"),
        (with_(U, KIND_IDX="1"), U, "FAILED", {"CHANGED"}, "a changed INTEGER value -- the *_IDX offsets count"),
        (with_(U, SPEC_VERSION="1.5"), U, "FAILED", {"AHEAD"}, "a copy claiming a minor the release does not have"),
        (with_(U, SPEC_VERSION="2.0"), U, "FAILED", {"MAJOR"}, "copy on a newer major than the release"),
        (U, with_(U, SPEC_VERSION="2.0"), "FAILED", {"MAJOR"}, "release on a newer major: the one forced re-vendor"),
        (
            with_(U, SPEC_VERSION="6.5", PREDICATE_REVISION=None),
            with_(U, SPEC_VERSION="1.2", SPEC_VERSION_SUPERSEDES="6.6", PREDICATE_REVISION=None),
            "BEHIND",
            {"MISSING", "RENUMBER"},
            "the 6.x renumbering hatch still holds",
        ),
    ]:
        got = classify(local, upstream)
        if got["status"] != status or set(got["reasons"]) != reasons:
            failures.append(
                f"classify: {why} -- want {status} {sorted(reasons)}, got {got['status']} {sorted(got['reasons'])}"
            )

    if verdict(False, True, verified=True)[:2] != (0, "BEHIND"):
        failures.append("missing constants on a verified read did not pass as BEHIND")

    # PROVENANCE. The copy must say where it came from, say it consistently, and BE that:
    # from the header's title line to the end, BYTE-IDENTICAL to the header at the stamped
    # sha. Above the title line is the copy's own -- stamp and notes, any length -- which is
    # how duck_block_utils' #38 reads it, so webbed's 23-line and sitting_duck's 5-line
    # preambles pass without special cases. No line-ending or trailing-newline tolerance.
    up_text = (
        "#pragma once\n"
        "\n"
        "// The duck_block vocabulary -- PUBLISHED INTERFACE.\n"
        "// upstream banner\n"
        'static constexpr const char *SPEC_VERSION = "1.4";\n'
        'static constexpr const char *TYPE_A = "a";\n'
    )
    full_sha = "95a84e6dbfb25d1925df2ad402f978c438a3f724"
    stamp = "// Vendored at upstream commit: 95a84e6 (SPEC_VERSION 1.4)  [duck_block_utils v3.3.0]\n"
    lines = up_text.splitlines(keepends=True)

    def vendored(stamp_line=stamp, block="// why it was re-vendored\n//\n", body=None):
        rest = lines[2:] if body is None else body
        return "".join(lines[:2]) + stamp_line + block + "".join(rest)

    if parse_stamp(vendored()) != {"sha": "95a84e6", "spec": "1.4", "tag": "v3.3.0"}:
        failures.append(f"parse_stamp misread a well-formed stamp: {parse_stamp(vendored())}")
    if parse_stamp(vendored(stamp_line="// Vendored at upstream commit: 95a84e6 (SPEC_VERSION 1.4)\n")) != {
        "sha": "95a84e6",
        "spec": "1.4",
        "tag": None,
    }:
        failures.append("parse_stamp refused a stamp without the optional release label")

    for local_text, at_sha, tag_sha, want_problem, why in [
        (vendored(), up_text, full_sha, False, "a faithful copy with its provenance block"),
        (
            vendored(stamp_line="// Vendored at upstream commit: 95a84e6 (SPEC_VERSION 1.4)\n"),
            up_text,
            None,
            False,
            "no release label, nothing to cross-check",
        ),
        (up_text, up_text, None, True, "stamp missing"),
        (
            vendored(stamp_line="// Vendored at upstream commit: main (SPEC_VERSION 1.4)\n"),
            up_text,
            None,
            True,
            "stamp malformed (not a sha)",
        ),
        (
            vendored(stamp_line="// Vendored at upstream commit: 95a84e6 (SPEC_VERSION 1.3)  [duck_block_utils v3.3.0]\n"),
            up_text,
            full_sha,
            True,
            "stamp claims a different SPEC_VERSION than the file",
        ),
        (vendored(), None, full_sha, True, "the header at the stamped sha could not be read"),
        (
            vendored(body=[l.replace('"a"', '"b"') for l in lines[2:]]),
            up_text,
            full_sha,
            True,
            "a code line differs from the header at the stamped sha",
        ),
        (
            vendored(block='// block\nstatic constexpr const char *EXTRA_X = "x";\n'),
            up_text,
            full_sha,
            False,
            "above the title line is the copy's own; a constant there is EXTRA's job, not provenance's",
        ),
        (
            vendored(body=[lines[2], "// a second, separate insertion\n"] + lines[3:]),
            up_text,
            full_sha,
            True,
            "an insertion below the title line is a local edit",
        ),
        (vendored(body=lines[3:]), up_text, full_sha, True, "the title line is gone, so nothing anchors the copy"),
        (vendored().replace("\n", "\r\n"), up_text, full_sha, True, "CRLF line endings are not the header"),
        (vendored().rstrip("\n"), up_text, full_sha, True, "a missing trailing newline is not the header"),
        (
            vendored(stamp_line="".join(f"// preamble {i}\n" for i in range(20)) + stamp),
            up_text,
            full_sha,
            False,
            "a long preamble with the stamp deep in it (webbed's is 23 lines)",
        ),
        (vendored(), up_text, "079123d0000000000000000000000000000000000", True, "the release label points elsewhere"),
    ]:
        got = provenance_problems(local_text, at_sha, tag_sha)
        if bool(got) != want_problem:
            failures.append(f"provenance_problems: {why} -- want {'a problem' if want_problem else 'none'}, got {got}")

    # The release to compare against, from a local clone's tags: the highest plain vX.Y.Z,
    # compared numerically (v3.10.0 > v3.9.0), never a pre-release.
    tags = ["v3.3.0", "v3.10.0", "v3.9.0", "v4.0.0-rc1", "nightly", "v2.0.0"]
    if pick_release_tag(tags) != "v3.10.0":
        failures.append(f"pick_release_tag({tags}) -- want v3.10.0, got {pick_release_tag(tags)}")
    if pick_release_tag(["nightly"]) is not None:
        failures.append("pick_release_tag invented a release from no release tags")

    return failures


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument(
        "--upstream",
        default=None,
        help="path to a local duck_block_utils clone " "(default: fetch the published header over HTTPS)",
    )
    ap.add_argument(
        "--ref",
        default=None,
        help="compare against this ref (tag, branch or sha) instead of the latest release",
    )
    ap.add_argument("--local", default=None, help="the vendored copy to check (default: panduck's)")
    ap.add_argument("--fetch", action="store_true", help="git fetch (with tags) first, only with --upstream")
    ap.add_argument(
        "--timeout",
        type=float,
        default=15.0,
        help="HTTPS timeout in seconds (default: 15)",
    )
    ap.add_argument(
        "--strict",
        action="store_true",
        help="treat an unreachable upstream as a failure rather than a skip",
    )
    ap.add_argument(
        "--self-test",
        action="store_true",
        help="verify the checker's own classification, then exit",
    )
    args = ap.parse_args()

    if args.self_test:
        return test_count_blindness()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    local_path = args.local or find_local(root)
    if not local_path or not os.path.exists(local_path):
        sys.exit("error: no duck_block_vocabulary.hpp found at " + (args.local or " or ".join(LOCAL_CANDIDATES)))
    local_text = open(local_path, encoding="utf-8").read()
    local = parse_constants(local_text)
    stamp = parse_stamp(local_text)
    kind = "upstream" if args.ref else "release"

    if args.upstream:
        repo = args.upstream
        if not os.path.exists(os.path.join(repo, ".git")):
            sys.exit(f"error: {repo} is not a git clone")
        if args.fetch:
            subprocess.run(["git", "-C", repo, "fetch", "--quiet", "--tags", "origin"], check=False)

        def resolve(ref):
            out = git_out(repo, "rev-parse", "--verify", "--quiet", f"{ref}^{{commit}}")
            return out.strip() if out else None

        label = args.ref or pick_release_tag((git_out(repo, "tag", "--list") or "").split())
        if not label:
            sys.exit(f"error: no release tag (vX.Y.Z) in {repo} -- run with --fetch, or pass --ref")
        sha = resolve(label)
        text = read_upstream_git(repo, sha) if sha else None
        if text is None:
            sys.exit(f"error: cannot read {HEADER_REL} at {label} in {repo}")
        at_stamp = read_upstream_git(repo, stamp["sha"]) if stamp else None
        label_sha = resolve(stamp["tag"]) if stamp and stamp["tag"] else None
        source = f"{repo} {label} @ {sha[:7]}"
    else:
        if args.ref:
            sha = resolve_commit(args.ref, args.timeout)
            target = (args.ref, sha) if sha else None
        else:
            target = resolve_release(args.timeout)
        text = fetch_header(target[1], args.timeout) if target else None
        if text is None:
            # Skipping loudly beats failing a build over a flaky network, but
            # --strict exists so CI can refuse to skip.
            what = f"ref {args.ref}" if args.ref else "latest release"
            msg = (
                f"SKIPPED: cannot reach upstream's {what}.\n"
                f"         The vendored copy was NOT verified against anything."
            )
            if args.strict:
                print(msg.replace("SKIPPED", "FAILED"))
                return 1
            print(msg)
            print("         Re-run with network, or --upstream <clone>, to check it.")
            return 0
        label, sha = target
        stamp_full = resolve_commit(stamp["sha"], args.timeout) if stamp else None
        at_stamp = fetch_header(stamp_full, args.timeout) if stamp_full else None
        label_sha = resolve_commit(stamp["tag"], args.timeout) if stamp and stamp["tag"] else None
        source = f"{UPSTREAM_REPO} {label} @ {sha[:7]}"

    upstream = parse_constants(text)
    if not upstream:
        sys.exit("error: parsed no constants from upstream -- has the header's " "shape changed?")
    problems = provenance_problems(local_text, at_stamp, label_sha)
    if stamp and stamp["tag"] and label_sha is None:
        problems.append(f"stamp labels {stamp['tag']}, which does not resolve upstream")

    print(f"local    {os.path.relpath(local_path, root)}  ({len(local)} constants)")
    print(f"{kind:<8} {source}  ({len(upstream)} constants)")
    print(f"spec     local {local.get('SPEC_VERSION', '?')}  {kind} {upstream.get('SPEC_VERSION', '?')}")
    if stamp:
        label_note = f"  [{stamp['tag']}]" if stamp["tag"] else ""
        print(f"stamp    {stamp['sha']} (SPEC_VERSION {stamp['spec']}){label_note}")
    print("         (counts are context, not the assertion -- a rename leaves them equal)")
    print()

    code, _ = report(local, upstream, root, verified=True, strict=args.strict, provenance=problems)
    return code


if __name__ == "__main__":
    sys.exit(main())
