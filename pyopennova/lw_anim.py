"""Land Warrior animation resolution chain.

LW character animation is sidecar to the ``.3di``: ``items.def`` names an
``anim_def`` (``.ANM`` move table: animation name -> slot) and a ``chr_file``
(the ``.KSA`` baked clip bundle, with ``.ACA``/``.SAF`` authoring sources).

This module resolves those files through an ``AssetResolver`` and samples each
named clip into per-frame, per-bone LOCAL transforms using the byte-faithful C
parsers + pose sampler (``pyopennova.lw_anim_ffi`` -> ``libs/threedi/threedi_lw_anim``,
reproducing ``LWAnim_PoseSkeleton @ 0x4A0C00`` with gameplay modifiers skipped).
The resulting :class:`LwAnimationContext` is consumed by
``scene_builder.build_lw_animations`` to author Blender Actions.

The pose MATH lives in C (re-derived from Dflw.exe); the earlier pure-Python MR
(reverted at c88ebc39) guessed the Euler order and is not used here.
"""
from __future__ import annotations

import logging
from dataclasses import dataclass, field
from pathlib import Path

from . import lw_anim_ffi

log = logging.getLogger(__name__)

# Sub-object translations are 16.16 fixed-point; the same scale the geometry
# promotion (threedi_ir_from_lw.cpp LW_PART_POS_SCALE) applies to part origins.
LW_PART_POS_SCALE = 1.0 / 65536.0


@dataclass
class LwClip:
    """One named, sampled LW clip.

    ``frames[f][bone]`` is a row-major 3x4 ``[R|t]`` LOCAL transform in raw LW
    space (the direct output of ``lw_anim_ffi.sample_frame``).
    """

    name: str
    slot: int
    frame_count: int
    loop_frame: int
    velocity: float
    bone_count: int
    frames: list


@dataclass
class LwAnimationContext:
    kind: str
    anim_name: str
    chr_name: str
    clips: list = field(default_factory=list)
    slot_count: int = 0

    # The BAD animation path probes these on any anim context; keep them inert
    # so a stray ``anim_ctx.reset_animation`` / ``.animations`` access is safe.
    @property
    def reset_animation(self):
        return None

    @property
    def animations(self):
        return ()


def _ensure_ext(name: str, ext: str) -> str:
    return name if name.lower().endswith(ext.lower()) else f"{name}{ext}"


def _strip_ext(name: str) -> str:
    return name.rsplit(".", 1)[0] if "." in name else name


def _anm_entries(anm) -> list[tuple[str, int, float]]:
    """Decode the (name, slot, velocity) tuples from a parsed ANM, in file order."""
    out: list[tuple[str, int, float]] = []
    for i in range(int(anm.entry_count)):
        e = anm.entries[i]
        raw = e.name  # ctypes c_char[64] -> bytes, NUL-trimmed
        name = raw.decode("ascii", "replace").strip() if raw else ""
        out.append((name, int(e.slot), float(e.velocity)))
    return out


def build_lw_animation_context(
    anim_def: str,
    chr_file: str,
    *,
    resolver,
    model_path: str,
) -> LwAnimationContext | None:
    """Resolve + sample the LW animation set for one character.

    ``anim_def``  - the ``.ANM`` move table stem (e.g. ``enemy00``).
    ``chr_file``  - the ``.KSA``/character stem (e.g. ``player01``).
    ``model_path``- path to the character ``.3di`` (supplies the skeleton rest
                    sub-objects the pose sampler needs).

    Returns ``None`` if the move table or skeleton cannot be resolved.
    """
    if not anim_def:
        return None

    anm_path = resolver.resolve(_ensure_ext(anim_def, ".anm"))
    if not anm_path:
        log.warning("LW: no ANM resolved for anim_def=%r", anim_def)
        return None

    chr_stem = _strip_ext(chr_file)
    ksa_path = resolver.resolve(_ensure_ext(chr_stem, ".ksa"))
    if not ksa_path:
        # ACA + loose SAF authoring fallback is not yet wired; without a baked
        # KSA there are no clips to sample.
        log.warning("LW: no KSA resolved for chr_file=%r (clips unavailable)", chr_file)

    try:
        subobjects, _flags = lw_anim_ffi.read_lw_subobjects(str(model_path), 0)
    except Exception as e:  # not a readable LW .3di / no skeleton
        log.warning("LW: skeleton read failed for %s: %s", model_path, e)
        return None
    if not subobjects:
        log.warning("LW: model %s has no LOD0 sub-objects", model_path)
        return None

    anm = lw_anim_ffi.read_anm(str(anm_path))
    clips: list[LwClip] = []
    slot_count = 0
    try:
        entries = _anm_entries(anm)
        ksa = lw_anim_ffi.read_ksa(str(ksa_path)) if ksa_path else None
        try:
            if ksa is not None:
                slot_count = int(ksa.slot_count)
                bone_count = len(subobjects)
                seen: set[str] = set()
                for name, slot, velocity in entries:
                    if not name or name in seen:
                        continue
                    seen.add(name)
                    if slot < 0 or slot >= ksa.slot_count:
                        continue
                    ksa_slot = ksa.slots[slot]
                    frame_count = int(ksa_slot.frame_count)
                    if frame_count <= 0:
                        continue
                    frames = []
                    for f in range(frame_count):
                        runtime_frame = ksa_slot.frames[f]
                        pose = lw_anim_ffi.sample_frame(
                            runtime_frame, subobjects, LW_PART_POS_SCALE
                        )
                        frames.append(pose)
                    clips.append(
                        LwClip(
                            name=name,
                            slot=slot,
                            frame_count=frame_count,
                            loop_frame=int(ksa_slot.loop_frame),
                            velocity=velocity,
                            bone_count=bone_count,
                            frames=frames,
                        )
                    )
        finally:
            if ksa is not None:
                lw_anim_ffi.free_ksa(ksa)
    finally:
        lw_anim_ffi.free_anm(anm)

    log.info(
        "LW: resolved %d clip(s) for chr=%s anim=%s (%d KSA slots)",
        len(clips), chr_stem, anim_def, slot_count,
    )
    return LwAnimationContext(
        kind="lw",
        anim_name=_strip_ext(anim_def).lower(),
        chr_name=chr_stem.lower(),
        clips=clips,
        slot_count=slot_count,
    )
