"""Land Warrior animation definition parsers.

The LW character path is separate from the later ADM/BAD animation stack:
``items.def`` references an ``anim_def`` (.ANM move table) and a ``chr_file``
(.KSA packed animation table, with .ACA/.SAF source lists as authoring data).
"""
from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class LwAnmEntry:
    name: str
    slot: int
    velocity: float = 0.0
    override: bool = False


@dataclass(frozen=True)
class LwAnmFile:
    entries: dict[str, LwAnmEntry]

    def unique_slots(self) -> set[int]:
        return {entry.slot for entry in self.entries.values()}


@dataclass(frozen=True)
class LwAcaSlot:
    slot: int
    filename: str
    loop_frame: int = 0


@dataclass(frozen=True)
class LwAcaFile:
    slots: dict[int, LwAcaSlot]


@dataclass(frozen=True)
class LwSafBoneRecord:
    raw: bytes


@dataclass(frozen=True)
class LwSafFrame:
    raw_header: bytes
    bone_records: tuple[LwSafBoneRecord, ...]
    runtime_bytes: bytes


@dataclass(frozen=True)
class LwSafFile:
    version: int
    frame_count: int
    frames: tuple[LwSafFrame, ...]


@dataclass(frozen=True)
class LwKsaSlot:
    slot: int
    frame_count: int
    stored_slot: int
    loop_frame: int
    frames: tuple[bytes, ...]


@dataclass(frozen=True)
class LwKsaFile:
    version: int
    slots: dict[int, LwKsaSlot]


@dataclass(frozen=True)
class LwAnimationContext:
    kind: str
    anim_name: str
    chr_name: str
    anm: LwAnmFile
    ksa: LwKsaFile | None = None
    aca: LwAcaFile | None = None

    @property
    def reset_animation(self):
        return None

    @property
    def animations(self) -> tuple[()]:
        return ()


def parse_anm(path: str | Path) -> LwAnmFile:
    entries: dict[str, LwAnmEntry] = {}
    for line in _text_lines(path):
        parts = line.split()
        if len(parts) < 2:
            continue
        try:
            slot = int(parts[1], 0)
        except ValueError:
            continue
        velocity = 0.0
        if len(parts) >= 3:
            try:
                velocity = float(parts[2])
            except ValueError:
                velocity = 0.0
        override = any(part.casefold() == "override" for part in parts[3:])
        name = parts[0]
        entries[name] = LwAnmEntry(
            name=name,
            slot=slot,
            velocity=velocity,
            override=override,
        )
    return LwAnmFile(entries=entries)


def parse_aca(path: str | Path) -> LwAcaFile:
    slots: dict[int, LwAcaSlot] = {}
    for line in _text_lines(path):
        parts = line.split()
        if len(parts) < 3 or parts[0].casefold() != "slot":
            continue
        try:
            slot = int(parts[1], 0)
        except ValueError:
            continue
        loop_frame = 0
        if len(parts) >= 4:
            try:
                loop_frame = int(parts[3], 0)
            except ValueError:
                loop_frame = 0
        slots[slot] = LwAcaSlot(slot=slot, filename=parts[2], loop_frame=loop_frame)
    return LwAcaFile(slots=slots)


def parse_saf(path: str | Path) -> LwSafFile:
    data = Path(path).read_bytes()
    if len(data) < 16 or data[:4] != b"SAF1":
        raise ValueError(f"not an SAF1 file: {path}")
    version, frame_count, header_size = struct.unpack_from("<III", data, 4)
    if header_size != 52:
        raise ValueError(f"unsupported SAF frame header size {header_size}: {path}")
    frame_size = 112
    expected_size = 16 + frame_count * frame_size
    if len(data) != expected_size:
        raise ValueError(f"SAF size mismatch for {path}: got {len(data)}, expected {expected_size}")

    frames: list[LwSafFrame] = []
    pos = 16
    for _ in range(frame_count):
        frame = data[pos : pos + frame_size]
        raw_header = frame[:header_size]
        bone_bytes = frame[header_size : header_size + 60]
        records = tuple(
            LwSafBoneRecord(bone_bytes[i : i + 4])
            for i in range(0, len(bone_bytes), 4)
        )
        frames.append(
            LwSafFrame(
                raw_header=raw_header,
                bone_records=records,
                runtime_bytes=_saf_runtime_frame(raw_header, records),
            )
        )
        pos += frame_size

    return LwSafFile(version=version, frame_count=frame_count, frames=tuple(frames))


