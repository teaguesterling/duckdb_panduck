#!/usr/bin/env python3
"""Compare panduck's vendored compact-format header and golden corpus against
duck_block_utils' LATEST RELEASE.

panduck vendors src/include/duck_block_compact.hpp (the compact line format's code
tables and C++ codec) and test/fixtures/dblocks/ (the golden corpus) from
duck_block_utils. The two implementations of the format -- duck_block_utils' SQL
macros and this codec -- agree only while the tables and the corpus are the same, so
both are compared here.

RELEASE-BASED, like the vocabulary check: the contract with upstream is its published
releases, so this compares against the latest one (resolved and PRINTED), at that
release's commit. It was pinned to a main commit until v4.2.0 shipped the header
(2026-11-20). A newer release that changes the tables or the corpus turns this red until
panduck re-vendors -- which is the point. --ref compares against a given tag or commit
instead.

THE HEADER IS COMPARED IGNORING FORMATTING. panduck's clang-format re-wraps vendored
files (its copy of duck_block_normalize.hpp differs from upstream only in wrapping), so
whitespace is removed and adjacent string literals are joined before comparing. A
changed code, key or byte of SQL is still a difference. The corpus is compared byte for
byte: it is data, and nothing reformats it.

    python3 scripts/check_duck_block_compact.py                 # network, or --upstream-git
    python3 scripts/check_duck_block_compact.py --strict        # an unreachable upstream fails
    python3 scripts/check_duck_block_compact.py --upstream-git ../duckdb_duck_block_utils
    python3 scripts/check_duck_block_compact.py --self-test
"""
import argparse
import os
import re
import subprocess
import sys
import urllib.request
from pathlib import Path

UPSTREAM_REPO = "teaguesterling/duckdb_duck_block_utils"
UPSTREAM_HEADER = "vendor/duck_block_compact.hpp"
UPSTREAM_CORPUS = "test/fixtures/compact/golden"
LOCAL_HEADER = "src/include/duck_block_compact.hpp"
LOCAL_CORPUS = "test/fixtures/dblocks"
RAW = "https://raw.githubusercontent.com/" + UPSTREAM_REPO + "/{sha}/{path}"
API_TREE = "https://api.github.com/repos/" + UPSTREAM_REPO + "/contents/{path}?ref={sha}"
API = "https://api.github.com/repos/" + UPSTREAM_REPO

ROOT = Path(__file__).resolve().parent.parent


def normalize(cpp: str) -> str:
    """Formatting-insensitive form of a C++ header: no whitespace, adjacent literals joined."""
    s = re.sub(r"\s+", "", cpp)
    return s.replace('""', "")


class Unreachable(Exception):
    pass


def api_json(url: str):
    import json

    req = urllib.request.Request(url)
    token = os.environ.get("GITHUB_TOKEN")
    if token:
        req.add_header("Authorization", f"Bearer {token}")
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return json.load(r)
    except Exception as e:  # noqa: BLE001
        raise Unreachable(f"{url}: {e}") from e


def resolve_https(ref):
    """(label, sha): the given ref, or upstream's latest published release."""
    if ref is None:
        ref = api_json(API + "/releases/latest")["tag_name"]
    return ref, api_json(f"{API}/commits/{ref}")["sha"]


def resolve_git(repo: Path, ref):
    """(label, sha) from a local clone: the given ref, or its newest v* tag."""
    try:
        if ref is None:
            tags = subprocess.run(["git", "-C", str(repo), "tag", "--list", "v*", "--sort=-v:refname"],
                                  check=True, capture_output=True, text=True).stdout.split()
            if not tags:
                raise Unreachable("no v* tags in the clone")
            ref = tags[0]
        sha = subprocess.run(["git", "-C", str(repo), "rev-parse", f"{ref}^{{commit}}"],
                             check=True, capture_output=True, text=True).stdout.strip()
    except subprocess.CalledProcessError as e:
        raise Unreachable(f"{ref}: {e.stderr.strip()}") from e
    return ref, sha


def https_source(sha: str):
    def show(path: str) -> bytes:
        return fetch_https(sha, path)

    def ls(path: str):
        return list_https(sha, path)

    return show, ls


def fetch_https(sha: str, path: str) -> bytes:
    try:
        with urllib.request.urlopen(RAW.format(sha=sha, path=path), timeout=20) as r:
            return r.read()
    except Exception as e:  # noqa: BLE001 -- any failure to reach upstream is one outcome
        raise Unreachable(f"{path}: {e}") from e


def list_https(sha: str, path: str):
    return sorted(e["name"] for e in api_json(API_TREE.format(sha=sha, path=path)) if e["type"] == "file")


