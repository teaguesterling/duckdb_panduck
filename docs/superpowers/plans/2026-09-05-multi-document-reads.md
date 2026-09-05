# Multi-document reads Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** let `read_panduck_doc` read a glob or a list of paths in one call, optionally
tagging each row with the document it came from, and let dispatch pass reader options it
currently cannot reach.

**Architecture:** dispatch already builds a SQL string and runs it through `query()`. Every
piece of this is more string-building: one new scalar expands a glob to a list, a scalar
expression maps that list to `UNION ALL` arms, and provenance is a projected literal rather
than a reader feature. Exactly one task is C++; the rest is macro SQL and tests.

**Tech Stack:** C++17, DuckDB v1.5.5 extension API, DuckDB `DefaultTableMacro` /
`ScalarFunction`, sqllogictest.

**Spec:** `docs/superpowers/specs/2026-09-05-multi-document-reads-design.md`

## Global Constraints

- `filename` is **off by default for every source form**; `filename := true` requests it.
- The provenance column is named `filename`, is **trailing**, and holds the full resolved
  path. Core's `filename := 'custom_name'` rename form is **not supported**.
- `element_order` is **per document**, never global.
- A glob matching nothing, a missing explicit path, and a missing list element all **raise**.
- Reader options are **structured data**; no SQL fragment is ever stored or interpolated.
  `param` must match `^[A-Za-z_][A-Za-z0-9_]*$`, validated at registration.
- Existing single-path behaviour must be byte-identical: same columns, same generated SQL.
- Every task ends green on `./build/release/test/unittest "test/sql/*"`.

**Build and test commands** (the implementer will need these constantly):

```bash
make release                                        # ~2 min incremental
./build/release/test/unittest "test/sql/*"          # full suite
./build/release/test/unittest "test/sql/NAME.test"  # one file
make check                                          # the eight auxiliary checks
```

**Repo conventions worth knowing before you start:**

- Macros live in `src/reader_registry.cpp` as `DefaultTableMacro` constants and are
  registered in the `for (auto *tm : {...})` loop near the bottom of
  `RegisterReaderRegistry`.
- `panduck_quote(s)` is the repo's SQL string-literal quoter, defined as a scalar macro in
  the same file. **Use it for every interpolated value.** Never build a literal by
  concatenation.
- Comments in this codebase explain *why*, and cite measurements. Match that density.
- sqllogictest files start with `# name:`, `# description:`, `# group: [sql]` then
  `require panduck`.

---

### Task 1: `panduck_glob` scalar

**Files:**
- Modify: `src/reader_registry.cpp` (add the function + register it)
- Test: `test/sql/multidoc.test` (create)

**Interfaces:**
- Produces: `panduck_glob(pattern VARCHAR) -> VARCHAR[]`. Sorted ascending by byte order.
  Returns an **empty list** when nothing matches — it does not raise; dispatch does that
  (Task 4). Returns a single-element list for a pattern with no metacharacter that exists.

- [ ] **Step 1: Write the failing test**

Create `test/sql/multidoc.test`:

```
# name: test/sql/multidoc.test
# description: multi-document reads -- glob expansion, plural sources, provenance
# group: [sql]

require panduck

# panduck_glob is the one primitive this feature needs in C++. DuckDB's own glob is a TABLE
# function, and dispatch cannot use it: a table function's arguments must be literals, so
# neither `LATERAL read_odt_blocks(g.file)` nor `query((SELECT ... FROM glob(...)))` binds.
# A scalar returning a list can be consumed by the scalar expression that builds dispatch's
# SQL string, which is the whole mechanism.
query I
SELECT len(panduck_glob('test/fixtures/*.odt')) > 1;
----
true

# Sorted, so a UNION ALL built from it is deterministic across runs and platforms.
query I
SELECT panduck_glob('test/fixtures/*.odt') = list_sort(panduck_glob('test/fixtures/*.odt'));
----
true

# An exact path that exists resolves to itself.
query I
SELECT panduck_glob('test/fixtures/constructs.odt');
----
[test/fixtures/constructs.odt]

# NOTHING MATCHED IS AN EMPTY LIST, NOT AN ERROR. The primitive is neutral and the policy
# lives in dispatch (Task 4), so there is exactly one site to change if the rule is ever
# revisited, and a future caller wanting "possibly none" has something to build on.
query I
SELECT len(panduck_glob('test/fixtures/*.nothing_matches_this'));
----
0
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./build/release/test/unittest "test/sql/multidoc.test"`
Expected: FAIL — `Catalog Error: Scalar Function with name panduck_glob does not exist!`

- [ ] **Step 3: Write minimal implementation**

In `src/reader_registry.cpp`, inside the anonymous namespace, above
`RegisterReaderRegistry`:

