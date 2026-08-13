"""Assembly of BAD/ADM export data — the inverse of animation_build.

The Blender exporter walks its armature and converts each frame's transforms
into BAD coordinate space using the helpers here; this module then stacks those
frames into the neutral carriers that the C serializer
(``bad_write``/``adm_write``) consumes. It is pure tuples + math so it runs in
Blender and ordinary CI.

Coordinate conventions mirror animation_build.py / coords.py exactly:
  * channel quaternions are BAD xyzw (what bad.cpp reads, what
    animation_build exposes as SampledBoneFrame.source_rotation_xyzw)
  * bone-table positions/translations are BAD (Y-up) space; the importer maps
    them through coords.bone_space ((x,y,z)->(x,-z,y))
  * bone-table rotations are row-major 3x3, matching BadBone.rotation
"""
from __future__ import annotations

import ctypes
from dataclasses import dataclass, field
from typing import Sequence, Tuple

from . import adm_ffi, bad_ffi, coords
from .animation_build import (
    Mat3,
    Quat,
    QuatXyzw,
    Vec3,
    mat_mul,
    mat_vec_mul,
)

IDENTITY_QUAT_XYZW: QuatXyzw = (0.0, 0.0, 0.0, 1.0)
ZERO_VEC3: Vec3 = (0.0, 0.0, 0.0)

ANIM_FLAG_LOOPED = 0x01
ANIM_FLAG_TRANSLATION = 0x02

# coords.bone_space as a matrix S (out = S @ in) and its inverse (== transpose).
_S_ROWS: Mat3 = ((1.0, 0.0, 0.0), (0.0, 0.0, -1.0), (0.0, 1.0, 0.0))
_S_INV_ROWS: Mat3 = ((1.0, 0.0, 0.0), (0.0, 0.0, 1.0), (0.0, -1.0, 0.0))


# ---------------------------------------------------------------------------
# Coordinate inverses (DCC-agnostic)
# ---------------------------------------------------------------------------


def inverse_bone_space(p: Sequence[float]) -> Vec3:
    """Inverse of coords.bone_space. Z-up RH -> BAD (Y-up)."""
    return (float(p[0]), float(p[2]), -float(p[1]))


def zup_quat_to_bad_xyzw(q: Quat) -> QuatXyzw:
    """Inverse of animation_build.bad_channel_to_zup_quat. zup wxyz -> BAD xyzw."""
    zw, zx, zy, zz = (float(q[0]), float(q[1]), float(q[2]), float(q[3]))
    return (zx, zz, -zy, zw)


def inverse_conjugate_y_to_z(m: Mat3) -> Mat3:
    """Inverse of coords.conjugate_y_to_z (S @ m @ S^-1) -> S^-1 @ m @ S."""
    return mat_mul(mat_mul(_S_INV_ROWS, m), _S_ROWS)


def dedupe_quat_sign(prev: QuatXyzw, cur: QuatXyzw) -> QuatXyzw:
    """Negate cur (xyzw) onto prev's hemisphere so a channel stays continuous."""
    dot = sum(float(prev[i]) * float(cur[i]) for i in range(4))
    if dot < 0.0:
        return (-float(cur[0]), -float(cur[1]), -float(cur[2]), -float(cur[3]))
    return (float(cur[0]), float(cur[1]), float(cur[2]), float(cur[3]))


