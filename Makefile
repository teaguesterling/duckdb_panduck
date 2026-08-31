PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=panduck
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile
# Verify panduck's Pandoc AST mapping against a real pandoc binary. Skips cleanly (exit
# 0) when pandoc is not installed, so it is safe to chain onto other targets.
.PHONY: test_pandoc_alignment
test_pandoc_alignment:
	python3 test/pandoc/check_pandoc_alignment.py

# Differential validation: read each fixture with BOTH panduck and pandoc and compare
# at declared levels of equivalence. Requires a built extension (make release) as well
# as pandoc; skips cleanly when either is missing. Add --report to see raw divergences
# without asserting.
.PHONY: test_roundtrip
test_roundtrip:
	python3 test/roundtrip/check_roundtrip.py