```cpp
// panduck_glob(pattern) -- expand a filesystem pattern to a sorted list of paths.
//
// WHY THIS EXISTS IN C++ AT ALL, since one new primitive in a design that is otherwise
// macro SQL deserves a reason. DuckDB's `glob` is a TABLE function, and dispatch cannot
// consume a table function: a table function's arguments must be LITERALS, so
// `LATERAL read_odt_blocks(g.file)` is refused ("does not support lateral join column
// parameters") and `query((SELECT ... FROM glob(...)))` is refused ("Table function cannot
// contain subqueries"). A SCALAR returning a list can be consumed by the scalar expression
// that builds dispatch's SQL string. That is the entire reason.
//
// Sorted, because the UNION ALL built from this must be deterministic across runs and
// platforms; the file system's enumeration order is not.
//
// Empty list rather than an error when nothing matches: the primitive stays neutral and
// read_panduck_doc owns the policy, so the raise has exactly one site.
void PanduckGlobFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	auto &fs = FileSystem::GetFileSystem(context);
	UnifiedVectorFormat input;
	args.data[0].ToUnifiedFormat(args.size(), input);
	auto patterns = UnifiedVectorFormat::GetData<string_t>(input);
	for (idx_t i = 0; i < args.size(); i++) {
		auto idx = input.sel->get_index(i);
		if (!input.validity.RowIsValid(idx)) {
			result.SetValue(i, Value(LogicalType::LIST(LogicalType::VARCHAR)));
			continue;
		}
		auto files = fs.GlobFiles(patterns[idx].GetString(), context,
		                          FileGlobOptions::ALLOW_EMPTY);
		std::vector<std::string> paths;
		paths.reserve(files.size());
		for (auto &f : files) {
			paths.push_back(f.path);
		}
		std::sort(paths.begin(), paths.end());
		vector<Value> out;
		out.reserve(paths.size());
		for (auto &p : paths) {
			out.push_back(Value(p));
		}
		result.SetValue(i, Value::LIST(LogicalType::VARCHAR, std::move(out)));
	}
}
```

Add these includes near the top of the file if absent:

```cpp
#include "duckdb/common/file_system.hpp"
#include <algorithm>
```

Register it in `RegisterReaderRegistry`, beside the other `loader.RegisterFunction`
scalar calls:

```cpp
	loader.RegisterFunction(ScalarFunction("panduck_glob", {LogicalType::VARCHAR},
	                                       LogicalType::LIST(LogicalType::VARCHAR),
	                                       PanduckGlobFun));
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make release && ./build/release/test/unittest "test/sql/multidoc.test"`
Expected: PASS, 4 assertions.

If `GlobFiles` does not compile with that signature, check
`duckdb/src/include/duckdb/common/file_system.hpp` in the submodule for the exact one on
the pinned version and adapt — the shape (pattern, context, options) is stable but the
return element type has changed across versions. Do not silently drop `ALLOW_EMPTY`; the
empty-list behaviour is asserted.

- [ ] **Step 5: Verify the full suite is still green**

Run: `./build/release/test/unittest "test/sql/*"`
Expected: PASS, previous total + 4 assertions.

- [ ] **Step 6: Commit**

```bash
git add src/reader_registry.cpp test/sql/multidoc.test
git commit -m "panduck_glob: expand a pattern to a sorted path list, as a scalar

DuckDB's glob is a TABLE function and dispatch cannot consume one -- a table
function's arguments must be literals, so LATERAL over glob() and query() over a
subquery are both refused. A scalar returning a list can be consumed by the scalar
expression that builds dispatch's SQL, which is why this is the one piece of C++ the
multi-document design needs.

Sorted so the generated UNION ALL is deterministic; empty list rather than an error
when nothing matches, so the raise has exactly one site in dispatch."
```

---

### Task 2: `panduck_source_list` — normalise any source form to a path list

**Files:**
- Modify: `src/reader_registry.cpp` (a scalar macro beside `panduck_quote`)
- Test: `test/sql/multidoc.test`

**Interfaces:**
- Consumes: `panduck_glob(VARCHAR) -> VARCHAR[]` from Task 1.
- Produces: `panduck_source_list(src)` accepting `VARCHAR` **or** `VARCHAR[]`, returning
  `VARCHAR[]`. A `VARCHAR` containing `*`, `?` or `[` is globbed; one without is returned
  as a single-element list **without touching the filesystem**; a `VARCHAR[]` has each
  element resolved the same way and the results concatenated in argument order.

- [ ] **Step 1: Write the failing test**

Append to `test/sql/multidoc.test`:

```
# ---------------------------------------------------------------------------
# One normalisation for every source form, so nothing downstream branches on which form
# the caller used. A plain path is NOT sent through the filesystem: a caller naming one
# file should get the same behaviour they get today, including the reader's own error if
# it does not exist, rather than a glob-shaped error from panduck.
# ---------------------------------------------------------------------------
query I
SELECT panduck_source_list('test/fixtures/constructs.odt');
----
[test/fixtures/constructs.odt]

query I
SELECT len(panduck_source_list('test/fixtures/*.odt')) > 1;
----
true

query I
SELECT panduck_source_list(['a.odt', 'b.docx']);
----
[a.odt, b.docx]

# A list element may itself glob, and order follows the argument, not the filesystem.
query I
SELECT len(panduck_source_list(['test/fixtures/constructs.odt', 'test/fixtures/*.odt']))
     = 1 + len(panduck_glob('test/fixtures/*.odt'));
----
true

# A non-existent plain path passes through untouched -- panduck does not pre-validate it.
query I
SELECT panduck_source_list('no/such/file.odt');
----
[no/such/file.odt]
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./build/release/test/unittest "test/sql/multidoc.test"`
Expected: FAIL — `panduck_source_list does not exist`.

