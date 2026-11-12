#!/usr/bin/env python3
"""Build descriptor64.docx -- a valid ZIP in the shape duckdb-excel issue #110 reports.

duckdb/duckdb-excel#110 (open on 2026-11-12): minizip-ng, which excel reads .xlsx with,
fails on a real-world file whose ZIP entries all have this shape, while `unzip -t`, Python
`zipfile` and LibreOffice read it:

  * local header: general-purpose flag 0x0008 (sizes follow in a data descriptor),
    version needed 45, crc/csize/usize all 0, extra field length 0
  * data descriptor after the data: signature 50 4b 07 08, crc, then csize and usize
    as 8-BYTE fields -- the Zip64 width -- although there is no Zip64 extra field
  * central directory: version needed 20, ordinary 32-bit sizes, no Zip64 extra

panduck reads DOCX, ODT and EPUB through one ZIP layer (libpanduck/src/zip_container.cpp).
This fixture exists so that moving that layer from miniz to minizip-ng cannot silently
inherit #110: it must read before the move (miniz) and after it.

The members are taken from constructs.docx unchanged; only the container is rewritten.
Python's zipfile cannot write this shape, so the bytes are laid out by hand.

    python3 test/fixtures/make_descriptor64_docx.py
"""

import struct
import zipfile
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
SOURCE = HERE / "constructs.docx"
TARGET = HERE / "descriptor64.docx"

# A fixed DOS timestamp, so the output is byte-reproducible: 1980-01-01 00:00.
DOS_TIME, DOS_DATE = 0, (0 << 9) | (1 << 5) | 1


def raw_deflate(data: bytes) -> bytes:
    c = zlib.compressobj(9, zlib.DEFLATED, -15)
    return c.compress(data) + c.flush()


def build() -> bytes:
    with zipfile.ZipFile(SOURCE) as src:
        members = [(i.filename, src.read(i.filename)) for i in src.infolist()]

    out = bytearray()
    central = bytearray()
    for name, data in members:
        name_b = name.encode("utf-8")
        comp = raw_deflate(data)
        crc = zlib.crc32(data) & 0xFFFFFFFF
        offset = len(out)
        # Local header: version 45, flag 0x0008, method 8, crc/sizes 0, no extra.
        out += struct.pack("<IHHHHHIIIHH", 0x04034B50, 45, 0x0008, 8, DOS_TIME, DOS_DATE, 0, 0, 0, len(name_b), 0)
        out += name_b
        out += comp
        # Data descriptor with 8-byte sizes, the #110 shape.
        out += struct.pack("<IIQQ", 0x08074B50, crc, len(comp), len(data))
        # Central directory: version made 20, needed 20, 32-bit sizes, no extra.
        central += struct.pack(
            "<IHHHHHHIIIHHHHHII",
            0x02014B50,
            20,
            20,
            0x0008,
            8,
            DOS_TIME,
            DOS_DATE,
            crc,
            len(comp),
            len(data),
            len(name_b),
            0,
            0,
            0,
            0,
            0,
            offset,
        )
        central += name_b

    cd_offset = len(out)
    out += central
    out += struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, len(members), len(members), len(central), cd_offset, 0)
    return bytes(out)


def main() -> None:
    data = build()
    TARGET.write_bytes(data)
    # Self-check: Python's own reader accepts it and every member's CRC holds.
    with zipfile.ZipFile(TARGET) as z:
        bad = z.testzip()
        assert bad is None, f"CRC failure in {bad}"
    print(f"wrote {TARGET.name}: {len(data)} bytes")


if __name__ == "__main__":
    main()