def bad_positions_from_model(
    bone_rotations: Sequence[Mat3],
    bone_parents: Sequence[int],
    model_rel_positions: Sequence[Sequence[float]],
) -> "list[Vec3]":
    """Reconstruct ``BadBone.position`` from the model part table.

    Retail derives the field at export and never reads it back (the runtime rig
    pivots come from the model bone table), so 12/43 JO viewmodel rigs ship it
    zeroed/stale.  The witnessed relation, corpus-exact on healthy rigs
    (docs/net/novaworld-net-re.md section 5.40):

        position[i] = bind_rows[parent(i)] @ (-rel.x, rel.y, rel.z)

    where ``bind_rows`` is the parent's stored bind 3x3 (``BadBone.rotation``,
    row-major as parsed — identical to the reset clip's frame-0 channel
    transposed) and ``rel`` is the model part's parent-relative pivot in the
    engine frame (``ThreediRenderObject.rel``); the x-negation is the
    engine's own model->render frame map.  Root bones (parent < 0 or
    self-parented) take the x-negated rel unrotated (zero on every shipped rig).

    Inputs are index-paired (bone i <-> part i, the runtime pairing); pass
    arrays already trimmed to the paired range.  ``bone_rotations`` may be
    longer than ``model_rel_positions`` (e.g. AKM_1st's 46 bones / 45 parts).
    """
    out: list[Vec3] = []
    for i, rel in enumerate(model_rel_positions):
        flipped = (-float(rel[0]), float(rel[1]), float(rel[2]))
        parent = int(bone_parents[i])
        if parent < 0 or parent == i or parent >= len(bone_rotations):
            out.append(flipped)
            continue
        out.append(mat_vec_mul(bone_rotations[parent], flipped))
    return out


# ---------------------------------------------------------------------------
# Neutral carriers (BAD coordinate space)
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class BadBoneOut:
    name: str
    parent_index: int
    length: float
    position_bad: Vec3
    rotation_rows_bad: Mat3  # row-major, == BadBone.rotation


@dataclass(frozen=True)
class BadChannelOut:
    frame_lengths: Tuple[int, ...]
    rotations_xyzw: Tuple[QuatXyzw, ...]


@dataclass(frozen=True)
class BadEventOut:
    velocity_bad: Vec3
    bottom: float = 0.0
    top: float = 0.0
    trigger: int = 0


@dataclass(frozen=True)
class BadClipOut:
    frame_count: int
    bones: Tuple[BadBoneOut, ...] = ()
    channels: Tuple[BadChannelOut, ...] = ()
    translations: Tuple[Tuple[Vec3, ...], ...] = ()  # [frame][bone], BAD space
    events: Tuple[BadEventOut, ...] = ()
    version: int = 1
    fps: int = 30
    flags: int = 0
    name: str = ""
    bad_name: str = ""
    is_reset: bool = False


@dataclass(frozen=True)
class AdmClipRef:
    animation_name: str
    bad_name: str
    is_reset: bool = False


# ---------------------------------------------------------------------------
# Assembly (per-frame BAD-space transforms -> BadClipOut)
# ---------------------------------------------------------------------------


def assemble_bad_clip(
    *,
    bones: Sequence[BadBoneOut],
    per_frame_rotations_xyzw: Sequence[Sequence[QuatXyzw]],
    frame_count: int,
    flags: int = 0,
    fps: int = 30,
    version: int = 1,
    per_frame_translations: Sequence[Sequence[Vec3]] | None = None,
    events: Sequence[BadEventOut] | None = None,
    name: str = "",
    bad_name: str = "",
    is_reset: bool = False,
) -> BadClipOut:
    """Stack already-BAD-space per-frame transforms into a BadClipOut.

    Channels, translations and events each gain the terminal-duplicate sample
    (frame_count+1 entries) that stock .bad files and the engine expect.
    per_frame_rotations_xyzw[frame][bone] are BAD channel quats (world).
    per_frame_translations[frame][bone] are BAD-space local translations.
    """
    bone_count = len(bones)
    channels: list[BadChannelOut] = []
    for b in range(bone_count):
        rots: list[QuatXyzw] = []
        prev: QuatXyzw | None = None
        for f in range(frame_count):
            q = tuple(float(c) for c in per_frame_rotations_xyzw[f][b])  # type: ignore[assignment]
            if prev is not None:
                q = dedupe_quat_sign(prev, q)
            prev = q  # type: ignore[assignment]
            rots.append(q)  # type: ignore[arg-type]
        rots.append(rots[-1] if rots else IDENTITY_QUAT_XYZW)  # terminal duplicate
        channels.append(BadChannelOut(
            frame_lengths=tuple([1] * frame_count + [1]),
            rotations_xyzw=tuple(rots),
        ))

    translations: Tuple[Tuple[Vec3, ...], ...] = ()
    if (flags & ANIM_FLAG_TRANSLATION) and per_frame_translations is not None:
        rows = [
            tuple(
                (float(per_frame_translations[f][b][0]),
                 float(per_frame_translations[f][b][1]),
                 float(per_frame_translations[f][b][2]))
                for b in range(bone_count)
            )
            for f in range(frame_count)
        ]
        if rows:
            rows.append(rows[-1])  # terminal duplicate
        translations = tuple(rows)

    evs: Tuple[BadEventOut, ...] = ()
    if events:
        evs = tuple(events)
        evs = evs + (evs[-1],)  # terminal duplicate

    return BadClipOut(
        frame_count=frame_count,
        bones=tuple(bones),
        channels=tuple(channels),
        translations=translations,
        events=evs,
        version=version,
        fps=fps,
        flags=flags,
        name=name,
        bad_name=bad_name,
        is_reset=is_reset,
    )