- [ ] **Step 3: Write minimal implementation**

Add two entries to the `SCALAR_MACROS[]` table in `src/reader_registry.cpp`, following the
existing `panduck_quote` entry's shape exactly:

```cpp
    // Is this source string a PATTERN or a PATH? Only these three characters make a glob in
    // DuckDB's matcher. A plain path deliberately does NOT go through the filesystem: a
    // caller naming one file must keep today's behaviour, including the reader's own error
    // if it is missing, rather than getting a glob-shaped error from panduck.
    {DEFAULT_SCHEMA,
     "panduck_is_glob",
     {"s", nullptr},
     {{nullptr, nullptr}},
     "contains(s, '*') OR contains(s, '?') OR contains(s, '[')"},

    // Every source form becomes one VARCHAR[] here, so nothing downstream has to branch on
    // which form the caller used. flatten() preserves argument order for a list, which is
    // what makes read_panduck_doc(['a','b']) deterministic independent of the filesystem.
    {DEFAULT_SCHEMA,
     "panduck_source_list",
     {"src", nullptr},
     {{nullptr, nullptr}},
     "CASE WHEN typeof(src) LIKE '%[]' "
     "     THEN flatten(list_transform(src::VARCHAR[], "
     "                  lambda p: CASE WHEN panduck_is_glob(p) THEN panduck_glob(p) ELSE [p] END)) "
     "     WHEN panduck_is_glob(src::VARCHAR) THEN panduck_glob(src::VARCHAR) "
     "     ELSE [src::VARCHAR] END"},
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make release && ./build/release/test/unittest "test/sql/multidoc.test"`
Expected: PASS, 9 assertions.

If `typeof(src) LIKE '%[]'` does not discriminate as expected inside a macro, print
`SELECT typeof(['a'])` and `SELECT typeof('a')` in the shell and adjust the predicate to
match what the pinned DuckDB actually reports. Do not switch to an overload — a macro
cannot be overloaded by parameter type, which is why this uses a runtime type test.

- [ ] **Step 5: Verify the full suite is still green**

Run: `./build/release/test/unittest "test/sql/*"`

- [ ] **Step 6: Commit**

```bash
git add src/reader_registry.cpp test/sql/multidoc.test
git commit -m "panduck_source_list: one normalisation for every source form

A VARCHAR that globs, a VARCHAR that does not, and a VARCHAR[] all become one
VARCHAR[], so nothing downstream branches on the form the caller used.

A plain path is deliberately NOT sent through the filesystem: a caller naming one
file keeps today's behaviour, including the reader's own error if it is missing,
rather than a glob-shaped error from panduck."
```

---

### Task 3: `panduck_read_arms` — build the UNION ALL, with optional provenance

**Files:**
- Modify: `src/reader_registry.cpp` (a scalar macro)
- Test: `test/sql/multidoc.test`

**Interfaces:**
- Consumes: `panduck_source_list`, `panduck_quote`, `panduck_reader_function_for`.
- Produces: `panduck_read_arms(paths VARCHAR[], with_filename BOOLEAN) -> VARCHAR`, a SQL
  string of `UNION ALL`-joined arms, each
  `SELECT *[, '<path>' AS filename] FROM <fn>('<path>')`.

- [ ] **Step 1: Write the failing test**

Append to `test/sql/multidoc.test`:

```
# ---------------------------------------------------------------------------
# The generated SQL is asserted as a STRING before anything runs it. A test that only
# checked row counts would pass on SQL that happened to work while emitting the provenance
# column in the wrong POSITION -- which is exactly the defect that shipped in two sibling
# extensions (webbed a865d37, markdown 340c0cd both emitted filename FIRST, which spec 6.4
# refuses). Asserting the string catches it in the producer.
# ---------------------------------------------------------------------------
query I
SELECT panduck_read_arms(['a.odt'], false);
----
SELECT * FROM read_odt_blocks('a.odt')

# filename LAST, after the canonical seven, because SELECT * emits them first. Trailing is
# not cosmetic: spec 6.4 keys 8-field acceptance on the exact type with filename last, and
# every consumer reads the struct BY INDEX, so a leading column puts the path where kind is
# expected.
query I
SELECT panduck_read_arms(['a.odt'], true);
----
SELECT *, 'a.odt' AS filename FROM read_odt_blocks('a.odt')

query I
SELECT panduck_read_arms(['a.odt', 'b.html'], true);
----
SELECT *, 'a.odt' AS filename FROM read_odt_blocks('a.odt') UNION ALL SELECT *, 'b.html' AS filename FROM read_html_blocks('b.html')

# A path with a quote in it survives, because every interpolated value goes through
# panduck_quote. This is the assertion that the string builder is not concatenating.
query I
SELECT contains(panduck_read_arms(['o''brien.odt'], true), 'o''''brien.odt');
----
true
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./build/release/test/unittest "test/sql/multidoc.test"`
Expected: FAIL — `panduck_read_arms does not exist`.

- [ ] **Step 3: Write minimal implementation**

Add to `SCALAR_MACROS[]`:

