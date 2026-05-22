"""Reverse roundtrip: stock .3di → our .3dp/.ase → ModSuperOED → .3di matches source.

The forward smoke (test_modsuperoed_automation.py::test_external_modsuperoed_smoke)
validates the OED automation hook by running the tracked CharModel.3dp/.ase through
ModSuperOED and checking the output matches CharModel.3di. This file is the reverse:
take a canonical .3di, regenerate the project files with our importer, push them back
through OED, and assert the resulting .3di matches the source.

It exercises libs/tdp::tdp_from_ir (the 3dp/3da writer), blender.ase_exporter, and
apps/importer/scene_builder.py end-to-end.

Add a fixture by dropping a stock .3di into fixtures/threedi/stock_jo/ and appending
its filename to FIXTURES.
"""
from __future__ import annotations

import os
import sys
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
STOCK_JO = ROOT / "fixtures" / "threedi" / "stock_jo"

FIXTURES: list[str] = [
    "wtrfall.3di",
]


@pytest.mark.skipif(not sys.platform.startswith("win"), reason="ModSuperOED is Windows-only")
@pytest.mark.parametrize("source_name", FIXTURES, ids=lambda n: Path(n).stem)
def test_ase_3dp_produces_correct_3di_via_original_oed_tool(
    source_name: str,
    tmp_path: Path,
) -> None:
    from apps import modsuperoed
    from apps.importer.import_runner import execute_import_request
    from apps.importer.jobs import ImportOptions, ImportRequest
    from blender.opennova.threedi_compare_ffi import compare_3di3_chunks

    tool_dir_env = os.environ.get("OPENNOVA_MODSUPEROED_DIR")
    if not tool_dir_env:
        pytest.skip("OPENNOVA_MODSUPEROED_DIR is not set")
    tool_dir = Path(tool_dir_env)

    source_3di = STOCK_JO / source_name
    paths_template = modsuperoed.resolve_paths(tool_dir=tool_dir, repo_root=ROOT)
    exe_path = tool_dir / "ModSuperOed.exe"
    required = [source_3di, exe_path, paths_template.injector, paths_template.hook_dll]
    missing = [p for p in required if not p.is_file()]
    if missing:
        pytest.skip("missing roundtrip artifacts: " + ", ".join(str(p) for p in missing))

    stem = source_3di.stem

    generated_root = tmp_path / "generated"
    request = ImportRequest.for_loose(
        threedi_path=str(source_3di),
        output_root=str(generated_root),
        output_stem=stem,
        options=ImportOptions(
            write_blend=False,
            write_3dp=True,
            write_ase=True,
            write_glb=False,
            write_fbx=False,
        ),
    )
    result = execute_import_request(request)
    assert result.ok, f"importer failed: {result.error}"
    generated_dir = Path(result.output_path)
    assert (generated_dir / f"{stem}.3dp").is_file()
    assert (generated_dir / f"{stem}.ase").is_file()

    paths = modsuperoed.ModSuperOEDPaths(
        tool_dir=generated_dir,
        injector=paths_template.injector,
        hook_dll=paths_template.hook_dll,
        exe=exe_path,
    )
    regenerated = tmp_path / f"{stem}.regenerated.3di"
    export_result = modsuperoed.export_3di(
        project_path=generated_dir / f"{stem}.3dp",
        output_path=regenerated,
        paths=paths,
        work_dir=tmp_path / "oed-work",
        timeout_s=120,
        output_title=stem,
    )

    assert export_result.returncode == 0
    assert regenerated.is_file()
    assert regenerated.stat().st_size > 0
    compare_3di3_chunks(
        source_3di,
        regenerated,
        "GHDR,USRP,INFO,CTRL,MTRL,OCCL,LGHT,MTRX,RDTA,CDTA",
    )
    assert "ExitProcess(0)" in export_result.log_path.read_text(encoding="utf-8", errors="replace")
