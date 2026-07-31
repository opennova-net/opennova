"""Current-scene Novalogic ADM + BAD animation export for 3ds Max.

The coordinate inversion lives in the DCC-neutral, CI-tested
pyopennova.bad_build.assemble_clip_from_max_world; this module is the thin pymxs
adapter that samples the live skeleton's world transforms per frame and feeds
them in. It is the inverse of opennova_max.scene_builder.build_armature_from_bad
+ opennova_max.animation.apply_sampled_clips.
"""
from __future__ import annotations

import json
import os
import re
from typing import Any, List, Optional, Sequence, Tuple

from pyopennova import bad_build

Vec3 = Tuple[float, float, float]
Mat3 = Tuple[Vec3, Vec3, Vec3]

_BN_RE = re.compile(r"^BN(\d+)", re.IGNORECASE)
_LEAF_LENGTH_FALLBACK = 0.05
_ROOT_NODE_NAMES = ("Bip001", "Skeleton")


def export_anims_with_dialog() -> bool:
    rt = _rt()
    filepath = _prompt_save_path(
        rt,
        caption="Export Novalogic Anims",
        types="Novalogic ADM (*.adm)|*.adm|All Files (*.*)|*.*|",
        extension=".adm",
    )
    if not filepath:
        return False
    return AnimSceneExporter(rt).export(filepath)


class AnimSceneExporter:
    """Translate the live Max skeleton's animation to .bad clips + a .adm index."""

    def __init__(self, rt: Any) -> None:
        self.rt = rt

    def export(self, adm_path: str) -> bool:
        root = self._find_root_node()
        bone_nodes, parents = self._collect_bone_nodes(root)
        if not bone_nodes:
            raise RuntimeError("No BN* bones found in the scene")

        clips = self._read_clip_manifest(root)
        if not clips:
            clips = self._fallback_clips(adm_path)

        out_dir = os.path.dirname(adm_path) or "."
        os.makedirs(out_dir, exist_ok=True)

        # Translations for every clip are measured against the RESET skeleton's
        # rest (the importer rebuilds all clips from the reset rest), so sample
        # the reset pose once up front and reuse it for all clips.
        reset_rest_origins = self._compute_reset_rest_origins(bone_nodes, parents, root, clips)

        refs: List[bad_build.AdmClipRef] = []
        reset_count = 0
        for clip in clips:
            bad_name = str(clip.get("bad_name") or clip.get("name") or "anim_clip")
            name = str(clip.get("name") or bad_name)
            is_reset = bool(clip.get("is_reset"))
            if is_reset:
                reset_count += 1
            flags = int(clip.get("flags", 0))
            fps = int(clip.get("fps", 30) or 30)
            start = int(clip.get("start_frame", 0))
            end = int(clip.get("end_frame", start))
            frame_count = max(1, end - start + 1)

            rows, node_pos, root_pos = self._sample_clip(bone_nodes, root, start, frame_count)
            bones_meta = self._bones_meta(bone_nodes, parents, node_pos[0])

            clip_out = bad_build.assemble_clip_from_max_world(
                bones_meta=bones_meta,
                per_frame_rows=rows,
                per_frame_node_pos=node_pos,
                per_frame_root_pos=root_pos,
                frame_count=frame_count,
                flags=flags,
                fps=fps,
                version=1,
                name=name,
                bad_name=bad_name,
                is_reset=is_reset,
                reset_rest_origins=reset_rest_origins,
            )
            bad_build.write_bad(os.path.join(out_dir, bad_name + ".bad"), clip_out)
            refs.append(bad_build.AdmClipRef(animation_name=name, bad_name=bad_name, is_reset=is_reset))

        if reset_count == 0:
            raise RuntimeError("No reset clip found (one clip must be marked is_reset / anim_reset)")

        bad_build.write_adm(adm_path, refs)
        print("[ANIM_EXPORT] wrote %d clip(s) + %s" % (len(refs), os.path.basename(adm_path)))
        return True

    # -- skeleton discovery -------------------------------------------------

    def _find_root_node(self) -> Any:
        rt = self.rt
        for name in _ROOT_NODE_NAMES:
            try:
                node = rt.getNodeByName(name)
            except Exception:
                node = None
            if node is not None:
                return node
        return None

    def _collect_bone_nodes(self, root: Any) -> Tuple[List[Any], List[int]]:
        rt = self.rt
        try:
            scene = list(rt.objects)
        except Exception:
            scene = []
        bones = [n for n in scene if _BN_RE.match(str(n.name))]
        bones.sort(key=lambda n: _bn_sort_key(str(n.name)))
        index = {id(n): i for i, n in enumerate(bones)}
        name_index = {str(n.name): i for i, n in enumerate(bones)}

        parents: List[int] = []
        for i, node in enumerate(bones):
            parent_node = None
            try:
                parent_node = node.parent
            except Exception:
                parent_node = None
            pi = -1
            if parent_node is not None:
                pi = name_index.get(str(parent_node.name), index.get(id(parent_node), -1))
            if pi >= i:  # non-topological / self / forward parent -> treat as root
                pi = -1
            parents.append(pi)
        return bones, parents

    def _compute_reset_rest_origins(self, bone_nodes, parents, root, clips):
        """Sample the reset clip's pose once -> shared rest origins for translations.

        Returns None if no reset clip is present (caller then falls back to each
        clip's own frame-0 rest, the pre-fix behavior).
        """
        reset = next((c for c in clips if bool(c.get("is_reset"))), None)
        if reset is None:
            return None
        start = int(reset.get("start_frame", 0))
        rows, node_pos, root_pos = self._sample_clip(bone_nodes, root, start, 1)
        return bad_build.rest_origins_from_max_world(rows[0], node_pos[0], root_pos[0], parents)

    def _bones_meta(self, bone_nodes, parents, frame0_pos):
        # length: prefer the stored BAD rest length, else child-head distance.
        children: dict[int, list[int]] = {i: [] for i in range(len(bone_nodes))}
        for i, p in enumerate(parents):
            if p >= 0:
                children[p].append(i)
        meta: List[Tuple[str, int, float]] = []
        for i, node in enumerate(bone_nodes):
            length = _read_float_prop(self.rt, node, "opennova_bad_length")
            if length is None:
                length = _LEAF_LENGTH_FALLBACK
                if children[i]:
                    dists = [_distance(frame0_pos[i], frame0_pos[c]) for c in children[i]]
                    best = max(dists) if dists else 0.0
                    if best > 1e-5:
                        length = best
            meta.append((str(node.name), int(parents[i]), float(length)))
        return meta

    # -- per-frame sampling -------------------------------------------------

    def _sample_clip(self, bone_nodes, root, start: int, frame_count: int):
        rows: List[List[Mat3]] = []
        node_pos: List[List[Vec3]] = []
        root_pos: List[Vec3] = []
        for f in range(frame_count):
            frame = start + f
            with _attime(frame):
                frame_rows: List[Mat3] = []
                frame_pos: List[Vec3] = []
                for node in bone_nodes:
                    tm = node.transform
                    frame_rows.append(_matrix_rows(tm))
                    frame_pos.append(_matrix_pos(tm))
                root_pos.append(_node_position(root) if root is not None else (0.0, 0.0, 0.0))
            rows.append(frame_rows)
            node_pos.append(frame_pos)
        return rows, node_pos, root_pos

    # -- clip sources -------------------------------------------------------

    def _read_clip_manifest(self, root: Any) -> List[dict]:
        if root is None:
            return []
        raw = None
        try:
            raw = self.rt.getUserProp(root, "opennova_animation_clips")
        except Exception:
            raw = None
        if not raw:
            return []
        try:
            data = json.loads(str(raw))
        except Exception:
            return []
        return list(data) if isinstance(data, list) else []

    def _fallback_clips(self, adm_path: str) -> List[dict]:
        """Without an import manifest, export the whole timeline as one reset clip."""
        rt = self.rt
        start, end = 0, 0
        try:
            rng = rt.animationRange
            start = int(rng.start)
            end = int(rng.end)
        except Exception:
            pass
        bad_name = os.path.splitext(os.path.basename(adm_path))[0] or "rst"
        print(
            "[ANIM_EXPORT] no import manifest found; exporting frames "
            "%d-%d as a single reset clip '%s'" % (start, end, bad_name)
        )
        return [{
            "name": "anim_reset",
            "bad_name": bad_name,
            "flags": 0,
            "fps": 30,
            "start_frame": start,
            "end_frame": end,
            "is_reset": True,
        }]