```cpp
    // Build the UNION ALL that dispatch runs. One arm per path, joined in list order.
    //
    // PROVENANCE IS PROJECTED, NOT REQUESTED. `SELECT *, '<path>' AS filename` puts the
    // column AFTER the canonical seven by construction, because SELECT * emits those first.
    // That matters more than it looks: duck_block spec 6.4 keys 8-field acceptance on the
    // exact type with filename LAST, every consumer reads the struct by index, and BOTH
    // shipped sibling producers got this wrong by emitting the column first (webbed
    // a865d37, markdown 340c0cd) -- refused at the binder rather than misread, but refused.
    // A projection cannot make that mistake.
    //
    // It also means panduck needs nothing from a sibling to have provenance: the path is
    // already in hand at dispatch time.
    {DEFAULT_SCHEMA,
     "panduck_read_arms",
     {"paths", "with_filename", nullptr},
     {{nullptr, nullptr}},
     "array_to_string(list_transform(paths, lambda p: "
     "  'SELECT *' || CASE WHEN with_filename THEN ', ' || panduck_quote(p) || ' AS filename' "
     "                     ELSE '' END || "
     "  ' FROM ' || panduck_reader_function_for(p) || '(' || panduck_quote(p) || ')'), "
     "  ' UNION ALL ')"},
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make release && ./build/release/test/unittest "test/sql/multidoc.test"`
Expected: PASS, 13 assertions.

- [ ] **Step 5: Verify the full suite is still green**

Run: `./build/release/test/unittest "test/sql/*"`

- [ ] **Step 6: Commit**

```bash
git add src/reader_registry.cpp test/sql/multidoc.test
git commit -m "panduck_read_arms: build the UNION ALL, provenance projected not requested

SELECT *, '<path>' AS filename puts the column after the canonical seven BY
CONSTRUCTION. Spec 6.4 keys 8-field acceptance on the exact type with filename last,
and both shipped sibling producers emitted it FIRST (webbed a865d37, markdown
340c0cd). A projection cannot make that mistake.

The generated SQL is asserted as a string, not only by row count: a count-only test
passes on SQL that works while emitting the column in the wrong position, which is
precisely how that defect reached two released extensions."
```

---

### Task 4: plural `read_panduck_doc`

**Files:**
- Modify: `src/reader_registry.cpp` — `READ_DOC_MACRO`
- Test: `test/sql/multidoc.test`

**Interfaces:**
- Consumes: `panduck_source_list`, `panduck_read_arms`.
- Produces: `read_panduck_doc(src, format := 'auto', pages := '', filename := false)` where
  `src` is `VARCHAR` or `VARCHAR[]`.

- [ ] **Step 1: Write the failing test**

Append to `test/sql/multidoc.test`:

```
# ---------------------------------------------------------------------------
# A glob returns exactly the sum of the individual reads. Written as a computed equality
# rather than a literal so that adding an .odt fixture cannot silently invalidate it.
# ---------------------------------------------------------------------------
query I
SELECT (SELECT count(*) FROM read_panduck_doc('test/fixtures/*.odt'))
     = (SELECT sum(n) FROM (
         SELECT (SELECT count(*) FROM read_panduck_doc(f)) AS n
         FROM (SELECT unnest(panduck_glob('test/fixtures/*.odt')) AS f)));
----
true

# Mixed formats in one call, each path dispatched through the registry independently.
query I
SELECT count(DISTINCT filename)
FROM read_panduck_doc(['test/fixtures/constructs.odt', 'test/fixtures/constructs.html'],
                      filename := true);
----
2

# PROVENANCE IS OFF BY DEFAULT FOR EVERY SOURCE FORM. A default that changed shape with
# the form of the argument would mean read_panduck_doc(x) returns seven or eight columns
# depending on whether x contains a '*' -- a rule every caller must know and none can see
# at the call site. It is also what keeps the default output consumable by a RELEASED
# duck_block_utils, which refuses an 8-field struct.
query I
SELECT count(*) FROM (DESCRIBE SELECT * FROM read_panduck_doc('test/fixtures/*.odt'));
----
7

query I
SELECT count(*) FROM (DESCRIBE SELECT * FROM read_panduck_doc('test/fixtures/constructs.odt'));
----
7

query I
SELECT count(*) FROM (DESCRIBE SELECT * FROM read_panduck_doc('test/fixtures/*.odt', filename := true));
----
8

# element_order is PER DOCUMENT. Making it global would break every consumer that uses it
# to reconstruct one document's order -- doc_section, doc_container, the pandoc writer.
query I
SELECT bool_and(has_zero) FROM (
  SELECT bool_or(element_order = 0) AS has_zero
  FROM read_panduck_doc('test/fixtures/*.odt', filename := true)
  GROUP BY filename);
----
true

# A glob matching nothing RAISES, matching DuckDB core, which errors identically for a
# pattern that matches nothing and for a named file that is absent.
statement error
SELECT count(*) FROM read_panduck_doc('test/fixtures/*.nothing_matches_this');
----
no files matched

# The default output is byte-identical to today's for a single path -- the check that this
# feature is additive and that a caller on a released duck_block_utils is unaffected.
query I
SELECT (SELECT count(*) FROM read_panduck_doc('test/fixtures/constructs.odt'))
     = (SELECT count(*) FROM read_odt_blocks('test/fixtures/constructs.odt'));
----
true
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./build/release/test/unittest "test/sql/multidoc.test"`
Expected: FAIL — the glob is passed to the reader as a literal path, giving
`IO Error: ... not a readable ZIP archive: test/fixtures/*.odt`.