# ---------------------------------------------------------------------------
# Marshalling to / from the C BadFile struct
# ---------------------------------------------------------------------------


def _badfile_from_clip(clip: BadClipOut):
    """Build a bad_ffi.BadFile from a BadClipOut.

    Returns (BadFile, keepalive) — the caller must keep `keepalive` referenced
    until after the C write call so the backing ctypes arrays are not collected.
    """
    keep: list = []
    bf = bad_ffi.BadFile()
    bf.version = int(clip.version)
    bf.header_size = 80
    bf.fps = int(clip.fps)
    bf.frame_count = int(clip.frame_count)
    bf.flags = int(clip.flags)

    bone_count = len(clip.bones)
    bf.bone_count = bone_count
    bones = (bad_ffi.BadBone * bone_count)()
    for i, b in enumerate(clip.bones):
        bones[i].name = str(b.name).encode("utf-8")[:32]
        bones[i].parent_index = int(b.parent_index)
        bones[i].length = float(b.length)
        for k in range(3):
            bones[i].position[k] = float(b.position_bad[k])
        rows = b.rotation_rows_bad
        for r in range(3):
            for c in range(3):
                bones[i].rotation[r * 3 + c] = float(rows[r][c])
    bf.bones = bones
    bf.num_bones = bone_count
    keep.append(bones)

    chans = (bad_ffi.BadChannel * len(clip.channels))()
    for i, ch in enumerate(clip.channels):
        nf = len(ch.rotations_xyzw)
        chans[i].frame_count = nf
        chans[i].frame_lengths_offset = 0
        chans[i].rotations_offset = 0
        fl = (ctypes.c_uint16 * nf)(*[int(x) for x in ch.frame_lengths])
        chans[i].frame_lengths = ctypes.cast(fl, ctypes.POINTER(ctypes.c_uint16))
        keep.append(fl)
        q = (bad_ffi.BadQuaternion * nf)()
        for j, rot in enumerate(ch.rotations_xyzw):
            q[j].x = float(rot[0])
            q[j].y = float(rot[1])
            q[j].z = float(rot[2])
            q[j].w = float(rot[3])
        chans[i].rotations = ctypes.cast(q, ctypes.POINTER(bad_ffi.BadQuaternion))
        keep.append(q)
    bf.channels = chans
    bf.num_channels = len(clip.channels)
    keep.append(chans)

    if clip.events:
        evs = (bad_ffi.BadEvent * len(clip.events))()
        for i, e in enumerate(clip.events):
            for k in range(3):
                evs[i].velocity[k] = float(e.velocity_bad[k])
            evs[i].bottom = float(e.bottom)
            evs[i].top = float(e.top)
            evs[i].trigger = int(e.trigger)
        bf.events = evs
        bf.num_events = len(clip.events)
        keep.append(evs)

    if (clip.flags & ANIM_FLAG_TRANSLATION) and clip.translations:
        flat: list = []
        for frame in clip.translations:
            for v in frame:
                flat.append(v)
        trans = ((ctypes.c_float * 3) * len(flat))()
        for i, v in enumerate(flat):
            trans[i][0] = float(v[0])
            trans[i][1] = float(v[1])
            trans[i][2] = float(v[2])
        bf.translations = ctypes.cast(trans, ctypes.POINTER(ctypes.c_float * 3))
        bf.num_translations = len(flat)
        keep.append(trans)

    return bf, keep


