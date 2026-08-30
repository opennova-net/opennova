#!/usr/bin/env python3
"""Cite sweep: join every `[orig: Name @ 0xADDR]` / `(retail: Name @0xADDR` marker in the
tree against the LIVE IDB and report the disagreements (the three-way-link drift check of
.claude/skills/grill-ida/LIFECYCLE.md section 4, made mechanical).

Needs either IDA running with the ida-pro-mcp plugin on http://127.0.0.1:13337/mcp and the
retail `Jointops.exe.kong.i64` loaded, or -- `--snapshot path.i64` -- an idat executable and
a copy of that database, which the sweep opens headless on a private copy (the snapshot is
never written; `--fix-reimpl --apply` is refused there). Either way the IDB's input-file
SHA-256 must equal the pin in docs/engine-primer.md section 2 (retail `Jointops.exe`,
imagebase 0x400000): a different hash is a different image, and the sweep stops before
joining. It is a maintainer tool, not a CI lint -- CI has no IDA.

For each marker the tool asks the IDB what lives at the address and classifies the pair:

  ok            the IDB name at the address is the cited name
  inner-site    the address is inside the cited function (a site cite: `Func @0xSITE`)
  call-site     an instruction at/after the address calls or references the cited symbol
                (the `Callee @0xCALLSITE` form)
  site-only     the marker names no symbol (`@0xADDR` alone, or a prose word before the @)
                and the address is a defined location -- accepted
  other-image   the marker carries another image's qualifier (dfx2med, ModSuperOed,
                misldr.dll, binkw32, jodemo, dfvas, TrnGen) -- skipped, this tool only knows the
                Jointops.exe IDB

  name-mismatch        a real symbol name is cited, the IDB names that address differently
  name-elsewhere       the cited symbol exists in the IDB, but at another address, and the
                       cited address neither lies inside it nor calls it
  unknown-name         the cited symbol-looking name does not exist in the IDB at all
  code-autoname        the marker cites an IDA auto-name (sub_/loc_/dword_/...) -- the IDB
                       has since named it, or never will
  idb-autoname         the IDB still carries an auto-name where the code has a real one
  undefined            the address is not code or data in the IDB
  malformed            the address is below the image (a dropped digit)
  unqualified-image    a DLL-range address (>= 0x10000000) with no image qualifier
  reimpl-stale         an IDB `reimpl:` entry comment names a repo path that no longer
                       exists (the reverse link half of the check)
  reimpl-moved         the named path exists but neither it nor its .h/.cpp sibling still
                       carries a marker inside the function -- another file does (a rename
                       kept the basename, or a push-down moved the port to its engine home)
  reimpl-orphan        the named path exists but no code file cites the function any more
                       (the port was deleted, or its marker dropped) -- adjudicate by hand

Exit status 1 when any of the defect classes is non-empty. Prints disagreements only;
`--all` prints every accepted row too, `--tsv PATH` writes the full join.

The reverse leg can also repair itself: `--fix-reimpl` prints one planned rewrite per
stale/moved link whose new home is unambiguous (the single citing file, else the citing
file whose basename matches the old one, else the single engine/ or godot/src home);
`--apply` writes those comment edits into the live IDB and `--save` runs idb_save after.
Orphans and ambiguous links are never rewritten, only listed.

Usage:
  python scripts/ida/cite_sweep.py                 # code + docs + reverse links
  python scripts/ida/cite_sweep.py --no-docs       # code markers only
  python scripts/ida/cite_sweep.py --tsv out.tsv   # keep the full join for adjudication
  python scripts/ida/cite_sweep.py --fix-reimpl    # plan the reverse-link rewrites (dry run)
  python scripts/ida/cite_sweep.py --fix-reimpl --apply --save   # write them, then idb_save
  python scripts/ida/cite_sweep.py --snapshot D:/ida_dbs/Jointops.exe.kong.i64   # headless, no IDA session
"""

from __future__ import annotations

import argparse
import bisect
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.request
from collections import Counter, defaultdict

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DEFAULT_URL = "http://127.0.0.1:13337/mcp"
URL = DEFAULT_URL  # the IDA MCP endpoint; --url overrides

CODE_ROOTS = ("engine", "godot/src", "godot/game", "godot/modtools", "godot/probes", "godot/shaders", "godot/tests",
              "apps", "tests", "assets", "fixtures")
