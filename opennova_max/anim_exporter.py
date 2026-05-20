"""Current-timeline Novalogic ADM/BAD export for 3ds Max."""
from __future__ import annotations

import os
import re
import struct
from dataclasses import dataclass
from typing import Any, Dict, Iterable, List, Sequence, Tuple

from .ase_scene_exporter import _prompt_save_path


ANIM_FLAG_LOOPED = 0x01
ANIM_FLAG_TRANSLATION = 0x02
BONE_SIZE = 100
FRAME_TABLE_ENTRY_SIZE = 12
LEAF_BONE_LENGTH_FALLBACK = 0.05

Vec3 = Tuple[float, float, float]
Mat3 = Tuple[Vec3, Vec3, Vec3]
Quat = Tuple[float, float, float, float]


@dataclass
class MaxAnimClip:
    action_name: str
    bad_name: str
    start_frame: int
    end_frame: int
    is_reset: bool
    flags: int


@dataclass
class MaxAnimExportPlan:
    adm_path: str
    reset_bad_path: str
    clip_bad_path: str
    reset_clip: MaxAnimClip
    clip: MaxAnimClip


@dataclass
class _ExportBone:
    name: str
    parent_index: int
    position_bad: Vec3
    rot_bad: Mat3
    length: float


class _BinaryWriter:
    def __init__(self) -> None:
        self.data = bytearray()

    def write_i32(self, value: int) -> None:
        self.data += struct.pack("<i", int(value))

    def write_u32(self, value: int) -> None:
        self.data += struct.pack("<I", int(value))

    def write_u16(self, value: int) -> None:
        self.data += struct.pack("<H", int(value))

    def write_f32(self, value: float) -> None:
        self.data += struct.pack("<f", float(value))

    def write_u8(self, value: int) -> None:
        self.data += struct.pack("<B", int(value))

    def write_bytes(self, value: bytes) -> None:
        self.data += value


