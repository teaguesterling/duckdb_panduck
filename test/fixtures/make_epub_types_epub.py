#!/usr/bin/env python3
"""Build epub_types.epub -- block elements that carry EPUB 3 epub:type and XHTML class.

panduck #182 puts "a class the document gave this block" into attributes['class']. For
EPUB that is the XHTML `class` tokens and the EPUB 3 `epub:type` tokens. No real-writer
fixture has epub:type on a block that the reader keeps: pandoc puts it only on <body>,
<nav> and an empty titlepage <section>, and the reader drops all three. So this book has:

  * a chapter <section> with both class="level1" and epub:type="chapter"
  * a paragraph with two class tokens and doubled whitespace between them
  * an epigraph <blockquote epub:type="epigraph">
  * a footnote <aside epub:type="footnote"> and a paragraph with no class at all

Regenerate with: python3 test/fixtures/make_epub_types_epub.py
"""

import zipfile
from pathlib import Path

OUT = Path(__file__).resolve().parent / "epub_types.epub"

CONTAINER = """<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/book.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>
"""

OPF = """<?xml version="1.0" encoding="UTF-8"?>
<package version="3.0" xmlns="http://www.idpf.org/2007/opf" unique-identifier="id">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="id">urn:uuid:panduck-epub-types</dc:identifier>
    <dc:title>Epub Types</dc:title>
    <dc:language>en</dc:language>
  </metadata>
  <manifest>
    <item id="c1" href="ch1.xhtml" media-type="application/xhtml+xml"/>
  </manifest>
  <spine>
    <itemref idref="c1"/>
  </spine>
</package>
"""

CH1 = """<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">
<head><title>One</title></head>
<body>
<section class="level1" epub:type="chapter">
<h1 class="chapter-title">The First Chapter</h1>
<blockquote epub:type="epigraph"><p>An opening line.</p></blockquote>
<p class="lead  first">The lead paragraph.</p>
<p>A plain paragraph.</p>
<aside epub:type="footnote" id="fn1"><p>The note text.</p></aside>
</section>
</body>
</html>
"""


def main():
    with zipfile.ZipFile(OUT, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr(zipfile.ZipInfo("mimetype"), "application/epub+zip", zipfile.ZIP_STORED)
        z.writestr("META-INF/container.xml", CONTAINER)
        z.writestr("OEBPS/book.opf", OPF)
        z.writestr("OEBPS/ch1.xhtml", CH1)
    print(f"wrote {OUT.name}")


if __name__ == "__main__":
    main()
