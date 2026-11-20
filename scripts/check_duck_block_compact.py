#!/usr/bin/env python3
"""Compare panduck's vendored compact-format header and golden corpus against
duck_block_utils at the PINNED commit.

panduck vendors src/include/duck_block_compact.hpp (the compact line format's code
tables and C++ codec) and test/fixtures/dblocks/ (the golden corpus) from
duck_block_utils. The two implementations of the format -- duck_block_utils' SQL
macros and this codec -- agree only while the tables and the corpus are the same, so
both are compared here.

PINNED TO A COMMIT, NOT A RELEASE. The vocabulary check compares against upstream's
latest release; no duck_block_utils release contains this header yet, so this one
compares against PIN_SHA. Teague's call, 2026-11-20. Move PIN_SHA when panduck takes a
newer copy, and switch to the release once one ships.

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
PIN_SHA = "8e54e995b2ce19304c59df51697b044df3d88544"
UPSTREAM_HEADER = "vendor/duck_block_compact.hpp"
UPSTREAM_CORPUS = "test/fixtures/compact/golden"
LOCAL_HEADER = "src/include/duck_block_compact.hpp"
LOCAL_CORPUS = "test/fixtures/dblocks"
RAW = "https://raw.githubusercontent.com/" + UPSTREAM_REPO + "/{sha}/{path}"
API_TREE = "https://api.github.com/repos/" + UPSTREAM_REPO + "/contents/{path}?ref={sha}"

ROOT = Path(__file__).resolve().parent.parent


def normalize(cpp: str) -> str:
    """Formatting-insensitive form of a C++ header: no whitespace, adjacent literals joined."""
    s = re.sub(r"\s+", "", cpp)
    return s.replace('""', "")


class Unreachable(Exception):
    pass


def fetch_https(path: str) -> bytes:
    try:
        with urllib.request.urlopen(RAW.format(sha=PIN_SHA, path=path), timeout=20) as r:
            return r.read()
    except Exception as e:  # noqa: BLE001 -- any failure to reach upstream is one outcome
        raise Unreachable(f"{path}: {e}") from e


def list_https(path: str):
    import json

    req = urllib.request.Request(API_TREE.format(sha=PIN_SHA, path=path))
    token = os.environ.get("GITHUB_TOKEN")
    if token:
        req.add_header("Authorization", f"Bearer {token}")
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return sorted(e["name"] for e in json.load(r) if e["type"] == "file")
    except Exception as e:  # noqa: BLE001
        raise Unreachable(f"{path}: {e}") from e


def git_source(repo: Path):
    def show(path: str) -> bytes:
        try:
            return subprocess.run(["git", "-C", str(repo), "show", f"{PIN_SHA}:{path}"],
                                  check=True, capture_output=True).stdout
        except subprocess.CalledProcessError as e:
            raise Unreachable(f"{path}: {e.stderr.decode().strip()}") from e

    def ls(path: str):
        try:
            out = subprocess.run(["git", "-C", str(repo), "ls-tree", "--name-only", f"{PIN_SHA}:{path}"],
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
    ap.add_argument("--upstream-git", type=Path, help="a local duck_block_utils clone holding PIN_SHA")
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

    show, ls = git_source(args.upstream_git) if args.upstream_git else (fetch_https, list_https)
    try:
        upstream_header = show(UPSTREAM_HEADER).decode()
        upstream_files = {n: show(f"{UPSTREAM_CORPUS}/{n}") for n in ls(UPSTREAM_CORPUS)}
    except Unreachable as e:
        msg = f"could not read upstream at {PIN_SHA[:10]}: {e}"
        if args.strict:
            print(f"FAIL: {msg}\n      --strict: an unreachable upstream is not a pass.")
            return 1
        print(f"SKIP: {msg}")
        return 0

    print(f"Comparing {LOCAL_HEADER} and {LOCAL_CORPUS}/ ({len(local_files)} files) "
          f"against {UPSTREAM_REPO}@{PIN_SHA[:10]}")
    problems = compare(local_header, upstream_header, local_files, upstream_files)
    for p in problems:
        print(f"  {p}")
    if problems:
        print(f"FAIL: {len(problems)} difference(s) from the pinned upstream")
        return 1
    print(f"OK: the vendored compact header and the {len(upstream_files)}-file corpus match upstream")
    return 0


if __name__ == "__main__":
    sys.exit(main())
