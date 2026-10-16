"""The original images a witness cite can name, and which image a cited address is in.

docs/README.md's rule: an address is retail `Jointops.exe` unless its marker names another
image, and one marker never mixes two. docs/engine-primer.md section 2 lists the images;
IMAGES below mirrors that table (scripts/ida/cite_sweep.py checks the two agree whenever
docs/ is checked out). A marker spells an image by its file name, one spelling per image;
the short forms in ALIASES are the records' established spellings and are read as the same
image, while a ratified file name written any other way (`gobj` for `gobj.dll`) is
recognized but reported by `misspelled()`.

Shared by scripts/ida/cite_sweep.py (which joins only Jointops cites against the live IDB)
and scripts/lint/cite_census.py (which keys its census by image and address). Stdlib only.
"""

from __future__ import annotations

import functools
import re

DEFAULT_IMAGE = "Jointops.exe"

# Every image engine-primer section 2 lists, by its file name.
IMAGES = (
    "Jointops.exe",
    "dfx2.exe",
    "Dfx_new_ver.exe",
    "DFX.EXE",
    "dfx2med.exe",
    "jomed.exe",
    "JOTACmed.exe",
    "ModSuperOed.exe",
    "dfvas.exe",
    "dfbhd.exe",
    "jodemo.exe",
    "TrnGen.exe",
    "ParticleEdit_v1_1.exe",
    "Dflw.exe",
    "Nile.exe",
    "MDEdit.exe",
    "misldr.dll",
    "gobj.dll",
    "binkw32.dll",
    "UPDATE.EXE",
    "REVUPDAT.EXE",
    "pack.exe",
    "RAT.exe",
)

# Established short forms (matched case-sensitively, as whole words): the image they name.
ALIASES = {
    "retail": "Jointops.exe",
    "Jointops": "Jointops.exe",
    "dfx2med": "dfx2med.exe",
    "jomed": "jomed.exe",
    "dfvas": "dfvas.exe",
    "dfbhd": "dfbhd.exe",
    "jodemo": "jodemo.exe",
    "demo": "jodemo.exe",          # only as `(demo)`, see ALIAS_TOKEN
    "TrnGen": "TrnGen.exe",
    "ParticleEdit": "ParticleEdit_v1_1.exe",
    "Dflw": "Dflw.exe",
    "Nile": "Nile.exe",
    "MDEdit": "MDEdit.exe",
    "misldr": "misldr.dll",
    "binkw32": "binkw32.dll",
    # the ratified file names' other spellings: recognized, reported by misspelled()
    "gobj": "gobj.dll",
    "JOTACmed": "JOTACmed.exe",
    "ModSuperOed": "ModSuperOed.exe",
    "REVUPDAT": "REVUPDAT.EXE",
}

# One spelling each, decided 2026-10-15: these file names are written exactly so.
RATIFIED = ("gobj.dll", "JOTACmed.exe", "ModSuperOed.exe", "UPDATE.EXE", "REVUPDAT.EXE",
            "pack.exe")

# Executables and modules of our own (and the engine's host) are never witness images.
OURS = re.compile(r"^(?:opennova|OpennovaGodotClient|godot|libgodot)", re.I)

_BY_LOWER = {name.lower(): name for name in IMAGES}
FILE_TOKEN = re.compile(r"(?<![\w.\-/\\])([A-Za-z0-9_\-]+\.(?:exe|dll))\b", re.I)
ALIAS_TOKEN = re.compile(
    r"(?<![\w.\-/\\])(?:\((demo)\)|("
    + "|".join(sorted((a for a in ALIASES if a != "demo"), key=len, reverse=True))
    + r"))(?![\w\-])"
)
MARKER_OPEN = re.compile(r"\[orig:|\(retail:")
RANGE_LEAD = re.compile(r"^\s*\.\.\s*0x[0-9A-Fa-f]+\b")
# a qualifier right after an address: `@0x1 (dfx2.exe)`, `@0x1, misldr.dll`, `@0x1 (retail)`
TAIL_LEAD = re.compile(r"^[ \t,]*\(?[ \t]*")
# what may stand between an image named before an address and the address: a name at most
NAME_GAP = re.compile(r"[\s,:]*(?:[A-Za-z_~][\w:~]*\s*)?")
SPAN_CAP = 600


def _image_of(spelled: str, is_file: bool) -> str | None:
    if is_file:
        if OURS.match(spelled):
            return None
        return _BY_LOWER.get(spelled.lower(), spelled)
    return ALIASES.get(spelled)


@functools.lru_cache(maxsize=8192)
def image_tokens(text: str):
    """(start, end, image, spelled) for every image named in text, in order."""
    found = []
    for m in FILE_TOKEN.finditer(text):
        image = _image_of(m.group(1), True)
        if image:
            found.append((m.start(1), m.end(1), image, m.group(1)))
    for m in ALIAS_TOKEN.finditer(text):
        spelled = m.group(1) or m.group(2)
        start = m.start(1) if m.group(1) else m.start(2)
        if any(s <= start < e for s, e, _, _ in found):
            continue  # `TrnGen` inside `TrnGen.exe`
        found.append((start, start + len(spelled), ALIASES[spelled], spelled))
    return tuple(sorted(found))


def marker_close(text: str, start: int, opener: str = "[", stop: int | None = None) -> int:
    """The index of the bracket that closes a marker whose body starts at text[start] (after
    its `[orig:` or `(retail:`), counting nested pairs (`[eax+54h]` inside a marker is not
    its end); -1 when the text ends first."""
    depth = 1
    for m in _BRACKETS[opener].finditer(text, start, len(text) if stop is None else stop):
        depth += 1 if m.group(0) == opener else -1
        if depth == 0:
            return m.start()
    return -1


