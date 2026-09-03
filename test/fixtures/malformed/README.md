# Deliberately malformed fixtures

Every other fixture in this tree is a well-formed document from a real writer. These are
not, and they live in a SUBDIRECTORY on purpose: `check-conformance` and `check-writeback`
glob `test/fixtures/*.<ext>` and are not recursive, so a broken file here cannot be swept
into a check that expects to read it successfully.

| file | damage | expected |
|---|---|---|
| `truncated.docx` | first 400 bytes of a real docx | `IO Error` naming the reader |
| `notzip.odt` | plain text with a `.odt` name | `IO Error` naming the reader |
| `badxml.docx` | valid ZIP, unclosed XML inside | `Invalid Input Error` naming the part |
| `unbalanced.rtf` | unclosed group, no trailing brace | degrades — RTF never throws |

The RTF row is the one worth stating: that reader documents "never throws on malformed
input -- unbalanced groups and unknown control words are tolerated", so its correct
behaviour is to return what it could read rather than to fail. The other three are
container formats where a damaged file cannot be partially honest, so they raise.
