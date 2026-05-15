"""Stock 3DI -> generated source -> ModSuperOED roundtrip harness."""

from __future__ import annotations

from dataclasses import dataclass
import json
from pathlib import Path
from typing import Iterable

from pyopennova import threedi_ffi
from pyopennova.ase_writer_ffi import write_ase_files_from_3di3
from pyopennova.legacy_tools import modsuperoed
from pyopennova.three_di_policy_ffi import (
    OBJECT_3DI_COMPARE_RELAX_GEOMETRY,
    compare_files,
)
from pyopennova.project_writer import write_3dp_from_3di3


STOCK_THREEDI_FIXTURE_NAMES: tuple[str, ...] = (
    "akcrate",
    "Armry01",
    "Beret",
    "mp5_1st",
    "US01",
)


@dataclass(frozen=True)
class StockThreediFixture:
    name: str
    directory: Path
    reference_3di_path: Path


@dataclass(frozen=True)
class GeneratedStockSource:
    fixture: StockThreediFixture
    directory: Path
    project_path: Path
    ase_paths: tuple[Path, ...]


@dataclass(frozen=True)
class StockRoundtripResult:
    fixture: StockThreediFixture
    source: GeneratedStockSource
    modsuperoed_output: Path
    log_path: Path
    report: dict
    report_path: Path


def repo_root_from_here() -> Path:
    return Path(__file__).resolve().parents[2]


def discover_stock_3di_fixtures(
    names: Iterable[str] | None = None,
    repo_root: str | Path | None = None,
) -> list[StockThreediFixture]:
    root = Path(repo_root) if repo_root is not None else repo_root_from_here()
    fixture_root = root / "fixtures" / "stock_3di"
    wanted = tuple(names) if names is not None else STOCK_THREEDI_FIXTURE_NAMES
    fixtures: list[StockThreediFixture] = []
    for name in wanted:
        directory = fixture_root / name
        if not directory.is_dir():
            raise FileNotFoundError(f"missing stock 3DI fixture directory: {directory}")
        reference = directory / f"{name}.3di"
        if not reference.is_file():
            raise FileNotFoundError(f"{directory} has no reference .3di at {reference}")
        fixtures.append(
            StockThreediFixture(
                name=name,
                directory=directory,
                reference_3di_path=reference,
            )
        )
    return fixtures


