"""Build a minimal, runnable retail JO install for ONE single-player mission.

Given a full retail install (base archives + an expansion), retail ``/FRISK`` file-access traces
of play sessions, and a mission stem, this script computes the closure of files that mission needs
(trace + hardcoded boot/menu/mission names + static closure over the trimmed defs, models, particle
libs, menus, terrain, sound banks), resolves every name through the engine-faithful VFS with the
expansion layered on top (so the expansion becomes the new base), trims the defs to what the
mission reaches (items/weapon/ammo/powerup/SndProf), moves the expansion sound bank into the base
bank-slot chain, prunes what only the dropped weapons reached, and writes the result next to the
retail exe -- loose (flat, for ``/d``: a ptl/ subfolder for particle libs, DDS textures archived
in ``localres.pff``) or as the three boot-table archives. Nothing here is a general tool -- it is
the recipe for ``~/Desktop/MINIMAL`` and stays deliberately small.

    uv run python apps/retail_minimal.py trace-parse --log .scratch/minimal/frisk_run1.txt         -o apps/retail_minimal_00tra.txt
    uv run python apps/retail_minimal.py plan  --game "C:/GAMES/JOTAC/Game/JO" --exp revx02         --names apps/retail_minimal_00tra.txt --protect apps/retail_minimal_00tra_min.txt
    uv run python apps/retail_minimal.py build --game ... --exp revx02 --names ... --protect ...         --out "C:/Users/taylor/Desktop/MINIMAL" [--layout pff]
    uv run python apps/retail_minimal.py verify --out "C:/Users/taylor/Desktop/MINIMAL"         --log "C:/Users/taylor/Desktop/MINIMAL/_filelog.txt"

Retail then runs as `Jointops.exe /w /d` (loose layout) or `Jointops.exe /w` (pff layout).

Witness sources: docs/required-resources.md (the hardcoded boot set), docs/vfs/vfs-pff-mount-re.md
(slot precedence, /FRISK), docs/audio/lwf-dbf-sound-re.md (bank slots + random member selection).
"""

from __future__ import annotations

import argparse
import ctypes
import json
import os
import re
import shutil
import struct
import sys
from collections import defaultdict
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT))

from pyopennova._native import load_lib  # noqa: E402
from pyopennova import gameprofile_ffi  # noqa: E402
from pyopennova.vfs_ffi import Vfs  # noqa: E402

# --------------------------------------------------------------------------------------------
# Configuration (00TRa on the JOTAC install)
# --------------------------------------------------------------------------------------------

MISSION_STEM = "00TRa"

# The mission's own armory (names 00TRa.bms carries) -- the trimmed weapon.def keeps exactly
# these plus every WPN_* items.def references (emplaced/vehicle guns; text only).
ARMORY = [
    "WPN_KNIFE", "WPN_M4AUTO", "WPN_M4", "WPN_colt45",
    "WPN_GRENADEHE", "WPN_GRENADEFB", "WPN_GRENADESM", "WPN_AT4",
]

# Boot / menu / mission-start literals [docs/required-resources.md + the port's own literals].
# Each is "include if it resolves" -- misses are reported, not fatal (retail skips most gracefully).
HARDCODED = [
    # fatal / boot
    "gameerr.bin", "gametext.bin", "vmacros.bin", "keyhelp.bin", "items.def", "main.mnu",
    "weapon.def", "Avatars.def", "SndProf.def", "charattr.def", "loading.pcx", "ammo.def",
    "powerup.def", "hudfx.def", "hudpos.def", "farppos.def", "Wingpos.def",
    # menu
    "game.bin", "menutxt.bin", "menu_style.mns", "brand.mns", "menu.lwf",
    "menumus.bin", "gamemus.bin", "medmssn.bin", "newarow1.tga",
    "PI_Idle.BAD", "PI_actv.BAD", "PI_LookR.BAD", "PI_lookL.BAD", "Dt1rst.bad", "HwmCube.dds",
    "Arial12b.fnt", "Arial14n.fnt", "Arial14b.fnt", "Arial16n.fnt", "Arial16b.fnt",
    "Impac22b.fnt", "Impac38b.fnt", "Arials18.fnt", "Arial22.fnt", "couri20b.fnt",
    "Gunpl22b.fnt", "Gunpb18b.fnt",
    "sp.mnu", "pre.mnu", "splash.mnu", "options.mnu", "player.mnu", "loadout.mnu", "weapon.mnu",
    # mission
    "failsafe.bad", "game.wac", "server.wac", "loadscrn.pcx", "cmap.mnu", "game.mnu",
    "vehicle.mnu", "stat.mnu", "death.mnu", "monogram.tga", "boxtile.tga", "border.tga",
    "MFD1.PCX", "upl.3di", "msun.3di", "fmoon4.3di", "mglare.3di", "mstar.3di",
    "Cloud01.pcx", "Cloud01b.pcx", "TSDicon.tga", "compring.tga", "WPIndctr.tga",
    "Binoculr.tga", "BinoCH.tga", "BNumbers.tga", "NVG.tga", "Nvgscale.tga",
    "gamelocl.lwf", "game.lwf", "game2.lwf", "game3.lwf",
    # loaded at every mission start regardless of weapon.def (the hardcoded 3rd-person table
    # @ 0x83b490 + player bodies): witnessed in the trimmed-def /FRISK run
    "Bino_3rd.3di", "nvg.3di", "parachut.3di", "US01.3di", "Indo01.3di",
] + [f"cross{i:02d}.tga" for i in range(1, 21)]

# Loose layout: particle libraries are enumerated as `ptl\*.ptl` [orig: Jointops.exe string
# "ptl\*.ptl"]; everything else resolves flat next to the exe.
LOOSE_SUBDIR = {"ptl": "ptl"}
# Loose layout: DDS textures MUST stay archived. Texture_LoadByNameWithChannel @ 0x58b470 truncates
# the model's texture name after the first extension ("KCNet1.dds.tga" -> "KCNet1.dds"), and under
# /d probes that literal name loose (File_CheckExists @ 0x75a5d0 = _lopen); a hit jumps to the
# TGA/MDT/PCX archive branch (@ 0x58b649), which returns 0 for a .DDS name -> checkerboard. Only
# when no loose file answers to the .dds name does it take the DDS branch (@ 0x58b556, via
# FileSystem_FileExists -> archives). A .tga-named request with a loose .dds twin is fine (the
# DDS branch finds the loose twin), so keeping every .dds in the archive is the one safe rule.
LOOSE_ARCHIVED_EXTS = {"dds"}

# Retail ships these loose at the game root (opened by path, not through the archives).
LOOSE_ROOT = ["menumus.sbf", "gamemus.sbf", "nw_cdata.coo", "cc.bin"]
# Loose in the expansion dir; flattened into the archives.
LOOSE_EXPANSION_TO_PFF = ["loadscrn.pcx"]
# Retail EXE + the DLL it imports (the install's binkw32.dll is a hook shim; binkw32_.dll is Bink).
EXE_FILES = {"Jointops.exe": "Jointops.exe", "binkw32_.dll": "binkw32.dll"}

# Per-mission sibling family (witnessed placement: bms/til/dbf/wac -> localres, bin/lwf/pcx -> language).
MISSION_EXTS = ["bms", "bin", "dbf", "lwf", "pcx", "til", "wac"]
# Files that belong to OTHER missions and must not ride along (mission-list scans load them all).
MISSION_LIST_EXTS = {"bms", "npz", "npj"}

ASSET_EXT_RE = re.compile(
    r"([A-Za-z0-9_\-]+\.(?:3di|tga|dds|pcx|mdt|wav|bad|adm|ptl|ptu|env|trn|cpt|til|lwf|dbf|wac|png|fnt|mnu|mns))\b",
    re.IGNORECASE,
)
SOUND_KEYS = ("soundset", "soundsetend", "soundfireloop", "soundhead", "soundtrailoff",
              "soundlockedtone", "sound", "soundstart", "soundend", "soundloop", "sound_profile")
