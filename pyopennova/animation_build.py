"""DCC-neutral BAD animation sampling.

This module contains the BAD/ADM frame math shared by DCC importers.  It
intentionally uses only plain tuples and Python math so it can run in Blender,
3ds Max's bundled Python, and normal CI.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Callable, Iterable, Sequence, Tuple

from . import coords

Vec3 = Tuple[float, float, float]
Quat = Tuple[float, float, float, float]  # w, x, y, z
QuatXyzw = Tuple[float, float, float, float]
Mat3 = Tuple[Vec3, Vec3, Vec3]

IDENTITY_QUAT: Quat = (1.0, 0.0, 0.0, 0.0)
ZERO_VEC3: Vec3 = (0.0, 0.0, 0.0)


@dataclass(frozen=True)
class SampledBoneFrame:
    """One bone's transform at one BAD frame."""

    bone_index: int
    name: str
    parent_index: int
    world_rotation: Quat
    world_position: Vec3
    local_rotation: Quat
    local_position: Vec3
    source_rotation_xyzw: QuatXyzw


@dataclass(frozen=True)
class SampledFrame:
    frame_index: int
    frame: int
    bones: tuple[SampledBoneFrame, ...]
    root_motion_position: Vec3 = ZERO_VEC3
    max_root_motion_position: Vec3 = ZERO_VEC3


@dataclass(frozen=True)
class SampledClip:
    name: str
    bad_name: str
    flags: int
    fps: int
    frame_count: int
    start_frame: int
    end_frame: int
    is_reset: bool
    frames: tuple[SampledFrame, ...] = field(default_factory=tuple)


@dataclass(frozen=True)
class SampledAnimationSet:
    clips: tuple[SampledClip, ...]
    warnings: tuple[str, ...] = field(default_factory=tuple)


def sample_bad_clip(
    bad_file,
    bone_infos: Sequence[object],
    *,
    animation_name: str,
    bad_name: str = "",
    flags: int | None = None,
    fps: int | None = None,
    start_frame: int = 1,
    is_reset: bool = False,
    world_rot_corrections: Sequence[Quat] | None = None,
) -> SampledClip:
    """Sample one parsed BAD file into DCC-neutral frame data."""

    frame_count = max(0, int(getattr(bad_file, "frame_count", 0)))
    bad_flags = int(getattr(bad_file, "flags", 0) if flags is None else flags)
    bone_count = min(int(getattr(bad_file, "num_bones", 0)), len(bone_infos))
    parent_indices = [_bone_parent(bone_infos[i], bone_count) for i in range(bone_count)]
    bone_names = [_bone_name(bone_infos[i], i) for i in range(bone_count)]
    rest_origins = [_bone_rest_origin(bone_infos[i]) for i in range(bone_count)]
    corrections = _coerce_corrections(world_rot_corrections, bone_count)
    translated = (bad_flags & 0x02) != 0

    frames: list[SampledFrame] = []
    root_motion = ZERO_VEC3
    max_root_xy = ZERO_VEC3
    max_root_motion = ZERO_VEC3
    for frame_idx in range(frame_count):
        world_rots: list[Quat] = [IDENTITY_QUAT] * bone_count
        world_positions: list[Vec3] = [ZERO_VEC3] * bone_count
        source_rots: list[QuatXyzw] = [(0.0, 0.0, 0.0, 1.0)] * bone_count

        for bone_idx in range(bone_count):
            source_q, zup_q = _channel_rotation(bad_file, bone_idx, frame_idx)
            source_rots[bone_idx] = source_q
            if corrections[bone_idx] != IDENTITY_QUAT:
                zup_q = quat_normalize(quat_mul(zup_q, corrections[bone_idx]))

            translation = ZERO_VEC3
            if translated:
                translation = _bone_translation(bad_file, bone_idx, frame_idx)

            parent_idx = parent_indices[bone_idx]
            if parent_idx < 0 or parent_idx >= bone_count:
                world_rots[bone_idx] = zup_q
                world_positions[bone_idx] = vec_add(rest_origins[bone_idx], translation)
            else:
                world_rots[bone_idx] = zup_q
                parent_rot = quat_to_matrix(world_rots[parent_idx])
                world_positions[bone_idx] = vec_add(
                    vec_add(world_positions[parent_idx], mat_vec_mul(parent_rot, rest_origins[bone_idx])),
                    translation,
                )

        bone_frames: list[SampledBoneFrame] = []
        for bone_idx in range(bone_count):
            parent_idx = parent_indices[bone_idx]
            if parent_idx < 0 or parent_idx >= bone_count:
                local_rot = world_rots[bone_idx]
                local_pos = world_positions[bone_idx]
            else:
                local_rot = quat_mul(quat_inv(world_rots[parent_idx]), world_rots[bone_idx])
                local_pos = mat_vec_mul(
                    mat_transpose(quat_to_matrix(world_rots[parent_idx])),
                    vec_sub(world_positions[bone_idx], world_positions[parent_idx]),
                )
            bone_frames.append(SampledBoneFrame(
                bone_index=bone_idx,
                name=bone_names[bone_idx],
                parent_index=parent_idx,
                world_rotation=quat_normalize(world_rots[bone_idx]),
                world_position=world_positions[bone_idx],
                local_rotation=quat_normalize(local_rot),
                local_position=local_pos,
                source_rotation_xyzw=source_rots[bone_idx],
            ))

        if frame_idx < int(getattr(bad_file, "num_events", 0)):
            evt = bad_file.events[frame_idx]
            event_velocity = coords.bone_space(evt.velocity)
            root_motion = vec_add(root_motion, event_velocity)
            max_root_xy = vec_add(max_root_xy, (event_velocity[0], event_velocity[1], 0.0))
            max_root_motion = (max_root_xy[0], max_root_xy[1], float(getattr(evt, "bottom", 0.0)))

        frames.append(SampledFrame(
            frame_index=frame_idx,
            frame=start_frame + frame_idx,
            bones=tuple(bone_frames),
            root_motion_position=root_motion,
            max_root_motion_position=max_root_motion,
        ))

    end_frame = start_frame + max(frame_count - 1, 0)
    return SampledClip(
        name=animation_name,
        bad_name=bad_name,
        flags=bad_flags,
        fps=int(getattr(bad_file, "fps", 0) if fps is None else fps),
        frame_count=frame_count,
        start_frame=start_frame,
        end_frame=end_frame,
        is_reset=is_reset,
        frames=tuple(frames),
    )


