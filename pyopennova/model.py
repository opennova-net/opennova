"""
Shared data classes used by the Novalogic import pipeline.
Adapted from the pre-repo import prototype (pymxs references removed).
"""

from __future__ import annotations

from dataclasses import dataclass, field


@dataclass
class AnimationMeta:
    animation_name: str
    bad_filepath: str
    fps: int
    frame_count: int
    bad_name: str = ""  # Original BAD filename stem (e.g. "Dt1RunF")
    flags: int = 0      # Original BAD flags (0x01=loop, 0x02=translation)


@dataclass
class AnimationContext:
    reset_animation: AnimationMeta
    animations: list[AnimationMeta] = field(default_factory=list)
