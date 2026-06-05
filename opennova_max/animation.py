"""3ds Max animation keying from shared BAD animation samples."""
from __future__ import annotations

import json
from contextlib import contextmanager, nullcontext
from dataclasses import replace
from typing import Any, Iterable, Sequence, Tuple

from pyopennova.animation_build import (
    Mat3,
    QuatXyzw,
    SampledAnimationSet,
    SampledClip,
    mat_mul,
    quat_xyzw_to_matrix_rows,
    sample_animation_context,
    vec_add,
)

Vec3 = Tuple[float, float, float]

_IDENTITY_ROWS: Mat3 = (
    (1.0, 0.0, 0.0),
    (0.0, 1.0, 0.0),
    (0.0, 0.0, 1.0),
)

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


def key_animation_context(
    rt,
    anim_context,
    skeleton_node,
    bone_nodes: Sequence[object],
    bone_infos: Sequence[object],
    root_motion_node=None,
) -> SampledAnimationSet:
    """Sample ADM/BAD animations and key them on the Max skeleton."""

    sampled_set = sample_animation_context(anim_context, bone_infos)
    timeline_clips = rebase_timeline_clips(sampled_set.clips)
    # Persist the full clip set (reset pinned to frame 0) so the .bad exporter can
    # round-trip clip names/flags/ranges, not just the on-timeline animations.
    reset_clips = tuple(
        replace(
            clip,
            start_frame=0,
            end_frame=0,
            frame_count=1,
            frames=(clip.frames[0],) if clip.frames else (),
        )
        for clip in sampled_set.clips
        if clip.is_reset
    )
    timeline_set = replace(sampled_set, clips=reset_clips + timeline_clips)
    store_animation_metadata(rt, skeleton_node, timeline_set)
    apply_sampled_clips(rt, sampled_set.clips, bone_nodes, root_motion_node)
    return sampled_set


def apply_sampled_clips(
    rt,
    clips: Iterable[SampledClip],
    bone_nodes: Sequence[object],
    root_motion_node=None,
) -> None:
    """Apply sampled BAD clips to Max nodes as world transform keys."""

    sampled_clips = tuple(clips)
    reset_clip = next((clip for clip in sampled_clips if clip.is_reset and clip.frames), None)
    clip_list = rebase_timeline_clips(sampled_clips)
    if not clip_list:
        if reset_clip is not None:
            with _redraw_disabled(rt):
                with _animate_context():
                    _apply_reset_pose(rt, reset_clip, bone_nodes, root_motion_node)
        return

    _set_timeline(rt, clip_list)
    with _redraw_disabled(rt):
        with _animate_context():
            if reset_clip is not None:
                _apply_reset_pose(rt, reset_clip, bone_nodes, root_motion_node)
            for clip in clip_list:
                for frame in clip.frames:
                    _assign_transforms_at_frame(
                        rt,
                        frame.frame,
                        _frame_assignments(frame, bone_nodes, root_motion_node),
                    )


def rebase_timeline_clips(clips: Iterable[SampledClip]) -> tuple[SampledClip, ...]:
    """Return non-reset clips rebased to a Max timeline starting at frame 1."""

    rebased: list[SampledClip] = []
    frame_cursor = 1
    for clip in clips:
        if clip.is_reset:
            continue
        offset = frame_cursor - int(clip.start_frame)
        frames = tuple(replace(frame, frame=int(frame.frame) + offset) for frame in clip.frames)
        end_frame = frame_cursor + max(int(clip.frame_count) - 1, 0)
        rebased.append(replace(
            clip,
            start_frame=frame_cursor,
            end_frame=end_frame,
            frames=frames,
        ))
        frame_cursor += max(int(clip.frame_count), 0)
    return tuple(rebased)


def max_rows_from_source_quat(q_xyzw: QuatXyzw) -> Mat3:
    """Convert a BAD xyzw quaternion sample to the Max BAD bone row order."""

    bad_rows = quat_xyzw_to_matrix_rows(q_xyzw)
    return mat_mul(mat_mul(_MAX_GLOBAL_MATRIX_ROWS, bad_rows), _MAX_GLOBAL_CORRECTION_ROWS)


def _frame_assignments(frame, bone_nodes: Sequence[object], root_motion_node=None):
    assignments: list[tuple[object, Mat3, Vec3]] = []
    root_position = frame.max_root_motion_position
    if root_motion_node is not None:
        assignments.append((root_motion_node, _IDENTITY_ROWS, root_position))
    for sampled_bone in frame.bones:
        if sampled_bone.bone_index >= len(bone_nodes):
            continue
        rows = max_rows_from_source_quat(sampled_bone.source_rotation_xyzw)
        position = vec_add(sampled_bone.world_position, root_position)
        assignments.append((bone_nodes[sampled_bone.bone_index], rows, position))
    return assignments


