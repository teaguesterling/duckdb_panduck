# Follow-ups

Known work deferred out of a release, with the measurement that found each one.

This file exists because the list previously lived only in `.superpowers/sdd/*/progress.md`,
which is **git-ignored** — one `git clean -fdx` from gone, and invisible to anyone who
looked for it. `duckeye` went looking for a durable record, found none, and said so.
Scheduling something and recording it are different acts; only one survives a restart.

Each item is also a GitHub issue (#3-#11). This file is the working list; the issues are
the linkable form for other repos.

## Cross-repo / release-gating

### 1. `doc_toc` must migrate to `duck_blocks_toc_structs` before the next tag

[#3](https://github.com/teaguesterling/duckdb_panduck/issues/3)
`src/reader_registry.cpp:682`. `doc_toc` is not a caller of `duck_blocks_toc` — it *is*
`duck_blocks_toc`, projecting five fields out of it:

    STRUCT("level" INTEGER, title VARCHAR, id VARCHAR, indent INTEGER, element_order INTEGER)[]

duck_block_utils 6.5 reshapes `duck_blocks_toc` to return `duck_blocks`. `.title`, `.id`
and `.indent` have no duck_block counterpart, so `doc_toc` **fails to bind** — it does not
degrade. One call-site swap, but only because the `_structs` sibling exists to swap to.
Blocked on the 6.5 sha. `duck_blocks_toc_structs` confirmed **not** present on installed
`3f2a0f0`.

### 2. `attributes := 'all'` has no builtin mapping

[#11](https://github.com/teaguesterling/duckdb_panduck/issues/11)
All 22 `kind='doc'` registry rows raise `has no mapping for attributes = 'all'`. Correct
today: published webbed `093856b` has no `capture_attributes` at all (verified — binder
error, candidates are `file_path/filename/ignore_errors/include_filepath/maximum_file_size`).
Populate when webbed publishes `60318d8`. `test/sql/html_attributes.test` is the gated
assertion and cannot pass until then — **the claim that a sibling reader honours an option
executes nowhere today.**

## Correctness

### 3. `doc_section` / `doc_container` silently interleave under a plural source

[#4](https://github.com/teaguesterling/duckdb_panduck/issues/4)
They slice by `element_order`, which restarts per document, and accept a glob or list
without complaint:

    doc_section('test/fixtures/sections.html','Alpha')  ->  4 rows
    doc_section('test/fixtures/*.html','Alpha')         -> 12 rows, element_order 0..3 three times over,
                                                           three documents mixed, no filename to separate them

**Pre-existing, not a branch regression**: the macro body is byte-identical to `a9b6505`,
and webbed globs natively (36 rows), so base dispatch did this too. Documented in
`docs/doc_namespace.md`; the guard is the follow-up.

Related inconsistency, also undocumented until now: `doc_toc`, `doc_render` and
`read_panduck_table` *reject* a list, but with leaked internal binder errors
(`replace(VARCHAR[], ...)` for the first two, `panduck_reader_kind_for(VARCHAR[])` for the
third), while `panduck_read_blocks`, `doc_section` and `doc_container` accept one.

## Security-adjacent

### 4. `ReaderEntry::function` is interpolated bare into generated SQL

[#5](https://github.com/teaguesterling/duckdb_panduck/issues/5)
Validated nowhere. Demonstrated executing:

    CALL panduck_register_doc_reader('webbed',
      'read_odt_blocks(''x.pwn2'') UNION ALL SELECT ''block'',''paragraph'',''PWNED-''||version(),1,''text'',MAP{},999 FROM range(1) WHERE 1=1 -- ', ['.pwn2']);
    SELECT panduck_read_arms(['x.pwn2'], false);
    -->  SELECT * FROM read_odt_blocks('x.pwn2') UNION ALL SELECT ... 999 ... ('x.pwn2')

Pre-existing since `27cd39d`; present in `READ_TABLE_MACRO` too. **Crosses no privilege
boundary** — reaching it already requires the ability to run `CALL
panduck_register_doc_reader`, i.e. arbitrary SQL. That is why it is a follow-up and not a
blocker. `src/include/reader_registry.hpp` now says so rather than implying registration is
wholly safe. Fix is an identifier check (dotted) at registration; the reason it was deferred
is that no survey exists of which legitimate registration shapes it would reject.

## API ergonomics — reported and reproduced by `duckeye`, verified here

### 5. `to_json()` on the pandoc AST double-encodes

[#6](https://github.com/teaguesterling/duckdb_panduck/issues/6)
`meta` and `blocks` serialise as escaped **strings** while `pandoc-api-version` serialises
as a real array, so pandoc rejects a document that looks correct:

    {"pandoc-api-version":[1,23,1],"meta":"{\"date\":...}","blocks":"[{\"t\":\"Header\"...]"}

Workaround `.meta::JSON` / `.blocks::JSON`. Inherited shape, not new. Belongs in the docs
beside the function, because `to_json()` is the obvious thing to reach for.

### 6. `panduck_pandoc_api_version()` returns VARCHAR where pandoc needs an array

[#7](https://github.com/teaguesterling/duckdb_panduck/issues/7)
Returns `'1.23'`; pandoc requires `[1, 23, 1]`, which the AST struct one field away already
carries correctly. Treated as a **defect, not a doc gap**: the helper's obvious use is the
one use it fails at.

### 7. No scalar `string -> blocks` route

[#8](https://github.com/teaguesterling/duckdb_panduck/issues/8)
`read_pandoc_blocks_string` is a table function and rejects a column argument even through
`LATERAL`, so a consumer holding pandoc JSON in a column has no route at all.

**The generalisable part, worth more than the fix** (duckeye's framing): swapping a scalar
for a table function is a *silent* change for every literal call site and a *hard break* for
every column one — so the blast radius is invisible at the swap and shows up per-consumer,
later. The same wall broke `duckeye -S` on ZIM archives for weeks: the single-entry path
passed a literal and kept working, only the archive path passed a column, and the suite
stayed green because its one assertion expected a non-zero exit — and a Binder Error is also
non-zero. A table *macro* takes a column happily and may be cheaper than a scalar reader.

## Tidy

### 8. Ten unreachable NULL-`level` branches

[#9](https://github.com/teaguesterling/duckdb_panduck/issues/9)
`odt, docx, epub, rtf, latex, rst, textile, ipynb, mediawiki, org` each carry
`row.has_level ? Value::INTEGER(row.level) : Value(LogicalType::INTEGER)`. Teague's ruling is
**level never null**, so that branch now encodes a possibility the spec forbids.

Measured before calling it dead: whole fixture corpus **1051 blocks, zero NULL levels**; and
`bool has_level = false` is the struct default but every row-emission site sets it true
immediately before `push_back` (two per reader). 0-of-1051 alone would have been coverage,
not impossibility — the `has_level` audit is what makes "unreachable" a claim.

Also `src/include/duck_block_types.hpp:180`: the `CreateBlock` overload "for blocks without
level" passes `Value()` (NULL). Under this ruling it is a footgun rather than a shortcut.

### 9. Per-file registry isolation in the test suite

[#10](https://github.com/teaguesterling/duckdb_panduck/issues/10)
The reader registry is process-wide and the unittest binary runs every file in one process,
so `register_reader.test`'s registrations are still present when `reader_registry.test` runs
(positions 6 and 11 of 23). This forced scoping one assertion to `source='builtin'`.
Verified to be an ordering *sensitivity*, not an ordering *requirement* — both files pass
alone and in suite order — but a future test that registers something could silently
invalidate an assertion in another file.
