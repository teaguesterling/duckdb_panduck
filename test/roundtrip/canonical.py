"""Canonical form for comparing panduck's duck_blocks against pandoc's AST.

Two independent readers of the same bytes will never agree byte-for-byte, and most of
the ways they disagree are meaningless. Measured on the RTF fixtures, pandoc and panduck
differ in three ways that carry no information:

  * pandoc emits explicit `Space` inlines; panduck folds spaces into text runs.
  * pandoc splits text per word (`A`, Space, `paragraph`); panduck emits coarse runs.
  * pandoc nests `Strong [Str "bold"]`; panduck puts the content on the `bold` inline.

This module defines the normal form those three collapse into, so a comparison can fail
on content and structure without failing on tokenization.

IMPORTANT: pandoc is the reference here, NOT duck_block_utils. Routing the reference
through `pandoc_ast_to_blocks` would compare panduck against a reader with known,
documented bugs -- it silently drops Figure, LineBlock and DefinitionList, and loses
Underline's text in both directions (see PANDOC_AST_GAPS.md in duckdb_duck_block_utils).
Comparing against pandoc's own JSON keeps the reference honest and drops a dependency.

Even so, pandoc is not ground truth either: its RTF reader loses the space after an
em-dash where panduck preserves it. Divergences are triaged in check_roundtrip.py rather
than assumed to be panduck's fault.
"""

import re
import unicodedata
from dataclasses import dataclass, field
from typing import List

# Inline markers. The syntax is deliberately unlike anything a document contains, so a
# marker in the output is always structure and never literal text.
OPEN, CLOSE, SEP = "«", "»", "|"


@dataclass
class CBlock:
    """One block in canonical form."""

    element_type: str
    heading_level: int = 0
    marked: str = ""
    #: Blocks this one contains, flattened in document order by the callers that care.
    children: List["CBlock"] = field(default_factory=list)


def _mark(tag: str, inner: str, arg: str = None) -> str:
    if arg is not None:
        return f"{OPEN}{tag}:{arg}{SEP}{inner}{CLOSE}"
    return f"{OPEN}{tag}:{inner}{CLOSE}"


def normalize_text(s: str) -> str:
    """Collapse whitespace and normalize Unicode composition.

    NFC matters because the same character can be encoded differently by different
    readers: an RTF `\\'e9` decodes to precomposed U+00E9, while a reader that saw
    `e` + combining acute would produce the decomposed form. They are the same text.
    """
    s = unicodedata.normalize("NFC", s)
    s = re.sub(r"\s+", " ", s)
    return s.strip()


# --------------------------------------------------------------------------- pandoc

#: Pandoc inline constructors that wrap content and map onto a duck_block inline type.
PANDOC_WRAPPERS = {
    "Emph": "i",
    "Strong": "b",
    "Strikeout": "s",
    "Underline": "u",
    "Superscript": "sup",
    "Subscript": "sub",
    "SmallCaps": "sc",
}

#: Constructors that are pure containers -- they contribute their children and nothing
#: of their own. Quoted's quote marks and Cite's citation metadata are presentational
#: for comparison purposes.
PANDOC_TRANSPARENT = {"Span", "Quoted", "Cite"}