DOC_ROOTS = ("docs",)
# every tracked text form a marker has been written in: sources, shaders, the engine-side
# .md records (ROADMAP.md), CMake lists
CODE_EXT = (".cpp", ".h", ".hpp", ".gd", ".py", ".gdshader", ".gdshaderinc", ".json", ".md", ".txt", ".cmake",
            ".mnu", ".mns", ".ptu")
DOC_EXT = (".md",)
COMMENT_LEAD = re.compile(r"^\s*(?://|##?|\*|--|;)+\s?")   # a wrapped marker's continuation line lead-in

MARKER_LINE = re.compile(r"\[orig:|\(retail:")
PAIR = re.compile(r"(?<![\w.}])(?:([A-Za-z_][A-Za-z0-9_]*(?:::~?[A-Za-z_][A-Za-z0-9_]*)*)\s*)?@\s*(0x[0-9A-Fa-f]{4,8})\b")
CAMEL = re.compile(r"^[A-Z][a-z0-9]+(?:[A-Z][A-Za-z0-9]*)+$")
QUALIFIER = re.compile(
    r"dfx2med|ModSuperOed|modsuperoed|misldr|binkw32|bink\.dll|jodemo|dfvas|Dflw|ParticleEdit|TrnGen|\(\s*demo\s*\)|\.dll\b",
    re.I,
)
RETAIL_TAG = re.compile(r"^\s*\(\s*(?:retail|Jointops(?:\.exe)?)\s*\)")
# a record that states another image in its preamble (docs/threedi/3di-gp-format-re.md: ModSuperOed / dfvas)
DOC_IMAGE = re.compile(r"(ModSuperOed|Dflw|dfvas|dfx2med|jodemo|ParticleEdit|TrnGen)[\s\S]{0,160}?(?:imagebase|IDB|\.i64|addresses)", re.I)
AUTO_PREFIXES = (
    "sub_", "loc_", "locret_", "dword_", "word_", "byte_", "unk_", "off_", "flt_",
    "dbl_", "stru_", "asc_", "nullsub_", "j_", "def_", "algn_", "qword_", "xmmword_",
)
LABELISH = re.compile(r"^(?:LABEL_\d+|var_[0-9A-Fa-f]+|arg_[0-9A-Fa-f]+|v\d+|a\d+|kong)$")
DEFECTS = (
    "name-mismatch", "name-elsewhere", "unknown-name", "code-autoname", "idb-autoname",
    "undefined", "malformed", "unqualified-image", "reimpl-stale", "reimpl-moved",
    "reimpl-orphan",
)
TEST_ROOTS = ("godot/tests/", "tests/")

# ---------------------------------------------------------------- MCP over HTTP

_SID = None


def rpc(method, params, rid=1, notify=False):
    global _SID
    body = {"jsonrpc": "2.0", "method": method, "params": params}
    if not notify:
        body["id"] = rid
    hdr = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream"}
    if _SID:
        hdr["Mcp-Session-Id"] = _SID
    req = urllib.request.Request(URL, data=json.dumps(body).encode(), headers=hdr)
    resp = urllib.request.urlopen(req, timeout=900)
    sid = resp.headers.get("Mcp-Session-Id")
    if sid:
        _SID = sid
    raw = resp.read().decode("utf-8", "replace")
    if notify or not raw.strip():
        return None
    payload = raw
    if raw.lstrip().startswith(("event:", "data:")):
        payload = "\n".join(l[5:].strip() for l in raw.splitlines() if l.startswith("data:"))
    return json.loads(payload)


def connect():
    rpc("initialize", {"protocolVersion": "2025-03-26", "capabilities": {},
                       "clientInfo": {"name": "cite-sweep", "version": "1"}})
    rpc("notifications/initialized", {}, notify=True)


def py_eval(code, rid):
    r = rpc("tools/call", {"name": "py_eval", "arguments": {"code": code}}, rid=rid)
    res = r["result"]
    txt = res["content"][0]["text"] if "content" in res else json.dumps(res)
    try:
        obj = json.loads(txt)
        if isinstance(obj, dict) and obj.get("stderr"):
            sys.stderr.write(obj["stderr"] + "\n")
        inner = obj.get("result") if isinstance(obj, dict) else obj
        return json.loads(inner) if isinstance(inner, str) else inner
    except json.JSONDecodeError:
        sys.stderr.write("py_eval reply not JSON (%d bytes; the server truncates large results -- lower --batch): %s...\n"
                         % (len(txt), txt[:200]))
        raise


