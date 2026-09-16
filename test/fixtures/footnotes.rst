Footnotes and citations
=======================

Every form docutils accepts for a footnote or citation body, so the word-loss guard can
see each one. constructs.rst carries only the next-line form, which is why #67 -- a
one-line footnote's text dropped outright -- survived every guard until it was read for.

A paragraph with a footnote reference. [1]_ A citation reference too. [CIT2002]_

.. [1] The one-line footnote body.

.. [2] A footnote body that wraps
   onto a second line.

.. [CIT2002] A one-line citation body.

.. [3]
   A next-line footnote body.

.. [4]
   A next-line footnote body that wraps
   onto a second line.

.. This is an ordinary comment and produces nothing.

.. An ordinary comment
   with an indented body, which also produces nothing.

A closing paragraph.
