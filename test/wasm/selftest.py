#!/usr/bin/env python3
"""Prove the WASM import check can FAIL before trusting it to pass.

A clean .wasm and a broken checker produce the same output: PASS. This builds two
minimal side modules by hand -- one importing a miniz symbol, one importing only a
host-provided libc symbol -- and asserts the checker separates them. Without this,
`run_wasm_checks.sh` reporting PASS on a real artifact carries no information.
"""

import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
FORBID = ['--forbid', '4pugi', '--forbid', '^mz_', '--forbid', '^tinfl_', '--forbid', '^tdefl_']


def _name(s):
    return bytes([len(s)]) + s.encode()


def module_importing(field):
    """Smallest valid side module that imports env.<field> as a function."""
    types = bytes([1, 4, 1, 0x60, 0, 0])  # one () -> () functype
    imp = bytes([1]) + _name("env") + _name(field) + bytes([0x00, 0x00])
    return b"\0asm" + struct.pack("<I", 1) + types + bytes([2, len(imp)]) + imp


def run(path):
    return subprocess.run(
        ['node', str(HERE / 'check_wasm_imports.mjs'), str(path)] + FORBID, capture_output=True, text=True
    ).returncode


def main():
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        cases = [
            ("mz_zip_reader_init", 1, "a miniz symbol must be REPORTED"),
            ("_ZN4pugi12xml_documentC1Ev", 1, "a pugixml symbol must be REPORTED"),
            ("malloc", 0, "a host-provided libc symbol must be IGNORED"),
        ]
        failures = []
        for field, want, why in cases:
            p = td / f"{field}.wasm"
            p.write_bytes(module_importing(field))
            got = run(p)
            status = "ok" if got == want else "FAILED"
            print(f"  [{status}] {why} (exit {got}, wanted {want})")
            if got != want:
                failures.append(why)

    if failures:
        print(
            f"\nself-test FAILED: the checker does not distinguish " f"{len(failures)} case(s); its PASS means nothing",
            file=sys.stderr,
        )
        return 1
    print("self-test passed: the checker reports dependency symbols and " "ignores host-provided ones")
    return 0


if __name__ == '__main__':
    sys.exit(main())
