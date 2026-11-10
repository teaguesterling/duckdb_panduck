# panduck serves duckeye's four asks, and libpanduck continues

## Summary

**Situation.** This repo, `teaguesterling/duckdb_panduck`, is the DuckDB extension that
reads eleven document formats into the duck_block vocabulary — the shared row shape that
duck_block_utils defines. duckeye is a separate repo, `teaguesterling/duckeye`: a terminal
document reader that is one bash script, and it already calls panduck for most formats.
Teague ruled on 2026-11-10 that **panduck is duckeye's SQL core**. The reason is a planned
Android app that runs duckdb-wasm, which cannot run bash. duckeye's logic must therefore
exist once, in SQL, so the bash CLI and the app run the same code. Separately, **libpanduck**
(issue #104) is the in-tree extraction of panduck's format engine, so the readers build with
no DuckDB on the include path.

**Task.** You own two tracks in this repo. Track A serves four asks that duckeye filed
against panduck. Track B continues libpanduck, issue #104. Teague assigned both to one
session on 2026-11-10 and said you can do the asks "and also continue writing libpanduck".
Track A is the one another session is waiting on.

**Action so far.** The duckeye session measured panduck 0.7.0 against duckeye's own SQL. It
found output equality for the two narrow operations, three behaviour deviations, and one
performance gap. A second session then measured panduck's Pandoc-AST entry point and found
it takes the AST as a `VARCHAR` value rather than a path. All the numbers are below.

**Result right now.** Nothing is built in panduck for track A, and the four asks are **not
filed as issues** — they exist only in duckeye's brief. libpanduck has 57 tracked files on
`main`, and its boundary is enforced by three separate mechanisms. This worktree is new, off
`origin/main` at `ac39019`.

## Status

**Approved direction, not specced.** Teague approved "panduck is the core" on 2026-11-10.
No panduck issue, spec or implementation exists for track A.

**Run `superpowers:brainstorming` first, and say which path you classified it as.** Track A
is a change to code that already exists here (`doc_section`, `doc_search_sections` in
`src/reader_registry.cpp`), so it may classify as bounded per ask. Ask 1's placement question
below may be architectural. Classify each ask on its own and say so.

## Your prompt

Read duckeye's brief first: `~/Projects/duckeye/docs/briefs/2026-11-10-panduck-as-duckeye-core.md`.
It holds the full text of the four asks, which this brief only summarises. Then read
`~/Projects/duckeye/docs/dev/sql-core.md` for `duckeye_like()` and the three deviations it
names D1 to D3.

Serve these four asks, each as its own PR:

1. **A glob match mode.** Add `match := 'glob'` to `doc_section` and `doc_search_sections`.
   `*` matches any run, `?` matches one character. `%`, `_` and `\` stay literal, and the
   match is case-insensitive and substring. duckeye's definition is `duckeye_like()`. Take
   it, so there is one copy. This also fixes three deviations that exist in today's
   `contains` mode: panduck does not escape `_`, so `-s a_b` also matches the prose
   "A backslash"; a pattern with a backslash does not match a literal heading.
   duckeye's `scripts/panduck-deviations.sh` measures all three, 10 cases, and exits 0 when
   they are gone.
2. **Variants that take a `blocks` list, not a path.** For example
   `doc_section_blocks(blocks, section, match)`, called as
   `FROM doc_section_blocks((SELECT blocks FROM d), 'x', match := 'contains')`. **Use a
   table macro, not a scalar** — see the 6.1 GiB measurement below. This is the ask the
   duckeye session is most blocked on, because path-only narrow functions break four routes
   duckeye already ships: a zim entry, stdin, a Pandoc AST, and a code AST.
3. **Make `doc_search_sections` linear.** It is 5.6x slower than duckeye on the same file
   with the same output. duckeye's `search_span_doc` carries each row's span forward with a
   window, `last_value(... IGNORE NULLS)`, not with a join. Its comments record that DuckDB
   planned the ASOF form as a nested loop, 288 million joined rows. Port that form, or beat it.
4. **Agree with duckeye on the slug id and on a miss.** duckeye's `-S` also matches a heading
   whose id equals the escaped pattern. Measure whether `doc_section` does the same. A panduck
   miss returns NULL today; duckeye expects zero rows or a NULL it can test. Either is fine,
   but write it down. **This overlaps open issue #85 part 2**, which records that
   `doc_render(..., 'html')` omits heading ids, and notes that duckeye's section search matches
   on id or title. Settle them together or say why not.

Then continue track B, libpanduck (#104), at whatever the next step is by that issue and
`docs/libpanduck.md`. Related open issues: #129 (five modules behind the seam compile
standalone but assert nothing there), #107, #140.

File each ask as an issue in this repo before you implement it, so the work is addressable
from duckeye.

## What is already measured — carry these, do not rediscover them

Measured 2026-11-10 on longbottom, DuckDB v2.0.0, panduck 0.7.0, comparing
`duck_blocks_to_text` output against duckeye's own path:

| duckeye | panduck | Output | 72,000-row file |
|---|---|---|---|
| `-S` | `doc_section(..., match := 'contains')` | identical: 4/4 patterns, and `-S 'Section 7'` (md5 `e40f58a9`) | 1.45 s (duckeye 2.21 s) |
| `-s` | `doc_search_sections` | identical: 3/3 patterns, and `-s b7777` (md5 `0a0bf963`) | **13.16 s** (duckeye 2.35 s) |

- **A scalar macro does not survive scale.** A scalar macro over a 72,000-block list ran out
  of memory at 6.1 GiB. A table macro with the same body matched duckeye in time and output.
  This is why ask 2 names a table macro.
- **duckdb-wasm loads panduck.** On DuckDB v2.0.0, `LOAD panduck` works (v0.7.0, 79
  functions). So does `LOAD` for duck_block_utils, markdown, webbed, zim and sitting_duck.
  `pdf` and `duck_tails` have no wasm build.
- **The Pandoc-AST door is a value, not a path.** Measured in a second session on the same
  day: `panduck_pandoc_ast_to_blocks` has signature
  `[VARCHAR] -> STRUCT(kind, element_type, content, level, encoding, attributes, element_order)[]`.
  It is a scalar function over the AST **text**. A whole conversion is one SQL expression
  with no host I/O:
  `SELECT duck_blocks_to_md(panduck_pandoc_ast_to_blocks('<ast json>'));` returns
  `# Page 1\n\nhello core\n\n`. So any parser that writes Pandoc AST JSON enters panduck
  without a file, a path or stdin, which matters because duckdb-wasm has no stdin.
- **DuckDB binds a macro eagerly.** `CREATE MACRO m(x) AS duck_blocks_to_md(x)` raises
  `Catalog Error: Scalar Function with name duck_blocks_to_md does not exist!` unless the
  `markdown` extension is loaded first. So a macro group cannot name a function from an
  extension the host has not loaded. Note `duck_blocks_to_md` lives in `markdown`, not in
  duck_block_utils.
- **duckeye's own baseline**, for the parity limit below: `test.sh` on duckeye `main` gives
  384 passed, 0 failed, 2 skipped, 1 known-broken upstream. The known-broken case is the rst
  footnote, which is this repo's issue #85.

Repo state, measured by the session that wrote this brief, 2026-11-10:

- `origin/main` is `ac39019` (PR #161), 2026-11-02. This worktree is off it, on branch
  `feat/duckeye-core-asks`.
- **Do not use the primary worktree `~/Projects/duckdb_panduck` for this work.** It is
  checked out on `fix/function-descriptions` (`c074986`, 2026-09-20), and its local `main` is
  **181 commits behind `origin/main`**. libpanduck does not exist in that tree at all.
- `libpanduck/` here holds `CMakeLists.txt`, `include`, `src` and `test`, 57 tracked files.

## Limits agreed up front

- **duckeye's CLI output must stay byte-identical** when it switches to these functions.
  duckeye checks that itself with `scripts/parity.sh BASE NEW`, 206 cases. You do not run it;
  the duckeye session does. Do not design in a change that breaks it.
- **Keep every function loadable in duckdb-wasm.** No new dependency without a wasm build.
  This is the whole point of the core, so it is not negotiable.
- **Nothing under `libpanduck/` may include a DuckDB header or name a DuckDB symbol.** Three
  mechanisms enforce this and each has caught a defect the others could not see:
  `scripts/check_libpanduck_seam.py`, the standalone `cmake -S libpanduck` project at C++11,
  and `libpanduck/test/` via `ctest`. A table macro cannot live behind the seam, so ask 2 is
  extension-layer work by construction.
- **Do not change duckeye.** It is a different repo and another session holds it. If an ask
  needs a duckeye change, say so in your report instead of making it.
- **Do not push to `main` or any shared branch, and do not merge, without Teague's explicit
  ok.** Open PRs and leave them for him.

## The first question to settle

**Where does glob matching live?** Ask 1 is pure string logic, and `libpanduck/include/panduck/slugify.hpp`
shows that this kind of code already sits behind the seam. So ask 1 could be C++ behind the
seam, reused by the SQL layer, or it could be SQL in the extension layer beside `doc_section`.

This forks both tracks, because it is the first place they touch. Behind the seam, the
matching becomes testable with no DuckDB and is reusable by any libpanduck consumer, but it
is C++ and it has to cross the boundary to reach a macro's `match :=` parameter. In the SQL
layer it is a direct port of `duckeye_like()` and lands faster, but then a libpanduck
consumer has no glob matching.

Settle this before writing ask 1. Ask 2, 3 and 4 do not depend on it.

## Which tool for each step

| Step | Use | Not |
|---|---|---|
| Find code, definitions, callers | `mcp__plugin_squackit_squackit__search_code`, `..._find`, `..._read_source` | `grep -r`, `find`, `rg` through Bash |
| Read a document or a source file | `duckeye` (`-T` for an outline, `-S` for one section) | `cat`, `head`, `sed -n` |
| Commit | `mcp__plugin_jetsam_jetsam__save(message, files=[...])`, then `confirm(id)` | `git add && git commit` through Bash |
| Push, PR, CI, issues | jetsam `sync`, `ship`, `checks`, `issues`, `pr_view` | raw `git`/`gh` through Bash |
| Build and test | `mcp__blq_mcp__run` / `mcp__blq_mcp__exec`, then `mcp__blq_mcp__output(run_id, grep=...)` | `make`/`ctest` through Bash with pipes |
| Anything heavy or long | load the `slurm` skill, then `srun -n 1 -c 8 --mem=16G -p batch -t 60` | a bare build on longbottom |
| Write an explanation for Teague | the `house-artifact` skill, then the `Artifact` tool | a long chat reply |

Deferred MCP tools load by schema first: `ToolSearch` with
`select:mcp__plugin_jetsam_jetsam__save,mcp__plugin_jetsam_jetsam__confirm` and the same form
for the squackit and blq names above. Load a tool's schema before you call it.

A C++ build is the one step with no better tool than blq, and blq is the right one because it
captures the output for query. If a named tool fails or returns incomplete output, use
whatever works for that step and record the substitution in your report.

## Report contract

For each of the four asks: the issue number you filed, the PR, the panduck version that
serves it, and the measurement that shows it — output equality against duckeye, and the time
on the 72,000-row file that duckeye's `test.sh:1464-1466` generates. For ask 1, also state
where you put the matching and why. For ask 4, state whether you settled #85 part 2 with it.

For libpanduck: which modules moved, the `libpanduck-seam` job's status, and the standalone
check count by configure.

Also report every step where you left a tool this brief named, and the reason.

Say which of "measured" and "inferred" applies to every claim. Absence of a failure is not a
pass.

## Links out

- duckeye's asks, in full: `~/Projects/duckeye/docs/briefs/2026-11-10-panduck-as-duckeye-core.md`
- duckeye's core reference, with `duckeye_like()` and deviations D1-D3: `~/Projects/duckeye/docs/dev/sql-core.md`
- duckeye's SQL-core design: `~/Projects/duckeye/docs/superpowers/specs/2026-11-10-duckeye-sql-core-design.md`
  (its header still says "waiting for Teague's approval"; commit `2ec1d82` and the asks brief
  both record approval on 2026-11-10, so the header is stale)
- This repo: issue #104 (libpanduck L1+L2), #129, #107, #140, #85, and `docs/libpanduck.md`
- Why an external parser is not a rival to the SQL core, with the host x format matrix:
  https://claude.ai/artifact/MsXES4p4atdVQVaCtgpT3w
- The Android assessment that started this: https://claude.ai/artifact/KYVUeoahMdLFNyY3f2Bxr9
- Originating session: https://claude.ai/code/session_01G4hzdTdzDmuh1Mdmb53wm6