class MaxTimelineAnimExporter:
    def __init__(self, rt: Any) -> None:
        self.rt = rt
        self.bones = _sorted_bn_nodes(_selected_or_scene_nodes(rt))
        if not self.bones:
            raise RuntimeError("No BN* skeleton nodes found to export")
        self.bone_index = {str(getattr(node, "name", "")): idx for idx, node in enumerate(self.bones)}
        self._reset_positions = [(0.0, 0.0, 0.0) for _ in self.bones]
        self._reset_quats = [(0.0, 0.0, 0.0, 1.0) for _ in self.bones]

    def export_plan(self, plan: MaxAnimExportPlan) -> bool:
        os.makedirs(os.path.dirname(plan.adm_path) or ".", exist_ok=True)
        self.configure_from_reset_clip(plan.reset_clip)
        self.write_bad(plan.reset_bad_path, plan.reset_clip)
        self.write_bad(plan.clip_bad_path, plan.clip)
        self.write_adm(plan.adm_path, (plan.reset_clip, plan.clip))
        return True

    def configure_from_reset_clip(self, clip: MaxAnimClip) -> None:
        self._set_frame(clip.start_frame)
        self._reset_positions = [_world_position(node) for node in self.bones]
        self._reset_quats = [_matrix_to_quat(_rotation_rows(_node_transform(node))) for node in self.bones]

    def write_adm(self, out_adm_path: str, clips: Sequence[MaxAnimClip]) -> None:
        reset = next((clip for clip in clips if clip.is_reset), None)
        if reset is None:
            raise RuntimeError("No reset clip selected")
        lines = [""]
        lines.append('anim_reset\t\t\t\t"%s"' % reset.bad_name)
        for clip in clips:
            if clip.is_reset:
                continue
            lines.append('%s\t\t\t\t"%s"' % (clip.action_name, clip.bad_name))
        text = "\r\n".join(lines) + "\r\n\r\n\r\n\x00"
        with open(out_adm_path, "wb") as handle:
            handle.write(text.encode("utf-8"))

    def write_bad(self, out_bad_path: str, clip: MaxAnimClip) -> None:
        sample_count = int(clip.end_frame) - int(clip.start_frame) + 1
        if sample_count < 1:
            raise RuntimeError("Clip '%s' has too few frames" % clip.action_name)

        has_translation = (int(clip.flags) & ANIM_FLAG_TRANSLATION) != 0
        bones = self._extract_clip_bones(clip.start_frame)
        channels, translations, events = self._extract_channels(clip, has_translation)
        num_bones = len(bones)
        num_events = len(events)

        writer = _BinaryWriter()
        header_size = 80
        writer.write_bytes(b"\x00" * header_size)
        frame_table_offset = len(writer.data)
        writer.write_bytes(b"\x00" * (FRAME_TABLE_ENTRY_SIZE * num_bones))

        frame_len_ptrs: List[int] = []
        quat_ptrs: List[int] = []
        for channel in channels:
            frame_len_ptrs.append(len(writer.data))
            for frame_length in channel["frame_lengths"]:
                writer.write_u16(frame_length)
            while len(writer.data) % 4 != 0:
                writer.write_u8(0)
            quat_ptrs.append(len(writer.data))
            for qx, qy, qz, qw in channel["rotations"]:
                writer.write_f32(qx)
                writer.write_f32(qy)
                writer.write_f32(qz)
                writer.write_f32(qw)

        root_offsets_offset = 0
        if num_events > 0:
            root_offsets_offset = len(writer.data)
            for vx, vy, vz, bottom, top, trigger in events:
                writer.write_f32(vx)
                writer.write_f32(vy)
                writer.write_f32(vz)
                writer.write_f32(bottom)
                writer.write_f32(top)
                writer.write_i32(trigger)

        bone_offset = len(writer.data)

        def bone_addr(index: int) -> int:
            return bone_offset + index * BONE_SIZE

        children: Dict[int, List[int]] = {idx: [] for idx in range(num_bones)}
        for idx, bone in enumerate(bones):
            if bone.parent_index >= 0:
                children[bone.parent_index].append(idx)

        for idx, bone in enumerate(bones):
            encoded_name = bone.name.encode("ascii", "ignore")[:31] + b"\0"
            writer.write_bytes(encoded_name.ljust(32, b"\0"))
            writer.write_u8(0)
            writer.write_u8(0)
            writer.write_u8(0)
            writer.write_u8(idx & 0xFF)
            child_indices = children[idx]
            writer.write_i32(len(child_indices))
            writer.write_i32(bone_addr(child_indices[0]) if child_indices else -1)
            writer.write_i32(bone_addr(bone.parent_index) if bone.parent_index >= 0 else -1)
            writer.write_f32(bone.length)
            writer.write_f32(bone.position_bad[0])
            writer.write_f32(bone.position_bad[1])
            writer.write_f32(bone.position_bad[2])
            for row in bone.rot_bad:
                writer.write_f32(row[0])
                writer.write_f32(row[1])
                writer.write_f32(row[2])

        if has_translation:
            for frame_translations in translations:
                for tx, ty, tz in frame_translations:
                    writer.write_f32(tx)
                    writer.write_f32(ty)
                    writer.write_f32(tz)

        for idx in range(num_bones):
            off = frame_table_offset + idx * FRAME_TABLE_ENTRY_SIZE
            writer.data[off:off + 12] = struct.pack(
                "<III",
                len(channels[idx]["frame_lengths"]),
                frame_len_ptrs[idx],
                quat_ptrs[idx],
            )

        writer.data[0:header_size] = struct.pack(
            "<20I",
            1,
            header_size,
            30,
            sample_count,
            int(clip.flags),
            num_bones,
            bone_offset,
            frame_table_offset,
            0,
            0,
            8,
            BONE_SIZE,
            FRAME_TABLE_ENTRY_SIZE,
            16,
            1,
            num_events,
            root_offsets_offset,
            1,
            0,
            0,
        )

        with open(out_bad_path, "wb") as handle:
            handle.write(writer.data)

    def _extract_clip_bones(self, start_frame: int) -> List[_ExportBone]:
        self._set_frame(start_frame)
        raw_bones: List[Dict[str, Any]] = []
        for idx, node in enumerate(self.bones):
            parent_idx = self._parent_index(node, idx)
            world_pos = _world_position(node)
            if parent_idx >= 0:
                parent_pos = _world_position(self.bones[parent_idx])
                local_pos = _vec_sub(world_pos, parent_pos)
            else:
                local_pos = world_pos
            if _bone_export_name(str(getattr(node, "name", ""))).upper().startswith("BN01"):
                local_pos = (0.0, 0.0, 0.0)
            raw_bones.append(
                {
                    "name": _bone_export_name(str(getattr(node, "name", ""))),
                    "parent_index": parent_idx,
                    "position_bad": _max_to_bad_vec(local_pos),
                    "rot_bad": _max_to_bad_matrix(_rotation_rows(_node_transform(node))),
                    "world_pos": world_pos,
                }
            )

        children: Dict[int, List[int]] = {idx: [] for idx in range(len(raw_bones))}
        for idx, bone in enumerate(raw_bones):
            if bone["parent_index"] >= 0:
                children[bone["parent_index"]].append(idx)

        out: List[_ExportBone] = []
        for idx, bone in enumerate(raw_bones):
            length = LEAF_BONE_LENGTH_FALLBACK
            child_indices = children[idx]
            if child_indices:
                length = max(
                    _vec_length(_vec_sub(raw_bones[child]["world_pos"], bone["world_pos"]))
                    for child in child_indices
                )
                if length <= 1e-5:
                    length = LEAF_BONE_LENGTH_FALLBACK
            out.append(
                _ExportBone(
                    name=bone["name"],
                    parent_index=bone["parent_index"],
                    position_bad=bone["position_bad"],
                    rot_bad=bone["rot_bad"],
                    length=float(length),
                )
            )
        return out

    def _extract_channels(self, clip: MaxAnimClip, has_translation: bool):
        frame_total = int(clip.end_frame) - int(clip.start_frame) + 1
        per_bone_rots: List[List[Quat]] = [[] for _ in self.bones]
        translations_per_frame: List[List[Vec3]] = []
        prev_quats: List[Any] = [None for _ in self.bones]
        root_positions: List[Vec3] = []
        bone_extents: List[Tuple[float, float]] = []

        for frame_idx in range(frame_total):
            frame = int(clip.start_frame) + frame_idx
            self._set_frame(frame)
            root_positions.append(_world_position(self.bones[0]))
            bn01_z = root_positions[-1][2]
            min_y = 0.0
            max_y = 0.0
            frame_translations: List[Vec3] = []
            for bone_idx, node in enumerate(self.bones):
                quat = _matrix_to_quat(_rotation_rows(_node_transform(node)))
                bad_quat = _max_to_bad_quat(quat)
                previous = prev_quats[bone_idx]
                if previous is not None and _quat_dot(previous, bad_quat) < 0.0:
                    bad_quat = tuple(-component for component in bad_quat)  # type: ignore[assignment]
                prev_quats[bone_idx] = bad_quat
                per_bone_rots[bone_idx].append(bad_quat)

                world_pos = _world_position(node)
                bad_y = world_pos[2] - bn01_z
                min_y = min(min_y, bad_y)
                max_y = max(max_y, bad_y)
                if has_translation:
                    rest = self._reset_positions[bone_idx]
                    frame_translations.append(_max_to_bad_vec(_vec_sub(world_pos, rest)))
            if has_translation:
                translations_per_frame.append(frame_translations)
            bone_extents.append((min_y, max_y))

        if has_translation and translations_per_frame:
            translations_per_frame.append(translations_per_frame[-1])

        channels = []
        for rotations in per_bone_rots:
            terminal = rotations[-1] if rotations else (0.0, 0.0, 0.0, 1.0)
            channels.append(
                {
                    "frame_lengths": [1] * frame_total + [1],
                    "rotations": rotations + [terminal],
                }
            )

        events = []
        for idx in range(frame_total):
            if idx == 0:
                velocity = root_positions[0]
            else:
                velocity = _vec_sub(root_positions[idx], root_positions[idx - 1])
            vel_bad = _max_to_bad_vec(velocity)
            min_y, max_y = bone_extents[idx]
            bottom = abs(min_y)
            top = max_y + abs(min_y)
            events.append((vel_bad[0], vel_bad[1], vel_bad[2], bottom, top, 0))
        if events:
            events.append(events[-1])

        return channels, translations_per_frame, events

    def _parent_index(self, node: Any, node_index: int) -> int:
        parent = getattr(node, "parent", None)
        if parent is None:
            return -1
        parent_name = str(getattr(parent, "name", ""))
        parent_idx = self.bone_index.get(parent_name, -1)
        if parent_idx < 0 or parent_idx >= node_index:
            return -1
        return parent_idx

    def _set_frame(self, frame: int) -> None:
        try:
            self.rt.sliderTime = int(frame)
        except Exception:
            pass