GFX_KEYS = ("gfx1", "gfx1a", "gfx1b", "gfx3", "gfx", "graphic", "graphic1", "graphic_us", "graphic_ru",
            "gfx2", "gfxlod1", "gfxlod2", "gfxlod3", "gfxlod4", "model", "graphicenemy", "husk")
# Formats whose bytes name other assets (textures/models/wavs/anims) and are followed as a closure.
NAMING_EXTS = {"ptl", "ptu", "mnu", "mns", "env", "trn", "til", "wac", "aip", "def"}


# --------------------------------------------------------------------------------------------
# Source install: engine-faithful mount (expansion layered on the base), plus per-archive sizes
# --------------------------------------------------------------------------------------------

class PffHeader(ctypes.Structure):
    _fields_ = [("header_size", ctypes.c_uint32), ("magic", ctypes.c_uint32),
                ("num_entries", ctypes.c_uint32), ("entry_size", ctypes.c_uint32),
                ("file_table_offset", ctypes.c_uint32)]


class PffEntry(ctypes.Structure):
    _fields_ = [("flags", ctypes.c_uint32), ("offset", ctypes.c_uint32), ("size", ctypes.c_uint32),
                ("timestamp", ctypes.c_uint32), ("filename", ctypes.c_char * 16),
                ("checksum", ctypes.c_uint32)]


class PffArchive(ctypes.Structure):
    _fields_ = [("header", PffHeader), ("entries", ctypes.POINTER(PffEntry)),
                ("entry_count", ctypes.c_uint32), ("_file", ctypes.c_void_p),
                ("_path", ctypes.c_char * 260)]


class PffWriteStreamEntry(ctypes.Structure):
    _fields_ = [("name", ctypes.c_char_p), ("size", ctypes.c_uint32), ("flags", ctypes.c_uint32),
                ("timestamp", ctypes.c_uint32), ("checksum", ctypes.c_uint32)]


PFF_READ_FN = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32,
                               ctypes.POINTER(ctypes.c_uint8), ctypes.c_uint32)
PFF_FORMAT_PFF4 = 1
_pff_bound = False


def _bind_pff():
    global _pff_bound
    if _pff_bound:
        return
    lib = load_lib()
    lib.pff_open.argtypes = [ctypes.POINTER(PffArchive), ctypes.c_char_p]
    lib.pff_open.restype = ctypes.c_int
    lib.pff_close.argtypes = [ctypes.POINTER(PffArchive)]
    lib.pff_close.restype = None
    lib.pff_write_archive_streamed.argtypes = [
        ctypes.c_char_p, ctypes.c_int, ctypes.POINTER(PffWriteStreamEntry), ctypes.c_uint32,
        PFF_READ_FN, ctypes.c_void_p]
    lib.pff_write_archive_streamed.restype = ctypes.c_int
    _pff_bound = True


def bfc1_inflate(raw: bytes) -> bytes:
    lib = load_lib()
    lib.bfc1_uncompressed_size.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_uint32)]
    lib.bfc1_uncompressed_size.restype = ctypes.c_int
    lib.bfc1_decompress.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.POINTER(ctypes.c_size_t)]
    lib.bfc1_decompress.restype = ctypes.c_int
    usize = ctypes.c_uint32(0)
    if lib.bfc1_uncompressed_size(raw, len(raw), ctypes.byref(usize)) != 0:
        raise RuntimeError("bfc1 header")
    out = ctypes.create_string_buffer(usize.value)
    n = ctypes.c_size_t(usize.value)
    if lib.bfc1_decompress(raw, len(raw), out, ctypes.byref(n)) != 0:
        raise RuntimeError("bfc1 decompress")
    return out.raw[:n.value]


def archive_entries(pff_path: str) -> dict[str, tuple[int, int, int, int]]:
    """name.lower() -> (size, flags, timestamp, checksum) for every entry of one archive."""
    _bind_pff()
    lib = load_lib()
    ar = PffArchive()
    if lib.pff_open(ctypes.byref(ar), pff_path.encode()) != 0:
        raise RuntimeError(f"pff_open failed: {pff_path}")
    try:
        out = {}
        for i in range(ar.entry_count):
            e = ar.entries[i]
            out[e.filename.decode("latin-1").lower()] = (e.size, e.flags, e.timestamp, e.checksum)
        return out
    finally:
        lib.pff_close(ctypes.byref(ar))


class Source:
    """The full retail install, mounted the way retail mounts it (expansion slots first)."""

    BASE_TABLE = ("language.pff", "localres.pff", "resource.pff")  # slots 2..4 [orig: @ 0x829f90]

    def __init__(self, game_root: str, expansion: str):
        self.root = Path(game_root)
        self.expansion = expansion
        # Mount by hand in the witnessed slot order (the C-ABI mount_game deliberately scans EVERY
        # *.pff -- D-VFS-2 -- which would let med.pff shadow resource.pff alphabetically).
        self.vfs = self._mount(expansion)
        self.vfs.set_scr_policy(gameprofile_ffi.scr_policy_for_code("jo"))
        # name.lower() -> (name, archive path) for the WINNING source of every logical name.
        self.index: dict[str, tuple[str, str]] = {}
        for i in range(self.vfs.file_count()):
            name, src, arch = self.vfs.file_at(i)
            self.index.setdefault(name.lower(), (name, arch))
        # Base-only mount (no expansion) tells us which base archive a name lives in.
        base = self._mount(None)
        self.base_archive: dict[str, str] = {}
        for i in range(base.file_count()):
            name, src, arch = base.file_at(i)
            self.base_archive.setdefault(name.lower(), os.path.basename(arch).lower())
        base.close()
        self._sizes: dict[str, dict[str, int]] = {}
        self.mission_stems = {n[:-4].lower() for n in self.index if n.endswith(".bms")}

    def _mount(self, expansion: str | None) -> Vfs:
        v = Vfs()
        if expansion:
            exp_dir = self.root / "expansion" / expansion
            local = exp_dir / f"{expansion}L.pff"
            if local.exists() and not v.set_primary_archive(str(local)):
                raise RuntimeError(v.last_error())
            if not v.add_secondary_archive(str(exp_dir / f"{expansion}.pff")):
                raise RuntimeError(v.last_error())
        for slot in self.BASE_TABLE:
            p = self.root / slot
            if p.exists() and not v.add_secondary_archive(str(p)):
                raise RuntimeError(v.last_error())
        return v

    def close(self):
        self.vfs.close()

    def has(self, name: str) -> bool:
        return name.lower() in self.index

    def canonical(self, name: str) -> str:
        return self.index[name.lower()][0]

    def archive_of(self, name: str) -> str:
        return os.path.basename(self.index[name.lower()][1]).lower()

    def entry_of(self, name: str) -> tuple[int, int, int, int]:
        """(size, flags, timestamp, checksum) of the winning archive entry."""
        arch = self.index[name.lower()][1]
        if arch not in self._sizes:
            self._sizes[arch] = archive_entries(arch)
        return self._sizes[arch][name.lower()]

    def size_of(self, name: str) -> int:
        return self.entry_of(name)[0]

    def read(self, name: str) -> bytes:
        """SCR/BFC1-decoded bytes: what a consumer sees. Loose files ship decoded -- retail's loose
        loader hands BFC1-compressed textures/wavs to the decoders undecompressed (checkerboards)."""
        data = self.vfs.read_file(name, decode=True)
        if data is None:
            raise KeyError(name)
        return data

    def read_loose(self, name: str) -> bytes:
        """What a loose file must contain: BFC1 payloads decompressed (retail's loose loader does
        not inflate them -- textures come out checkerboard), but SCR envelopes kept intact (the
        shader/particle/def consumers decrypt SCR themselves; handing them plain text garbles it and
        every shader-driven object vanishes)."""
        raw = self.read_raw(name)
        if raw[:4] == b"BFC1":
            return bfc1_inflate(raw)
        return raw

    def read_raw(self, name: str) -> bytes:
        """The stored archive bytes (container-decrypted only) -- what a repacked archive carries."""
        data = self.vfs.read_file(name, decode=False)
        if data is None:
            raise KeyError(name)
        return data

    def text(self, name: str) -> str:
        """SCR-decoded text (several retail .adm/.def payloads are SCR-encrypted)."""
        data = self.vfs.read_file(name, decode=True)
        if data is None:
            raise KeyError(name)
        return data.decode("latin-1")

    def names_with_ext(self, ext: str) -> list[str]:
        return [v[0] for k, v in self.index.items() if k.endswith("." + ext)]


