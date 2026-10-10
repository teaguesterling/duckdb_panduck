#!/usr/bin/env python3
"""No file under libpanduck/ may name DuckDB.

WHY THIS EXISTS AS A GREP AND NOT ONLY AS A BUILD.

panduck's CMakeLists applies duckdb's include paths DIRECTORY-WIDE
(`include_directories(...)` at CMakeLists.txt:122 plus the paths the duckdb
build adds in the parent scope). A child target declared there inherits all of
them, so a `#include "duckdb.hpp"` inside libpanduck/ would compile perfectly in
the extension build. The compiler cannot be the guard in that build.

Two things cover it instead:

  1. libpanduck/CMakeLists.txt configures the engine standalone, with no duckdb
     on the include path. That is real compiler enforcement, and CI runs it.
  2. This script, which is the fast local signal and does not need a configure.

MEASURED, so the rule is not arbitrary: across the eleven readers ~10,500 lines
contain ~195 that name a DuckDB type (~2%), and every DuckDB break this project
has absorbed -- named_parameters (#101), SetValue (#102), ExecuteWithNulls in a
sibling -- lived in that 2%. The seam is worth keeping sharp because it is what
holds the other 98% harmless.

Exit 0 when clean, 1 when the seam leaks.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LIB = ROOT / "libpanduck"

# An #include of a duckdb header is the leak that matters: it is what drags the
# API surface across. Bare mentions in prose are not -- the moved core carries
# comments explaining what stayed behind, and a guard that fails on the word
# "duckdb" in a comment is a guard people learn to silence.
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)[">]', re.MULTILINE)

# Named in code, these mean a DuckDB type reached the engine even if the header
# arrived transitively. Checked outside comments only.
SYMBOLS = ("duckdb::", "LogicalType", "DataChunk", "ExtensionLoader", "TableFunction")


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", text)


def main() -> int:
    if not LIB.is_dir():
        print(f"no {LIB.relative_to(ROOT)}/ directory; nothing to check")
        return 0

    problems = []
    checked = 0
    for path in sorted(LIB.rglob("*")):
        if path.suffix not in {".hpp", ".cpp", ".h", ".cc"}:
            continue
        checked += 1
        rel = path.relative_to(ROOT)
        text = path.read_text(encoding="utf-8")

        for inc in INCLUDE_RE.findall(text):
            if inc == "duckdb.hpp" or inc.startswith("duckdb/"):
                problems.append(f"{rel}: includes DuckDB header {inc!r}")

        code = strip_comments(text)
        # MASK PER SITE, NOT PER FILE. The vendored duck_block vocabulary
        # declares its constants in `namespace duckdb` (its lines 242..806), so
        # panduck/vocabulary.hpp must alias `::duckdb::DuckBlockVocabulary`
        # once. Blanking that one spelling keeps every OTHER `duckdb::` in the
        # same file visible to the scan; exempting the file would take the real
        # leaks with it.
        code = code.replace("::duckdb::DuckBlockVocabulary", "")
        for sym in SYMBOLS:
            if sym in code:
                line = next(
                    (i for i, l in enumerate(code.splitlines(), 1) if sym in l), 0
                )
                problems.append(f"{rel}:{line}: names DuckDB symbol {sym!r}")

    if problems:
        print("libpanduck seam LEAKS:\n")
        for p in problems:
            print(f"  {p}")
        print(
            "\nThe engine must not name DuckDB. Put the DuckDB-facing half in src/ "
            "and re-export what it needs, as src/include/ipynb_reader.hpp does."
        )
        return 1

    print(f"libpanduck seam holds ({checked} file(s) checked, no DuckDB API named)")
    # SAY WHAT THIS DOES NOT COVER, so "seam holds" is not read as more than it
    # is. ONE dependency on DuckDB's *build* still survives behind the seam,
    # tracked in issue #104's L1 contract rather than silently tolerated:
    #   - the duck_block vocabulary declares its constants in `namespace duckdb`
    #     (aliased once in panduck/vocabulary.hpp)
    # It is not a DuckDB API dependency -- nothing here would break on a DuckDB
    # API change, which is the property the seam exists to buy -- but a
    # standalone build has to answer it.
    #
    # yyjson WAS the second gap, and is recorded here rather than deleted so the
    # next reader does not rediscover it: libpanduck/src/ipynb.cpp included
    # DuckDB's vendored `namespace duckdb_yyjson` header directly, so the
    # standalone build -- the seam's only compile-level enforcement -- could not
    # compile that one module. Answered in #123 by
    # libpanduck/include/panduck/yyjson_compat.hpp, which selects the vendored
    # copy or a standalone global-scope `<yyjson.h>` from
    # PANDUCK_YYJSON_VENDORED_DUCKDB; the standalone build links vcpkg's yyjson
    # and compiles ipynb.
    print("  known gaps (issue #104): duck_block vocabulary namespace")
    return 0


if __name__ == "__main__":
    sys.exit(main())