- [ ] **Step 3: Write minimal implementation**

In `READ_DOC_MACRO`, add `filename` to the named parameters:

```cpp
                                          {{"format", "'auto'"}, {"pages", "''"},
                                           {"filename", "false"}, {nullptr, nullptr}},
```

Then add a **plural branch as the first `WHEN`** inside the existing `CASE`, before the
`pages` guard:

```sql
        -- PLURAL SOURCES. Resolved to a path list, then one arm per path. This branch runs
        -- only when the source is not a single plain path, so a caller naming one file
        -- generates exactly the SQL it generates today -- byte-identical, asserted.
        --
        -- The raise for an empty match lives HERE rather than in panduck_glob, so the
        -- primitive stays neutral and the policy has one site. It matches DuckDB core,
        -- which errors identically for a pattern matching nothing and for a named file
        -- that is absent -- measured on read_csv, both "No files found that match the
        -- pattern". A nicer rule was drafted and rejected: panduck is not the place to
        -- diverge from core's file resolution unilaterally.
        WHEN typeof(src) LIKE '%[]' OR panduck_is_glob(src::VARCHAR)
            THEN CASE WHEN len(panduck_source_list(src)) = 0
                 THEN error('panduck: no files matched ' || src::VARCHAR)
                 ELSE panduck_read_arms(panduck_source_list(src), filename) END
```

and make the existing single-path generic branch honour `filename` too, by replacing its
`'SELECT * FROM '` construction with a call to the same builder:

```sql
            THEN CASE WHEN panduck_ensure_extension(panduck_reader_extension_for(src))
                 THEN panduck_read_arms([src::VARCHAR], filename)
                 ELSE error('panduck: ' || src || ' needs the ' ||
                            panduck_reader_extension_for(src) || ' extension') END
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make release && ./build/release/test/unittest "test/sql/multidoc.test"`
Expected: PASS, 22 assertions.

- [ ] **Step 5: Verify the full suite is still green**

Run: `./build/release/test/unittest "test/sql/*"` then `make check`
Expected: both green. If `doc_namespace.test` or `reader_registry.test` fail, the
single-path SQL is no longer byte-identical — fix that rather than the test.

- [ ] **Step 6: Commit**

```bash
git add src/reader_registry.cpp test/sql/multidoc.test
git commit -m "read_panduck_doc reads globs and path lists

One arm per resolved path, UNION ALLed. A caller naming a single plain file
generates exactly the SQL it generated before, which the suite asserts.

filename is OFF by default for every source form. A default that changed shape with
the form of the argument would mean read_panduck_doc(x) returns seven or eight
columns depending on whether x contains a '*'. It is also what keeps the default
output consumable by a RELEASED duck_block_utils, which refuses an 8-field struct.

An empty glob raises, matching core, which errors identically for a pattern matching
nothing and a named file that is absent."
```

---

### Task 5: the junction check — what panduck emits must be consumable

**Files:**
- Test: `test/sql/multidoc.test`

**Interfaces:**
- Consumes: everything above. Adds no production code.

- [ ] **Step 1: Write the test**

Append to `test/sql/multidoc.test`:

```
# ---------------------------------------------------------------------------
# THE JUNCTION. Every test above verifies panduck against panduck. What actually broke in
# two sibling extensions this week was the junction: an emitter in one repo producing a
# shape a consumer in another repo refuses. No repo's own suite can catch that, because
# each verifies its own behaviour -- webbed had 3940 assertions and shipped it.
#
# panduck's own writer is a real consumer that binds the canonical struct STRUCTURALLY, so
# it is a junction check that needs no second extension installed.
# ---------------------------------------------------------------------------
query I
SELECT length(panduck_blocks_to_pandoc_ast(
    (SELECT list(b ORDER BY b.element_order)
     FROM read_panduck_doc('test/fixtures/constructs.odt') b))::VARCHAR) > 0;
----
true

# THE EXACT EMITTED TYPE, asserted as a full string. This is the producer-side instrument
# that webbed and markdown both lacked: it fails HERE, in the producer, if the provenance
# column ever moves, instead of three repos away at someone else's binder.
query I
SELECT typeof(list(b)) FROM read_panduck_doc('test/fixtures/constructs.odt', filename := true) b;
----
STRUCT(kind VARCHAR, element_type VARCHAR, "content" VARCHAR, "level" INTEGER, "encoding" VARCHAR, attributes MAP(VARCHAR, VARCHAR), element_order INTEGER, filename VARCHAR)[]

# And the seven-field shape is unchanged when provenance is off.
query I
SELECT typeof(list(b)) FROM read_panduck_doc('test/fixtures/constructs.odt') b;
----
STRUCT(kind VARCHAR, element_type VARCHAR, "content" VARCHAR, "level" INTEGER, "encoding" VARCHAR, attributes MAP(VARCHAR, VARCHAR), element_order INTEGER)[]
```

- [ ] **Step 2: Run and confirm it passes**