def export_anims_with_dialog() -> bool:
    rt = _rt()
    adm_path = _prompt_save_path(
        rt,
        caption="Export Novalogic Anims",
        types="Novalogic ADM (*.adm)|*.adm|All Files (*.*)|*.*|",
        extension=".adm",
    )
    if not adm_path:
        return False
    plan = build_timeline_export_plan(rt, adm_path)
    return MaxTimelineAnimExporter(rt).export_plan(plan)


def build_timeline_export_plan(rt: Any, adm_path: str) -> MaxAnimExportPlan:
    adm_path = _ensure_extension(adm_path, ".adm")
    out_dir = os.path.dirname(adm_path) or "."
    stem = _sanitize_name(os.path.splitext(os.path.basename(adm_path))[0])
    start, end = _animation_range(rt)
    if end < start:
        end = start
    action_name = "anim_%s" % stem
    reset_bad = "%s_reset" % stem
    reset_clip = MaxAnimClip(
        action_name="anim_reset",
        bad_name=reset_bad,
        start_frame=start,
        end_frame=start,
        is_reset=True,
        flags=0,
    )
    clip = MaxAnimClip(
        action_name=action_name,
        bad_name=stem,
        start_frame=start,
        end_frame=end,
        is_reset=False,
        flags=ANIM_FLAG_LOOPED | ANIM_FLAG_TRANSLATION,
    )
    return MaxAnimExportPlan(
        adm_path=adm_path,
        reset_bad_path=os.path.join(out_dir, reset_bad + ".bad"),
        clip_bad_path=os.path.join(out_dir, stem + ".bad"),
        reset_clip=reset_clip,
        clip=clip,
    )