_BRACKETS = {"[": re.compile(r"[\[\]]"), "(": re.compile(r"[()]")}


def marker_span(text: str, pos: int) -> tuple[int, int]:
    """The marker an address at pos sits in: from its `[orig:` / `(retail:` to the bracket
    that closes it. An address outside every marker gets its own line."""
    line_start = text.rfind("\n", 0, pos) + 1
    opened = None
    for m in MARKER_OPEN.finditer(text, max(0, pos - SPAN_CAP), pos):
        opened = m
    if opened is not None:
        opener = opened.group(0)[0]
        closed = marker_close(text, opened.end(), opener, pos) >= 0
        if not closed:
            end = marker_close(text, opened.end(), opener, pos + SPAN_CAP)
            if end < 0:  # an unclosed marker: its own line only
                end = text.find("\n", pos)
                end = end - 1 if end >= 0 else len(text) - 1
            return opened.start(), end + 1
    line_end = text.find("\n", pos)
    return line_start, (line_end if line_end >= 0 else len(text))


@functools.lru_cache(maxsize=4096)
def clauses(seg: str):
    """(start, end) of each clause of a marker: its text split at the `;` and `|` that stand
    outside parentheses (`[orig: A @0x1 (the ';' @0x2), misldr.dll; B @0x3, JOTACmed.exe]`
    is two clauses)."""
    out, depth, begin = [], 0, 0
    for i, ch in enumerate(seg):
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth = max(0, depth - 1)
        elif ch in ";|" and depth <= 1:   # depth 1: inside the marker's own bracket
            out.append((begin, i))
            begin = i + 1
    out.append((begin, len(seg)))
    return tuple(out)


CLAUSE_HEAD = re.compile(r"(?:\[orig:|\(retail:|[;|])\s*$")


@functools.lru_cache(maxsize=4096)
def qualifiers(seg: str):
    """The image tokens of a marker that stand where a qualifier does, as opposed to a prose
    mention (`(NWJoin.dll PUB* cookies)`, `TrnGen's draw-back`): `(image)`, `(image, ...` /
    `(image: ...`, a list item `..., image;` / `, image]`, the marker's last word
    (`both misldr.dll]`), or a clause's head (`[orig: JOTACmed.exe sub_44E640 @ ...`,
    `; RAT.exe @...`)."""
    out = []
    for s, e, image, spelled in image_tokens(seg):
        prev = seg[:s].rstrip(" \t")
        nxt = seg[e:].lstrip(" \t")[:1]
        if prev.endswith("(") and nxt in (")", ",", ":", ";"):
            out.append((s, e, image, spelled))
        elif prev.endswith((",", ";")) and nxt in ("", ")", "]", ";", ",", "|"):
            out.append((s, e, image, spelled))
        elif nxt == "]":          # the marker's closing word: `...; both misldr.dll]`
            out.append((s, e, image, spelled))
        elif CLAUSE_HEAD.search(prev) or not prev:
            out.append((s, e, image, spelled))
    return tuple(out)


def _one_image(seg: str):
    """The one image a clause or marker qualifies, or None. `Jointops.exe` / `Jointops`
    named anywhere in it counts too (`@0x54dfb0 in retail Jointops.exe (jodemo: ...)` is
    mixed, not jodemo's)."""
    tokens = list(qualifiers(seg)) + [t for t in image_tokens(seg)
                                      if t[2] == DEFAULT_IMAGE and t[3] != "retail"]
    images = {t[2] for t in tokens}
    if len(images) == 1:
        return tokens[0][2], tokens[0][3]
    return None


def image_at(text: str, start: int, end: int):
    """(image, spelled) for the address text[start:end] (an `@ 0xADDR` or a range's far end),
    or None when nothing names an image (the address is the default image's). Order: a
    qualifier right after the address (past a `..0xEND` range: `(dfx2.exe)`, `, misldr.dll`,
    `(retail)`), one right before it (`RAT.exe @0x4012d2`, `gobj sub_1000B4A1 @0x...`), the
    one image its clause qualifies, then the one image its marker qualifies (`[orig: A @0x1,
    B @0x2 (dfx2.exe)]`; a marker that qualifies two images, `(retail)` among them, binds
    nothing beyond its qualified addresses)."""
    tail = text[end:end + 80]
    lead = RANGE_LEAD.match(tail)
    if lead:
        tail = tail[lead.end():]
    lead_len = TAIL_LEAD.match(tail).end()
    for s, e, image, spelled in image_tokens(tail[:lead_len + 40]):
        if s <= lead_len and tail[e:e + 1] not in ("'", "-"):   # not `TrnGen's`
            return image, spelled
        break
    span_start, span_end = marker_span(text, start)
    before = text[max(span_start, start - 60):start]
    tokens = image_tokens(before)
    if tokens:
        s, e, image, spelled = tokens[-1]
        if NAME_GAP.fullmatch(before[e:]):
            return image, spelled
    seg = text[span_start:span_end]
    rel = start - span_start
    for c0, c1 in clauses(seg):
        if c0 <= rel < c1:
            bound = _one_image(seg[c0:c1])
            if bound:
                return bound
            break
    bound = _one_image(seg)
    if bound and bound[0] != DEFAULT_IMAGE:
        return bound
    return None


def misspelled(image: str, spelled: str) -> bool:
    """A ratified file name written any other way (`gobj`, `JOTACmed`, `update.exe`)."""
    return image in RATIFIED and bool(spelled) and spelled != image