Run: `./build/release/test/unittest "test/sql/multidoc.test"`
Expected: PASS, 25 assertions. If the `typeof` strings differ, **copy the actual output
into the test** — but first check the difference is only quoting/spacing and that
`filename` really is last. If it is not last, that is a Task 3 bug, not a test bug.

- [ ] **Step 3: Negative control**

Temporarily change `filename VARCHAR)[]` to `filename INTEGER)[]` in the test, re-run,
confirm it FAILS, then change it back and confirm it passes again. A type assertion that
cannot fail is the thing this task exists to prevent.

- [ ] **Step 4: Commit**

```bash
git add test/sql/multidoc.test
git commit -m "Junction check: assert the exact emitted type, not just row counts

Every other test here verifies panduck against panduck. What broke in two sibling
extensions this week was the junction -- an emitter producing a shape a consumer in
another repo refuses -- and no repo's own suite catches that; webbed had 3940
assertions and shipped it.

The full typeof string is the producer-side instrument they both lacked: it fails
here if the provenance column ever moves, instead of three repos away."
```

---

### Task 6: structured reader options

**Files:**
- Modify: `src/include/reader_registry.hpp` — `ReaderEntry`
- Modify: `src/reader_registry.cpp` — registration, `RegistryScan`, `panduck_read_arms`
- Test: `test/sql/multidoc.test`

**Interfaces:**
- Consumes: `panduck_read_arms` from Task 3, extended here.
- Produces: `ReaderEntry::options` as
  `std::vector<ReaderOption>` where
  `struct ReaderOption { std::string intent, value, param, arg, arg_type; };`
  and `panduck_register_doc_reader(..., options := [...])`.

- [ ] **Step 1: Write the failing test**

Append to `test/sql/multidoc.test`:

```
# ---------------------------------------------------------------------------
# Reader options are STRUCTURED DATA, never a stored SQL fragment. Registrations are
# process-wide and persistent, so a fragment stored by one caller would execute inside
# every later read_panduck_doc, including other sessions' calls in a shared process.
# panduck renders the argument itself from typed fields.
# ---------------------------------------------------------------------------
statement ok
CALL panduck_register_doc_reader('webbed', 'read_html_blocks', ['.htmltest'],
     options := [{intent: 'attributes', value: 'all',
                  param: 'capture_attributes', arg: 'classes', arg_type: 'VARCHAR'}]);

# The intent reaches the sibling's parameter; the caller never types capture_attributes.
query I
SELECT panduck_read_arms_opt(['x.htmltest'], false, 'attributes', 'all');
----
SELECT * FROM read_html_blocks('x.htmltest', capture_attributes := 'classes')

# A non-identifier param is REJECTED AT REGISTRATION, so a malformed row cannot be stored.
statement error
CALL panduck_register_doc_reader('webbed', 'read_html_blocks', ['.badopt'],
     options := [{intent: 'a', value: 'b',
                  param: 'x := 1) UNION SELECT', arg: 'c', arg_type: 'VARCHAR'}]);
----
param must be an identifier

# ... and it is ABSENT afterwards. Asserting the error alone would pass on a warning.
query I
SELECT count(*) FROM panduck_reader_registry() WHERE ext = '.badopt';
----
0

# A requested intent with no mapping RAISES rather than being dropped. This is the
# pages-was-a-lie lesson applied in advance: a parameter accepted and ignored reads as a
# feature at the call site.
statement error
SELECT panduck_read_arms_opt(['a.odt'], false, 'attributes', 'all');
----
has no mapping for
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./build/release/test/unittest "test/sql/multidoc.test"`
Expected: FAIL — `panduck_register_doc_reader` has no `options` parameter.

- [ ] **Step 3: Write minimal implementation**

In `src/include/reader_registry.hpp`, above `ReaderEntry`:

```cpp
//! One reader option, held as STRUCTURED DATA rather than a SQL fragment. Registrations are
//! process-wide and persistent, so a stored fragment would execute inside every later
//! read_panduck_doc, including other sessions' calls in a shared process. panduck renders
//! the argument itself: `param` is validated as an identifier at registration, and `arg` is
//! rendered according to `arg_type`. There is no path from registration data to arbitrary
//! SQL, which is a shorter security argument than any that a fragment would need.
struct ReaderOption {
	std::string intent;   //!< panduck's vocabulary: "attributes"
	std::string value;    //!< the intent's value: "all"
	std::string param;    //!< the READER's parameter name: "capture_attributes"
	std::string arg;      //!< the value to pass, unrendered
	std::string arg_type; //!< VARCHAR | BOOLEAN | INTEGER
};
```

and add to `ReaderEntry`:

```cpp
	std::vector<ReaderOption> options; //!< intent -> this reader's spelling; see ReaderOption
```

In `src/reader_registry.cpp`, add the identifier validator beside `ExtOfPath`:

```cpp
// A reader parameter name must be a bare identifier. Checked AT REGISTRATION so a malformed
// row cannot be stored at all, rather than failing later at query time where the caller has
// no idea which registration is responsible.
bool IsIdentifier(const std::string &s) {
	if (s.empty() || (!std::isalpha((unsigned char)s[0]) && s[0] != '_')) {
		return false;
	}
	for (char c : s) {
		if (!std::isalnum((unsigned char)c) && c != '_') {
			return false;
		}
	}
	return true;
}
```

