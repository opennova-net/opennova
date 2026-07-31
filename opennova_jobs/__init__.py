"""Shared orchestration models for OpenNova importers.

This package is intentionally pure data and validation logic. It must not
import Qt, Blender, bpy, or any DCC-specific integration.
"""
from __future__ import annotations

import os
import time
import uuid
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable


IMPORT_MODE_DEF = "def"
IMPORT_MODE_LOOSE = "loose"

ITEM_TYPE_WEAPON = "weapon"
ITEM_TYPE_ITEM = "item"
ITEM_TYPE_LOOSE = "loose"

JOB_PENDING = "pending"
JOB_RUNNING = "running"
JOB_DONE = "done"
JOB_ERROR = "error"

ACTIVE_JOB_STATUSES = {JOB_PENDING, JOB_RUNNING}
ASE_OWNER_BLENDER = "blender"
ASE_OWNER_MAX = "max"


def _norm_path(value: str) -> str:
    if not value:
        return ""
    return os.path.normcase(os.path.abspath(os.path.expanduser(value)))


@dataclass(frozen=True)
class ImportOptions:
    import_animations: bool = True
    import_collisions: bool = True
    import_occlusion: bool = True
    import_lights: bool = True
    import_arms: bool = True
    write_blend: bool = True
    write_3dp: bool = True
    write_ase: bool = True
    write_glb: bool = False
    write_fbx: bool = False
    write_max: bool = False

    def writes_any_output_file(self) -> bool:
        return any(
            (
                self.write_blend,
                self.write_3dp,
                self.write_ase,
                self.write_glb,
                self.write_fbx,
                self.write_max,
            )
        )

    def writes_any_export_format(self) -> bool:
        return any(
            (
                self.write_blend,
                self.write_3dp,
                self.write_ase,
                self.write_glb,
                self.write_fbx,
                self.write_max,
            )
        )

    def ase_export_owner(self) -> str:
        if not self.write_ase:
            return ""
        if self.write_blend:
            return ASE_OWNER_BLENDER
        if self.write_max:
            return ASE_OWNER_MAX
        return ""

    def as_def_kwargs(self) -> dict[str, bool]:
        return {
            "import_arms": self.import_arms,
            "import_animations": self.import_animations,
            "import_collisions": self.import_collisions,
            "import_occlusion": self.import_occlusion,
            "import_lights": self.import_lights,
            "write_blend": self.write_blend,
            "write_ase": self.write_ase,
            "write_3dp": self.write_3dp,
            "write_glb": self.write_glb,
            "write_fbx": self.write_fbx,
        }

    def as_loose_kwargs(self) -> dict[str, bool]:
        return {
            "import_collisions": self.import_collisions,
            "import_occlusion": self.import_occlusion,
            "import_lights": self.import_lights,
            "write_blend": self.write_blend,
            "write_ase": self.write_ase,
            "write_3dp": self.write_3dp,
            "write_glb": self.write_glb,
            "write_fbx": self.write_fbx,
        }

    def dedupe_tuple(self) -> tuple[bool, ...]:
        return (
            self.import_animations,
            self.import_collisions,
            self.import_occlusion,
            self.import_lights,
            self.import_arms,
            self.write_blend,
            self.write_3dp,
            self.write_ase,
            self.write_glb,
            self.write_fbx,
            self.write_max,
        )


@dataclass(frozen=True)
class ScanItem:
    """One importable definition entry returned by a game-directory scan."""

    name: str
    type: str
    source_model: str = ""
    output_stem: str = ""

    @classmethod
    def from_mapping(cls, item: dict[str, str]) -> "ScanItem":
        return cls(
            name=str(item.get("name", "")),
            type=str(item.get("type", "")),
            source_model=str(item.get("source_model", "")),
            output_stem=str(item.get("output_stem", "")),
        )

    def to_dict(self) -> dict[str, str]:
        return {
            "name": self.name,
            "type": self.type,
            "source_model": self.source_model,
            "output_stem": self.output_stem,
        }

    def __getitem__(self, key: str) -> str:
        return self.to_dict()[key]


@dataclass(frozen=True)
class ImportRequest:
    mode: str
    output_root: str
    options: ImportOptions = field(default_factory=ImportOptions)
    base_dir: str = ""
    item_name: str = ""
    item_type: str = ""
    threedi_path: str = ""
    output_stem: str = ""
    # Source game code (e.g. "jo", "jodemo"); selects the SCR decode key. Defaults to JO.
    game: str = "jo"

    @classmethod
    def for_definition(
        cls,
        *,
        base_dir: str,
        item_name: str,
        item_type: str,
        output_root: str,
        output_stem: str = "",
        options: ImportOptions | None = None,
        game: str = "jo",
    ) -> "ImportRequest":
        return cls(
            mode=IMPORT_MODE_DEF,
            base_dir=base_dir,
            item_name=item_name,
            item_type=item_type,
            output_root=output_root,
            output_stem=output_stem,
            options=options or ImportOptions(),
            game=game,
        )

    @classmethod
    def for_loose(
        cls,
        *,
        threedi_path: str,
        output_root: str,
        output_stem: str = "",
        base_dir: str = "",
        options: ImportOptions | None = None,
        game: str = "jo",
    ) -> "ImportRequest":
        return cls(
            mode=IMPORT_MODE_LOOSE,
            base_dir=base_dir,
            item_type=ITEM_TYPE_LOOSE,
            item_name=Path(threedi_path).stem,
            threedi_path=threedi_path,
            output_stem=output_stem,
            output_root=output_root,
            options=options or ImportOptions(),
            game=game,
        )

    @property
    def display_name(self) -> str:
        if self.mode == IMPORT_MODE_LOOSE:
            return self.output_stem or Path(self.threedi_path).stem
        return self.item_name

    @property
    def output_display_name(self) -> str:
        return self.output_stem or self.display_name

    @property
    def label(self) -> str:
        if self.mode == IMPORT_MODE_LOOSE:
            return f"loose {self.display_name}"
        return f"{self.item_type} {self.item_name}"

    @property
    def loose_output_dir(self) -> str:
        return str(Path(self.output_root) / self.display_name)

    @property
    def likely_output_dir(self) -> str:
        if self.mode == IMPORT_MODE_LOOSE:
            return self.loose_output_dir
        return str(Path(self.output_root) / self.output_display_name)

    def dedupe_key(self) -> tuple[object, ...]:
        return (
            self.mode,
            _norm_path(self.base_dir),
            self.item_type,
            self.item_name.casefold(),
            _norm_path(self.threedi_path),
            self.output_stem.casefold(),
            _norm_path(self.output_root),
            self.options.dedupe_tuple(),
        )