def _rt() -> Any:
    try:
        import pymxs
    except ImportError as exc:  # pragma: no cover
        raise RuntimeError(
            "pymxs is not available; 3ds Max animation export only runs inside 3ds Max."
        ) from exc
    return pymxs.runtime


def _animation_range(rt: Any) -> Tuple[int, int]:
    interval = getattr(rt, "animationRange", None)
    start = _time_value(getattr(interval, "start", 0))
    end = _time_value(getattr(interval, "end", start))
    return int(round(start)), int(round(end))


def _time_value(value: Any) -> float:
    for attr in ("frame", "ticks"):
        try:
            return float(getattr(value, attr))
        except Exception:
            pass
    try:
        return float(value)
    except Exception:
        return 0.0


def _selected_or_scene_nodes(rt: Any) -> List[Any]:
    try:
        selected = list(rt.selection)
    except Exception:
        selected = []
    selected_bn = [node for node in selected if _bone_export_name(str(getattr(node, "name", ""))).startswith("BN")]
    if selected_bn:
        return selected_bn
    try:
        return list(rt.objects)
    except Exception:
        return []


def _sorted_bn_nodes(nodes: Iterable[Any]) -> List[Any]:
    return sorted(
        [node for node in nodes if _bone_export_name(str(getattr(node, "name", ""))).startswith("BN")],
        key=lambda node: (_bone_number(str(getattr(node, "name", ""))), str(getattr(node, "name", ""))),
    )