Extend `RegisterBind` to read the `options` named parameter, validating each row:

```cpp
	auto opt_entry = input.named_parameters.find("options");
	if (opt_entry != input.named_parameters.end() && !opt_entry->second.IsNull()) {
		for (auto &row : ListValue::GetChildren(opt_entry->second)) {
			auto &f = StructValue::GetChildren(row);
			ReaderOption o{f[0].GetValue<string>(), f[1].GetValue<string>(),
			               f[2].GetValue<string>(), f[3].GetValue<string>(),
			               f[4].GetValue<string>()};
			if (!IsIdentifier(o.param)) {
				throw InvalidInputException(
				    "panduck: param must be an identifier, got '%s'", o.param);
			}
			if (o.arg_type != "VARCHAR" && o.arg_type != "BOOLEAN" && o.arg_type != "INTEGER") {
				throw InvalidInputException("panduck: unknown arg_type '%s'", o.arg_type);
			}
			result->options.push_back(std::move(o));
		}
	}
```

Declare the named parameter on both registration functions:

```cpp
	reg_doc.named_parameters["options"] =
	    LogicalType::LIST(LogicalType::STRUCT({{"intent", LogicalType::VARCHAR},
	                                           {"value", LogicalType::VARCHAR},
	                                           {"param", LogicalType::VARCHAR},
	                                           {"arg", LogicalType::VARCHAR},
	                                           {"arg_type", LogicalType::VARCHAR}}));
```

Add a scalar that renders one option for a path, plus `panduck_read_arms_opt` as
`panduck_read_arms` with the rendered option appended inside the reader call. Add a
`panduck_reader_option_for(path, intent, value) -> VARCHAR` C++ scalar that looks the row
up and returns the rendered `param := literal`, raising when there is no mapping:

```cpp
// Renders `param := literal` for one intent, or raises. The rendering is BY TYPE --
// VARCHAR through the same escaping panduck_quote uses, BOOLEAN bare, INTEGER only after it
// parses as one -- so nothing registrant-supplied reaches the generated SQL as SQL.
```

- [ ] **Step 4: Wire options into dispatch**

Building the option and never using it would be a feature that exists only in its own test.
Add `attributes` to `READ_DOC_MACRO`'s named parameters:

```cpp
                                          {{"format", "'auto'"}, {"pages", "''"},
                                           {"filename", "false"}, {"attributes", "'default'"},
                                           {nullptr, nullptr}},
```

and have both the plural and single-path branches call `panduck_read_arms_opt(..., 'attributes', attributes)`
instead of `panduck_read_arms(...)`. When `attributes = 'default'` the renderer returns an
empty string and the generated SQL is unchanged — which is what keeps Task 4's
byte-identical assertion true.

- [ ] **Step 5: Assert the option reaches the READER, not just the string**

Append to `test/sql/multidoc.test`:

```
# THE OPTION MUST CHANGE THE READER'S OUTPUT, not merely appear in the generated SQL.
# Asserting the string proves panduck built it; asserting the rows proves the reader
# received and honoured it. Those are different claims and only the second is the feature.
#
# webbed's published build drops `class` unless asked. constructs.html carries a class on
# its heading, so this is the discriminating document.
statement ok
INSTALL webbed;

statement ok
LOAD webbed;

query I
SELECT count(*) FROM read_panduck_doc('test/fixtures/constructs.html')
WHERE attributes['class'] IS NOT NULL;
----
0

query I
SELECT count(*) > 0 FROM read_panduck_doc('test/fixtures/constructs.html', attributes := 'all')
WHERE attributes['class'] IS NOT NULL;
----
true
```

Add `class="lead"` to the `<h1>` in `test/fixtures/constructs.html` first, and say why in
the commit — a fixture without a class cannot distinguish the two cases, and a test that
cannot distinguish them is the failure this repo has been hunting all week.

**If the published webbed in CI does not yet support `capture_attributes`,** gate these two
assertions behind `require-env PANDUCK_TEST_WEBBED_ATTRS` and name the gap in a comment, as
`pdf_reader.test` does. Do not weaken the assertion to make it pass.

- [ ] **Step 6: Run test to verify it passes**

Run: `make release && ./build/release/test/unittest "test/sql/multidoc.test"`
Expected: PASS, 32 assertions.

- [ ] **Step 7: Verify the full suite is still green**

Run: `./build/release/test/unittest "test/sql/*"` then `make check`

Note `reader_registry.test` asserts registry rows; adding a column to
`panduck_reader_registry()` output would break it. **Do not add one** — options are not
surfaced as a registry column in this task.

- [ ] **Step 8: Commit**

```bash
git add src/include/reader_registry.hpp src/reader_registry.cpp \
        test/fixtures/constructs.html test/sql/multidoc.test
git commit -m "Reader options: structured data, rendered by panduck

Registrations are process-wide and persistent, so a stored SQL fragment would execute
inside every later read_panduck_doc, including other sessions' calls in a shared
process. Options are typed fields instead: param validated as an identifier AT
REGISTRATION so a malformed row cannot be stored, arg rendered by arg_type.

An intent with no mapping raises rather than being dropped -- the pages-was-a-lie
lesson applied before it can happen again."
```

---