def py_eval_big(code, rid):
    """py_eval for a reply the server's ~1 KB result cap would truncate: the IDA side leaves
    its `payload` as JSON in a temp file (IDA runs on this machine) and the client reads it."""
    fd, path = tempfile.mkstemp(prefix="cite_sweep_", suffix=".json")
    os.close(fd)
    py_eval("OUT = %r\n%s\nopen(OUT, 'w', encoding='utf-8').write(json.dumps(payload))\nresult = json.dumps('ok')"
            % (path, code), rid)
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    finally:
        os.remove(path)


# ---------------------------------------------------------------- the image pin

PIN_DOC = "docs/engine-primer.md"
PIN_ROW = re.compile(r"IDB input `Jointops\.exe` SHA-256 \| `([0-9a-f]{64})`")
SHA_CODE = "import ida_nalt, json\nresult = json.dumps(ida_nalt.retrieve_input_file_sha256().hex())"


def pinned_sha256():
    """The IDB input hash docs/engine-primer.md section 2 pins. The sweep joins against
    nothing else: a demo, another title, or an IDB rebuilt from another file all carry a
    different hash and would silently invalidate every address."""
    with open(os.path.join(REPO, PIN_DOC), encoding="utf-8") as f:
        m = PIN_ROW.search(f.read())
    if not m:
        sys.exit("[cite-sweep] %s no longer carries the `IDB input Jointops.exe SHA-256` pin row" % PIN_DOC)
    return m.group(1)


# ---------------------------------------------------------------- IDB backends
#
# A backend runs IDAPython snippets and hands back their values. A snippet is
# ("small", code) when it sets `result` to a JSON string (the MCP reply fits its ~1 KB cap)
# or ("big", code) when it leaves `payload` for the temp-file transport.

class LiveIdb:
    """The ida-pro-mcp endpoint of a running IDA: the read/write backend."""

    writable = True

    def __init__(self, url):
        global URL
        URL = url
        connect()
        self.input_sha256 = py_eval(SHA_CODE, rid=1)

    def run(self, snippets):
        out = []
        for i, (kind, code) in enumerate(snippets):
            out.append(py_eval_big(code, rid=100 + i) if kind == "big" else py_eval(code, rid=100 + i))
            sys.stderr.write("  idb %d/%d\n" % (i + 1, len(snippets)))
        return out

    def call(self, name, arguments, rid):
        return rpc("tools/call", {"name": name, "arguments": arguments}, rid=rid)


IDAT_CANDIDATES = (
    "C:/Program Files/IDA Essential 9.3/idat.exe",
    "C:/Program Files/IDA Pro 9.3/idat.exe",
    "C:/Program Files/IDA 9.3/idat.exe",
)


def find_idat(explicit):
    for cand in ([explicit] if explicit else []) + [shutil.which("idat"), shutil.which("idat64")] + list(IDAT_CANDIDATES):
        if cand and os.path.isfile(cand):
            return cand
    sys.exit("[cite-sweep] idat not found: pass --idat <path to idat.exe>")


