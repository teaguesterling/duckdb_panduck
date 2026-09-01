PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=panduck
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile
# Run every independent guard and report ALL of them, then fail if any failed.
#
# WRITTEN THIS WAY DELIBERATELY. A recipe of sequential lines stops at the first non-zero
# exit, so a red run silently skips whatever came after it and reads as an ordinary failure
# rather than "and three things went unverified". duck_block_utils shipped exactly that and
# measured it: 2 of 8 checks executing on a failure. These four are independent -- nothing
# justifies sequencing them -- so each runs, each reports, and the target fails at the end.
#
# The same hazard bit this repo one level down and cost more: sqllogictest aborts a .test
# file at the first failed assertion, so a real runaway defect sat behind an earlier
# failure and the test that existed for it never ran. THAT one cannot be fixed from here --
# when the suite is red, treat the assertion count as a floor, not a total.
.PHONY: check
check:
	@rc=0; \
	for c in check-vocabulary check-conformance check-converter test_pandoc_alignment test_roundtrip; do \
	  printf '\n=== %s ===\n' "$$c"; \
	  $(MAKE) --no-print-directory $$c || rc=1; \
	done; \
	printf '\n'; \
	if [ $$rc -ne 0 ]; then echo "FAILED: one or more checks above"; else echo "All checks passed."; fi; \
	exit $$rc

# Check the vendored duck_block vocabulary against upstream, by NAME AND VALUE. A
# vendored copy and a submodule pin are both copies, and neither notices when upstream
# moves; more importantly, the C++ constants catch a rename but NOT a changed value,
# which compiles clean and silently stops matching. Skips cleanly (exit 0) when upstream
# is unreachable; pass --strict to make that a failure instead.
.PHONY: check-vocabulary
check-vocabulary:
	python3 scripts/check_duck_block_vocabulary.py

# The CONVERTER'S regression net, moved here ahead of the converter itself.
#
# duck_block_utils' rule for this relocation is FIX BEFORE MOVING: relocating buggy code
# relocates the bugs, so the net is green HERE first and the code moves under it. A
# regression after the move is then unambiguously caused by the move.
#
# During the handoff it runs against the converter still living upstream, loaded into
# panduck's binary. Once the converter is linked in locally, drop --load and the same
# assertions run unchanged -- which is the point: the net does not know which side of the
# move it is on.
.PHONY: check-converter
check-converter:
	python3 test/converter/check_roundtrip_sweep.py \
	  --load "$(realpath ../duckdb_duck_block_utils)/build/release/extension/duck_block_utils/duck_block_utils.duckdb_extension"

# Check every reader's output against duck_block_utils' pure-SQL conformance macros.
# Until this target existed, panduck's test suite checked panduck's own behaviour and
# NOTHING about duck_block conformance -- every such claim came from loading
# duck_block_utils by hand, once, interactively.
#
# The macros are READ FROM the upstream checkout, not copied into this tree: upstream
# already compares them against its own extension, and a copy here would be a third
# party to that agreement, checked by nobody. Skips cleanly (exit 0) when the checkout
# is absent; --strict makes that a failure. Override with:
#     python3 scripts/check_duck_block_conformance.py --upstream <path>
#
# They are pure SQL, which matters because panduck's duckdb submodule tracks the BRANCH
# v1.5-variegata rather than a tag. It sits on the v1.5.5 release commit today, so the
# real validator loads; one `git submodule update --remote` moves it off-release and the
# validator becomes unloadable by any route, while these keep working.
#
# The script self-tests against five constructed documents FIRST and fails if the
# defects are not caught or the conforming control is rejected -- a check that cannot
# fire reports the same clean output as one that can.
.PHONY: check-conformance
check-conformance:
	python3 scripts/check_duck_block_conformance.py

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