def sample_animation_context(
    anim_context,
    bone_infos: Sequence[object],
    *,
    world_rot_corrections: Sequence[Quat] | None = None,
    parse_bad: Callable[[str], object] | None = None,
    free_bad: Callable[[object], None] | None = None,
) -> SampledAnimationSet:
    """Parse and sample reset + ADM-listed BAD clips, continuing on failures."""

    if parse_bad is None or free_bad is None:
        from .bad_ffi import free_bad as _free_bad, parse_bad as _parse_bad
        parse_bad = parse_bad or _parse_bad
        free_bad = free_bad or _free_bad

    clips: list[SampledClip] = []
    warnings: list[str] = []
    frame_cursor = 1
    for is_reset, meta in _iter_animation_metas(anim_context):
        bad_file = None
        try:
            bad_file = parse_bad(meta.bad_filepath)
            clip = sample_bad_clip(
                bad_file,
                bone_infos,
                animation_name=meta.animation_name,
                bad_name=getattr(meta, "bad_name", ""),
                flags=getattr(meta, "flags", None),
                fps=getattr(meta, "fps", None),
                start_frame=frame_cursor,
                is_reset=is_reset,
                world_rot_corrections=world_rot_corrections,
            )
            clips.append(clip)
            frame_cursor += max(clip.frame_count, 0)
        except Exception as exc:
            warnings.append(f"{getattr(meta, 'animation_name', '<unknown>')}: {exc}")
        finally:
            if bad_file is not None:
                try:
                    free_bad(bad_file)
                except Exception as exc:
                    warnings.append(f"{getattr(meta, 'animation_name', '<unknown>')} cleanup: {exc}")

    return SampledAnimationSet(clips=tuple(clips), warnings=tuple(warnings))


def bad_channel_to_zup_quat(rq) -> Quat:
    """Convert a BAD channel quaternion (x, y, z, w) to Z-up wxyz."""

    return quat_normalize((float(rq.w), float(rq.x), -float(rq.z), float(rq.y)))


def quat_xyzw_to_matrix_rows(q: QuatXyzw) -> Mat3:
    """Return row vectors in BAD's stored rotation-matrix convention."""

    x, y, z, w = (float(q[0]), float(q[1]), float(q[2]), float(q[3]))
    w, x, y, z = quat_normalize((w, x, y, z))
    return mat_transpose(quat_to_matrix((w, x, y, z)))


def quat_normalize(q: Quat) -> Quat:
    w, x, y, z = (float(q[0]), float(q[1]), float(q[2]), float(q[3]))
    n2 = w * w + x * x + y * y + z * z
    if n2 <= 1e-24:
        return IDENTITY_QUAT
    inv = n2 ** -0.5
    return (w * inv, x * inv, y * inv, z * inv)


