"""Pure output-collision decisions for queued import requests."""
from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from pathlib import Path

from opennova_jobs import ImportRequest


class CollisionDecision(Enum):
    IMPORT_ANYWAY = "import_anyway"
    SKIP = "skip"
    CANCEL = "cancel"


@dataclass(frozen=True)
class CollisionChoice:
    requests: list[ImportRequest]
    canceled: bool = False
    skipped_count: int = 0
    collision_paths: list[str] | None = None


def output_collision_paths(requests: list[ImportRequest]) -> list[str]:
    seen: set[str] = set()
    paths: list[str] = []
    for request in requests:
        path = request.likely_output_dir
        if path in seen:
            continue
        if Path(path).exists():
            seen.add(path)
            paths.append(path)
    return paths


def resolve_output_collisions(
    requests: list[ImportRequest],
    decision: CollisionDecision,
) -> CollisionChoice:
    collisions = set(output_collision_paths(requests))
    if not collisions:
        return CollisionChoice(requests=requests, collision_paths=[])
    if decision is CollisionDecision.CANCEL:
        return CollisionChoice(requests=[], canceled=True, collision_paths=sorted(collisions))
    if decision is CollisionDecision.SKIP:
        filtered = [
            request for request in requests if request.likely_output_dir not in collisions
        ]
        return CollisionChoice(
            requests=filtered,
            skipped_count=len(requests) - len(filtered),
            collision_paths=sorted(collisions),
        )
    return CollisionChoice(requests=requests, collision_paths=sorted(collisions))