def generate_source_from_stock_3di(
    fixture: StockThreediFixture,
    output_dir: str | Path,
    *,
    collision_lod_index: int | None = None,
) -> GeneratedStockSource:
    out_dir = Path(output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    ase_path = out_dir / f"{fixture.name}.ase"
    project_path = out_dir / f"{fixture.name}.3dp"

    model = threedi_ffi.read_model_3di3(str(fixture.reference_3di_path))
    try:
        ase_paths = tuple(Path(path) for path in write_ase_files_from_3di3(model, str(ase_path)))
        write_3dp_from_3di3(model, str(project_path), collision_lod_index=collision_lod_index)
    finally:
        threedi_ffi.free_model_3di3(model)

    return GeneratedStockSource(
        fixture=fixture,
        directory=out_dir,
        project_path=project_path,
        ase_paths=ase_paths,
    )


def generate_source_via_blender(
    fixture: StockThreediFixture,
    output_dir: str | Path,
    *,
    collision_lod_index: int | None = None,
) -> GeneratedStockSource:
    """Alternative to generate_source_from_stock_3di that routes .ase generation
    through headless Blender (for the Phase E DCC roundtrip gate).

    Same outputs as generate_source_from_stock_3di (.ase + .3dp), but the .ase
    files are produced by importing the source FlatMeshes into bpy, exporting
    them back to FlatMeshes via the flat_mesh_bridge, and then writing the
    final .ase from those round-tripped meshes via the C++ chain.

    Requires bpy. Raises ImportError if bpy is not installed.
    """
    from opennova_blender.ase_export import export_ase_via_blender
    out_dir = Path(output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    ase_path = out_dir / f"{fixture.name}.ase"
    project_path = out_dir / f"{fixture.name}.3dp"

    model = threedi_ffi.read_model_3di3(str(fixture.reference_3di_path))
    try:
        ase_paths = tuple(Path(p) for p in export_ase_via_blender(model, str(ase_path)))
        write_3dp_from_3di3(model, str(project_path), collision_lod_index=collision_lod_index)
    finally:
        threedi_ffi.free_model_3di3(model)

    return GeneratedStockSource(
        fixture=fixture,
        directory=out_dir,
        project_path=project_path,
        ase_paths=ase_paths,
    )


def generate_source_via_max(
    fixture: StockThreediFixture,
    output_dir: str | Path,
    *,
    collision_lod_index: int | None = None,
) -> GeneratedStockSource:
    """Alternative to generate_source_from_stock_3di that routes .ase generation
    through 3ds Max (for the Phase H DCC roundtrip).

    Same outputs as generate_source_from_stock_3di (.ase + .3dp), but the .ase
    files are produced by importing the source FlatMeshes into Max, exporting
    them back to FlatMeshes via the flat_mesh_bridge, and then writing the
    final .ase from those round-tripped meshes via the C++ chain.

    Requires pymxs (3ds Max). Raises ImportError if pymxs is not installed.
    """
    from opennova_max.ase_export import export_ase_via_max
    out_dir = Path(output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    ase_path = out_dir / f"{fixture.name}.ase"
    project_path = out_dir / f"{fixture.name}.3dp"

    model = threedi_ffi.read_model_3di3(str(fixture.reference_3di_path))
    try:
        ase_paths = tuple(Path(p) for p in export_ase_via_max(model, str(ase_path)))
        write_3dp_from_3di3(model, str(project_path), collision_lod_index=collision_lod_index)
    finally:
        threedi_ffi.free_model_3di3(model)

    return GeneratedStockSource(
        fixture=fixture,
        directory=out_dir,
        project_path=project_path,
        ase_paths=ase_paths,
    )


def roundtrip_stock_3di_with_modsuperoed(
    fixture: StockThreediFixture,
    output_dir: str | Path,
    *,
    import_delay_ms: int = 5000,
    export_delay_ms: int = 5000,
    timeout_s: int = 120,
) -> StockRoundtripResult:
    out_dir = Path(output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    source = generate_source_from_stock_3di(fixture, out_dir / "source")
    output_title = reference_model_stem(fixture)
    output_path = out_dir / f"{output_title}.3di"

    export = modsuperoed.export_3di(
        source.project_path,
        output_path,
        work_dir=out_dir / "modsuperoed_work",
        import_delay_ms=import_delay_ms,
        export_delay_ms=export_delay_ms,
        timeout_s=timeout_s,
        output_title=output_title,
    )

    report_path = out_dir / f"{fixture.name}_roundtrip_compare.json"
    report = compare_files(
        fixture.reference_3di_path,
        export.output_path,
        policy=OBJECT_3DI_COMPARE_RELAX_GEOMETRY,
        report_path=report_path,
    )
    return StockRoundtripResult(
        fixture=fixture,
        source=source,
        modsuperoed_output=export.output_path,
        log_path=export.log_path,
        report=report,
        report_path=report_path,
    )


def write_stock_roundtrip_report(result: StockRoundtripResult, path: str | Path) -> None:
    payload = {
        "fixture": result.fixture.name,
        "reference_3di": str(result.fixture.reference_3di_path),
        "project": str(result.source.project_path),
        "ase": [str(path) for path in result.source.ase_paths],
        "modsuperoed_output": str(result.modsuperoed_output),
        "log": str(result.log_path),
        "compare_report": str(result.report_path),
        "failed": bool(result.report.get("failed", True)),
        "status": result.report.get("status"),
        "events": result.report.get("events", []),
    }
    Path(path).write_text(json.dumps(payload, indent=2, sort_keys=True), encoding="utf-8")


def reference_model_stem(fixture: StockThreediFixture) -> str:
    model = threedi_ffi.read_model_3di3(str(fixture.reference_3di_path))
    try:
        name = _decode_c_string(model.header.name)
    finally:
        threedi_ffi.free_model_3di3(model)
    if name.lower().endswith(".3di"):
        name = name[:-4]
    return Path(name).name or fixture.name


def _decode_c_string(value) -> str:
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace").rstrip("\x00")
    return str(value).rstrip("\x00")