class SnapshotIdb:
    """A `.i64` file opened headless by idat on a private copy (scripts/ida/idb_batch.py):
    the snapshot is never written and no IDA session is needed. Every snippet runs in ONE
    idat pass, so the whole sweep costs one database open."""

    writable = False

    def __init__(self, i64, idat):
        if not os.path.isfile(i64):
            sys.exit("[cite-sweep] no such database: %s" % i64)
        self.i64, self.idat, self.input_sha256 = i64, idat, None

    def run(self, snippets):
        work = tempfile.mkdtemp(prefix="cite_sweep_snap_")
        try:
            db = os.path.join(work, os.path.basename(self.i64))
            shutil.copyfile(self.i64, db)
            shutil.copyfile(os.path.join(REPO, "scripts", "ida", "idb_batch.py"), os.path.join(work, "idb_batch.py"))
            with open(os.path.join(work, "job.json"), "w", encoding="utf-8") as f:
                json.dump({"snippets": [code for _, code in snippets]}, f)
            # bare names + cwd=work: idat splits the -S value on spaces, so no path may carry one
            cmd = [self.idat, "-A", "-Lidat.log", "-Sidb_batch.py job.json out.json", db]
            sys.stderr.write("  idat %s (%d snippet(s))\n" % (os.path.basename(self.i64), len(snippets)))
            proc = subprocess.run(cmd, cwd=work, capture_output=True, text=True, timeout=1800)
            out_path = os.path.join(work, "out.json")
            if proc.returncode != 0 or not os.path.isfile(out_path):
                log = ""
                try:
                    with open(os.path.join(work, "idat.log"), encoding="utf-8", errors="replace") as f:
                        log = f.read()[-2000:]
                except OSError:
                    pass
                sys.exit("[cite-sweep] idat failed (rc=%d)\n%s%s%s" % (proc.returncode, proc.stdout[-1000:], proc.stderr[-1000:], log))
            with open(out_path, encoding="utf-8") as f:
                out = json.load(f)
        finally:
            shutil.rmtree(work, ignore_errors=True)
        self.input_sha256 = out["input_sha256"]
        values = []
        for i, r in enumerate(out["results"]):
            if not r["ok"]:
                sys.exit("[cite-sweep] snippet %d failed in idat:\n%s" % (i, r["error"]))
            values.append(r["value"])
        return values

    def call(self, name, arguments, rid):
        sys.exit("[cite-sweep] %s needs the live IDB: a snapshot is read-only" % name)


# ---------------------------------------------------------------- extraction

def tracked_files(roots, exts):
    out = subprocess.run(["git", "ls-files", "--", *roots], cwd=REPO,
                         capture_output=True, text=True, check=True).stdout
    return [p for p in out.split("\n") if p and p.endswith(exts)]


def symbol_like(tok):
    """A token that names a symbol, as opposed to a prose word before the `@` ("the gate
    @0x..."), a Hex-Rays local (`leanAngle`), a text-token key (`STRMISC_KILLEDBLUE`) or a
    label (`LABEL_19`). Symbol names in this IDB are `Subsystem_Action`, `CClass_Method`,
    `g_global`, or a CamelCase short form; the IDB name table is the tie-breaker."""
    if not tok or LABELISH.match(tok):
        return False
    if tok.startswith(AUTO_PREFIXES) or tok.startswith("g_") or "::" in tok:
        return True
    if not tok[0].isupper():
        return False
    if tok.isupper():          # STROVER_MISSIONOBJECTIVES, AABB, PRNG: keys and acronyms
        return False
    return "_" in tok or bool(CAMEL.match(tok))


def extract(paths):
    """Yield (path, line_no, token, addr, bracket_text) per address in every marker.
    bracket_text carries an image qualifier when the address itself is tagged (`@ 0x42ec20
    (ParticleEdit_v1_1.exe)`), when the bracket names another image and the address is not
    tagged `(retail)`, or when the record's preamble pins another image."""
    for path in paths:
        try:
            lines = open(os.path.join(REPO, path), encoding="utf-8", errors="replace").read().split("\n")
        except OSError:
            continue
        doc_image = path.startswith("docs/") and DOC_IMAGE.search("\n".join(lines[:40]))
        for i, line in enumerate(lines):
            if not MARKER_LINE.search(line):
                continue
            # a marker may wrap onto the next line(s); take up to 2 continuation lines, minus
            # their comment lead-in (`Name @ // 0x4c8750` is one marker, not a dropped cite)
            text = line
            if "]" not in line.split("[orig:")[-1] and "(retail:" not in line:
                text = " ".join([line] + [COMMENT_LEAD.sub("", l) for l in lines[i + 1:i + 3]])
            for m in PAIR.finditer(text):
                tok, addr = m.group(1), m.group(2).lower()
                tail = text[m.end():m.end() + 40]
                if RETAIL_TAG.match(tail):
                    ctx = "(retail)"
                elif QUALIFIER.match(tail.lstrip(" (")) or QUALIFIER.search(text) or doc_image:
                    ctx = "(other-image)"
                else:
                    ctx = ""
                yield path, i + 1, tok, addr, ctx


# ---------------------------------------------------------------- IDB join