# --------------------------------------------------------------------------------------------
# /FRISK trace
# --------------------------------------------------------------------------------------------

def parse_frisk(paths: list[str]) -> tuple[set[str], set[str]]:
    """Return (pff names, loose paths) named by one or more retail _filelog.txt files."""
    pff, loose = set(), set()
    for p in paths:
        for line in Path(p).read_text(encoding="latin-1", errors="replace").splitlines():
            line = line.strip()
            if line.startswith("PFF LOADED FILE:"):
                pff.add(line.split(":", 1)[1].strip())
            elif line.startswith("LOADED FILE:"):
                loose.add(line.split(":", 1)[1].strip())
    return pff, loose


def read_names_file(path: str) -> tuple[set[str], set[str]]:
    pff, loose = set(), set()
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("loose:"):
            loose.add(line[len("loose:"):])
        else:
            pff.add(line)
    return pff, loose


# --------------------------------------------------------------------------------------------
# Static closure helpers
# --------------------------------------------------------------------------------------------

def lwf_sets(data: bytes) -> dict[str, set[str]]:
    """set name (lower) -> member wav basenames (lower). Layout per docs/audio/lwf-dbf-sound-re.md
    (header 28 / single 52 / multi 80 with name @+4 / playlist 48 / sndparm 28)."""
    hs, magic, single_count, trig, mho, spo, _ = struct.unpack_from("<7I", data, 0)
    singles = []
    for i in range(single_count):
        off = hs + i * 52
        po, = struct.unpack_from("<I", data, off + 48)
        p = data[po:po + 256].split(b"\0", 1)[0].decode("latin-1")
        singles.append(os.path.basename(p.replace("\\", "/")).lower())
    mhs, multi_count, mto, _, _ = struct.unpack_from("<5I", data, mho)
    out: dict[str, set[str]] = {}
    for i in range(multi_count):
        m = mto + i * 80
        name = data[m + 4:m + 28].split(b"\0", 1)[0].decode("latin-1").lower()
        pl_count, = struct.unpack_from("<I", data, m + 36)
        pl_offs = struct.unpack_from("<8I", data, m + 40)
        wavs = out.setdefault(name, set())
        for pl in pl_offs[:min(pl_count, 8)]:
            mem_count, = struct.unpack_from("<I", data, pl)
            mem_offs = struct.unpack_from("<8I", data, pl + 16)
            for so in mem_offs[:min(mem_count, 8)]:
                si, = struct.unpack_from("<I", data, so)
                if si < len(singles):
                    wavs.add(singles[si])
    return out


def def_blocks(text: str, kind: str) -> dict[str, str]:
    """`<kind> "NAME" ... end` blocks (nested action...end honoured; case-insensitive) -> name -> block text."""
    lines = text.splitlines(keepends=True)
    blocks: dict[str, str] = {}
    start_re = re.compile(rf'^\s*{kind}\s+"?([^"\s]+)"?', re.IGNORECASE)
    i = 0
    while i < len(lines):
        m = start_re.match(lines[i])
        if not m:
            i += 1
            continue
        depth, j = 0, i + 1
        while j < len(lines):
            tok = lines[j].split("//", 1)[0].strip().split()
            head = tok[0].lower() if tok else ""
            if head in ("action", "effects_table"):
                depth += 1
            elif head == "end":
                if depth == 0:
                    break
                depth -= 1
            j += 1
        blocks[m.group(1)] = "".join(lines[i:j + 1])
        i = j + 1
    return blocks


def def_refs(block: str) -> tuple[set[str], set[str], set[str], set[str]]:
    """(3di stems, adm stems, sound set names, other asset filenames) referenced by a def block."""
    gfx, adm, snd, other = set(), set(), set(), set()
    for raw in block.splitlines():
        line = raw.split("//", 1)[0].strip()
        if not line:
            continue
        tok = line.replace('"', " ").split()
        key = tok[0].lower()
        vals = tok[1:]
        if key in GFX_KEYS and vals:
            gfx.add(vals[0])
        elif key in ("animadm", "anim_def") and vals:
            adm.add(vals[0])
        elif key in SOUND_KEYS and vals:
            snd.add(vals[0].lower())
        for m in ASSET_EXT_RE.finditer(line):
            other.add(m.group(1))
    return gfx, adm, snd, other


def trim_weapon_def(text: str, keep: set[str]) -> tuple[str, list[str]]:
    """Keep the preamble and only the named weapon blocks. Returns (text, kept names in order)."""
    lines = text.splitlines(keepends=True)
    start_re = re.compile(r'^\s*weapon\s+"([^"]+)"', re.IGNORECASE)
    first = next((i for i, l in enumerate(lines) if start_re.match(l)), len(lines))
    out = lines[:first]
    kept = []
    keep_l = {k.lower() for k in keep}
    # The file's own line ending: a bare LF between blocks stopped retail's parser after block 0.
    nl = "\r\n" if "\r\n" in text else "\n"
    for name, block in def_blocks(text, "weapon").items():
        if name.lower() in keep_l:
            out.append(block)
            if not block.endswith("\n"):
                out.append(nl)
            out.append(nl)
            kept.append(name)
    return "".join(out), kept



def item_blocks(text: str) -> list[tuple[int, str, str]]:
    """items.def `begin "Name" ... end` blocks (flat, no nesting) -> [(id, type, block text)]."""
    out, cur = [], None
    for line in text.splitlines(keepends=True):
        tok = line.split("//", 1)[0].strip().split()
        head = tok[0].lower() if tok else ""
        if head == "begin":
            cur = [line]
        elif cur is not None:
            cur.append(line)
            if head == "end":
                block = "".join(cur)
                m = re.search(r"(?im)^\s*id\s+(\d+)", block)
                t = re.search(r"(?im)^\s*type\s+(\S+)", block)
                out.append((int(m.group(1)) if m else -1, t.group(1).lower() if t else "", block))
                cur = None
    return out


def mission_mis_text(src: Source, stem: str) -> str:
    """The mission as .mis text, through the mission document's writer -- so no BMS byte layout is
    restated here. Item rows carry `type_id` (items.def id - 100000 [orig: the wire/BMS id space,
    engine/runtime/simassets/item_traits.cpp]) and `ai_textfile` (the .aip stem)."""
    lib = load_lib()

    class Bytes(ctypes.Structure):
        _fields_ = [("data", ctypes.POINTER(ctypes.c_uint8)), ("size", ctypes.c_size_t)]

    lib.opennova_mission_create.restype = ctypes.c_void_p
    lib.opennova_mission_destroy.argtypes = [ctypes.c_void_p]
    lib.opennova_mission_load_path.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    lib.opennova_mission_load_path.restype = ctypes.c_int
    lib.opennova_mission_write_mis_text.argtypes = [ctypes.c_void_p, ctypes.POINTER(Bytes)]
    lib.opennova_mission_write_mis_text.restype = ctypes.c_int
    lib.opennova_mission_free_bytes.argtypes = [ctypes.POINTER(Bytes)]
    tmp = Path(os.environ.get("TEMP", ".")) / f"retail_minimal_{stem}.bms"
    tmp.write_bytes(src.read(f"{stem}.bms"))
    doc = lib.opennova_mission_create()
    try:
        if lib.opennova_mission_load_path(doc, str(tmp).encode()) != 1:
            raise RuntimeError(f"mission load failed: {stem}.bms")
        b = Bytes()
        if lib.opennova_mission_write_mis_text(doc, ctypes.byref(b)) != 1:
            raise RuntimeError("mis text write failed")
        text = ctypes.string_at(b.data, b.size).decode("latin-1")
        lib.opennova_mission_free_bytes(ctypes.byref(b))
    finally:
        lib.opennova_mission_destroy(doc)
        tmp.unlink(missing_ok=True)
    return text


