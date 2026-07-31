"""DCC-neutral assembly of BAD/ADM export data — the inverse of animation_build.

A DCC adapter (Blender or 3ds Max) walks its armature and converts each frame's
transforms into BAD coordinate space using the DCC-specific helpers here; this
module then stacks those frames into the neutral carriers that the C serializer
(``bad_write``/``adm_write``) consumes. It is pure tuples + math so it runs in
Blender, 3ds Max's bundled Python, and ordinary CI.

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
    bad_channel_to_zup_quat,
    mat_mul,
    mat_transpose,
    mat_vec_mul,
    quat_to_matrix,
    quat_xyzw_to_matrix_rows,
    vec_sub,
)

IDENTITY_QUAT_XYZW: QuatXyzw = (0.0, 0.0, 0.0, 1.0)
ZERO_VEC3: Vec3 = (0.0, 0.0, 0.0)

ANIM_FLAG_LOOPED = 0x01
ANIM_FLAG_TRANSLATION = 0x02

# Max world-rotation conjugation constants — identical to the import side in
# opennova_max/animation.py (cross-checked by tests/test_bad_build_inverse.py).
_MAX_GLOBAL_MATRIX_ROWS: Mat3 = (
    (0.0, 0.0, 1.0),
    (0.0, 1.0, 0.0),
    (-1.0, 0.0, 0.0),
)
_MAX_GLOBAL_CORRECTION_ROWS: Mat3 = (
    (1.0, 0.0, 0.0),
    (0.0, 0.0, 1.0),
    (0.0, -1.0, 0.0),
)

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


def bad_rows_from_max_world_rows(rows: Mat3) -> Mat3:
    """Inverse of opennova_max.animation.max_rows_from_source_quat's row transform.

    Recovers the BAD-stored rotation rows (== quat_xyzw_to_matrix_rows(q)) from
    a Max world-rotation matrix's rows.
    """
    g_inv = mat_transpose(_MAX_GLOBAL_MATRIX_ROWS)
    c_inv = mat_transpose(_MAX_GLOBAL_CORRECTION_ROWS)
    return mat_mul(mat_mul(g_inv, rows), c_inv)


def bad_xyzw_from_max_world_rows(rows: Mat3) -> QuatXyzw:
    """Recover the BAD channel quaternion (xyzw) from Max world-rotation rows."""
    bad_rows = bad_rows_from_max_world_rows(rows)
    # quat_xyzw_to_matrix_rows(q) = transpose(quat_to_matrix(wxyz)); undo it.
    rot = mat_transpose(bad_rows)
    w, x, y, z = matrix_to_quat_wxyz(rot)
    return (x, y, z, w)


def matrix_to_quat_wxyz(m: Mat3) -> Quat:
    """Convert a row-major 3x3 rotation matrix to a normalized wxyz quaternion."""
    m00, m01, m02 = (float(m[0][0]), float(m[0][1]), float(m[0][2]))
    m10, m11, m12 = (float(m[1][0]), float(m[1][1]), float(m[1][2]))
    m20, m21, m22 = (float(m[2][0]), float(m[2][1]), float(m[2][2]))
    tr = m00 + m11 + m22
    if tr > 0.0:
        s = (tr + 1.0) ** 0.5 * 2.0
        w = 0.25 * s
        x = (m21 - m12) / s
        y = (m02 - m20) / s
        z = (m10 - m01) / s
    elif m00 > m11 and m00 > m22:
        s = (1.0 + m00 - m11 - m22) ** 0.5 * 2.0
        w = (m21 - m12) / s
        x = 0.25 * s
        y = (m01 + m10) / s
        z = (m02 + m20) / s
    elif m11 > m22:
        s = (1.0 + m11 - m00 - m22) ** 0.5 * 2.0
        w = (m02 - m20) / s
        x = (m01 + m10) / s
        y = 0.25 * s
        z = (m12 + m21) / s
    else:
        s = (1.0 + m22 - m00 - m11) ** 0.5 * 2.0
        w = (m10 - m01) / s
        x = (m02 + m20) / s
        y = (m12 + m21) / s
        z = 0.25 * s
    n = (w * w + x * x + y * y + z * z) ** 0.5
    if n <= 1e-24:
        return (1.0, 0.0, 0.0, 0.0)
    return (w / n, x / n, y / n, z / n)


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
    engine frame (``ThreediIRPart.rel_position``); the x-negation is the
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


def rest_origins_from_max_world(
    rows0: Sequence[Mat3],
    node_pos0: Sequence[Vec3],
    root_pos0: Vec3,
    parents: Sequence[int],
) -> list[Vec3]:
    """Parent-local zup rest origins from one frame of Max world transforms.

    rows0[b] = bone b's Max node rotation rows at the reference frame;
    node_pos0[b] = its world position; root_pos0 = the root_motion node position;
    parents[b] = parent bone index (-1 for root). This recovers the same rest
    origins the importer derived via coords.bone_space(bone.position).
    """
    bone_count = len(rows0)
    zup_rot0: list[Mat3] = []
    for b in range(bone_count):
        xyzw = bad_xyzw_from_max_world_rows(rows0[b])
        zup = bad_channel_to_zup_quat(bad_ffi.BadQuaternion(xyzw[0], xyzw[1], xyzw[2], xyzw[3]))
        zup_rot0.append(quat_to_matrix(zup))
    swp0 = [vec_sub(node_pos0[b], root_pos0) for b in range(bone_count)]
    rest: list[Vec3] = [ZERO_VEC3] * bone_count
    for b in range(bone_count):
        p = int(parents[b])
        if p < 0 or p >= bone_count:
            rest[b] = swp0[b]
        else:
            rest[b] = mat_vec_mul(mat_transpose(zup_rot0[p]), vec_sub(swp0[b], swp0[p]))
    return rest


def assemble_clip_from_max_world(
    *,
    bones_meta: Sequence[Tuple[str, int, float]],
    per_frame_rows: Sequence[Sequence[Mat3]],
    per_frame_node_pos: Sequence[Sequence[Vec3]],
    per_frame_root_pos: Sequence[Vec3],
    frame_count: int,
    flags: int = 0,
    fps: int = 30,
    version: int = 1,
    name: str = "",
    bad_name: str = "",
    is_reset: bool = False,
    reset_rest_origins: Sequence[Vec3] | None = None,
) -> BadClipOut:
    """Invert the 3ds Max import keying into a BadClipOut.

    This is the exact inverse of opennova_max.animation.apply_sampled_clips +
    build_armature_from_bad, kept DCC-agnostic (no pymxs) so it is testable:

      * per_frame_rows[f][b]      = the rotation rows of bone b's Max node.transform
      * per_frame_node_pos[f][b]  = bone b's Max node.position (world; includes root)
      * per_frame_root_pos[f]     = the Bip001 (root_motion) node.position
      * bones_meta[b]             = (name, parent_index, length)

    The bone-table rest (bone.position) is taken from this clip's frame 0, matching
    stock per-clip bone tables. Per-frame translations, however, MUST be measured
    against the RESET clip's rest origins, because the importer accumulates every
    clip's world positions from the reset skeleton's rest (build_armature_from_bad
    uses the reset BAD for all clips). Pass `reset_rest_origins` for that; when it
    is None the clip's own frame-0 rest is used (correct only for the reset clip).
    Root motion events are recovered from the root-node displacement so the
    importer's accumulation reproduces per_frame_root_pos.
    """
    bone_count = len(bones_meta)
    parents = [int(bones_meta[b][1]) for b in range(bone_count)]

    # Recover BAD channel quats and zup world-rotation matrices per frame.
    source: list[list[QuatXyzw]] = []
    zup_rot: list[list[Mat3]] = []
    for f in range(frame_count):
        s_row: list[QuatXyzw] = []
        r_row: list[Mat3] = []
        for b in range(bone_count):
            xyzw = bad_xyzw_from_max_world_rows(per_frame_rows[f][b])
            s_row.append(xyzw)
            zup = bad_channel_to_zup_quat(bad_ffi.BadQuaternion(xyzw[0], xyzw[1], xyzw[2], xyzw[3]))
            r_row.append(quat_to_matrix(zup))
        source.append(s_row)
        zup_rot.append(r_row)

    # Strip the root offset to recover the sampled (zup) world positions.
    swp: list[list[Vec3]] = [
        [vec_sub(per_frame_node_pos[f][b], per_frame_root_pos[f]) for b in range(bone_count)]
        for f in range(frame_count)
    ]

    # Per-clip frame-0 rest (parent-local). Used for the bone table, matching
    # stock per-clip bone tables.
    rest_origin = rest_origins_from_max_world(
        per_frame_rows[0], per_frame_node_pos[0], per_frame_root_pos[0], parents
    )
    # Translations are anchored to the RESET skeleton's rest, because the importer
    # rebuilds every clip's world positions from the reset rest. Fall back to this
    # clip's frame-0 rest only when no reset reference is supplied (reset clip).
    trans_ref = list(reset_rest_origins) if reset_rest_origins is not None else rest_origin

    bones_out: list[BadBoneOut] = []
    for b in range(bone_count):
        rrows = bad_rows_from_max_world_rows(per_frame_rows[0][b])
        # Root bones sit at the skeleton origin in BAD space (matches the
        # importer's bone_space(bone.position)==0 for the root). Zero by root
        # status, not array index, so an unusual bone order stays correct.
        pos_bad = ZERO_VEC3 if parents[b] < 0 else inverse_bone_space(rest_origin[b])
        bones_out.append(BadBoneOut(
            name=str(bones_meta[b][0]),
            parent_index=parents[b],
            length=float(bones_meta[b][2]),
            position_bad=pos_bad,
            rotation_rows_bad=rrows,
        ))

    per_frame_trans: list[list[Vec3]] | None = None
    if flags & ANIM_FLAG_TRANSLATION:
        per_frame_trans = []
        for f in range(frame_count):
            row: list[Vec3] = []
            for b in range(bone_count):
                p = parents[b]
                if p < 0 or p >= bone_count:
                    t_zup = vec_sub(swp[f][b], trans_ref[b])
                else:
                    t_zup = vec_sub(
                        vec_sub(swp[f][b], swp[f][p]),
                        mat_vec_mul(zup_rot[f][p], trans_ref[b]),
                    )
                row.append(inverse_bone_space(t_zup))
            per_frame_trans.append(row)

    # Root-motion events: x,y accumulate from bone_space(velocity); z = bottom.
    events: list[BadEventOut] = []
    for f in range(frame_count):
        if f == 0:
            dxy = per_frame_root_pos[0]
        else:
            dxy = vec_sub(per_frame_root_pos[f], per_frame_root_pos[f - 1])
        vel_bad = inverse_bone_space((dxy[0], dxy[1], 0.0))
        bottom = float(per_frame_root_pos[f][2])
        events.append(BadEventOut(velocity_bad=vel_bad, bottom=bottom, top=bottom, trigger=0))

    return assemble_bad_clip(
        bones=bones_out,
        per_frame_rotations_xyzw=source,
        frame_count=frame_count,
        flags=flags,
        fps=fps,
        version=version,
        per_frame_translations=per_frame_trans,
        events=events,
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