@dataclass
class ImportResult:
    request: ImportRequest
    ok: bool
    message: str = ""
    output_path: str = ""
    error: str = ""
    written_files: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)
    elapsed_seconds: float = 0.0

    @classmethod
    def success(
        cls,
        request: ImportRequest,
        *,
        message: str = "Import complete.",
        output_path: str = "",
        written_files: list[str] | None = None,
        warnings: list[str] | None = None,
        elapsed_seconds: float = 0.0,
    ) -> "ImportResult":
        return cls(
            request=request,
            ok=True,
            message=message,
            output_path=output_path,
            written_files=written_files or [],
            warnings=warnings or [],
            elapsed_seconds=elapsed_seconds,
        )

    @classmethod
    def failure(
        cls,
        request: ImportRequest,
        *,
        error: str,
        message: str = "Import failed.",
        output_path: str = "",
        written_files: list[str] | None = None,
        warnings: list[str] | None = None,
        elapsed_seconds: float = 0.0,
    ) -> "ImportResult":
        return cls(
            request=request,
            ok=False,
            message=message,
            output_path=output_path,
            error=error,
            written_files=written_files or [],
            warnings=warnings or [],
            elapsed_seconds=elapsed_seconds,
        )


@dataclass
class ImportJob:
    request: ImportRequest
    id: str = field(default_factory=lambda: uuid.uuid4().hex[:12])
    status: str = JOB_PENDING
    created_at: float = field(default_factory=time.time)
    started_at: float | None = None
    finished_at: float | None = None
    result: ImportResult | None = None
    error: str = ""

    @property
    def label(self) -> str:
        return self.request.label

    def mark_running(self) -> None:
        self.status = JOB_RUNNING
        self.started_at = time.time()
        self.finished_at = None
        self.error = ""

    def finish(self, result: ImportResult) -> None:
        self.result = result
        self.finished_at = time.time()
        self.status = JOB_DONE if result.ok else JOB_ERROR
        self.error = result.error

    def retry(self) -> None:
        self.status = JOB_PENDING
        self.started_at = None
        self.finished_at = None
        self.result = None
        self.error = ""

    @property
    def elapsed_seconds(self) -> float:
        if self.result and self.result.elapsed_seconds:
            return self.result.elapsed_seconds
        if self.started_at is None:
            return 0.0
        end = self.finished_at or time.time()
        return max(0.0, end - self.started_at)


@dataclass
class ScanResult:
    ok: bool
    items: list[ScanItem] = field(default_factory=list)
    error: str = ""


def validate_import_request(
    request: ImportRequest,
    *,
    check_paths: bool = True,
) -> list[str]:
    errors: list[str] = []

    output_root = request.output_root.strip()
    if not output_root:
        errors.append("Output directory is required.")
    elif check_paths:
        output_path = Path(output_root)
        if output_path.exists() and not output_path.is_dir():
            errors.append("Output path exists and is not a directory.")

    if request.mode == IMPORT_MODE_DEF:
        if request.item_type not in (ITEM_TYPE_WEAPON, ITEM_TYPE_ITEM):
            errors.append("Definition imports require item type 'weapon' or 'item'.")
        if not request.item_name.strip():
            errors.append("Item name is required.")
        if not request.base_dir.strip():
            errors.append("Game directory is required.")
        elif check_paths and not Path(request.base_dir).is_dir():
            errors.append("Game directory does not exist.")
    elif request.mode == IMPORT_MODE_LOOSE:
        if not request.threedi_path.strip():
            errors.append(".3di file path is required.")
        else:
            threedi = Path(request.threedi_path)
            if threedi.suffix.lower() != ".3di":
                errors.append("Loose imports require a .3di file.")
            if check_paths and not threedi.is_file():
                errors.append(".3di file does not exist.")
        if request.base_dir.strip() and check_paths and not Path(request.base_dir).is_dir():
            errors.append("Asset search directory does not exist.")
    else:
        errors.append(f"Unknown import mode: {request.mode}")

    if not request.options.writes_any_output_file():
        errors.append("Select at least one file to write.")
    if request.options.write_ase and not request.options.ase_export_owner():
        errors.append("ASE export requires .blend or .max output.")
    if (request.options.write_glb or request.options.write_fbx) and not request.options.write_blend:
        errors.append("GLB/FBX export requires .blend output.")

    return errors


def has_active_duplicate(jobs: Iterable[ImportJob], request: ImportRequest) -> bool:
    key = request.dedupe_key()
    return any(job.status in ACTIVE_JOB_STATUSES and job.request.dedupe_key() == key for job in jobs)