def mission_item_ids(mis: str) -> set[int]:
    return {100000 + int(x) for x in re.findall(r"(?m)^\s*type_id\s+(\d+)", mis)}


def mission_aip_stems(mis: str) -> set[str]:
    return {m.lower() for m in re.findall(r'(?m)^\s*ai_textfile\s+"([^"]+)"', mis) if m.strip()}


def def_key_values(text: str, key: str) -> set[str]:
    """Values of `<key> <value>` lines (case-insensitive key), comments stripped."""
    out = set()
    for raw in text.splitlines():
        tok = raw.split("//", 1)[0].replace('"', " ").split()
        if len(tok) > 1 and tok[0].lower() == key:
            out.add(tok[1])
    return out


def keep_blocks(text: str, kind: str, keep: set[str]) -> tuple[str, list[str]]:
    """Generic def trim: the preamble plus the `<kind> NAME ... end` blocks whose name is in `keep`
    (case-insensitive), in file order, with the file's own line ending."""
    blocks = def_blocks(text, kind)
    lines = text.splitlines(keepends=True)
    start_re = re.compile(rf'^\s*{kind}\s+"?', re.IGNORECASE)
    first = next((i for i, l in enumerate(lines) if start_re.match(l)), len(lines))
    nl = "\r\n" if "\r\n" in text else "\n"
    out, kept = lines[:first], []
    keep_l = {k.lower() for k in keep}
    for name, block in blocks.items():
        if name.lower() in keep_l:
            out.append(block)
            if not block.endswith("\n"):
                out.append(nl)
            out.append(nl)
            kept.append(name)
    return "".join(out), kept


# items.def objects the engine spawns/preloads by its own id table, not by placement -- witnessed as
# the mission-start preload burst right after ammo.def and after the player bodies in the retail
# /FRISK trace (projectiles: rockets/tracers/thrown grenades/mines/satchel/claymore; then
# parachute, NVG, binoculars, palm). Kept by their `graphic` name.
ENGINE_ITEM_GRAPHICS = {
    "at4rcket", "javelin", "stinger", "30mmtrcr", "bstone", "rpg", "oicwnade", "frag_3rd", "stch_3ad",
    "cly2_3ad", "clym_3ad", "vmne_3af", "vmne_3ad", "flsh_3rd", "tanktrcr", "strike1", "trace1",
    "palmx1", "parachut", "nvg", "bino_3rd",
}


def trim_items_def(text: str, wanted_ids: set[int], extra_keep: set[int] = frozenset()) -> tuple[str, list[int]]:
    """Keep the preamble, the placed ids, the `Null` marker, the PLAYER person defs (the engine
    spawns `Player #1 (Singleplayer)` 105310 by its own id), `extra_keep`, and every 6-digit item id
    those blocks name (addeweap ... <id> etc.), to a fixpoint. Line endings preserved."""
    blocks = item_blocks(text)
    by_id = {i: b for i, _, b in blocks}
    keep = {i for i, t, b in blocks if re.search(r"(?im)^\s*disk_function\s+PLAYER", b)
            or {v.lower() for v in def_key_values(b, "graphic")} & ENGINE_ITEM_GRAPHICS}
    keep |= (wanted_ids | {100000} | set(extra_keep)) & set(by_id)
    while True:
        new = set(keep)
        for i in keep:
            for m in re.finditer(r"\b(1\d{5})\b", by_id[i]):
                if int(m.group(1)) in by_id:
                    new.add(int(m.group(1)))
        if new == keep:
            break
        keep = new
    lines = text.splitlines(keepends=True)
    first = next((k for k, l in enumerate(lines)
                  if (l.split("//", 1)[0].strip().split() or [""])[0].lower() == "begin"), len(lines))
    nl = "\r\n" if "\r\n" in text else "\n"
    out, kept = lines[:first], []
    for i, _, b in blocks:
        if i in keep:
            out.append(b)
            if not b.endswith("\n"):
                out.append(nl)
            out.append(nl)
            kept.append(i)
    return "".join(out), kept


TEX_NAME_RE = re.compile(rb"[A-Za-z0-9_\-]{1,13}\.(?:[Tt][Gg][Aa]|[Dd][Dd][Ss]|[Pp][Cc][Xx]|[Mm][Dd][Tt]|3[Dd][Ii]|[Ww][Aa][Vv]|[Bb][Aa][Dd])")


def threedi_textures(data: bytes) -> set[str]:
    """Texture names a 3DI model references. The MTRL texture slots are plain fixed-width ASCII
    names, so a byte scan for <name>.<tga|dds|pcx|mdt> is exact enough here (verified against the
    engine reader on the JOTAC weapon models) and keeps this script off the struct-mirror FFI."""
    return {m.group(0).decode("latin-1") for m in TEX_NAME_RE.finditer(data)}


# --------------------------------------------------------------------------------------------
# The manifest: name -> Entry
# --------------------------------------------------------------------------------------------

class Manifest:
    def __init__(self, src: Source, mission: str):
        self.src = src
        self.mission = mission
        self.entries: dict[str, dict] = {}   # name.lower() -> {name, reasons}
        self.missing: dict[str, str] = {}    # requested but unresolvable -> reason
        self.excluded: dict[str, str] = {}   # resolvable but deliberately dropped
        self.other_stems = src.mission_stems - {mission.lower()}

    def _foreign_mission_file(self, name: str) -> bool:
        low = name.lower()
        stem, _, ext = low.rpartition(".")
        if ext in MISSION_LIST_EXTS and stem != self.mission.lower():
            return True
        return stem in self.other_stems and ext in MISSION_EXTS

    def add(self, name: str, reason: str) -> bool:
        low = name.lower()
        if not self.src.has(low):
            self.missing.setdefault(name, reason)
            return False
        if self._foreign_mission_file(low):
            self.excluded[self.src.canonical(low)] = "belongs to another mission"
            return False
        e = self.entries.get(low)
        if e is None:
            self.entries[low] = {"name": self.src.canonical(low), "reasons": [reason]}
        elif reason not in e["reasons"]:
            e["reasons"].append(reason)
        return True

    def names(self) -> list[str]:
        return [e["name"] for e in self.entries.values()]