IDB_CODE = r'''
import idc, ida_funcs, ida_bytes, idautils, json
AUTO = %r
out = {}
members = set()
try:
    for sidx, sid, sname in idautils.Structs():
        for off, mname, msize in idautils.StructMembers(sid):
            members.add(mname)
except Exception:
    pass  # IDA 9 dropped ida_struct; without the member table field cites fall to unknown-name
def refs_symbol(ea, tea, tok, f):
    """The nearest instruction in ea's function (or +-0x60) that calls/references the symbol
    at tea, or -- when the symbol is a short form -- calls a function whose name ends in
    `_tok`. Returns the site or None."""
    lo = max(ea - 0x60, f.start_ea if f else ea - 0x60)
    hi = min(ea + 0x60, f.end_ea if f else ea + 0x60)
    size = max(idc.get_item_size(tea), 4) if tea != idc.BADADDR else 0
    best = None
    for x in range(lo, hi):
        if idc.get_item_head(x) != x or not idc.is_code(idc.get_full_flags(x)): continue
        for op in (0, 1):
            t = idc.get_operand_type(x, op)
            if t not in (idc.o_near, idc.o_far, idc.o_imm, idc.o_mem, idc.o_displ): continue
            v = idc.get_operand_value(x, op)
            hit = (tea != idc.BADADDR and tea <= v < tea + size)
            if not hit and tok and t in (idc.o_near, idc.o_far):
                tn = idc.get_func_name(v) or idc.get_name(v) or ""
                hit = tn.endswith("_" + tok)
            if hit and (best is None or abs(x - ea) < abs(best - ea)):
                best = x
    return ("0x%%x" %% best) if best is not None else None
for key in KEYS:
    a, tok = key
    ea = int(a, 16)
    head = idc.get_item_head(ea)
    fl = idc.get_full_flags(head if head != idc.BADADDR else ea)
    nm = idc.get_name(ea) or ""
    f = ida_funcs.get_func(ea)
    fs = ("0x%%x" %% f.start_ea) if f else None
    fn = idc.get_func_name(ea) if f else ""
    in_data_seg = idc.get_segm_attr(ea, idc.SEGATTR_TYPE) == idc.SEG_DATA  # a table interior is a legitimate data cite
    defined = bool(f or idc.is_code(fl) or idc.is_data(fl) or nm or in_data_seg)
    if (tok and nm == tok) or (not tok and defined):
        continue  # trivially ok; omitted rows read as ok on the client side (keeps the reply small)
    tea = idc.get_name_ea_simple(tok) if tok else idc.BADADDR
    tea_s = ("0x%%x" %% tea) if tea != idc.BADADDR else None
    ref = refs_symbol(ea, tea, tok, f) if tok and tea != ea else None
    out[a + "|" + (tok or "")] = [nm, fs, fn, defined, tea_s, ref, bool(tok in members)]
result = json.dumps(out)
'''
OK_INFO = ["", None, "", True, None, None, False]  # an omitted key: the IDB name is the cited name


def classify(tok, addr, ctx, info):
    a = int(addr, 16)
    if ctx == "(other-image)":
        return "other-image"
    if a < 0x401000:
        return "malformed"
    if a >= 0x10000000:
        return "unqualified-image"
    nm, fs, fn, defined, tea, ref, member = info
    sym = symbol_like(tok)
    if not sym or member:
        # a prose word, a struct member, an unnamed site: only the address matters
        if tok and tea and (tea == addr or ref or fn == tok):
            return "ok" if tea == addr else "call-site"
        return "site-only" if defined else "undefined"
    if nm == tok:
        return "ok"
    if fn and fs != addr and (fn == tok or tok in fn):
        return "inner-site"   # `Func @0xSITE`, the short form `Method @0xSITE`, or an IDB `_0` twin
    if fs == addr and fn.endswith("_" + tok):
        return "short-form"   # `Method @0xFUNC` for `CClass_Method`: accepted, the full name is nicer
    if ref:
        return "call-site"
    if tok.startswith(AUTO_PREFIXES):
        if tea and not tok.startswith(("sub_", "loc_", "locret_", "nullsub_", "j_")):
            return "ok"           # a literal-pool / BSS auto-name the IDB still uses (flt_7C3B94 = 0.5)
        if tea:
            return "idb-autoname"  # an unnamed function cited by its auto-name: the IDB owes it a name
        return "code-autoname"
    if tea and tea != addr:
        return "name-elsewhere"
    if not defined and not fs:
        return "undefined"
    if nm.startswith(AUTO_PREFIXES):
        return "idb-autoname"
    if not tea:
        return "unknown-name" if not nm else "name-mismatch"
    return "name-mismatch"


