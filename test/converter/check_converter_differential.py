#!/usr/bin/env python3
"""Differential: community duck_block_utils' Pandoc converter against panduck's copy.

A ONE-TIME CHECK FOR CONVERTER HANDOFF STEP 4, not a gate, and not in `make check`.
The handoff spec makes a differential between the two copies a precondition for
duck_block_utils removing its copy. This is that differential. Its window closes when
community duck_block_utils stops registering the converter: after that, there is
nothing to compare against. Results: docs/superpowers/plans/2026-10-30-converter-handoff-step4.md.

It needs `duckdb` (the CLI, same version as the panduck build) and `pandoc` on PATH,
and network access for `INSTALL duck_block_utils FROM community`. It loads both
extensions into one session, which works only while their names do not overlap: run
it against a panduck build from BEFORE the canonical names were registered (b762217).

Usage: check_converter_differential.py <repo> <panduck_ext> <outdir>"""

import json, subprocess, sys, pathlib

repo, ext, out = map(pathlib.Path, sys.argv[1:4])
corpus = out / "corpus"
FMT = {
    "md": "markdown",
    "rst": "rst",
    "org": "org",
    "textile": "textile",
    "wiki": "mediawiki",
    "tex": "latex",
    "docx": "docx",
    "odt": "odt",
    "epub": "epub",
    "html": "html",
    "rtf": "rtf",
    "ipynb": "ipynb",
}
cases = []
for p in sorted(repo.joinpath("test").rglob("*")):
    if p.suffix.lstrip(".") in FMT and p.is_file():
        name = str(p.relative_to(repo)).replace("/", "__") + ".json"
        r = subprocess.run(["pandoc", "-f", FMT[p.suffix[1:]], "-t", "json", str(p)], capture_output=True)
        if r.returncode == 0:
            (corpus / name).write_bytes(r.stdout)
            cases.append(corpus / name)
        else:
            print("pandoc could not read", p, r.stderr.decode()[:100].strip())
for p in repo.joinpath("test").rglob("*.pandoc.json"):
    cases.append(p)


# adversarial inputs
def nest(n):
    s = '{"t":"Para","c":[{"t":"Str","c":"x"}]}'
    for _ in range(n):
        s = '{"t":"BlockQuote","c":[' + s + ']}'
    return '{"pandoc-api-version":[1,23,1],"meta":{},"blocks":[' + s + ']}'


adv = {
    "depth_100": nest(100),
    "depth_200": nest(200),
    "depth_50000": nest(50000),
    "malformed": '{"pandoc-api-version":[1,23,1],"blocks":[',
    "not_object": "[1,2,3]",
    "no_blocks": '{"pandoc-api-version":[1,23,1],"meta":{}}',
    "empty": "",
    "old_api": '{"pandoc-api-version":[1,17],"meta":{},"blocks":[]}',
    "unknown_ctor": '{"pandoc-api-version":[1,23,1],"meta":{},"blocks":[{"t":"Frobnicate","c":[]}]}',
}
for k, v in adv.items():
    (corpus / f"adv__{k}.json").write_text(v)
    cases.append(corpus / f"adv__{k}.json")

PAIRS = [
    ("to_blocks", "pandoc_ast_to_blocks({j})", "panduck_pandoc_ast_to_blocks({j})"),
    (
        "roundtrip_blocks",
        "duck_blocks_to_pandoc_blocks(pandoc_ast_to_blocks({j}))",
        "panduck_blocks_to_pandoc_blocks(panduck_pandoc_ast_to_blocks({j}))",
    ),
    (
        "roundtrip_ast",
        "duck_blocks_to_pandoc_ast(pandoc_ast_to_blocks({j}))",
        "panduck_blocks_to_pandoc_ast(panduck_pandoc_ast_to_blocks({j}))",
    ),
]
PRE = f"INSTALL duck_block_utils FROM community; LOAD duck_block_utils; LOAD '{ext}'; "


def q(sql):
    r = subprocess.run(
        ["duckdb", "-unsigned", "-noheader", "-list", "-c", PRE + sql], capture_output=True, text=True, timeout=300
    )
    if r.returncode != 0:
        msg = r.stderr.strip().splitlines()
        first = next((l for l in msg if "Error" in l), msg[0] if msg else f"rc={r.returncode}")
        return ("ERR", first[:160])
    return ("OK", r.stdout)


rows = []
for c in cases:
    j = f"(SELECT content FROM read_text('{c}'))"
    for name, a, b in PAIRS:
        ra, rb = q("SELECT " + a.format(j=j) + "::VARCHAR"), q("SELECT " + b.format(j=j) + "::VARCHAR")
        same = ra == rb
        rows.append(
            {
                "case": c.name,
                "pair": name,
                "same": same,
                "dbu": ra[0] if ra[0] == "OK" else ra[1],
                "panduck": rb[0] if rb[0] == "OK" else rb[1],
                "dbu_len": len(ra[1]),
                "panduck_len": len(rb[1]),
            }
        )
        if not same and ra[0] == rb[0] == "OK":
            (out / "diffs").mkdir(exist_ok=True)
            (out / "diffs" / f"{c.name}.{name}.dbu").write_text(ra[1])
            (out / "diffs" / f"{c.name}.{name}.panduck").write_text(rb[1])
json.dump(rows, open(out / "results.json", "w"), indent=1)
tot = len(rows)
diff = [r for r in rows if not r["same"]]
print(f"{len(cases)} inputs, {tot} comparisons, {len(diff)} differ")
for r in diff:
    print(f"  {r['case']:<60} {r['pair']:<17} dbu={r['dbu'][:70]!s:<20} panduck={r['panduck'][:70]}")
