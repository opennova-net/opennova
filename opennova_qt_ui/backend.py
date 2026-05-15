"""ImportBackend Protocol and BackendCapabilities for the Qt importer.

Both `opennova_blender.backend.BlenderBackend` and
`opennova_max.backend.MaxBackend` implement this Protocol structurally.
The Qt dialog depends on this module — never on a specific backend.

3ds Max 2022 ships Python 3.7, where ``typing.Protocol`` does not exist.
We fall back to a stub so the module still imports there. Backends are
duck-typed at runtime regardless of the Protocol class — the dialog only
calls ``capabilities``/``scan``/``execute``/``shutdown`` on whatever
object it gets, so the runtime contract holds either way. The
``isinstance(obj, ImportBackend)`` check (only used by tests on Python
3.11) keeps working when ``runtime_checkable`` is available.
"""
from __future__ import annotations

from dataclasses import dataclass

try:
    from typing import Protocol, runtime_checkable
except ImportError:  # pragma: no cover - Python 3.7 (3ds Max 2022)
    Protocol = object  # type: ignore[misc,assignment]

    def runtime_checkable(cls):  # type: ignore[no-redef]
        return cls

from opennova_jobs import ImportRequest, ImportResult, ScanResult


@dataclass(frozen=True)
class BackendCapabilities:
    """What outputs and features the active backend supports.

    The dialog reads this once at construction to choose which option
    checkboxes to display and whether to expose parallel-batch UI.
    """

    name: str
    supports_blend: bool
    supports_max: bool
    supports_parallel: bool


@runtime_checkable
class ImportBackend(Protocol):
    """Surface the Qt dialog needs to drive an importer."""

    def capabilities(self) -> BackendCapabilities: ...

    def scan(self, base_dir: str) -> ScanResult: ...

    def execute(self, request: ImportRequest) -> ImportResult: ...

    def shutdown(self) -> None: ...