# ---------------------------------------------------------------- reverse links

# A reverse link lives in one of four comment slots at the function entry: the function
# comment (slot 0), the repeatable function comment (1), the regular (2) or repeatable (3)
# instruction comment. The leg reads every slot and rewrites only the slot it found.

REIMPL_ROWS_CODE = r'''
import idc, idautils, ida_funcs, json
rows = []
for ea in idautils.Functions():
    f = None
    for i in range(4):
        c = idc.get_func_cmt(ea, i) if i < 2 else idc.get_cmt(ea, i - 2)
        if c and "reimpl:" in c:
            f = f or ida_funcs.get_func(ea)
            rows.append(["0x%x" % ea, "0x%x" % (f.start_ea if f else ea), "0x%x" % (f.end_ea if f else ea + 1),
                         idc.get_func_name(ea) or "", i, c])
payload = rows
'''

REIMPL_WRITE_CODE = r'''
import idc, json
n = 0
for a, slot, text in EDITS:
    ea = int(a, 16)
    ok = idc.set_func_cmt(ea, text, slot) if slot < 2 else idc.set_cmt(ea, text, slot - 2)
    n += 1 if ok else 0
result = json.dumps(n)
'''

REIMPL_LINE = re.compile(r"reimpl:\s*([^\n]+)")
REIMPL_TOKEN = re.compile(r"([A-Za-z0-9_][A-Za-z0-9_./-]*\.(?:cpp|h|hpp|gdshaderinc|gdshader|gd|py))(?![A-Za-z0-9])")


def stem_siblings(path):
    """`x.h` / `x.hpp` / `x.cpp` in one directory are one reimpl home: the marker sits on
    whichever side declares or defines the port."""
    base, ext = os.path.splitext(path)
    return {base + e for e in (".h", ".hpp", ".cpp")} - {path}


ORPHAN_OK = re.compile(r"unported|not ported|retired|deferr|no cite|no marker", re.I)   # a documented absence


def collapse_homes(homes):
    """Citing files grouped by directory + stem (`x.h` and `x.cpp` are one home); each group
    is represented by its .cpp when it has one."""
    groups = defaultdict(set)
    for h in homes:
        base, ext = os.path.splitext(h)
        groups[base if ext in (".h", ".hpp", ".cpp") else h].add(h)
    out = []
    for key, members in groups.items():
        cpp = [m for m in members if m.endswith(".cpp")]
        out.append(cpp[0] if cpp else sorted(members)[0])
    return out


def renamed_twin(old_tok, by_base):
    """The file a retired path became when only its name changed: the `nova_` prefix
    dropped (ADR 0040) or the pre-engine `libs/<lib>/src/` layout -- a unique tracked file
    with the surviving basename."""
    base = os.path.basename(old_tok)
    for cand in (base[5:] if base.startswith("nova_") else base,):
        same = by_base.get(cand, [])
        if len(same) == 1:
            return same[0]
    return None


def pick_home(homes, old_tok, path_exists, by_base):
    """The unambiguous new home for a link. A retired path follows its rename when the twin
    still exists, else the single citing file (siblings collapsed), else the single citing
    engine/ or godot/src file. A path that still EXISTS but lost its cite is rewritten only
    when exactly one file cites the function and it lives under engine/ (the push-down
    direction); everything else is listed for a hand adjudication."""
    if not path_exists:
        twin = renamed_twin(old_tok, by_base)
        if twin:
            return twin
    homes = collapse_homes(homes)
    if len(homes) == 1:
        return homes[0] if (not path_exists or homes[0].startswith("engine/")) else None
    if path_exists:
        return None
    for root in ("engine/", "godot/src/"):
        under = [h for h in homes if h.startswith(root)]
        if len(under) == 1:
            return under[0]
    return None