# ---------------------------------------------------------------------------
# pymxs helpers
# ---------------------------------------------------------------------------


def _rt() -> Any:
    try:
        import pymxs
    except ImportError as exc:  # pragma: no cover - only available inside Max
        raise RuntimeError(
            "pymxs is not available; animation export only runs inside 3ds Max."
        ) from exc
    return pymxs.runtime


def _attime(frame: int):
    try:
        import pymxs  # type: ignore[import-not-found]

        return pymxs.attime(int(frame))
    except Exception:
        from contextlib import nullcontext

        return nullcontext()


def _matrix_rows(tm: Any) -> Mat3:
    r1, r2, r3 = tm.row1, tm.row2, tm.row3
    return (
        (float(r1.x), float(r1.y), float(r1.z)),
        (float(r2.x), float(r2.y), float(r2.z)),
        (float(r3.x), float(r3.y), float(r3.z)),
    )


def _matrix_pos(tm: Any) -> Vec3:
    p = tm.position
    return (float(p.x), float(p.y), float(p.z))


def _node_position(node: Any) -> Vec3:
    try:
        return _matrix_pos(node.transform)
    except Exception:
        p = node.position
        return (float(p.x), float(p.y), float(p.z))


def _bn_sort_key(name: str) -> Tuple[int, int, str]:
    m = _BN_RE.match(name)
    if m:
        return (0, int(m.group(1)), name)
    return (1, 0, name)


def _read_float_prop(rt: Any, node: Any, key: str) -> Optional[float]:
    try:
        raw = rt.getUserProp(node, key)
    except Exception:
        return None
    if raw in (None, "", "undefined"):
        return None
    try:
        return float(raw)
    except (TypeError, ValueError):
        return None


def _distance(a: Sequence[float], b: Sequence[float]) -> float:
    return ((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2 + (a[2] - b[2]) ** 2) ** 0.5


def _prompt_save_path(rt: Any, caption: str, types: str, extension: str) -> str:
    try:
        value = rt.execute(
            'getSaveFileName caption:"%s" types:"%s"'
            % (caption.replace('"', "'"), types.replace('"', "'"))
        )
    except Exception:
        try:
            value = rt.getSaveFileName(caption=caption, types=types)
        except Exception:
            value = None
    if not value:
        return ""
    path = str(value)
    root, ext = os.path.splitext(path)
    if not ext:
        path = root + extension
    return path
