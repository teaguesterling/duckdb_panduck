#!/usr/bin/env python3
"""Build spine_order.epub -- the one EPUB property no writer's output can test.

pandoc and LibreOffice both emit single-chapter books whose ZIP member order happens to
match their spine. So neither fixture can tell "panduck follows the spine" apart from
"panduck reads members in archive order", which is the bug the spine exists to prevent.
This fixture makes the two disagree on purpose, and adds the path handling that a
real-world multi-directory book needs and a one-file book never exercises:

  * chapters are STORED in the archive in reverse of their reading order
  * the package document sits in its own directory, so manifest hrefs climb with ../
  * one chapter's filename contains a space, so its href is percent-encoded
  * a TABLE, whose text must survive even though its structure cannot yet be
    represented -- losing structure is a gap, losing text is a bug
  * a DEFINITION list, which must not be mislabelled as a bullet list -- duck_block
    has no settled shape for one, so the reader must not invent a list_type for it
  * an ORDERED list with a non-default start -- every <ol> in the two real-writer
    fixtures lives in a nav/toc document outside the spine, so nothing exercised
    ordered lists at all and `start`/`number_style`/`number_delim` went untested
  * each chapter links a DIFFERENT stylesheet, and chapter two USES a class that only
    chapter one's stylesheet defines -- so a leaked rule set is visible

    Testing that with a REDEFINED class does not work, and the failed attempt is worth
    recording: chapter two's own rule overwrites the leaked one, so the leak is masked and
    the assertion passes either way. Scoping is only observable through a name the second
    document uses and does NOT define.

Regenerate with: python3 test/fixtures/make_spine_order_epub.py
"""

import zipfile
from pathlib import Path

OUT = Path(__file__).resolve().parent / "spine_order.epub"

CONTAINER = """<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/pkg/book.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>
"""

# Manifest hrefs are relative to OEBPS/pkg/, so every one of them climbs out with ../ and
# the second chapter's space is percent-encoded the way a URL requires.
OPF = """<?xml version="1.0" encoding="UTF-8"?>
<package version="3.0" xmlns="http://www.idpf.org/2007/opf" unique-identifier="id">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="id">urn:uuid:panduck-spine-order</dc:identifier>
    <dc:title>Spine Order</dc:title>
    <dc:language>en</dc:language>
  </metadata>
  <manifest>
    <item id="css" href="../css/book.css" media-type="text/css"/>
    <item id="css2" href="../css/second.css" media-type="text/css"/>
    <item id="c1" href="../text/ch1.xhtml" media-type="application/xhtml+xml"/>
    <item id="c2" href="../text/ch%202.xhtml" media-type="application/xhtml+xml"/>
    <item id="cover" href="../images/cover.png" media-type="image/png"/>
  </manifest>
  <spine>
    <itemref idref="c1"/>
    <itemref idref="c2"/>
  </spine>
</package>
"""

CSS = """
/* A comment containing a { brace } so the parser has to strip comments first. */
.shout { font-weight: bold; }
.whisper { font-style: italic; }
.gone, .also-gone { text-decoration: line-through; }
.big { font-size: 24pt; font-weight: 700; }
/* Defined HERE and used only in chapter two, which links a different sheet. */
.only-in-book-css { font-weight: bold; }
p.tight { margin: 0; }
#not-a-class { font-weight: bold; }
.a .b { font-weight: bold; }
"""

# The SAME class name, meaning something else. Rules are gathered per content document, so
# chapter two must not inherit chapter one's .shout. That holds by construction today --
# the rule map is rebuilt per document and the cache is keyed by resolved path -- and "by
# construction" is a property of today's implementation, which is why it is asserted.
CSS2 = """
.shout { font-style: italic; }
"""  # note: NOT .only-in-book-css

CHAPTER = """<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml">
<head><title>%(title)s</title><link rel="stylesheet" type="text/css" href="../css/second.css"/></head>
<body>
<h1>%(title)s</h1>
<p><span class="shout">%(body)s</span></p>
<p><span class="only-in-book-css">Chapter one cannot reach me.</span></p>
</body>
</html>
"""

CH1 = """<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml">
<head><title>First</title><link rel="stylesheet" type="text/css" href="../css/book.css"/></head>
<body>
<section id="s1">
<h1>First Chapter</h1>
<p>A <span class="shout">loud</span> and <span class="whisper">quiet</span> and
<span class="gone">struck</span> sentence.</p>
<p class="tight">A <a href="ch%202.xhtml#top">cross reference</a> and an
<img src="../images/cover.png" alt="a cover"/> image.</p>
<p>Numeric weight is <span class="big">bold</span> too, but its font size says nothing.</p>
<ul><li>tight item</li><li><p>loose item</p></li></ul>
<blockquote><p>Quoted.</p></blockquote>
<div></div>
<hr/>
<pre>  indented   code</pre>
<ol start="3"><li>third</li></ol>
<dl><dt>term</dt><dd>definition</dd></dl>
<table><tr><td>cell one</td><td>cell two</td></tr></table>
</section>
</body>
</html>
"""

CH2 = CHAPTER % {"title": "Second Chapter", "body": "Read me second, stored me first."}


def main():
    with zipfile.ZipFile(OUT, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr(zipfile.ZipInfo("mimetype"), "application/epub+zip", zipfile.ZIP_STORED)
        z.writestr("META-INF/container.xml", CONTAINER)
        z.writestr("OEBPS/pkg/book.opf", OPF)
        z.writestr("OEBPS/css/book.css", CSS)
        z.writestr("OEBPS/css/second.css", CSS2)
        # REVERSED on purpose: chapter two is stored first, so reading the archive in
        # member order gets the book backwards.
        z.writestr("OEBPS/text/ch 2.xhtml", CH2)
        z.writestr("OEBPS/text/ch1.xhtml", CH1)
        z.writestr("OEBPS/images/cover.png", b"\x89PNG\r\n\x1a\n")
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