def classify_reimpl(rows, pairs, all_tracked):
    """Join each IDB reverse link against the tree. A link is ok when its path exists and
    that file (or a .h/.cpp sibling) cites an address inside the function; otherwise it is
    stale (path gone), moved (path exists, the cite lives elsewhere) or orphan (nothing
    cites the function). Returns (n_links, sweep_rows, planned_edits)."""
    tracked = set(all_tracked)
    by_base = defaultdict(list)
    for p in tracked:
        by_base[os.path.basename(p)].append(p)
    cites = sorted({(int(addr, 16), path) for path, _, _, addr, ctx in pairs
                    if ctx != "(other-image)" and not path.startswith(DOC_ROOTS + TEST_ROOTS)})
    addrs = [a for a, _ in cites]
    text_cache = {}

    def names_function(path, fname):
        """The file still names the function in prose (a marker without this address, a
        `[merged-into:]`, a short form): keep the link, a human decides."""
        if path not in text_cache:
            text_cache[path] = ""
            for p in (path,) + tuple(stem_siblings(path)):
                try:
                    text_cache[path] += open(os.path.join(REPO, p), encoding="utf-8", errors="replace").read()
                except OSError:
                    pass
        return bool(fname) and fname in text_cache[path]

    n, out, edits = 0, [], []
    for ea, start, end, name, slot, text in rows:
        lo, hi = int(start, 16), int(end, 16)
        homes = {p for _, p in cites[bisect.bisect_left(addrs, lo):bisect.bisect_left(addrs, hi)]}
        new_text = text
        for line in REIMPL_LINE.findall(text):
            n += 1
            for tok in REIMPL_TOKEN.findall(line):
                cands = [tok] if tok in tracked else ([] if "/" in tok else by_base.get(tok, []))
                if any(c in homes or stem_siblings(c) & homes for c in cands):
                    continue
                if cands and ORPHAN_OK.search(line):
                    continue          # the line documents the absence itself (an unported leg, a
                                      # retired port, a device-side leg the ratchets keep unmarked)
                if cands and any(names_function(c, name) for c in cands):
                    continue          # the file names the function without this address
                target = pick_home(homes, tok, bool(cands), by_base)
                cls = "reimpl-stale" if not cands else ("reimpl-moved" if homes else "reimpl-orphan")
                out.append((cls, ea, name, tok, target or "", "slot%d" % slot, line.strip()[:160],
                            " ".join(sorted(homes))[:200]))
                if target:
                    new_text = new_text.replace(tok, target)
        if new_text != text:
            edits.append((ea, slot, new_text))
    return n, out, edits