def parse_ksa(path: str | Path) -> LwKsaFile:
    data = Path(path).read_bytes()
    if len(data) < 100 or data[:4] != b"KSA\x00":
        raise ValueError(f"not a KSA file: {path}")
    version = struct.unpack_from("<I", data, 4)[0]
    slot_count = struct.unpack_from("<I", data, 52)[0]
    table_off = 100
    table_size = slot_count * 28
    frame_off = table_off + table_size
    if frame_off > len(data):
        raise ValueError(f"KSA table exceeds file size: {path}")

    slots: dict[int, LwKsaSlot] = {}
    cursor = frame_off
    for slot in range(slot_count):
        rec_off = table_off + slot * 28
        frame_count, _runtime_ptr, stored_slot, loop_frame = struct.unpack_from(
            "<IIII", data, rec_off
        )
        frames: list[bytes] = []
        for _ in range(frame_count):
            end = cursor + 88
            if end > len(data):
                raise ValueError(f"KSA frame data exceeds file size at slot {slot}: {path}")
            frames.append(data[cursor:end])
            cursor = end
        slots[slot] = LwKsaSlot(
            slot=slot,
            frame_count=frame_count,
            stored_slot=stored_slot,
            loop_frame=loop_frame,
            frames=tuple(frames),
        )

    return LwKsaFile(version=version, slots=slots)


def build_lw_animation_context(
    anim_def: str,
    chr_file: str,
    *,
    resolver,
) -> LwAnimationContext | None:
    anm_name = _ensure_ext(anim_def, ".anm")
    chr_stem = _strip_ext(chr_file)
    anm_path = resolver.resolve(anm_name)
    if not anm_path:
        return None

    ksa_path = resolver.resolve(_ensure_ext(chr_stem, ".ksa"))
    aca_path = resolver.resolve(_ensure_ext(chr_stem, ".aca"))
    return LwAnimationContext(
        kind="lw",
        anim_name=_strip_ext(anim_def).lower(),
        chr_name=chr_stem.lower(),
        anm=parse_anm(anm_path),
        ksa=parse_ksa(ksa_path) if ksa_path else None,
        aca=parse_aca(aca_path) if aca_path else None,
    )


def _text_lines(path: str | Path) -> list[str]:
    out: list[str] = []
    for raw in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.split("//", 1)[0].strip()
        if line:
            out.append(line)
    return out


def _saf_runtime_frame(raw_header: bytes, records: tuple[LwSafBoneRecord, ...]) -> bytes:
    out = bytearray(88)
    for i, rec in enumerate(records[:15]):
        if len(rec.raw) != 4:
            continue
        dst = i * 4
        out[dst + 0] = (rec.raw[0] + 0x80) & 0xFF
        out[dst + 1] = rec.raw[1]
        out[dst + 2] = rec.raw[2]
        out[dst + 3] = 0
    values = struct.unpack_from("<13f", raw_header)
    tail_values = [
        _saf_tail_i16(values[5] * 85.333336),
        _saf_tail_i16(values[8] * 85.333336),
        _saf_tail_i16(values[6] * 85.333336),
        _saf_tail_i16(values[9] * 85.333336),
        _saf_tail_i16(values[7] * 85.333336),
        _saf_tail_i16(values[10] * 85.333336),
        _saf_tail_i16(values[3] * 1365.3334),
        _saf_tail_i16(values[2] * -1365.3334),
        _saf_tail_i16(values[4] * -1365.3334),
    ]
    if tail_values[4] > -30:
        tail_values[4] = -30
    out[60:78] = struct.pack("<9h", *tail_values)
    out[78:88] = b"\x00" * 10
    return bytes(out)


def _saf_tail_i16(value: float) -> int:
    as_int = int(value)
    return max(-32768, min(32767, as_int))


def _ensure_ext(name: str, ext: str) -> str:
    return name if name.lower().endswith(ext.lower()) else f"{name}{ext}"


def _strip_ext(name: str) -> str:
    return name.rsplit(".", 1)[0] if "." in name else name