class DefClosure:
    """Asset names a set of weapon.def blocks (+ ammo rounds) reach: 3di (+ their textures),
    adm (+ their bads), LWF set names, other named files. Used twice -- for the KEPT weapons
    (what must stay) and the DROPPED ones (what may go)."""

    def __init__(self, src: Source, banks: dict[str, dict[str, set[str]]]):
        self.src = src
        self.banks = banks
        self.files: set[str] = set()   # lower-case file names
        self.sets: set[str] = set()    # lower-case LWF set names
        self.models: set[str] = set()  # lower-case 3di names

    def add_blocks(self, blocks: list[str]) -> None:
        gfx, adms, snds, other = set(), set(), set(), set()
        for b in blocks:
            g, a, s, o = def_refs(b)
            gfx |= g; adms |= a; snds |= s; other |= o
        for g in gfx:
            self._model((g if "." in g else g + ".3di").lower())
        for a in adms:
            adm = (a if a.lower().endswith(".adm") else a + ".adm").lower()
            self.files.add(adm)
            if self.src.has(adm):
                for bad in re.findall(r'"\s*([^"]+?)\s*"', self.src.text(adm)):
                    self.files.add((bad if "." in bad else bad + ".bad").lower())
        for sname in snds:
            for bank, sets in self.banks.items():  # first bank in slot order wins
                if sname in sets:
                    self.sets.add(sname)
                    self.files |= sets[sname]
                    break
        for o in other:
            self._texture_family(o.lower())

    def _model(self, name: str) -> None:
        self.models.add(name)
        self.files.add(name)
        if self.src.has(name):
            for t in threedi_textures(self.src.read(name)):
                self._texture_family(t.lower())

    def _texture_family(self, name: str) -> None:
        # A model names "X.tga"; retail substitutes X.dds (texcompression) or X.mdt -- take the
        # variants that exist rather than reporting the literal name as missing.
        stem = name.rsplit(".", 1)[0]
        found = False
        for ext in ("tga", "dds", "mdt", "pcx"):
            if self.src.has(f"{stem}.{ext}"):
                self.files.add(f"{stem}.{ext}")
                found = True
        if not found:
            self.files.add(name)


def ammo_names_in(text: str) -> set[str]:
    return {m.lower() for m in re.findall(r"\bAMMO_[A-Za-z0-9_]+", text)}


def rounds_of(blocks: list[str]) -> set[str]:
    out = set()
    for b in blocks:
        for raw in b.splitlines():
            tok = raw.split("//", 1)[0].replace('"', " ").split()
            if tok and tok[0].lower() == "round_type" and len(tok) > 1:
                out.add(tok[1].lower())
    return out


def build_manifest(src: Source, traced: set[str], texture_siblings: bool = True,
                   protect: set[str] = frozenset()) -> Manifest:
    """`protect`: names a /FRISK run of the TRIMMED build loaded -- never pruned."""
    man = Manifest(src, MISSION_STEM)

    # --- the def transforms decide what the closure is computed over
    mis = mission_mis_text(src, MISSION_STEM)
    placed = mission_item_ids(mis)
    items_full = src.text("items.def")
    itext, kept_items = trim_items_def(items_full, placed)
    wtext = src.text("weapon.def")
    wblocks = def_blocks(wtext, "weapon")
    keep_names = {k.lower() for k in ARMORY} | {k.lower() for k in re.findall(r"WPN_[A-Za-z0-9_]+", itext)}
    kept = [n for n in wblocks if n.lower() in keep_names]
    dropped = [n for n in wblocks if n.lower() not in keep_names]
    wtrim, _ = trim_weapon_def(wtext, set(kept))
    # powerup.def: the pickups of the kept weapons (dropped-weapon pickups); then the items.def
    # powerup rows that name those powerupdefs ride along.
    ptext_full = src.text("powerup.def")
    pblocks = def_blocks(ptext_full, "powerup")
    kept_pu = [n for n, b in pblocks.items()
               if any(w.lower() in keep_names for w in def_key_values(b, "weapon"))]
    ptrim, _ = keep_blocks(ptext_full, "powerup", set(kept_pu))
    pu_items = {i for i, t, b in item_blocks(items_full) if t == "powerup"
                and any(v.lower() in {k.lower() for k in kept_pu} for v in def_key_values(b, "powerupdef"))}
    if pu_items:
        itext, kept_items = trim_items_def(items_full, placed, pu_items)
    # .aip: the mission's ai_textfile stems + the kept items' default_aip
    aips = {a for a in mission_aip_stems(mis)} | {a.lower() for a in def_key_values(itext, "default_aip")}
    aip_texts = [src.text(f"{a}.aip") for a in aips if src.has(f"{a}.aip")]
    # SndProf.def: "default" + the profiles the kept items name
    stext_full = src.text("SndProf.def")
    sp_names = {"default"} | {v for v in def_key_values(itext, "sound_profile")} \
        | {v for v in def_key_values(itext, "sound_profilefemale")}
    strim, kept_sp = keep_blocks(stext_full, "begin", sp_names)
    man.items_text, man.weapon_text = itext, wtrim  # type: ignore[attr-defined]
    man.powerup_text, man.sndprof_text = ptrim, strim  # type: ignore[attr-defined]
    man.kept_items, man.kept_weapons = kept_items, kept  # type: ignore[attr-defined]
    man.kept_powerups, man.kept_sndprof, man.aips = kept_pu, kept_sp, sorted(aips)  # type: ignore[attr-defined]

    banks = {}
    for b in (f"{src.expansion}.lwf", "gamelocl.lwf", "game.lwf", "menu.lwf", f"{MISSION_STEM}.lwf",
              "game2.lwf", "game3.lwf"):
        if src.has(b):
            banks[b] = lwf_sets(src.read(b))
    ammo = {k.lower(): v for k, v in def_blocks(src.text("ammo.def"), "ammo").items()}
    # AI rounds: the kept items' ammo_closeattack + every .aip's authored ammo names -- those rounds'
    # tracer models / launch sounds are needed even when no player weapon fires them.
    ai_rounds = ammo_names_in(itext)
    for at in aip_texts:
        ai_rounds |= ammo_names_in(at)
    kept_rounds = rounds_of([wblocks[n] for n in kept]) | ai_rounds
    dropped_rounds = rounds_of([wblocks[n] for n in dropped]) - kept_rounds
    man.ammo_text, man.kept_rounds = keep_blocks(src.text("ammo.def"), "ammo", kept_rounds)  # type: ignore[attr-defined]
    K = DefClosure(src, banks)
    K.add_blocks([wblocks[n] for n in kept] + [ammo[r] for r in kept_rounds if r in ammo]
                 + [strim, itext] + aip_texts)
    D = DefClosure(src, banks)
    D.add_blocks([wblocks[n] for n in dropped] + [ammo[r] for r in dropped_rounds if r in ammo])
    man.dropped_closure = D.files - K.files  # type: ignore[attr-defined]

    # --- the set: trace + hardcoded + cheap classes + mission family + kept-def closure
    for n in traced:
        man.add(n, "traced")
    for n in HARDCODED:
        man.add(n, "hardcoded")
    for a in aips:
        man.add(f"{a}.aip", "mission-aip")
    for ext in MISSION_EXTS:
        man.add(f"{MISSION_STEM}.{ext}", "mission-family")
    for n in LOOSE_EXPANSION_TO_PFF:
        pth = src.root / "expansion" / src.expansion / n
        if pth.exists():
            man.entries[n.lower()] = {"name": n, "reasons": ["expansion-loose"], "loose_path": str(pth)}
    for n in list(man.names()):  # terrain / env text closure
        if n.lower().endswith((".trn", ".env")):
            for m in ASSET_EXT_RE.finditer(src.text(n)):
                man.add(m.group(1), f"named-by:{n}")
    for f in K.files:
        man.add(f, "kept-def")

    # --- the kept items: their models (graphic/graphicenemy/husk...), anim adm -> bads, named files
    igfx, iadm, _, iother = def_refs(itext)
    for g in igfx:
        man.add(g if "." in g else g + ".3di", "kept-items")
    for a in iadm:
        adm = a if a.lower().endswith(".adm") else a + ".adm"
        if man.add(adm, "kept-items"):
            for bad in re.findall(r'"\s*([^"]+?)\s*"', src.text(adm)):
                man.add(bad if "." in bad else bad + ".bad", f"adm:{adm}")
    for o in iother:
        man.add(o, "kept-items")

    # --- everything named by particle libs, menus, styles, env, terrain, tile sets, image lists,
    #     scripts, AI profiles: textures, models, wavs, anims. Iterated so a model pulled in here
    #     gets its own textures below.
    trimmed_texts = {"items.def": itext, "weapon.def": wtrim, "ammo.def": man.ammo_text,
                     "powerup.def": ptrim, "sndprof.def": strim}
    for n in list(man.names()):
        if n.lower().rsplit(".", 1)[-1] in NAMING_EXTS:
            data = trimmed_texts[n.lower()].encode("latin-1") if n.lower() in trimmed_texts else src.read(n)
            for m in TEX_NAME_RE.finditer(data):
                man.add(m.group(0).decode("latin-1"), f"named-by:{n}")

    # --- 3di -> textures for every model in the manifest (world models included)
    for n in list(man.names()):
        if n.lower().endswith(".3di"):
            for t in threedi_textures(src.read(n)):
                stem = t.rsplit(".", 1)[0] if "." in t else t
                for ext in ("tga", "dds", "mdt", "pcx"):
                    if src.has(f"{stem}.{ext}"):
                        man.add(f"{stem}.{ext}", f"3di:{n}")

    # --- LWF closure: every set that names a traced wav (members are picked at random) -> all
    #     member wavs; the mission's own dialog bank and the UI bank whole.
    wav_to_sets: dict[str, set[tuple[str, str]]] = defaultdict(set)
    for b, sets in banks.items():
        for sname, wavs in sets.items():
            for w in wavs:
                wav_to_sets[w].add((b, sname))
    wanted_sets: set[tuple[str, str]] = set()
    for b in (f"{MISSION_STEM}.lwf", "menu.lwf"):
        for sname in banks.get(b, {}):
            wanted_sets.add((b, sname))
    for n in list(man.names()):
        if n.lower().endswith(".wav"):
            wanted_sets |= wav_to_sets.get(n.lower(), set())
    all_sets = {sname for sets in banks.values() for sname in sets}
    for text in (strim, itext, wtrim, man.ammo_text) + tuple(aip_texts):
        for tok in set(re.findall(r"[A-Za-z][A-Za-z0-9_]{2,}", text)):
            if tok.lower() in all_sets:
                for b, sets in banks.items():  # first bank in slot order wins
                    if tok.lower() in sets:
                        wanted_sets.add((b, tok.lower()))
                        break
    for b, sname in wanted_sets:
        for w in banks[b][sname]:
            man.add(w, f"lwf:{b}:{sname}")

    # --- texture siblings (retail probes .dds for .tga names depending on texcompression)
    if texture_siblings:
        for n in list(man.names()):
            low = n.lower()
            if low.endswith(".tga") and src.has(low[:-4] + ".dds"):
                man.add(low[:-4] + ".dds", "sibling")
            elif low.endswith(".dds") and src.has(low[:-4] + ".tga"):
                man.add(low[:-4] + ".tga", "sibling")

    # --- prune: assets only the DROPPED weapons reach. The trace loaded them because retail
    #     preloads every weapon.def entry at mission start; with the trimmed def it does not.
    protect_prefixes = ("named-by:", "hardcoded", "mission-family", "mission-aip", "expansion-loose",
                        "kept-def", "kept-items", "adm:")
    pruned = []
    protect_l = {n.lower() for n in protect}
    for low, e in list(man.entries.items()):
        if low not in man.dropped_closure:
            continue
        keep_it = low in protect_l
        for r in e["reasons"]:
            if r.startswith(protect_prefixes):
                keep_it = True
            elif r.startswith("3di:") and r[4:].lower() not in D.models:
                keep_it = True  # a world/kept model shares this texture
            elif r.startswith("lwf:"):
                _, bank, sname = r.split(":", 2)
                if sname not in D.sets:
                    keep_it = True  # a set outside the dropped weapons plays this wav
        if not keep_it:
            pruned.append(e["name"])
            del man.entries[low]
    man.pruned = pruned  # type: ignore[attr-defined]
    return man


