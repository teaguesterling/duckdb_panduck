# WASM artifact checks

## What this guards

The loadable `.wasm` is not produced by the normal link. It comes from a separate
step in `duckdb/extension/extension_build_tools.cmake` (the `EMSCRIPTEN` block):

```cmake
emcc <archive> -o <out>.wasm -O3 -sSIDE_MODULE=2 ... ${TO_BE_LINKED}
```

`TO_BE_LINKED` is set from `DUCKDB_EXTENSION_PANDUCK_LINKED_LIBS` and **nothing
else**. The `target_link_libraries()` calls in `CMakeLists.txt` are ignored here.
A dependency that is not named ends up imported by the module and defined nowhere,
so the `.wasm` builds, uploads, and passes CI — then throws in the user's browser
on the first call into a DOCX, ODT, or EPUB reader.

Three sibling extensions shipped exactly this: `duckdb_markdown#19`,
`duckdb_webbed#96`, `duckdb_yaml#40`. panduck was about to be the fourth: the
`LINKED_LIBS` list was deliberately left empty during Phase 1, when nothing called
pugixml or miniz, and the note recording that deferral outlived the condition it
rested on by ten readers.

**CI builds the `.wasm` but never instantiates it.** That is why a build-time green
is not evidence here, and why this check inspects the artifact instead.

## What it checks

`check_wasm_imports.mjs` statically parses the module (no instantiation, no
duckdb-wasm runtime, no version match — so a failure is unambiguously the link bug)
and asks the only question that matters: is a symbol **imported, not defined by this
module, and not host-provided**?

"Is it imported?" is the wrong question. Emscripten side modules use
position-independent linking, so a module legitimately imports many symbols it also
exports, plus host-provided ones. The three buckets are resolved against the right
export kind — `env`/function, `GOT.func`/global, `GOT.mem`/global — because data
symbols like vtables (`_ZTV*`) break loading exactly as missing functions do.

## Running it

```bash
make wasm_mvp                      # or wasm_eh / wasm_threads
test/wasm/run_wasm_checks.sh       # defaults to build/wasm_*
test/wasm/run_wasm_checks.sh path/to/artifacts   # or point at downloaded CI artifacts
```

The runner self-tests first (`selftest.py`) against hand-built modules importing a
miniz symbol, a pugixml symbol, and `malloc`. A clean artifact and a broken checker
both print PASS, so the self-test is what makes the PASS mean something. It also
exits non-zero when it finds **no** artifact, rather than reporting coverage it did
not provide.
