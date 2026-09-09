#!/usr/bin/env python3
"""Pre-parsed duck_block fixtures, and the drift check that guards them.

WHAT THIS IS FOR
================

panduck's readers for markdown, html and pdf are not panduck's code -- they dispatch to
the `markdown`, `webbed` and `pdf` community extensions. Those move independently, and
when they move, panduck's OUTPUT changes with no commit in this repo. That happened
during development: duck_block_utils went spec 6.3 -> 6.5 and markdown 340c0cd ->
75e9d0b in the registry, and extraction behaviour changed with them. Nothing announced
it.

So this stores what each of those readers produced, as blocks, labelled with the
versions that produced them. Two jobs come out of one artifact:

  1. TEST INPUT THAT NEEDS NO EXTENSION. A stored fixture is already parsed, so a test
     reading it needs no reader at all. This is what makes pdf-derived and html-derived
     behaviour assertable on a clean runner -- the constraint that forced doc_search's
     fixtures to .textile, which is the only NATIVE format that emits heading id slugs.
     A pre-parsed fixture has no such constraint because there is nothing left to parse.

  2. DRIFT DETECTION. `--check` re-parses each source with whatever extensions are
     installed now and compares against the stored blocks. It answers one question:
     WOULD TODAY'S EXTENSIONS STILL PRODUCE THIS FIXTURE? A failure is not a bug in
     panduck; it is notice that an upstream reader changed under it.

WHY THE COMPARISON IS STRUCTURAL
================================

The blocks are compared as ROWS, not as serialized text. Two runs can produce identical
blocks with different JSON key order or whitespace, and a text diff would fail on that
and teach everyone to regenerate reflexively -- which destroys the signal the check
exists to give. DuckDB's EXCEPT operates across the MAP(VARCHAR,VARCHAR) attributes
column directly, so the comparison is exact without serializing anything. Measured: a
single perturbed nested attribute value is caught and localised to the block and field.

THE FAILURE MODE THIS IS DESIGNED AGAINST
=========================================

A red drift job that people regenerate without reading is WORSE than no job, because it
launders an upstream behaviour change into a green build under a commit message saying
"regenerate fixtures". So `--regen` prints the structural diff it is about to bake in
and writes it to a file intended for the commit message. The diff is the record of what
upstream changed; a regeneration without one is a regeneration nobody read.
"""

import argparse
import csv
import hashlib
import json
import pathlib
import subprocess
import sys
from datetime import datetime, timezone

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT_DIR = ROOT / "test" / "fixtures" / "parsed"
MANIFEST = OUT_DIR / "manifest.csv"

# name, source, the extension whose reader produces it.
#
# ONE FIXTURE PER READER IS NOT ENOUGH. Each reader gets several sources chosen to
# exercise different constructors, because drift usually shows in one construct (a list
# item becoming a paragraph, a heading level shifting) and a single simple document can
# miss it entirely.
FIXTURES = [
    ("constructs_md", "test/fixtures/constructs.source.md", "markdown"),
    ("lists_md", "test/fixtures/lists.source.md", "markdown"),
    ("rich_md", "test/fixtures/rich.source.md", "markdown"),
    ("two_pages_md", "test/fixtures/two_pages.source.md", "markdown"),
    ("constructs_html", "test/fixtures/constructs.html", "webbed"),
    ("containers_html", "test/fixtures/containers.html", "webbed"),
    ("sections_html", "test/fixtures/sections.html", "webbed"),
    ("two_pages_pdf", "test/fixtures/two_pages.pdf", "pdf"),
]

BLOCK_COLS = ["kind", "element_type", "content", "level", "encoding", "attributes", "element_order"]


