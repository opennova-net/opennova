"""
Shared data classes used by the Novalogic import pipeline.
Adapted from .scratch/opennova/model.py (pymxs references removed).
"""

from __future__ import annotations

from dataclasses import dataclass, field


@dataclass
class AnimationMeta:
    animation_name: str
    bad_filepath: str
    fps: int
    frame_count: int


@dataclass
class AnimationContext:
    reset_animation: AnimationMeta
    animations: list[AnimationMeta] = field(default_factory=list)