# --------------------------------------------------------------------------------------------
# Placement + packing
# --------------------------------------------------------------------------------------------

def target_archive(src: Source, name: str) -> str:
    low = name.lower()
    base = src.base_archive.get(low)
    if base in ("language.pff", "localres.pff", "resource.pff"):
        return base
    ext = low.rsplit(".", 1)[-1]
    if src.has(low) and src.archive_of(low).endswith("l.pff"):
        return "language.pff"
    if ext in ("dds", "3di", "tga", "mdt", "trn", "cpt", "env", "pcx"):
        return "resource.pff"
    return "localres.pff"


def write_pff(path: Path, entries: list[tuple], trailer: bool = True) -> None:
    """entries: (name, size, read() -> bytes[, timestamp, checksum]). Streams payloads through
    pff_write_archive_streamed; retained entries keep their source timestamp/checksum, and the
    12-byte pack.exe trailer (`0a 05 00 02 .. .. .. .. K I N G`) retail archives end with is appended."""
    _bind_pff()
    lib = load_lib()
    names = [e[0].encode("latin-1") for e in entries]
    arr = (PffWriteStreamEntry * len(entries))()
    for i, e in enumerate(entries):
        arr[i].name = names[i]
        arr[i].size = e[1]
        arr[i].flags = 0
        arr[i].timestamp = e[3] if len(e) > 3 else 0
        arr[i].checksum = e[4] if len(e) > 4 else 0
    done = [0]

    def read_cb(_ctx, index, out, size):
        data = entries[index][2]()
        if len(data) != size:
            print(f"  ! size mismatch for {entries[index][0]}: {len(data)} != {size}")
            return 1
        ctypes.memmove(out, data, size)
        done[0] += 1
        if done[0] % 500 == 0:
            print(f"    {done[0]}/{len(entries)}")
        return 0

    cb = PFF_READ_FN(read_cb)
    rc = lib.pff_write_archive_streamed(str(path).encode(), PFF_FORMAT_PFF4, arr, len(entries), cb, None)
    if rc != 0:
        raise RuntimeError(f"pff_write_archive_streamed({path}) failed rc={rc}")
    if trailer:
        with open(path, "ab") as f:
            f.write(bytes.fromhex("0a05000200000000") + b"KING")


def plan_entries(src: Source, man: Manifest, decoded: bool) -> dict[str, list[dict]]:
    """Materialize the plan: archive -> [{name, size, source, reasons, read, timestamp, checksum}]
    (the archive is the retail placement; the loose layout writes the same rows flat). Applies the
    transforms: the trimmed defs and the expansion LWF slot rename. `decoded` selects the loose
    payload form (BFC1 inflated, SCR kept) over the stored archive bytes."""
    per: dict[str, list[dict]] = {"language.pff": [], "localres.pff": [], "resource.pff": []}
    seen: set[str] = set()
    fetch = src.read_loose if decoded else src.read_raw

    def put(archive: str, name: str, size: int, source: str, reasons: list[str], read, meta=(0, 0)):
        key = name.upper()
        if key in seen:
            return
        seen.add(key)
        per[archive].append({"name": name, "size": size, "source": source, "reasons": reasons, "read": read,
                             "timestamp": meta[0], "checksum": meta[1]})

    # transform 0/1: the trimmed defs (computed with the manifest)
    trimmed = {
        "items.def": (man.items_text, f"kept {len(man.kept_items)} items"),
        "weapon.def": (man.weapon_text, f"kept {len(man.kept_weapons)} weapons"),
        "ammo.def": (man.ammo_text, f"kept {len(man.kept_rounds)} rounds"),
        "powerup.def": (man.powerup_text, f"kept {len(man.kept_powerups)} powerups"),
        "SndProf.def": (man.sndprof_text, f"kept {len(man.kept_sndprof)} profiles"),
    }
    for name, (text, why) in trimmed.items():
        data = text.encode("latin-1")
        put(target_archive(src, name), name, len(data), "transform:trimmed", [why], lambda d=data: d)

    # transform 2: expansion bank -> gamelocl slot; base gamelocl -> game3 slot (docs/audio/lwf-dbf-sound-re.md)
    exp_lwf = f"{src.expansion}.lwf"
    if src.has(exp_lwf) and src.has("gamelocl.lwf"):
        put("language.pff", "gamelocl.lwf", src.size_of(exp_lwf), f"transform:{src.canonical(exp_lwf)}",
            ["expansion slot-1 bank moved to slot 2"], lambda n=exp_lwf: fetch(n))
        put("localres.pff", "game3.lwf", src.size_of("gamelocl.lwf"), "transform:gamelocl.lwf",
            ["base slot-2 bank moved to slot 4"], lambda: fetch("gamelocl.lwf"))

    for low, e in sorted(man.entries.items()):
        name = e["name"]
        if "loose_path" in e:
            p = Path(e["loose_path"])
            put(target_archive(src, name), name, p.stat().st_size, f"loose:{p}", e["reasons"],
                lambda p=p: p.read_bytes())
            continue
        if low in ("gamelocl.lwf", exp_lwf.lower()) or name.upper() in seen:
            continue
        _, _, ts, cs = src.entry_of(name)
        put(target_archive(src, name), name, src.size_of(name), src.archive_of(name), e["reasons"],
            lambda n=name: fetch(n), (ts, cs))
    return per


