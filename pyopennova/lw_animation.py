"""Delta Force: Land Warrior sidecar animation parsing.

LW skeletal animation is not embedded in v10 ``.3di`` files.  Character
models provide skinned geometry; animation arrives through ``.KSA`` baked clip
tables plus optional ``.ACA``/``.SAF`` slot overrides and ``.ANM`` movement
metadata.  This module keeps that data out of the geometry IR until the
runtime transform semantics are fully proven.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field
from pathlib import Path
from typing import Mapping


SAF_MAGIC = b"SAF1"
KSA_MAGIC = b"KSA\0"
LW_ANIMATION_FPS = 30
LW_RUNTIME_FRAME_SIZE = 88
LW_SAF_ROOT_BLOCK_SIZE = 0x34
LW_MAX_PART_RECORDS = 15


@dataclass(frozen=True)
class LwRuntimeFrame:
    """One game-normalized 88-byte LW skeletal frame."""

    root_values: tuple[int, int, int, int, int, int, int, int, int]
    part_records: tuple[tuple[int, int], ...]
    raw: bytes = b""


@dataclass(frozen=True)
class LwSafFile:
    name: str
    tick_rate: int
    frame_count: int
    root_block_size: int
    frames: tuple[LwRuntimeFrame, ...]


@dataclass(frozen=True)
class LwKsaEntry:
    frame_count: int
    runtime_pointer: int
    slot_id: int
    loop_frame: int
    unknown: tuple[int, int, int]
    frames: tuple[LwRuntimeFrame, ...]


@dataclass(frozen=True)
class LwKsaFile:
    name: str
    version: int
    payload_size: int
    entry_count: int
    entries: tuple[LwKsaEntry, ...]


@dataclass(frozen=True)
class LwAnmMovement:
    name: str
    slot_id: int
    velocity: float
    override: bool = False


@dataclass(frozen=True)
class LwAnmFile:
    name: str
    movements: Mapping[str, LwAnmMovement]


@dataclass(frozen=True)
class LwAcaSlot:
    slot_id: int
    saf_name: str
    loop_frame: int = 0


@dataclass(frozen=True)
class LwAcaFile:
    name: str
    slots: Mapping[int, LwAcaSlot]


@dataclass(frozen=True)
class LwAnimationClip:
    slot_id: int
    source_name: str
    source_type: str
    frame_count: int
    loop_frame: int
    frames: tuple[LwRuntimeFrame, ...]


@dataclass(frozen=True)
class LwAnimationContext:
    """Resolved LW animation sidecar data for one character model."""

    anim_field: str
    chr_file: str
    anm_name: str = ""
    ksa_name: str = ""
    aca_name: str = ""
    anm: LwAnmFile | None = None
    ksa: LwKsaFile | None = None
    aca: LwAcaFile | None = None
    clips: Mapping[int, LwAnimationClip] = field(default_factory=dict)
    movement_clips: Mapping[str, LwAnimationClip] = field(default_factory=dict)
    saf_clips: Mapping[int, LwSafFile] = field(default_factory=dict)
    warnings: tuple[str, ...] = ()
    fps: int = LW_ANIMATION_FPS


def parse_saf_bytes(data: bytes, *, name: str = "") -> LwSafFile:
    if len(data) < 16:
        raise ValueError("SAF file is shorter than its 16-byte header")
    magic, tick_rate, frame_count, root_block_size = struct.unpack_from("<4sIII", data, 0)
    if magic != SAF_MAGIC:
        raise ValueError(f"{name or '<buffer>'} is not a SAF1 file")
    if root_block_size != LW_SAF_ROOT_BLOCK_SIZE:
        raise ValueError(f"{name or '<buffer>'} has unsupported SAF root block size {root_block_size}")

    offset = 16
    frames: list[LwRuntimeFrame] = []
    for frame_index in range(frame_count):
        if offset + root_block_size > len(data):
            raise ValueError(f"{name or '<buffer>'} frame {frame_index} root block is truncated")
        root = struct.unpack_from("<13f", data, offset)
        offset += root_block_size
        record_count = max(0, min(int(root[0]), LW_MAX_PART_RECORDS))
        if offset + record_count * 4 > len(data):
            raise ValueError(f"{name or '<buffer>'} frame {frame_index} part records are truncated")
        part_records: list[tuple[int, int]] = []
        raw = bytearray(LW_RUNTIME_FRAME_SIZE)
        for i in range(record_count):
            part_byte = data[offset]
            angle = struct.unpack_from("<h", data, offset + 1)[0]
            offset += 4
            token = (part_byte + 0x80) & 0xFF
            part_records.append((token, angle))
            rec_off = i * 4
            raw[rec_off] = token
            struct.pack_into("<h", raw, rec_off + 1, angle)
        root_values = _normalize_saf_root_values(root)
        _write_runtime_root_values(raw, root_values)
        frames.append(LwRuntimeFrame(root_values=root_values, part_records=tuple(part_records), raw=bytes(raw)))

    return LwSafFile(
        name=name,
        tick_rate=int(tick_rate),
        frame_count=int(frame_count),
        root_block_size=int(root_block_size),
        frames=tuple(frames),
    )


def parse_saf_file(path: str | Path) -> LwSafFile:
    p = Path(path)
    return parse_saf_bytes(p.read_bytes(), name=p.name)


def parse_ksa_bytes(data: bytes, *, name: str = "") -> LwKsaFile:
    if len(data) < 100:
        raise ValueError("KSA file is shorter than its 100-byte header")
    if data[:4] != KSA_MAGIC:
        raise ValueError(f"{name or '<buffer>'} is not a KSA file")
    dwords = [struct.unpack_from("<I", data, off)[0] for off in range(4, 100, 4)]
    version = dwords[0]
    payload_size = dwords[10]  # file offset +0x2c
    entry_count = dwords[12]   # file offset +0x34
    payload_end = 100 + payload_size
    if payload_end > len(data):
        raise ValueError(f"{name or '<buffer>'} KSA payload is truncated")
    table_size = entry_count * 28
    frame_offset = 100 + table_size
    if frame_offset > payload_end:
        raise ValueError(f"{name or '<buffer>'} KSA slot table exceeds payload")

    entries: list[LwKsaEntry] = []
    cursor = frame_offset
    for index in range(entry_count):
        off = 100 + index * 28
        frame_count, runtime_pointer, slot_id, loop_frame, u0, u1, u2 = struct.unpack_from("<7I", data, off)
        byte_count = frame_count * LW_RUNTIME_FRAME_SIZE
        if cursor + byte_count > payload_end:
            raise ValueError(f"{name or '<buffer>'} KSA frames for slot {slot_id} are truncated")
        frames = tuple(
            _parse_runtime_frame(data[cursor + i * LW_RUNTIME_FRAME_SIZE: cursor + (i + 1) * LW_RUNTIME_FRAME_SIZE])
            for i in range(frame_count)
        )
        cursor += byte_count
        entries.append(LwKsaEntry(
            frame_count=int(frame_count),
            runtime_pointer=int(runtime_pointer),
            slot_id=int(slot_id),
            loop_frame=int(loop_frame),
            unknown=(int(u0), int(u1), int(u2)),
            frames=frames,
        ))

    return LwKsaFile(
        name=name,
        version=int(version),
        payload_size=int(payload_size),
        entry_count=int(entry_count),
        entries=tuple(entries),
    )


def parse_ksa_file(path: str | Path) -> LwKsaFile:
    p = Path(path)
    return parse_ksa_bytes(p.read_bytes(), name=p.name)


def parse_anm_text(text: str, *, name: str = "") -> LwAnmFile:
    movements: dict[str, LwAnmMovement] = {}
    for line in text.splitlines():
        tokens = _tokens_before_comment(line)
        if len(tokens) < 2:
            continue
        try:
            slot_id = int(tokens[1], 0)
        except ValueError:
            continue
        velocity = 0.0
        if len(tokens) >= 3:
            try:
                velocity = float(tokens[2])
            except ValueError:
                velocity = 0.0
        override = any(token.lower() == "override" for token in tokens[3:])
        movement = LwAnmMovement(tokens[0], slot_id, velocity, override)
        movements[movement.name] = movement
    return LwAnmFile(name=name, movements=movements)


def parse_anm_file(path: str | Path) -> LwAnmFile:
    p = Path(path)
    return parse_anm_text(p.read_text(encoding="latin1"), name=p.name)


def parse_aca_text(text: str, *, name: str = "") -> LwAcaFile:
    slots: dict[int, LwAcaSlot] = {}
    for line in text.splitlines():
        tokens = _tokens_before_comment(line)
        if len(tokens) < 3 or tokens[0].lower() != "slot":
            continue
        try:
            slot_id = int(tokens[1], 0)
        except ValueError:
            continue
        loop_frame = 0
        if len(tokens) >= 4:
            try:
                loop_frame = int(tokens[3], 0)
            except ValueError:
                loop_frame = 0
        slots[slot_id] = LwAcaSlot(slot_id=slot_id, saf_name=tokens[2], loop_frame=loop_frame)
    return LwAcaFile(name=name, slots=slots)


def parse_aca_file(path: str | Path) -> LwAcaFile:
    p = Path(path)
    return parse_aca_text(p.read_text(encoding="latin1"), name=p.name)


def build_lw_animation_context(
    anim_field: str | None,
    *,
    resolver,
    chr_file: str | None = None,
) -> LwAnimationContext | None:
    if not anim_field or resolver is None:
        return None

    warnings: list[str] = []
    anm_name = _ensure_extension(anim_field, ".anm")
    anm_path = resolver.resolve(anm_name)
    if not anm_path:
        return None

    anm = parse_anm_file(anm_path)
    chr_stem = chr_file or "player01"
    ksa = None
    ksa_name = ""
    ksa_path = resolver.resolve(_ensure_extension(chr_stem, ".ksa"))
    if ksa_path:
        try:
            ksa = parse_ksa_file(ksa_path)
            ksa_name = ksa.name
        except Exception as exc:
            warnings.append(f"{Path(ksa_path).name}: {exc}")

    aca = None
    aca_name = ""
    aca_path = resolver.resolve(_ensure_extension(chr_stem, ".aca"))
    if aca_path:
        try:
            aca = parse_aca_file(aca_path)
            aca_name = aca.name
        except Exception as exc:
            warnings.append(f"{Path(aca_path).name}: {exc}")

    clips: dict[int, LwAnimationClip] = {}
    if ksa:
        for entry in ksa.entries:
            clips[entry.slot_id] = LwAnimationClip(
                slot_id=entry.slot_id,
                source_name=ksa.name,
                source_type="ksa",
                frame_count=entry.frame_count,
                loop_frame=entry.loop_frame,
                frames=entry.frames,
            )

    saf_clips: dict[int, LwSafFile] = {}
    if aca:
        for slot in aca.slots.values():
            saf_path = resolver.resolve(slot.saf_name)
            if not saf_path:
                warnings.append(f"{slot.saf_name}: not found")
                continue
            try:
                saf = parse_saf_file(saf_path)
            except Exception as exc:
                warnings.append(f"{slot.saf_name}: {exc}")
                continue
            saf_clips[slot.slot_id] = saf
            clips[slot.slot_id] = LwAnimationClip(
                slot_id=slot.slot_id,
                source_name=saf.name or slot.saf_name,
                source_type="saf",
                frame_count=saf.frame_count,
                loop_frame=slot.loop_frame,
                frames=saf.frames,
            )

    movement_clips: dict[str, LwAnimationClip] = {}
    for movement in anm.movements.values():
        clip = clips.get(movement.slot_id)
        if clip is not None:
            movement_clips[movement.name] = clip

    return LwAnimationContext(
        anim_field=anim_field,
        chr_file=chr_stem,
        anm_name=anm.name,
        ksa_name=ksa_name,
        aca_name=aca_name,
        anm=anm,
        ksa=ksa,
        aca=aca,
        clips=clips,
        movement_clips=movement_clips,
        saf_clips=saf_clips,
        warnings=tuple(warnings),
    )


def is_lw_animation_context(value: object) -> bool:
    return isinstance(value, LwAnimationContext)


def _tokens_before_comment(line: str) -> list[str]:
    line = line.split("//", 1)[0].strip()
    if not line:
        return []
    return line.split()


def _ensure_extension(filename: str, ext: str) -> str:
    return filename if filename.lower().endswith(ext.lower()) else f"{filename}{ext}"


def _parse_runtime_frame(raw: bytes) -> LwRuntimeFrame:
    if len(raw) != LW_RUNTIME_FRAME_SIZE:
        raise ValueError("runtime frame must be exactly 88 bytes")
    records: list[tuple[int, int]] = []
    for i in range(LW_MAX_PART_RECORDS):
        off = i * 4
        token = raw[off]
        angle = struct.unpack_from("<h", raw, off + 1)[0]
        if token != 0 or angle != 0:
            records.append((token, angle))
    root_values = tuple(struct.unpack_from("<9h", raw, 60))
    return LwRuntimeFrame(root_values=root_values, part_records=tuple(records), raw=bytes(raw))


def _normalize_saf_root_values(root: tuple[float, ...]) -> tuple[int, int, int, int, int, int, int, int, int]:
    v4 = _ftol(root[7] * 85.333336)
    if v4 > -30:
        v4 = -30
    return (
        _ftol(root[5] * 85.333336),
        _ftol(root[8] * 85.333336),
        _ftol(root[6] * 85.333336),
        _ftol(root[9] * 85.333336),
        v4,
        _ftol(root[10] * 85.333336),
        _ftol(root[3] * 1365.3334),
        _ftol(root[2] * -1365.3334),
        _ftol(root[4] * -1365.3334),
    )


def _write_runtime_root_values(raw: bytearray, values: tuple[int, ...]) -> None:
    for i, value in enumerate(values[:9]):
        struct.pack_into("<h", raw, 60 + i * 2, _clamp_i16(value))


def _ftol(value: float) -> int:
    return int(value)


def _clamp_i16(value: int) -> int:
    return max(-32768, min(32767, int(value)))