def git_source(repo: Path, sha: str):
    def show(path: str) -> bytes:
        try:
            return subprocess.run(["git", "-C", str(repo), "show", f"{sha}:{path}"],
                                  check=True, capture_output=True).stdout
        except subprocess.CalledProcessError as e:
            raise Unreachable(f"{path}: {e.stderr.decode().strip()}") from e

    def ls(path: str):
        try:
            out = subprocess.run(["git", "-C", str(repo), "ls-tree", "--name-only", f"{sha}:{path}"],
                                 check=True, capture_output=True, text=True).stdout
        except subprocess.CalledProcessError as e:
            raise Unreachable(f"{path}: {e.stderr.strip()}") from e
        return sorted(n for n in out.splitlines() if n)

    return show, ls


def compare(local_header: str, upstream_header: str, local_files: dict, upstream_files: dict):
    """Return a list of problems; empty means in step."""
    problems = []
    if normalize(local_header) != normalize(upstream_header):
        a, b = normalize(local_header), normalize(upstream_header)
        i = next((k for k in range(min(len(a), len(b))) if a[k] != b[k]), min(len(a), len(b)))
        problems.append(f"header differs from upstream at normalized offset {i}: "
                        f"local ...{a[max(0, i - 40):i + 40]}... upstream ...{b[max(0, i - 40):i + 40]}...")
    for name in sorted(set(local_files) | set(upstream_files)):
        if name not in local_files:
            problems.append(f"corpus file missing here: {name}")
        elif name not in upstream_files:
            problems.append(f"corpus file not upstream: {name}")
        elif local_files[name] != upstream_files[name]:
            problems.append(f"corpus file differs byte for byte: {name}")
    return problems


def self_test() -> int:
    hdr = 'static constexpr TypeCode TYPE_CODES[] = {\n    {"block", "heading", "H"},\n};\nconst char *s = "ab" "cd";\n'
    files = {"a.dblocks": b'{"@":"4.2.0"}\n'}
    cases = [
        ("identical", hdr, hdr, files, files, False),
        ("re-wrapped and literal split", hdr.replace("\n    ", "\n\t\t").replace('"ab" "cd"', '"a"\n  "bcd"'), hdr,
         files, files, False),
        ("a changed code", hdr.replace('"H"', '"h"'), hdr, files, files, True),
        ("a changed corpus byte", hdr, hdr, {"a.dblocks": b'{"@":"4.2.1"}\n'}, files, True),
        ("a missing corpus file", hdr, hdr, {}, files, True),
    ]
    bad = 0
    for label, lh, uh, lf, uf, should_fail in cases:
        failed = bool(compare(lh, uh, lf, uf))
        ok = failed == should_fail
        bad += not ok
        print(f"  {'ok ' if ok else 'BAD'} {label}: {'drift' if failed else 'in step'}")
    if bad:
        print(f"FAIL: self-test: {bad} case(s) misclassified")
        return 1
    print("OK: self-test classified every case")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--strict", action="store_true", help="an unreachable upstream is a failure")
    ap.add_argument("--upstream-git", type=Path, help="a local duck_block_utils clone")
    ap.add_argument("--ref", help="compare against this tag or commit instead of the latest release")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        return self_test()

    local_header_path = ROOT / LOCAL_HEADER
    if not local_header_path.exists():
        print(f"FAIL: {LOCAL_HEADER} is missing")
        return 1
    local_header = local_header_path.read_text()
    local_dir = ROOT / LOCAL_CORPUS
    local_files = {p.name: p.read_bytes() for p in sorted(local_dir.iterdir()) if p.is_file()} if local_dir.is_dir() else {}

    try:
        if args.upstream_git:
            label, sha = resolve_git(args.upstream_git, args.ref)
            show, ls = git_source(args.upstream_git, sha)
        else:
            label, sha = resolve_https(args.ref)
            show, ls = https_source(sha)
        upstream_header = show(UPSTREAM_HEADER).decode()
        upstream_files = {n: show(f"{UPSTREAM_CORPUS}/{n}") for n in ls(UPSTREAM_CORPUS)}
    except Unreachable as e:
        msg = f"could not read upstream ({args.ref or 'latest release'}): {e}"
        if args.strict:
            print(f"FAIL: {msg}\n      --strict: an unreachable upstream is not a pass.")
            return 1
        print(f"SKIP: {msg}")
        return 0

    print(f"Comparing {LOCAL_HEADER} and {LOCAL_CORPUS}/ ({len(local_files)} files) "
          f"against {UPSTREAM_REPO} {label} ({sha[:10]})")
    problems = compare(local_header, upstream_header, local_files, upstream_files)
    for p in problems:
        print(f"  {p}")
    if problems:
        print(f"FAIL: {len(problems)} difference(s) from upstream {label}")
        return 1
    print(f"OK: the vendored compact header and the {len(upstream_files)}-file corpus match upstream")
    return 0


if __name__ == "__main__":
    sys.exit(main())
