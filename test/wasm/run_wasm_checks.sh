#!/usr/bin/env bash
# Run the WASM dependency-symbol check against built .wasm artifacts.
#
# Fast, deterministic gate for the LINKED_LIBS class of bug (see
# check_wasm_imports.mjs). It statically inspects the side module's imports vs
# exports; no duckdb-wasm runtime and no duckdb-version match needed, so a
# failure here is unambiguously the link bug rather than an ABI mismatch.
#
# Usage: test/wasm/run_wasm_checks.sh [dir ...]
#   defaults to build/wasm_mvp build/wasm_eh build/wasm_threads if present.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$here/../.." && pwd)"

# Dependency symbols that MUST be resolved inside the module rather than left as
# unresolved imports:
#   pugixml -> C++, namespace `pugi`, so every mangled name contains `4pugi`
#              (_ZN4pugi..., _ZNK4pugi..., _ZTVN4pugi... for vtables)
#   miniz   -> C, `mz_*` plus the tinfl/tdefl inflate/deflate cores
#
# Deliberately NOT forbidden: miniz's optional zlib-compatible aliases
# (compress/uncompress/crc32/adler32). Those names are ambiguous -- duckdb-wasm's
# host can provide them -- so forbidding them risks a false positive that would
# teach us to ignore this check. The `mz_`/`tinfl_`/`tdefl_` names are unambiguous
# and cannot come from anywhere but miniz.
FORBID=( --forbid '4pugi'
         --forbid '^mz_' --forbid '^tinfl_' --forbid '^tdefl_' )

# Prove the checker can fail before trusting it to pass. A clean artifact and a
# broken checker are indistinguishable from the outside.
echo "==> self-test"
python3 "$here/selftest.py"

dirs=("$@")
if [ ${#dirs[@]} -eq 0 ]; then
  for d in build/wasm_mvp build/wasm_eh build/wasm_threads; do
    [ -d "$repo_root/$d" ] && dirs+=("$repo_root/$d")
  done
fi

if [ ${#dirs[@]} -eq 0 ]; then
  echo "no wasm build dirs found; build first (e.g. make wasm_mvp)" >&2
  exit 2
fi

found_any=0
rc=0
for d in "${dirs[@]}"; do
  while IFS= read -r -d '' wasm; do
    found_any=1
    echo "==> checking $wasm"
    node "$here/check_wasm_imports.mjs" "$wasm" "${FORBID[@]}" || rc=1
  done < <(find "$d" -name 'panduck.duckdb_extension.wasm' -print0 2>/dev/null)
done

# An empty run must FAIL, not pass. A check that silently finds nothing reports
# coverage it is not providing -- the same reason the roundtrip job uses --require.
if [ "$found_any" -eq 0 ]; then
  echo "no panduck.duckdb_extension.wasm found under: ${dirs[*]}" >&2
  exit 2
fi
exit $rc