def pandoc_inlines_to_marked(inlines) -> str:
    out = []
    for node in inlines or []:
        if not isinstance(node, dict):
            continue
        t, c = node.get("t"), node.get("c")
        if t == "Str":
            out.append(c if isinstance(c, str) else "")
        elif t in ("Space", "SoftBreak"):
            out.append(" ")
        elif t == "LineBreak":
            out.append(" ")
        elif t in PANDOC_WRAPPERS:
            out.append(_mark(PANDOC_WRAPPERS[t], pandoc_inlines_to_marked(c)))
        elif t == "Code":
            out.append(_mark("c", c[1] if isinstance(c, list) and len(c) > 1 else ""))
        elif t == "Math":
            out.append(_mark("m", c[1] if isinstance(c, list) and len(c) > 1 else ""))
        elif t == "Link":
            url = c[2][0] if isinstance(c, list) and len(c) > 2 else ""
            out.append(_mark("l", pandoc_inlines_to_marked(c[1]), url))
        elif t == "Image":
            src = c[2][0] if isinstance(c, list) and len(c) > 2 else ""
            out.append(_mark("img", pandoc_inlines_to_marked(c[1]), src))
        elif t == "RawInline":
            # Raw content is format-specific; keep the text so L0 still sees it.
            out.append(c[1] if isinstance(c, list) and len(c) > 1 else "")
        elif t in PANDOC_TRANSPARENT:
            # All three carry their inlines in the SECOND slot: Span is [Attr, inlines],
            # Quoted is [QuoteType, inlines], Cite is [[Citation], inlines]. Passing the
            # whole `c` recurses over the first slot too, which is a list -- so every
            # Span silently dropped its content. LibreOffice's RTF wraps headings and
            # accented text in Spans, so that lost whole paragraphs.
            inner = c[1] if isinstance(c, list) and len(c) > 1 else c
            out.append(pandoc_inlines_to_marked(inner))
        elif t == "Note":
            continue  # footnote bodies are a separate stream, not inline content
        elif isinstance(c, list):
            out.append(pandoc_inlines_to_marked(c))
    return "".join(out)


#: Block types whose identity is STRUCTURAL: they mean something while carrying no text of
#: their own, because the text is in the blocks they contain. A paragraph is not one of
#: them -- an empty paragraph is nothing in either model.
CONTAINER_TYPES = {"div", "blockquote", "list_item", "figure", "table", "hr", "section", "caption"}


def _drop_empty_paragraphs(blocks: List[CBlock]) -> List[CBlock]:
    """Remove text-free non-container blocks, symmetrically on both sides.

    pandoc's EPUB reader injects an empty Para per spine document as a cross-document link
    target -- pure bookkeeping, present in no other format's output and in no reader that
    is not pandoc. panduck's readers already skip whitespace-only paragraphs. Applying one
    rule to both sides is what makes that a shared convention rather than a divergence, and
    it must be symmetric or it would just be excusing one reader.
    """
    return [b for b in blocks if b.marked or b.element_type in CONTAINER_TYPES]


def pandoc_blocks_to_canonical(blocks, out=None) -> List[CBlock]:
    """Flatten a Pandoc block list into canonical blocks, in document order."""
    top_level = out is None
    if out is None:
        out = []
    for node in blocks or []:
        if not isinstance(node, dict):
            continue
        t, c = node.get("t"), node.get("c")
        if t == "Header":
            out.append(CBlock("heading", int(c[0]), normalize_text(pandoc_inlines_to_marked(c[2]))))
        elif t in ("Para", "Plain"):
            out.append(CBlock("paragraph", 0, normalize_text(pandoc_inlines_to_marked(c))))
        elif t == "CodeBlock":
            out.append(CBlock("code", 0, normalize_text(c[1] if isinstance(c, list) and len(c) > 1 else "")))
        elif t == "BlockQuote":
            out.append(CBlock("blockquote", 0, ""))
            pandoc_blocks_to_canonical(c, out)
        elif t in ("BulletList", "OrderedList"):
            items = c[1] if t == "OrderedList" else c
            for item in items or []:
                # A TIGHT list item's text belongs to the item. pandoc says so itself: it
                # uses Plain rather than Para for exactly this case, so folding on that
                # signal reads pandoc's own distinction rather than guessing. Without it,
                # <li>bullet one</li> is two blocks here and one everywhere else, and every
                # later position shifts -- which looks like a reader defect and is not one.
                item_blocks = list(item or [])
                text = ""
                if item_blocks and isinstance(item_blocks[0], dict) and item_blocks[0].get("t") == "Plain":
                    text = normalize_text(pandoc_inlines_to_marked(item_blocks[0].get("c")))
                    item_blocks = item_blocks[1:]
                out.append(CBlock("list_item", 0, text))
                pandoc_blocks_to_canonical(item_blocks, out)
        elif t == "DefinitionList":
            for term, defs in c or []:
                out.append(CBlock("list_item", 0, normalize_text(pandoc_inlines_to_marked(term))))
                for d in defs or []:
                    pandoc_blocks_to_canonical(d, out)
        elif t == "LineBlock":
            for line in c or []:
                out.append(CBlock("paragraph", 0, normalize_text(pandoc_inlines_to_marked(line))))
        elif t == "Figure":
            out.append(CBlock("figure", 0, ""))
            pandoc_blocks_to_canonical(c[2] if isinstance(c, list) and len(c) > 2 else [], out)
        elif t == "Div":
            out.append(CBlock("div", 0, ""))
            pandoc_blocks_to_canonical(c[1] if isinstance(c, list) and len(c) > 1 else [], out)
        elif t == "HorizontalRule":
            out.append(CBlock("hr", 0, ""))
        elif t == "Table":
            out.append(CBlock("table", 0, ""))
        elif t == "Null":
            continue
    return _drop_empty_paragraphs(out) if top_level else out