def quat_mul(a: Quat, b: Quat) -> Quat:
    aw, ax, ay, az = a
    bw, bx, by, bz = b
    return (
        aw * bw - ax * bx - ay * by - az * bz,
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
    )


def quat_inv(q: Quat) -> Quat:
    w, x, y, z = quat_normalize(q)
    return (w, -x, -y, -z)


def quat_to_matrix(q: Quat) -> Mat3:
    w, x, y, z = quat_normalize(q)
    xx = x * x
    yy = y * y
    zz = z * z
    xy = x * y
    xz = x * z
    yz = y * z
    wx = w * x
    wy = w * y
    wz = w * z
    return (
        (1.0 - 2.0 * (yy + zz), 2.0 * (xy - wz), 2.0 * (xz + wy)),
        (2.0 * (xy + wz), 1.0 - 2.0 * (xx + zz), 2.0 * (yz - wx)),
        (2.0 * (xz - wy), 2.0 * (yz + wx), 1.0 - 2.0 * (xx + yy)),
    )


def mat_mul(a: Mat3, b: Mat3) -> Mat3:
    return tuple(
        tuple(sum(float(a[row][k]) * float(b[k][col]) for k in range(3)) for col in range(3))
        for row in range(3)
    )  # type: ignore[return-value]


def mat_transpose(m: Mat3) -> Mat3:
    return (
        (m[0][0], m[1][0], m[2][0]),
        (m[0][1], m[1][1], m[2][1]),
        (m[0][2], m[1][2], m[2][2]),
    )


def mat_vec_mul(m: Mat3, v: Sequence[float]) -> Vec3:
    return (
        m[0][0] * float(v[0]) + m[0][1] * float(v[1]) + m[0][2] * float(v[2]),
        m[1][0] * float(v[0]) + m[1][1] * float(v[1]) + m[1][2] * float(v[2]),
        m[2][0] * float(v[0]) + m[2][1] * float(v[1]) + m[2][2] * float(v[2]),
    )


def vec_add(a: Sequence[float], b: Sequence[float]) -> Vec3:
    return (float(a[0]) + float(b[0]), float(a[1]) + float(b[1]), float(a[2]) + float(b[2]))


def vec_sub(a: Sequence[float], b: Sequence[float]) -> Vec3:
    return (float(a[0]) - float(b[0]), float(a[1]) - float(b[1]), float(a[2]) - float(b[2]))


def _iter_animation_metas(anim_context) -> Iterable[tuple[bool, object]]:
    reset = getattr(anim_context, "reset_animation", None)
    if reset is not None:
        yield True, reset
    for meta in getattr(anim_context, "animations", ()) or ():
        yield False, meta


def _channel_rotation(bad_file, bone_idx: int, frame_idx: int) -> tuple[QuatXyzw, Quat]:
    if bone_idx >= int(getattr(bad_file, "num_channels", 0)):
        return (0.0, 0.0, 0.0, 1.0), IDENTITY_QUAT
    channel = bad_file.channels[bone_idx]
    if frame_idx >= int(getattr(channel, "frame_count", 0)):
        return (0.0, 0.0, 0.0, 1.0), IDENTITY_QUAT
    rq = channel.rotations[frame_idx]
    source = (float(rq.x), float(rq.y), float(rq.z), float(rq.w))
    return source, bad_channel_to_zup_quat(rq)


def _bone_translation(bad_file, bone_idx: int, frame_idx: int) -> Vec3:
    stride = int(getattr(bad_file, "num_bones", 0))
    idx = frame_idx * stride + bone_idx
    if idx >= int(getattr(bad_file, "num_translations", 0)):
        return ZERO_VEC3
    return coords.bone_space(bad_file.translations[idx])


def _coerce_corrections(values: Sequence[Quat] | None, bone_count: int) -> list[Quat]:
    out = [IDENTITY_QUAT] * bone_count
    if values is None:
        return out
    for i in range(min(bone_count, len(values))):
        out[i] = quat_normalize(values[i])
    return out


def _bone_name(info: object, index: int) -> str:
    try:
        name = info[0]  # type: ignore[index]
    except Exception:
        name = getattr(info, "name", "")
    return str(name or f"BN{index + 1:02d}")


def _bone_parent(info: object, bone_count: int) -> int:
    try:
        parent = int(info[1])  # type: ignore[index]
    except Exception:
        parent = int(getattr(info, "parent_index", -1))
    return parent if 0 <= parent < bone_count else -1


def _bone_rest_origin(info: object) -> Vec3:
    try:
        value = info[2]  # type: ignore[index]
    except Exception:
        value = getattr(info, "rest_origin", ZERO_VEC3)
    return (float(value[0]), float(value[1]), float(value[2]))