def print_plan(src: Source, man: Manifest, per: dict[str, list[dict]]) -> None:
    print(f"\n== plan for {MISSION_STEM} ({len(man.entries)} names)")
    for arch, rows in per.items():
        total = sum(r["size"] for r in rows)
        by_ext: dict[str, list[int]] = defaultdict(lambda: [0, 0])
        for r in rows:
            ext = r["name"].rsplit(".", 1)[-1].lower()
            by_ext[ext][0] += 1
            by_ext[ext][1] += r["size"]
        print(f"  {arch}: {len(rows)} files, {total / 1e6:.1f} MB")
        for ext, (c, s) in sorted(by_ext.items(), key=lambda kv: -kv[1][1])[:14]:
            print(f"      {ext:5} {c:5}  {s / 1e6:8.1f} MB")
    src_count: dict[str, int] = defaultdict(int)
    for rows in per.values():
        for r in rows:
            src_count[r["source"].split(":")[0]] += 1
    print("  by source:", dict(src_count))
    print(f"  kept items: {len(man.kept_items)} of items.def; rounds {len(man.kept_rounds)}; "
          f"powerups {len(man.kept_powerups)}; sound profiles {len(man.kept_sndprof)}; aip {man.aips}")
    print(f"  pruned (dropped-weapon-only assets): {len(man.pruned)}")
    print(f"  kept weapons ({len(man.kept_weapons)}): {', '.join(man.kept_weapons)}")
    if man.missing:
        print(f"  unresolvable ({len(man.missing)}):")
        for n, why in sorted(man.missing.items()):
            print(f"      {n}  [{why}]")
    if man.excluded:
        print(f"  excluded (other missions): {len(man.excluded)}")


# Highest-quality display knobs for the shipped game.cfg (the values the JOTAC install runs at;
# retail's regenerated defaults sit one notch lower on most of them).
GAME_CFG_QUALITY = {
    "terrain_polydetail": 3, "terrain_texdetail": 3, "object_polydetail": 3, "object_texdetail": 1,
    "water_quality": 3, "shadow_quality": 3, "particle_density": 2, "texfilter_level": 3,
    "fbeffects_level": 3, "shader_usage_level": 2, "texcompression_level": 2, "antialias_mode": 0,
    "lock_framerate": 0, "gamma": "1.0",
}

LFS_ATTRS = ("*.pff *.sbf *.exe *.dll *.3di *.dds *.tga *.mdt *.pcx *.png *.wav *.bad *.cpt *.bms *.til "
             "*.fnt *.ptl *.ptu *.ptg *.lwf *.dbf *.bin *.sav *.coo").split()


def clean_previous(out: Path) -> None:
    """Remove what the previous build wrote (per its MANIFEST.json) so a rebuild never leaves stale
    files behind; anything else in the directory (git, user files) is untouched."""
    old = out / "MANIFEST.json"
    if not old.exists():
        return
    prev = json.loads(old.read_text(encoding="utf-8"))
    n = 0
    for rows in prev.get("archives", {}).values():
        for r in rows:
            for cand in (out / r["name"], out / LOOSE_SUBDIR.get(r["name"].rsplit(".", 1)[-1].lower(), "") / r["name"]):
                if cand.is_file():
                    cand.unlink()
                    n += 1
    for name in ("language.pff", "localres.pff", "resource.pff", "game.cfg", "MANIFEST.json"):
        if (out / name).exists():
            (out / name).unlink()
    print(f"  cleaned {n} files from the previous build")


def assemble(src: Source, out: Path, man: Manifest, per: dict[str, list[dict]], layout: str) -> None:
    out.mkdir(parents=True, exist_ok=True)
    clean_previous(out)
    if layout == "pff":
        for arch, rows in per.items():
            print(f"  writing {arch} ({len(rows)} entries, {sum(r['size'] for r in rows) / 1e6:.1f} MB)")
            write_pff(out / arch, [(r["name"], r["size"], r["read"], r["timestamp"], r["checksum"]) for r in rows])
    else:
        # Loose (for `/d`): every file flat next to the exe except LOOSE_ARCHIVED_EXTS (see the note
        # at the top) and the ptl\ subdirectory. The mission .bms stays loose only: a loose .bms lists
        # (the "*" row) and loads under /d, and shipping it archived too shows the mission twice.
        n = 0
        archived = []
        for rows in per.values():
            for r in rows:
                ext = r["name"].rsplit(".", 1)[-1].lower()
                if ext in LOOSE_ARCHIVED_EXTS:
                    archived.append(r)
                    continue
                sub = LOOSE_SUBDIR.get(ext)
                dest = out / sub / r["name"] if sub else out / r["name"]
                dest.parent.mkdir(exist_ok=True)
                data = r["read"]()
                dest.write_bytes(data)
                r["size"] = len(data)  # decoded size (the plan reports stored sizes)
                n += 1
                if n % 500 == 0:
                    print(f"    {n} files")
        # The archive half: the DDS textures (stored bytes, retail inflates BFC1 itself on the
        # archive path) -- it also satisfies the boot gate [orig: PFF_OpenAllArchives fatal @ 0x4a6f44].
        write_pff(out / "localres.pff",
                  [(r["name"], src.size_of(r["name"]), (lambda nm=r["name"]: src.read_raw(nm)),
                    r["timestamp"], r["checksum"]) for r in archived])
        for r in archived:
            r["source"] = r["source"] + " [archived]"
        print(f"  wrote {n} loose files + localres.pff with {len(archived)} DDS textures")
    cfg = src.root / "game.cfg"
    if cfg.exists():  # the install's config (every key present) with the quality knobs pinned high
        # Bytes, not text: read_text folds the CRLF retail writes into LF, and retail's line
        # parsers fail silently on a bare LF (the .def / .mns lesson). The substitution stops
        # before the \r so the line ending survives too.
        text = cfg.read_bytes().decode("latin-1")
        for key, val in GAME_CFG_QUALITY.items():
            text, n = re.subn(rf"(?m)^({key}\s*=\s*)[^\r\n]*", rf"\g<1>{val}", text)
            if n == 0:
                print(f"  ! game.cfg has no key {key}")
        (out / "game.cfg").write_bytes(text.encode("latin-1"))
    for src_name, dst_name in EXE_FILES.items():
        shutil.copy2(src.root / src_name, out / dst_name)
    for n in LOOSE_ROOT:
        p = src.root / n
        if p.exists():
            shutil.copy2(p, out / n)
        else:
            print(f"  ! loose root file missing in source: {n}")
    manifest = {
        "mission": MISSION_STEM,
        "source": str(src.root),
        "expansion": src.expansion,
        "layout": layout,
        "kept_weapons": man.kept_weapons,
        "kept_item_ids": man.kept_items,
        "kept_rounds": man.kept_rounds,
        "kept_powerups": man.kept_powerups,
        "kept_sound_profiles": man.kept_sndprof,
        "aips": man.aips,
        "pruned": man.pruned,
        "archives": {arch: [{k: v for k, v in r.items() if k != "read"} for r in rows]
                     for arch, rows in per.items()},
        "loose": [n for n in LOOSE_ROOT if (src.root / n).exists()],
        "exe": list(EXE_FILES.values()),
        "unresolvable": man.missing,
    }
    (out / "MANIFEST.json").write_text(json.dumps(manifest, indent=1), encoding="utf-8")
    (out / ".gitattributes").write_text(
        "".join(f"{pat} filter=lfs diff=lfs merge=lfs -text\n" for pat in LFS_ATTRS), encoding="utf-8")
    (out / "README.md").write_text(README.format(mission=MISSION_STEM, weapons=", ".join(man.kept_weapons),
                                                 exp=src.expansion, layout=layout), encoding="utf-8")