def apply_reimpl(idb, edits, batch):
    snippets = [("small", "EDITS = %s\n" % json.dumps(edits[i:i + batch]) + REIMPL_WRITE_CODE)
                for i in range(0, len(edits), batch)]
    return sum(idb.run(snippets))


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--no-docs", action="store_true", help="skip docs/ markers")
    ap.add_argument("--no-reimpl", action="store_true", help="skip the IDB reimpl: reverse-link leg")
    ap.add_argument("--all", action="store_true", help="print accepted rows too")
    ap.add_argument("--tsv", help="write the full join to this path")
    ap.add_argument("--batch", type=int, default=300)
    ap.add_argument("--fix-reimpl", action="store_true",
                    help="plan the reverse-link rewrites (stale/moved paths -> the file that carries the cite); dry run")
    ap.add_argument("--apply", action="store_true", help="with --fix-reimpl: write the planned rewrites into the IDB")
    ap.add_argument("--save", action="store_true", help="with --apply: idb_save afterwards")
    ap.add_argument("--url", default=DEFAULT_URL, help="the IDA MCP endpoint (default %(default)s)")
    ap.add_argument("--snapshot", metavar="I64",
                    help="join against this .i64 headless via idat (on a private copy; never written) instead of the live endpoint")
    ap.add_argument("--idat", help="the idat executable for --snapshot (default: PATH, then the IDA 9.3 install directories)")
    ap.add_argument("--no-pin-check", action="store_true",
                    help="report on an IDB whose input hash differs from the docs/engine-primer.md pin (to diagnose the mismatch only)")
    args = ap.parse_args()
    if args.snapshot and args.apply:
        sys.exit("[cite-sweep] --apply writes the IDB: run it against the live endpoint, not a snapshot")

    code_paths = tracked_files(CODE_ROOTS, CODE_EXT)
    doc_paths = [] if args.no_docs else tracked_files(DOC_ROOTS, DOC_EXT)
    pairs = list(extract(code_paths + doc_paths))
    keys = sorted({(addr, tok or "") for _, _, tok, addr, ctx in pairs
                   if ctx != "(other-image)" and 0x401000 <= int(addr, 16) < 0x10000000})

    idb = SnapshotIdb(args.snapshot, find_idat(args.idat)) if args.snapshot else LiveIdb(args.url)
    pin = pinned_sha256()
    if idb.input_sha256 and idb.input_sha256 != pin and not args.no_pin_check:
        sys.exit(pin_mismatch(idb.input_sha256, pin))
    snippets = [("small", "KEYS = " + json.dumps(keys[i:i + args.batch]) + "\n" + (IDB_CODE % (AUTO_PREFIXES,)))
                for i in range(0, len(keys), args.batch)]
    if not args.no_reimpl:
        snippets.append(("big", REIMPL_ROWS_CODE))
    values = idb.run(snippets)
    if idb.input_sha256 != pin:
        msg = pin_mismatch(idb.input_sha256, pin)
        if not args.no_pin_check:
            sys.exit(msg)
        sys.stderr.write(msg + "\n")
    info = {}
    for chunk in values[:len(values) - (0 if args.no_reimpl else 1)]:
        info.update(chunk)

    rows = []
    for path, ln, tok, addr, ctx in pairs:
        key = addr + "|" + (tok or "")
        hint = ""
        if key in info:
            cls = classify(tok, addr, ctx, info[key])
            nm, fs, fn = info[key][:3]
            hint = info[key][5] or ""
        elif ctx == "(other-image)" or not (0x401000 <= int(addr, 16) < 0x10000000):
            cls = classify(tok, addr, ctx, OK_INFO)
            nm, fs, fn = "", None, ""
        else:
            cls, nm, fs, fn = ("ok" if tok else "site-only"), (tok or ""), None, ""
        rows.append((cls, addr, tok or "", nm, fs or "", fn, "%s:%d" % (path, ln), hint))

    n_reimpl, edits = 0, []
    if not args.no_reimpl:
        every_tracked = subprocess.run(["git", "ls-files"], cwd=REPO, capture_output=True, text=True, check=True).stdout.split("\n")
        n_reimpl, reimpl_findings, edits = classify_reimpl(values[-1], pairs, [p for p in every_tracked if p])
        rows.extend(reimpl_findings)

    if args.tsv:
        with open(args.tsv, "w", encoding="utf-8") as f:
            f.write("class\taddr\tcited\tidb_name\tfunc_start\tfunc_name\tsite\tnearest_ref\n")
            for r in rows:
                f.write("\t".join(r) + "\n")

    counts = Counter(r[0] for r in rows)
    n_defect = sum(counts[c] for c in DEFECTS)
    print("[cite-sweep] %s sha256=%s; %d marker addresses (%d code files, %d docs), %d IDB reimpl links; %d disagreements: %s"
          % ("snapshot " + os.path.basename(args.snapshot) if args.snapshot else "live IDB", idb.input_sha256,
             len(pairs), len(code_paths), len(doc_paths), n_reimpl, n_defect,
             ", ".join("%s=%d" % (k, v) for k, v in sorted(counts.items()))))
    for r in sorted(rows, key=lambda r: (r[0], r[1], r[6])):
        if r[0] in DEFECTS or args.all:
            print("\t".join(r))
    if args.fix_reimpl:
        print("[fix-reimpl] %d comment rewrite(s) planned (%s)" % (len(edits), "applying" if args.apply else "dry run"))
        for ea, slot, text in edits:
            print("\t%s\tslot%d\t%s" % (ea, slot, " | ".join(REIMPL_LINE.findall(text))[:200]))
        if args.apply and edits:
            print("[fix-reimpl] %d written" % apply_reimpl(idb, edits, 100))
            if args.save:
                idb.call("idb_save", {}, rid=900)
                print("[fix-reimpl] idb_save done")
    return 1 if n_defect else 0


def pin_mismatch(actual, pin):
    return ("[cite-sweep] IDB input SHA-256 %s is not the pinned retail Jointops.exe %s (%s section 2): "
            "a demo, another title, or an IDB rebuilt from another file -- every address would be wrong. "
            "--no-pin-check reports anyway, for diagnosis only." % (actual, pin, PIN_DOC))


if __name__ == "__main__":
    sys.exit(main())