def _bone_export_name(name: str) -> str:
    if name.startswith("BN") and len(name) >= 4 and name[2:4].isdigit():
        return name[:4]
    return name


def _bone_number(name: str) -> int:
    match = re.match(r"^BN(\d+)", name.upper())
    return int(match.group(1)) if match else 999999


def _sanitize_name(name: str) -> str:
    safe = re.sub(r"[^A-Za-z0-9_]+", "_", name.strip())
    return safe or "anim_clip"


def _ensure_extension(path: str, extension: str) -> str:
    root, ext = os.path.splitext(path)
    if not ext:
        return root + extension
    if ext.lower() != extension.lower():
        return root + extension
    return path


def _node_transform(node: Any) -> Any:
    try:
        return node.transform
    except Exception:
        return _IdentityMatrix(_world_position(node))


def _world_position(node: Any) -> Vec3:
    try:
        return _point_tuple(node.transform.position)
    except Exception:
        pass
    try:
        return _point_tuple(node.position)
    except Exception:
        return (0.0, 0.0, 0.0)


def _rotation_rows(matrix: Any) -> Mat3:
    return (_matrix_row(matrix, 0), _matrix_row(matrix, 1), _matrix_row(matrix, 2))


def _matrix_row(matrix: Any, row_index: int) -> Vec3:
    try:
        return _point_tuple(getattr(matrix, "row%d" % (row_index + 1)))
    except Exception:
        pass
    try:
        row = matrix[row_index]
        return (float(row[0]), float(row[1]), float(row[2]))
    except Exception:
        pass
    return (1.0, 0.0, 0.0) if row_index == 0 else (0.0, 1.0, 0.0) if row_index == 1 else (0.0, 0.0, 1.0)


def _point_tuple(value: Any) -> Vec3:
    try:
        return (float(value.x), float(value.y), float(value.z))
    except Exception:
        return (float(value[0]), float(value[1]), float(value[2]))


def _max_to_bad_vec(value: Vec3) -> Vec3:
    return (float(value[0]), float(value[2]), -float(value[1]))


def _max_to_bad_quat(value: Quat) -> Quat:
    x, y, z, w = value
    return _quat_normalize((x, z, -y, w))


def _max_to_bad_matrix(rows: Mat3) -> Mat3:
    return (
        (rows[0][0], rows[0][2], -rows[0][1]),
        (rows[2][0], rows[2][2], -rows[2][1]),
        (-rows[1][0], -rows[1][2], rows[1][1]),
    )


def _matrix_to_quat(rows: Mat3) -> Quat:
    m00, m01, m02 = rows[0]
    m10, m11, m12 = rows[1]
    m20, m21, m22 = rows[2]
    trace = m00 + m11 + m22
    if trace > 0.0:
        s = (trace + 1.0) ** 0.5 * 2.0
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
    return _quat_normalize((x, y, z, w))


def _quat_normalize(quat: Quat) -> Quat:
    x, y, z, w = quat
    length = (x * x + y * y + z * z + w * w) ** 0.5
    if length <= 1e-8:
        return (0.0, 0.0, 0.0, 1.0)
    return (x / length, y / length, z / length, w / length)


def _quat_dot(left: Quat, right: Quat) -> float:
    return left[0] * right[0] + left[1] * right[1] + left[2] * right[2] + left[3] * right[3]


def _vec_sub(left: Vec3, right: Vec3) -> Vec3:
    return (left[0] - right[0], left[1] - right[1], left[2] - right[2])


def _vec_length(value: Vec3) -> float:
    return (value[0] * value[0] + value[1] * value[1] + value[2] * value[2]) ** 0.5


class _IdentityMatrix:
    def __init__(self, position: Vec3) -> None:
        self.row1 = (1.0, 0.0, 0.0)
        self.row2 = (0.0, 1.0, 0.0)
        self.row3 = (0.0, 0.0, 1.0)
        self.position = position