def sha256_of(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


class Duck:
    """A DuckDB CLI wrapper. Uses the CLI rather than the python module on purpose: the
    module ships its own DuckDB, which is not the one the extension was built against,
    and loading a mismatched extension fails in a way that reads like a fixture bug."""

    def __init__(self, binary, extension):
        self.binary = binary
        self.extension = extension

    def sql(self, body, json_out=True):
        prelude = "INSTALL markdown FROM community; INSTALL webbed FROM community; " \
                  "INSTALL pdf FROM community; LOAD markdown; LOAD webbed; LOAD pdf;\n"
        if self.extension:
            prelude = f"LOAD '{self.extension}';\n" + prelude
        args = [self.binary, "-unsigned"]
        if json_out:
            args += ["-json"]
        args += ["-c", prelude + body]
        r = subprocess.run(args, capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(f"duckdb failed:\n{r.stderr.strip()}\n--- sql ---\n{body}")
        if not json_out:
            return r.stdout
        out = r.stdout.strip()
        return json.loads(out) if out else []


def collect_versions(duck):
    rows = duck.sql(
        "SELECT extension_name AS n, coalesce(extension_version,'?') AS v FROM duckdb_extensions() "
        "WHERE extension_name IN ('markdown','webbed','pdf','duck_block_utils');")
    versions = {r["n"]: r["v"] for r in rows}
    versions["duckdb"] = duck.sql("SELECT version() AS v;")[0]["v"]
    # duck_block_spec_version() is CONTEXT, not cause: panduck's readers emit blocks
    # without duck_block_utils loaded. It is recorded because a vocabulary change is the
    # thing most likely to explain a diff a reader version alone does not.
    try:
        versions["duck_block_spec"] = duck.sql(
            "LOAD duck_block_utils; SELECT duck_block_spec_version() AS v;")[0]["v"]
    except Exception:
        versions["duck_block_spec"] = "unavailable"
    return versions


def block_count(duck, source):
    """Count only. Selecting the blocks themselves through the CLI's -json mode is not an
    option: a MAP(VARCHAR,VARCHAR) column does not render as valid JSON there, which is
    also why every comparison in this file happens INSIDE DuckDB rather than in Python."""
    return int(duck.sql(
        f"SELECT count(*) AS n FROM read_panduck_doc('{source}');")[0]["n"])


def write_fixture(duck, name, source):
    out = OUT_DIR / f"{name}.blocks.parquet"
    duck.sql(
        f"COPY (SELECT {', '.join(BLOCK_COLS)} FROM read_panduck_doc('{source}') "
        f"ORDER BY element_order) TO '{out}' (FORMAT parquet);", json_out=False)
    return out


def diff_fixture(duck, name, source):
    """Structural diff of a re-parse against the stored blocks.

    Returns a list of human-readable difference lines, empty when they agree. The
    comparison runs inside DuckDB so the MAP column is compared as a MAP rather than as
    text -- see the module docstring."""
    stored = OUT_DIR / f"{name}.blocks.parquet"
    if not stored.exists():
        return [f"{name}: NO STORED FIXTURE at {stored.relative_to(ROOT)} -- run --regen"]

    cols = ", ".join(BLOCK_COLS)
    rows = duck.sql(f"""
        CREATE TEMP TABLE live AS
            SELECT {cols} FROM read_panduck_doc('{source}') ORDER BY element_order;
        CREATE TEMP TABLE stored AS SELECT {cols} FROM '{stored}';

        SELECT 'count' AS kind_of_diff,
               NULL::INTEGER AS element_order, NULL AS field,
               (SELECT count(*) FROM stored)::VARCHAR AS stored_value,
               (SELECT count(*) FROM live)::VARCHAR AS live_value
        WHERE (SELECT count(*) FROM stored) <> (SELECT count(*) FROM live)

        UNION ALL
        -- FIELD-LEVEL, joined on element_order, so the report names WHICH block and
        -- WHICH column moved rather than only that something did.
        SELECT 'field', s.element_order, f.field, f.sv, f.lv
        FROM stored s JOIN live l USING (element_order),
             LATERAL (VALUES ('kind', s.kind, l.kind),
                             ('element_type', s.element_type, l.element_type),
                             ('content', s.content, l.content),
                             ('level', s.level::VARCHAR, l.level::VARCHAR),
                             ('encoding', s.encoding, l.encoding),
                             ('attributes', s.attributes::VARCHAR, l.attributes::VARCHAR))
             AS f(field, sv, lv)
        WHERE f.sv IS DISTINCT FROM f.lv

        UNION ALL
        -- Blocks present on only one side. A pure count diff would miss a reader that
        -- both added and dropped a block.
        SELECT 'only_in_live', element_order, element_type, NULL, content
        FROM (SELECT element_order, element_type, content FROM live
              EXCEPT SELECT element_order, element_type, content FROM stored)

        UNION ALL
        SELECT 'only_in_stored', element_order, element_type, content, NULL
        FROM (SELECT element_order, element_type, content FROM stored
              EXCEPT SELECT element_order, element_type, content FROM live)

        ORDER BY 1, 2, 3;
    """)

    def trim(v, n=60):
        if v is None:
            return "NULL"
        v = str(v).replace("\n", "\\n")
        return v if len(v) <= n else v[: n - 1] + "…"

    lines = []
    for r in rows:
        k = r["kind_of_diff"]
        if k == "count":
            lines.append(f"{name}: BLOCK COUNT {r['stored_value']} -> {r['live_value']}")
        elif k == "field":
            lines.append(
                f"{name}: block {r['element_order']} field '{r['field']}': "
                f"stored={trim(r['stored_value'])} live={trim(r['live_value'])}")
        elif k == "only_in_live":
            lines.append(f"{name}: block {r['element_order']} ONLY IN LIVE "
                         f"({r['field']}) {trim(r['live_value'])}")
        else:
            lines.append(f"{name}: block {r['element_order']} ONLY IN STORED "
                         f"({r['field']}) {trim(r['stored_value'])}")
    return lines


def read_manifest():
    if not MANIFEST.exists():
        return {}
    with MANIFEST.open() as fh:
        return {r["fixture"]: r for r in csv.DictReader(fh)}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--duckdb", default=str(ROOT / "build" / "release" / "duckdb"))
    ap.add_argument("--extension", default=None,
                    help="path to panduck.duckdb_extension; omit if the binary links it")
    ap.add_argument("--regen", action="store_true", help="regenerate fixtures and manifest")
    ap.add_argument("--check", action="store_true", help="drift check: re-parse and diff")
    ap.add_argument("--diff-out", default=None,
                    help="write the structural diff here (for the regeneration commit message)")
    args = ap.parse_args()

    if not (args.regen or args.check):
        ap.error("one of --regen or --check is required")

    duck = Duck(args.duckdb, args.extension)
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    # FAIL, DO NOT SKIP, when a reader is missing. A skip here would report coverage the
    # run is not providing -- the same reasoning as check_roundtrip.py's --require.
    versions = collect_versions(duck)
    missing = [e for e in ("markdown", "webbed", "pdf") if e not in versions]
    if missing:
        print(f"FAIL: required extensions not installed: {', '.join(missing)}", file=sys.stderr)
        print("      This job must install them; skipping would report coverage it lacks.",
              file=sys.stderr)
        return 2

    print("readers in this environment:")
    for k in sorted(versions):
        print(f"  {k:<18} {versions[k]}")
    print()

    if args.check:
        prior = read_manifest()
        all_diffs, stale_labels = [], []
        for name, source, ext in FIXTURES:
            all_diffs += diff_fixture(duck, name, source)
            row = prior.get(name)
            if row:
                if row.get("source_sha256") != sha256_of(ROOT / source):
                    all_diffs.append(f"{name}: SOURCE DOCUMENT CHANGED -- the fixture is no "
                                     f"longer of this input; regenerate deliberately")
                if row.get("reader_version") != versions.get(ext, "?"):
                    stale_labels.append(
                        f"  {name}: {ext} {row.get('reader_version')} -> {versions.get(ext)}")

        # A VERSION MOVE WITH NO DIFF IS THE GOOD NEWS CASE, and it is reported rather
        # than silent: it is positive evidence that the upgrade did not change output.
        if stale_labels:
            print("reader versions have moved since these fixtures were generated:")
            print("\n".join(stale_labels))
            print()

        if all_diffs:
            print("DRIFT: today's readers would not reproduce these fixtures.\n")
            print("\n".join(all_diffs))
            print(f"\n{len(all_diffs)} difference(s).")
            print("\nThis is NOT a panduck bug -- it is an upstream reader changing under it.")
            print("Read the diff above, decide whether the new output is correct, then:")
            print("    make regen-parsed-fixtures")
            print("and put the diff in the commit message. A regeneration whose commit does")
            print("not say what changed has laundered an upstream change into a green build.")
            if args.diff_out:
                pathlib.Path(args.diff_out).write_text("\n".join(all_diffs) + "\n")
            return 1

        if stale_labels:
            print("OK: readers moved but output is byte-identical. Refresh the labels with")
            print("    make regen-parsed-fixtures")
            print("    (no block changes; the manifest is what goes stale.)")
        else:
            print("OK: today's readers reproduce every stored fixture exactly.")
        return 0

    # --regen: show what is about to change BEFORE overwriting, so the diff can go in the
    # commit message rather than being destroyed by the thing that produced it.
    pending = []
    for name, source, _ in FIXTURES:
        pending += diff_fixture(duck, name, source)
    if pending:
        print("These fixtures are about to change:\n")
        print("\n".join(pending))
        print()
        if args.diff_out:
            pathlib.Path(args.diff_out).write_text("\n".join(pending) + "\n")
            print(f"(written to {args.diff_out} -- put it in the commit message)\n")
    else:
        print("No block changes; refreshing labels only.\n")

    rows = []
    for name, source, ext in FIXTURES:
        out = write_fixture(duck, name, source)
        n = block_count(duck, source)
        rows.append({
            "fixture": name,
            "source": source,
            "source_sha256": sha256_of(ROOT / source),
            "reader_extension": ext,
            "reader_version": versions.get(ext, "?"),
            "duck_block_spec_version": versions["duck_block_spec"],
            "duckdb_version": versions["duckdb"],
            "block_count": n,
            "generated_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        })
        print(f"  wrote {out.relative_to(ROOT)}  ({n} blocks, {ext} {versions.get(ext)})")

    with MANIFEST.open("w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)
    print(f"\nwrote {MANIFEST.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
