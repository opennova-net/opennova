"""End-to-end importer integration tests.

Drives the same code path the GUI and CLI use (`execute_import_request`)
against real fixture .3di files, then asserts the on-disk outputs match
the requested options.
"""
from __future__ import annotations

from pathlib import Path

import pytest

from apps.importer.import_runner import execute_import_request
from apps.importer.jobs import ImportOptions, ImportRequest


ROOT = Path(__file__).resolve().parents[1]
FIXTURE_DIR = ROOT / "fixtures" / "threedi"

FIXTURES: list[Path] = [
    FIXTURE_DIR / "3di3" / "Shed.3di",
    FIXTURE_DIR / "Fsldr03.3di",
    FIXTURE_DIR / "mp5_1st.3di",
]

OPTION_PRESETS: dict[str, ImportOptions] = {
    "round_trip": ImportOptions(),
    "all_formats": ImportOptions(write_glb=True, write_fbx=True),
    "glb_only": ImportOptions(
        write_blend=False, write_3dp=False, write_ase=False, write_glb=True,
    ),
    "blend_only": ImportOptions(write_3dp=False, write_ase=False),
}

ALL_FORMAT_SUFFIXES = {".blend", ".3dp", ".3da", ".ase", ".glb", ".fbx"}


def _expected_files(options: ImportOptions, stem: str) -> set[str]:
    expected: set[str] = set()
    if options.write_blend:
        expected.add(f"{stem}.blend")
    if options.write_3dp:
        expected.add(f"{stem}.3dp")
        expected.add(f"{stem}.3da")
    if options.write_ase:
        expected.add(f"{stem}.ase")
    if options.write_glb:
        expected.add(f"{stem}.glb")
    if options.write_fbx:
        expected.add(f"{stem}.fbx")
    return expected


@pytest.mark.parametrize("threedi_path", FIXTURES, ids=lambda p: p.stem)
@pytest.mark.parametrize("preset_name", list(OPTION_PRESETS), ids=str)
def test_loose_import_produces_expected_outputs(
    threedi_path: Path,
    preset_name: str,
    tmp_path: Path,
) -> None:
    if not threedi_path.is_file():
        pytest.fail(f"Fixture missing (LFS not pulled?): {threedi_path}")

    options = OPTION_PRESETS[preset_name]
    request = ImportRequest.for_loose(
        threedi_path=str(threedi_path),
        output_root=str(tmp_path),
        options=options,
    )

    result = execute_import_request(request)

    assert result.ok, f"import failed: {result.error}"

    output_dir = Path(result.output_path)
    assert output_dir.is_dir(), f"expected output dir at {output_dir}"

    stem = threedi_path.stem
    expected = _expected_files(options, stem)
    actual = {p.name for p in output_dir.iterdir() if p.is_file()}

    missing = expected - actual
    assert not missing, (
        f"missing outputs for {stem} ({preset_name}): {sorted(missing)}; "
        f"got {sorted(actual)}"
    )

    expected_suffixes = {Path(name).suffix for name in expected}
    leaked = {
        name for name in actual
        if Path(name).suffix in ALL_FORMAT_SUFFIXES
        and Path(name).suffix not in expected_suffixes
    }
    assert not leaked, (
        f"unexpected outputs for {stem} ({preset_name}): {sorted(leaked)}"
    )

    for name in expected:
        path = output_dir / name
        assert path.stat().st_size > 0, f"empty output {path}"


def test_consecutive_imports_in_one_session_both_write_glb(
    tmp_path: Path,
) -> None:
    """Run two imports back-to-back in one process and assert both write GLB.

    With process isolation each call to ``execute_import_request`` spawns a
    fresh worker, so cross-import bpy state degradation cannot occur.
    """
    available = [p for p in FIXTURES if p.is_file()]
    if len(available) < 2:
        pytest.skip("need at least two fixtures (LFS not pulled?)")

    options = ImportOptions(
        write_blend=False, write_3dp=False, write_ase=False, write_glb=True,
    )

    for index, threedi_path in enumerate(available[:2]):
        out_root = tmp_path / f"run_{index}"
        out_root.mkdir()
        request = ImportRequest.for_loose(
            threedi_path=str(threedi_path),
            output_root=str(out_root),
            options=options,
        )
        result = execute_import_request(request)
        assert result.ok, f"run {index} ({threedi_path.stem}) failed: {result.error}"

        glb_path = Path(result.output_path) / f"{threedi_path.stem}.glb"
        assert glb_path.is_file(), f"run {index}: expected {glb_path}"
        assert glb_path.stat().st_size > 0, f"run {index}: empty {glb_path}"