def _apply_reset_pose(rt, reset_clip: SampledClip, bone_nodes: Sequence[object], root_motion_node=None) -> None:
    reset_frame = reset_clip.frames[0]
    root_position = (0.0, 0.0, reset_frame.max_root_motion_position[2])
    assignments: list[tuple[object, Mat3, Vec3]] = []
    if root_motion_node is not None:
        assignments.append((root_motion_node, _IDENTITY_ROWS, root_position))
    for sampled_bone in reset_frame.bones:
        if sampled_bone.bone_index >= len(bone_nodes):
            continue
        rows = max_rows_from_source_quat(sampled_bone.source_rotation_xyzw)
        position = vec_add(sampled_bone.world_position, root_position)
        assignments.append((bone_nodes[sampled_bone.bone_index], rows, position))
    _assign_transforms_at_frame(rt, 0, assignments)


def build_clip_manifest(clips: Iterable[SampledClip]) -> str:
    """Return a compact JSON manifest for Max custom properties."""

    manifest = [
        {
            "name": clip.name,
            "bad_name": clip.bad_name,
            "flags": int(clip.flags),
            "fps": int(clip.fps),
            "frame_count": int(clip.frame_count),
            "start_frame": int(clip.start_frame),
            "end_frame": int(clip.end_frame),
            "is_reset": bool(clip.is_reset),
        }
        for clip in clips
    ]
    return json.dumps(manifest, separators=(",", ":"))


def store_animation_metadata(rt, skeleton_node, sampled_set: SampledAnimationSet) -> None:
    if skeleton_node is None:
        return
    _set_user_prop(
        rt,
        skeleton_node,
        "opennova_animations",
        ";".join(clip.name for clip in sampled_set.clips),
    )
    _set_user_prop(
        rt,
        skeleton_node,
        "opennova_animation_clips",
        build_clip_manifest(sampled_set.clips),
    )
    if sampled_set.warnings:
        _set_user_prop(
            rt,
            skeleton_node,
            "opennova_animation_warnings",
            "\n".join(sampled_set.warnings),
        )


def matrix3_from_rows(rt, rows: Sequence[Sequence[float]], position: Sequence[float]):
    matrix3 = getattr(rt, "matrix3", None) or getattr(rt, "Matrix3")
    return matrix3(
        _point3(rt, rows[0]),
        _point3(rt, rows[1]),
        _point3(rt, rows[2]),
        _point3(rt, position),
    )


def _assign_transforms_at_frame(
    rt,
    frame: int,
    assignments: Sequence[tuple[object, Mat3, Vec3]],
) -> None:
    if not assignments:
        return

    def assign_all() -> None:
        for node, rows, position in assignments:
            node.transform = matrix3_from_rows(rt, rows, position)

    frame_context, auto_keys = _attime_context(frame)
    if not auto_keys:
        _try_set(rt, "sliderTime", int(frame))
    with frame_context:
        assign_all()
    if not auto_keys:
        for node, _rows, _position in assignments:
            _try_add_transform_key(rt, node, int(frame))


@contextmanager
def _redraw_disabled(rt):
    disabled = False
    try:
        rt.disableSceneRedraw()
        disabled = True
    except Exception:
        pass
    try:
        yield
    finally:
        if disabled:
            try:
                rt.enableSceneRedraw()
            except Exception:
                pass


def _animate_context():
    try:
        import pymxs  # type: ignore[import-not-found]

        return pymxs.animate(True)
    except Exception:
        return nullcontext()


def _attime_context(frame: int):
    try:
        import pymxs  # type: ignore[import-not-found]

        return pymxs.attime(int(frame)), True
    except Exception:
        return nullcontext(), False


def _set_timeline(rt, clips: Sequence[SampledClip]) -> None:
    fps = next((int(clip.fps) for clip in clips if int(clip.fps) > 0), None)
    if fps is not None:
        _try_set(rt, "frameRate", fps)

    start = min((int(clip.start_frame) for clip in clips), default=1)
    end = max((int(clip.end_frame) for clip in clips), default=start)
    if end < start:
        end = start
    try:
        rt.animationRange = rt.interval(start, end)
    except Exception:
        try:
            rt.animationRange = rt.Interval(start, end)
        except Exception:
            pass


def _try_add_transform_key(rt, node, frame: int) -> None:
    try:
        controller = node.transform.controller
    except Exception:
        return
    try:
        rt.addNewKey(controller, frame)
    except Exception:
        pass


def _point3(rt, value: Sequence[float]):
    return rt.Point3(float(value[0]), float(value[1]), float(value[2]))


def _set_user_prop(rt, obj, key: str, value: Any) -> None:
    try:
        rt.setUserProp(obj, key, value)
    except Exception:
        pass


def _try_set(obj, attr: str, value: Any) -> None:
    try:
        setattr(obj, attr, value)
    except Exception:
        pass