# ----------------------------------------------------------------------- duck_block

DUCKBLOCK_WRAPPERS = {
    "bold": "b",
    "italic": "i",
    "strikethrough": "s",
    "underline": "u",
    "superscript": "sup",
    "subscript": "sub",
    "smallcaps": "sc",
    "code": "c",
    "math": "m",
}


def duckblocks_to_canonical(rows) -> List[CBlock]:
    """Fold duck_block rows (block followed by its inline children) into canonical form.

    panduck emits inline runs flat with the content carried on the run itself. A run with
    empty content is a container whose children follow; those contribute nothing of their
    own here, so the children's own markers carry the structure.
    """
    out: List[CBlock] = []
    for row in rows:
        kind = row.get("kind")
        etype = row.get("element_type") or ""
        content = row.get("content") or ""
        if kind == "block":
            level = 0
            attrs = row.get("attributes") or {}
            if etype == "heading":
                raw = attrs.get("heading_level")
                if isinstance(raw, list):
                    raw = raw[0] if raw else None
                try:
                    level = int(raw)
                except (TypeError, ValueError):
                    level = 0
            out.append(CBlock(etype, level, normalize_text(content)))
        elif kind == "inline":
            if not out:
                out.append(CBlock("paragraph", 0, ""))
            if etype == "text":
                piece = content
            elif etype in ("space", "softbreak", "linebreak"):
                piece = " "
            elif etype == "link":
                piece = _mark("l", content, (row.get("attributes") or {}).get("href", ""))
            elif etype == "image":
                piece = _mark("img", content, (row.get("attributes") or {}).get("src", ""))
            elif etype in DUCKBLOCK_WRAPPERS:
                piece = _mark(DUCKBLOCK_WRAPPERS[etype], content) if content else ""
            else:
                piece = content
            # Accumulate raw and normalize once at the end: normalizing per-append
            # strips the trailing space off each run before the next is concatenated,
            # which silently welds "with " and "bold" into "withbold".
            out[-1].marked = out[-1].marked + piece
    for b in out:
        b.marked = normalize_text(b.marked)
    return _drop_empty_paragraphs(out)


# --------------------------------------------------------------------------- levels

_MARKER_RE = re.compile(rf"{OPEN}[a-z]+:(?:[^{SEP}{CLOSE}]*{re.escape(SEP)})?|{CLOSE}")


def strip_markers(s: str) -> str:
    return _MARKER_RE.sub("", s)


def level0(blocks: List[CBlock]) -> str:
    """All visible text, markers removed. Catches data loss and nothing else."""
    return normalize_text(" ".join(strip_markers(b.marked) for b in blocks if b.marked))


def level1(blocks: List[CBlock]):
    """Block skeleton: types and heading levels. Ignores all inline detail."""
    return [(b.element_type, b.heading_level) for b in blocks]


def level2(blocks: List[CBlock]):
    """Skeleton plus canonical inline markup."""
    return [(b.element_type, b.heading_level, b.marked) for b in blocks]


LEVELS = {"text": level0, "skeleton": level1, "marked": level2}
