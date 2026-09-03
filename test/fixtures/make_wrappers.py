#!/usr/bin/env python3
"""Build wrappers.docx / wrappers.odt: content hidden behind WRAPPER elements.

Both formats let an element sit between a container and the thing a reader is looking
for. A reader that iterates `children("w:r")` or matches a fixed tag list never sees
what is inside one, and the failure is SILENT -- the text simply is not in the output.

panduck shipped five of these at once (found by probing, not by the test suite):

    docx  <w:hyperlink>  anchor text and href      -- lost
          <w:ins>        tracked insertions        -- lost
          <w:sdt>        content controls          -- whole paragraphs lost
          <w:smartTag>   legacy entity markup      -- lost
          <w:fldSimple>  field results             -- lost
    odt   <text:section> a wrapped block           -- whole paragraphs lost
          <text:meta>    RDF-annotated text        -- lost
          <text:ruby>    ruby base text            -- lost

Two wrappers must stay INVISIBLE, and are here so a future "descend into everything"
fix cannot quietly break them:

    docx  <w:del>              text the author DELETED
    odt   <office:annotation>  a comment ABOUT the document

Regenerate:  python3 test/fixtures/make_wrappers.py
"""

import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent

DOCX_PROBE = (
    '<w:p><w:r><w:t xml:space="preserve">before </w:t></w:r>'
    '<w:ins w:id="900" w:author="a"><w:r><w:t xml:space="preserve">INSERTED</w:t></w:r></w:ins>'
    '<w:r><w:t xml:space="preserve"> after</w:t></w:r></w:p>'
    '<w:sdt><w:sdtPr /><w:sdtContent>'
    '<w:p><w:r><w:t xml:space="preserve">SDTPARA</w:t></w:r></w:p>'
    '</w:sdtContent></w:sdt>'
    '<w:p><w:smartTag w:element="x"><w:r><w:t xml:space="preserve">SMARTTAG</w:t></w:r></w:smartTag></w:p>'
    '<w:p><w:fldSimple w:instr="REF x"><w:r><w:t xml:space="preserve">FIELDTEXT</w:t></w:r></w:fldSimple></w:p>'
    '<w:p><w:r><w:t xml:space="preserve">kept </w:t></w:r>'
    '<w:del w:id="901" w:author="a"><w:r><w:delText>DELETEDWORD</w:delText></w:r></w:del>'
    '<w:r><w:t xml:space="preserve">tail</w:t></w:r></w:p>'
)

ODT_PROBE = (
    '<text:section text:name="S1"><text:p>SECTIONPARA</text:p></text:section>'
    '<text:p>meta <text:meta>METATEXT</text:meta> tail</text:p>'
    '<text:p>ruby <text:ruby><text:ruby-base>RUBYBASE</text:ruby-base>'
    '<text:ruby-text>GLOSS</text:ruby-text></text:ruby> tail</text:p>'
    '<text:p>note <office:annotation><text:p>COMMENTTEXT</text:p></office:annotation> tail</text:p>'
)


def inject(src, dst, member, anchor, probe):
    """Splice `probe` in before `anchor` inside one member of a zip container."""
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        with zipfile.ZipFile(src) as z:
            z.extractall(td)
        target = td / member
        text = target.read_text(encoding="utf-8")
        i = text.find(anchor)
        if i < 0:
            raise SystemExit(f"anchor {anchor!r} not found in {member}")
        target.write_text(text[:i] + probe + text[i:], encoding="utf-8")
        if dst.exists():
            dst.unlink()
        # `zip` rather than zipfile: it writes the member order and the stored mimetype
        # entry that ODF readers expect.
        subprocess.run(["zip", "-qr", str(dst), "."], cwd=td, check=True)


def main():
    for name, member, anchor, probe in (
        ("docx", "word/document.xml", '<w:p><w:pPr><w:pStyle w:val="Heading1" />', DOCX_PROBE),
        ("odt", "content.xml", "<text:h", ODT_PROBE),
    ):
        src = HERE / f"constructs.{name}"
        if not src.exists():
            raise SystemExit(f"missing base fixture {src}")
        dst = HERE / f"wrappers.{name}"
        inject(src, dst, member, anchor, probe)
        print(f"wrote {dst.relative_to(HERE.parent.parent)}")


if __name__ == "__main__":
    sys.exit(main())