### Task 7: `reader_params` passthrough

**Files:**
- Modify: `src/reader_registry.cpp` — `READ_DOC_MACRO`, and the option renderer from Task 6
- Test: `test/sql/multidoc.test`

**Interfaces:**
- Consumes: the type-directed renderer from Task 6.
- Produces: `read_panduck_doc(..., reader_params := MAP{'name': 'value'})`, rendering each
  entry as `name := 'value'` with the same identifier validation.

The intent vocabulary (Task 6) exists so panduck can ask on **its own** behalf — `doc_render`
needs `class` for a faithful Pandoc `Attr` and has no caller to supply a parameter. This task
is the complement: an escape hatch for a caller who knows exactly which reader parameter they
want, so panduck never grows a vocabulary entry per sibling option.

- [ ] **Step 1: Write the failing test**

Append to `test/sql/multidoc.test`:

```
# The escape hatch. panduck has no intent for `ignore_errors` and should never grow one --
# there are dozens of such parameters across the readers panduck dispatches to, and a
# vocabulary entry per sibling option is a maintenance burden with no upside.
#
# Same identifier discipline as the registry mapping: a key that is not a bare identifier is
# rejected, so there is still no path from caller input to arbitrary SQL.
query I
SELECT panduck_render_params(MAP{'ignore_errors': 'true'});
----
, ignore_errors := 'true'

query I
SELECT panduck_render_params(MAP{});
----
(empty)

statement error
SELECT panduck_render_params(MAP{'x := 1) UNION SELECT': 'y'});
----
must be an identifier
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./build/release/test/unittest "test/sql/multidoc.test"`
Expected: FAIL — `panduck_render_params does not exist`.

- [ ] **Step 3: Write minimal implementation**

Add a C++ scalar `panduck_render_params(MAP(VARCHAR, VARCHAR)) -> VARCHAR` that validates
each key with the `IsIdentifier` helper from Task 6 and renders `, key := 'value'` for each,
quoting values the way `panduck_quote` does. Values are rendered as VARCHAR literals only —
a caller needing a boolean or a list passes the literal text and the reader casts it, which
keeps this function's surface one type wide.

Then add `reader_params` to `READ_DOC_MACRO`'s named parameters with default `MAP{}`, and
append `panduck_render_params(reader_params)` inside the reader call in both branches,
after the option from Task 6.

- [ ] **Step 4: Run test to verify it passes**

Run: `make release && ./build/release/test/unittest "test/sql/multidoc.test"`
Expected: PASS, 35 assertions.

- [ ] **Step 5: Verify the full suite is still green**

Run: `./build/release/test/unittest "test/sql/*"` then `make check`
An empty `reader_params` must render an empty string, or Task 4's byte-identical assertion
fails — which is the check that this addition is inert by default.

- [ ] **Step 6: Commit**

```bash
git add src/reader_registry.cpp test/sql/multidoc.test
git commit -m "reader_params: pass a reader parameter through without an intent

The intent vocabulary exists so panduck can ask on its OWN behalf -- doc_render needs
class for a faithful Pandoc Attr and has no caller to supply a parameter. This is the
complement: a caller who knows which parameter they want says so directly, and
panduck never grows a vocabulary entry per sibling option.

Same identifier validation as the registry mapping, so there is still no path from
caller input to arbitrary SQL. Empty by default, which Task 4's byte-identical
assertion enforces."
```

---

### Task 8: docs

**Files:**
- Modify: `docs/doc_namespace.md`, `README.md`
- Test: none (documentation)

- [ ] **Step 1: Document the plural forms in `docs/doc_namespace.md`**

Add a section covering: the three source forms; `filename := true` and that it is off by
default; that the column is trailing and named `filename`; that core's string-rename form
is deliberately unsupported because a renamed column does not bind as a `duck_block`; that
`element_order` is per document; and that an empty glob raises, matching core. Copy the
error texts from a real run rather than composing them.

- [ ] **Step 2: Add one line to README's quick tour**

```sql
SELECT * FROM read_panduck_doc('docs/*.docx', filename := true);  -- a whole corpus
```

- [ ] **Step 3: Verify every SQL example in the new docs actually runs**

Run each example in `./build/release/duckdb -unsigned` with panduck loaded. This repo has
shipped docs that described capabilities the code did not have; do not add to that.

- [ ] **Step 4: Commit**

```bash
git add docs/doc_namespace.md README.md
git commit -m "Document multi-document reads

Every SQL example verified against a real run rather than composed."
```

---

## Notes for the implementer

**Why there is no task for `filename := 'custom_name'`.** Core supports it; panduck
deliberately does not. Measured: a struct whose eighth field is named anything other than
`filename` does not bind as a `duck_block`, so honouring the rename would hand callers rows
no consumer accepts. If you find yourself adding it, re-read §3.4 of the spec.

**Why provenance is projected rather than requested from readers.** panduck already has the
path at dispatch time. Asking a sibling for it would make panduck depend on that sibling
having shipped `filename := true`, which markdown has not (PR #52, unmerged). Projection has
no such dependency and is trailing by construction.

**If a task's test passes on the first run, stop and find out why.** Every task here is
written test-first and the failure mode is named. A test that passes before the
implementation exists is testing something other than what it claims.