README = """# MINIMAL -- one-mission retail Joint Operations install

Retail `Jointops.exe` plus the smallest asset set that boots to the menu and plays the
single-player mission **{mission}**. Built by `apps/retail_minimal.py` in the opennova repo from
the JOTAC install with the `{exp}` expansion flattened into the base archives (wherever the
expansion overrode a base file, the expansion's file is the one packed here).

Layout: **{layout}**. Loose layout: every asset sits flat next to the exe and retail must run with
`/d` (loose-first lookup) -- except the `.dds` textures, which stay in `localres.pff`: retail's
texture loader (`Texture_LoadByNameWithChannel @ 0x58b470`) cannot load a `.dds`-named texture
from a loose file under `/d` (a loose hit routes it to the TGA/MDT/PCX-only branch), so archived
DDS is the only form that renders. Particle libraries live in the `ptl` folder (retail scans ptl/*.ptl).
Pff layout: the three boot-table archives instead, no `/d` needed.

Launch: `Jointops.exe /w /d` (add `/FRISK` to log every file load to `_filelog.txt`).

Contents: `Jointops.exe`, `binkw32.dll` (retail Bink), the assets, `menumus.sbf` / `gamemus.sbf`
(music banks), `nw_cdata.coo`, `cc.bin`, `game.cfg` (highest quality settings). `MANIFEST.json`
lists every entry with its source archive and why it is included (traced by a retail `/FRISK`
play session, hardcoded boot name, or static closure). `items.def` is trimmed to what the mission
places (plus every marker/powerup/effect definition); `weapon.def` to the mission's armory
({weapons}) plus the emplaced/vehicle guns the kept items name. The expansion sound bank rides as
`gamelocl.lwf` and the base `gamelocl.lwf` as `game3.lwf` (same lookup order once the expansion
slots are gone).

Deliberately absent: `expansion/`, videos, profiles/config (regenerated on first run), MED,
launcher and hook binaries, other missions.
"""


# --------------------------------------------------------------------------------------------
# verify
# --------------------------------------------------------------------------------------------

def verify(out: Path, log: str, reference: str | None) -> int:
    man = json.loads((out / "MANIFEST.json").read_text(encoding="utf-8"))
    packed = {r["name"].lower() for rows in man["archives"].values() for r in rows}
    loaded, loose = parse_frisk([log])
    # loose layout: asset loads log as `LOADED FILE: <name>` (bare names); paths and archives are noise
    loose_assets = {p.lower() for p in loose if "\\" not in p and "/" not in p and not p.lower().endswith(".pff")}
    loaded_l = {n.lower() for n in loaded} | loose_assets
    ignore = {"game.cfg", "player.sav", "weapon.sav", "hiscore.txt", "score.ini", "cc.bin", "nw_cdata.coo",
              "menumus.sbf", "gamemus.sbf"}
    not_packed = sorted(n for n in loaded_l if n not in packed and n not in ignore)
    print(f"minimal run loaded {len(loaded_l)} names; "
          f"{len(not_packed)} loaded names are NOT in the manifest (should be 0):")
    for n in not_packed:
        print("   ", n)
    if reference:
        ref, _ = read_names_file(reference)
        ref_l = {n.lower() for n in ref}
        missing = sorted(n for n in ref_l - loaded_l if n in packed)
        print(f"reference names not loaded this run (play-path dependent, informational): {len(missing)}")
        for n in missing[:60]:
            print("   ", n)
    err = out / "_errlog.txt"
    if err.exists():
        print("---- _errlog.txt ----")
        print(err.read_text(encoding="latin-1", errors="replace"))
    # The staged game.cfg must keep retail's CRLF: a bare LF is read as far as the parser gets
    # and then silently falls back to defaults, undoing the pinned quality knobs.
    bare_lf = 0
    staged_cfg = out / "game.cfg"
    if staged_cfg.exists():
        raw = staged_cfg.read_bytes()
        bare_lf = raw.count(b"\n") - raw.count(b"\r\n")
        if bare_lf:
            print(f"game.cfg has {bare_lf} bare LF line endings (retail writes CRLF)")
    return 1 if (not_packed or bare_lf) else 0


# --------------------------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------------------------

def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="retail_minimal")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("trace-parse", help="fold one or more retail /FRISK logs into a names file")
    p.add_argument("--log", action="append", required=True)
    p.add_argument("-o", "--out", required=True)

    for name in ("plan", "build"):
        q = sub.add_parser(name)
        q.add_argument("--game", required=True, help="retail install root")
        q.add_argument("--exp", default="revx02")
        q.add_argument("--names", required=True, help="names file from trace-parse")
        q.add_argument("--no-texture-siblings", action="store_true")
        q.add_argument("--protect", help="names file from a /FRISK run of the trimmed build; never pruned")
        if name == "build":
            q.add_argument("--out", required=True)
            q.add_argument("--layout", choices=("loose", "pff"), default="loose")

    v = sub.add_parser("verify")
    v.add_argument("--out", required=True)
    v.add_argument("--log", required=True)
    v.add_argument("--reference", help="names file used for the build")

    args = ap.parse_args(argv)
    if args.cmd == "trace-parse":
        pff, loose = parse_frisk(args.log)
        lines = ["# names retail loaded (PFF LOADED FILE) during the /FRISK play sessions; loose: lines are",
                 "# LOADED FILE entries (paths). Filenames only -- no retail bytes."]
        lines += sorted(pff, key=str.lower)
        lines += ["loose:" + p for p in sorted(loose, key=str.lower)]
        Path(args.out).write_text("\n".join(lines) + "\n", encoding="utf-8")
        print(f"{len(pff)} archive names, {len(loose)} loose paths -> {args.out}")
        return 0

    if args.cmd == "verify":
        return verify(Path(args.out), args.log, args.reference)

    src = Source(args.game, args.exp)
    try:
        traced, loose = read_names_file(args.names)
        # A loose file in the install (e.g. a modder's expansion\<n>\X.3di) shadows the archived X:
        # the retail trace logs the loose path, but the archived copy is what a base install needs.
        for pth in loose:
            base = os.path.basename(pth.replace("\\", "/"))
            if not base.lower().endswith((".pff", ".sav", ".bin", ".cfg")):
                traced.add(base)
        protect = read_names_file(args.protect)[0] if args.protect else set()
        man = build_manifest(src, traced, texture_siblings=not args.no_texture_siblings, protect=protect)
        per = plan_entries(src, man, decoded=(args.cmd == "plan" or args.layout == "loose"))
        print_plan(src, man, per)
        interesting = [p for p in loose if not p.lower().endswith(".pff")]
        print("  loose paths in trace (root files are copied only from LOOSE_ROOT):", interesting)
        if args.cmd == "build":
            assemble(src, Path(args.out), man, per, args.layout)
            print(f"done -> {args.out}")
    finally:
        src.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