def neutral_from_badfile(bf) -> BadClipOut:
    """Build a BadClipOut from a parsed bad_ffi.BadFile (round-trip helper / tests)."""
    bone_count = int(bf.num_bones)
    bones: list[BadBoneOut] = []
    for i in range(bone_count):
        b = bf.bones[i]
        name = b.name.decode("utf-8", "replace") if isinstance(b.name, bytes) else str(b.name)
        rows = (
            (float(b.rotation[0]), float(b.rotation[1]), float(b.rotation[2])),
            (float(b.rotation[3]), float(b.rotation[4]), float(b.rotation[5])),
            (float(b.rotation[6]), float(b.rotation[7]), float(b.rotation[8])),
        )
        bones.append(BadBoneOut(
            name=name,
            parent_index=int(b.parent_index),
            length=float(b.length),
            position_bad=(float(b.position[0]), float(b.position[1]), float(b.position[2])),
            rotation_rows_bad=rows,
        ))

    channels: list[BadChannelOut] = []
    for i in range(int(bf.num_channels)):
        ch = bf.channels[i]
        nf = int(ch.frame_count)
        fl = tuple(int(ch.frame_lengths[j]) for j in range(nf))
        rots = tuple(
            (float(ch.rotations[j].x), float(ch.rotations[j].y),
             float(ch.rotations[j].z), float(ch.rotations[j].w))
            for j in range(nf)
        )
        channels.append(BadChannelOut(frame_lengths=fl, rotations_xyzw=rots))

    events: list[BadEventOut] = []
    for i in range(int(bf.num_events)):
        e = bf.events[i]
        events.append(BadEventOut(
            velocity_bad=(float(e.velocity[0]), float(e.velocity[1]), float(e.velocity[2])),
            bottom=float(e.bottom),
            top=float(e.top),
            trigger=int(e.trigger),
        ))

    translations: Tuple[Tuple[Vec3, ...], ...] = ()
    if (int(bf.flags) & ANIM_FLAG_TRANSLATION) and int(bf.num_translations) > 0:
        fc = int(bf.frame_count)
        rows_out: list = []
        idx = 0
        for _f in range(fc):
            frame: list = []
            for _b in range(bone_count):
                v = bf.translations[idx]
                frame.append((float(v[0]), float(v[1]), float(v[2])))
                idx += 1
            rows_out.append(tuple(frame))
        translations = tuple(rows_out)

    return BadClipOut(
        frame_count=int(bf.frame_count),
        bones=tuple(bones),
        channels=tuple(channels),
        translations=translations,
        events=tuple(events),
        version=int(bf.version),
        fps=int(bf.fps),
        flags=int(bf.flags),
    )


# ---------------------------------------------------------------------------
# Public write API
# ---------------------------------------------------------------------------


def write_bad(path: str, clip: BadClipOut, options=None) -> None:
    """Serialize a BadClipOut to a .bad file via the C writer."""
    bf, keep = _badfile_from_clip(clip)
    bad_ffi.write_bad(path, bf, options)
    # `keep` (and bf's pointer fields) must stay alive across the C call above.
    keep.clear()


def write_adm(path: str, clips) -> None:
    """Write the companion .adm. `clips` is an iterable of AdmClipRef-like objects.

    The reset clip is emitted first under the key ``anim_reset``; the rest use
    their animation_name as the key. Both pull their value from bad_name.
    """
    refs = list(clips)
    entries: list[tuple[str, str]] = []
    for r in refs:
        if getattr(r, "is_reset", False):
            entries.append(("anim_reset", r.bad_name))
    for r in refs:
        if not getattr(r, "is_reset", False):
            entries.append((r.animation_name, r.bad_name))
    adm_ffi.write_adm(path, entries)
