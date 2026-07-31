"""DCC-agnostic backend protocol for the Qt importer."""
from __future__ import annotations

from dataclasses import dataclass
from typing import Protocol, runtime_checkable

from opennova_jobs import ImportRequest, ImportResult, ScanResult


@dataclass(frozen=True)
class BackendCapabilities:
    name: str
    supports_blend: bool
    supports_max: bool
    supports_parallel: bool


@runtime_checkable
class ImportBackend(Protocol):
    def capabilities(self) -> BackendCapabilities: ...

    def scan(self, base_dir: str) -> ScanResult: ...

    def execute(self, request: ImportRequest) -> ImportResult: ...

    def shutdown(self) -> None: ...
