"""Run Max current-scene ASE parity fixtures under 3dsmaxbatch.exe."""
from __future__ import annotations

import argparse
import difflib
import json
import os
import re
import sys
import traceback
from pathlib import Path
from typing import Any


WORKTREE = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT_ROOT = WORKTREE / "build-max-ase-exporter-parity"


FORBIDDEN_IMPORT_METADATA = (
    "_lod_index",
    "_part_index",
    "_material_names",
    "nl_ase_name",
    "nl_raw_position",
    "nl_raw_direction",
    "nl_parent_index",
    "nl_type_code",
    "opennova_zero_axis",
    "opennova_part_index",
    "opennova_material_ids",
    "opennova_max_material_ids",
    "opennova_source_face_material_ids",
    "opennova_face_material_ids",
    "opennova_face_material_id_mode",
    "atten_start",
    "falloff",
    "tm_row2",
    "light_type",
)

ASE_NUMERIC_TOKEN = re.compile(r"[-+]?(?:\d+\.\d*|\.\d+|\d+)(?:[eE][-+]?\d+)?")
ASE_FLOAT_TOLERANCE = 1.1e-4


def _emit(line: str) -> None:
    print(line, flush=True)


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--output-root",
        default=os.environ.get("OPENNOVA_MAX_ASE_PARITY_OUTPUT_ROOT", str(DEFAULT_OUTPUT_ROOT)),
        help="Directory for Max current-scene ASE parity outputs.",
    )
    parser.add_argument(
        "--fixture",
        action="append",
        dest="fixtures",
        help="Fixture name to run. May be provided multiple times.",
    )
    return parser.parse_args()


def _env_fixtures() -> tuple[str, ...]:
    value = os.environ.get("OPENNOVA_MAX_ASE_PARITY_FIXTURES", "")
    return tuple(part.strip() for part in value.split(",") if part.strip())


def _result_record(result: dict[str, object]) -> dict[str, object]:
    return {
        "fixture": result["fixture"],
        "direct": str(result["direct"]),
        "exported": str(result["exported"]),
        "direct_project": str(result["direct_project"]),
        "report": str(result["report"]),
        "comparison": result["comparison"],
        "stripped_metadata_count": result["stripped_metadata_count"],
    }


def _run_fixture(name: str, output_dir: Path) -> dict[str, object]:
    import pymxs

    from pyopennova.asset_resolver import AssetResolver
    from pyopennova.host_outputs import write_host_neutral_outputs
    from pyopennova.legacy_tools.stock_roundtrip import discover_stock_3di_fixtures
    from pyopennova.threedi_ffi import free_model_3di3, read_model_3di3
    from opennova_max.ase_scene_exporter import AseSceneExporter
    from opennova_max.output_writers import reset_scene
    from opennova_max.scene_builder import MaxSceneBuilder

    rt = pymxs.runtime
    fixture = discover_stock_3di_fixtures(names=[name], repo_root=WORKTREE)[0]
    direct_dir = output_dir / "direct"
    max_dir = output_dir / "max"
    direct_dir.mkdir(parents=True, exist_ok=True)
    max_dir.mkdir(parents=True, exist_ok=True)

    reset_scene()
    model = read_model_3di3(str(fixture.reference_3di_path))
    try:
        written = write_host_neutral_outputs(model, str(direct_dir), name)
        direct_ase = direct_dir / f"{name}.ase"
        direct_project = direct_dir / f"{name}.3dp"
        if direct_ase not in {Path(path) for path in written}:
            raise AssertionError(f"direct ASE was not written: {direct_ase}")
        if not direct_project.is_file():
            raise AssertionError(f"direct project was not written: {direct_project}")

        with AssetResolver(str(fixture.directory)) as resolver:
            builder = MaxSceneBuilder(model, resolver=resolver)
            if not builder.build_basic_scene(name):
                raise AssertionError(f"MaxSceneBuilder produced no scene objects for {name}")

        stripped_metadata_count = _strip_forbidden_metadata(rt)
        leaked = _find_forbidden_metadata(rt)
        if leaked:
            raise AssertionError("forbidden importer metadata survived strip: " + ", ".join(leaked[:20]))

        exported_ase = max_dir / f"{name}.ase"
        exporter = AseSceneExporter(rt)
        exporter.export_textures = False
        if not exporter.export_scene(str(exported_ase)):
            raise AssertionError(f"AseSceneExporter returned false for {exported_ase}")

        report_path = output_dir / f"{name}_ase_mismatch.txt"
        matches, comparison = _ase_outputs_match(direct_ase, exported_ase)
        if not matches:
            report_path.write_text(
                _ase_mismatch_report(direct_ase, exported_ase)
                + "\nASE comparison:\n"
                + comparison
                + "\nMax material diagnostics:\n"
                + _max_material_diagnostics(rt),
                encoding="utf-8",
                errors="replace",
            )
            raise AssertionError(f"Max current-scene ASE does not match direct ASE: {report_path}")
        report_path.write_text(comparison, encoding="utf-8")
        return {
            "fixture": name,
            "direct": direct_ase,
            "exported": exported_ase,
            "direct_project": direct_project,
            "report": report_path,
            "comparison": comparison.splitlines()[0] if comparison else "unknown",
            "stripped_metadata_count": stripped_metadata_count,
        }
    finally:
        free_model_3di3(model)
        reset_scene()


def _strip_forbidden_metadata(rt: Any) -> int:
    stripped = 0
    for obj in _scene_objects(rt):
        stripped += _strip_props(rt, obj)
        material = getattr(obj, "material", None)
        stripped += _strip_material_tree(rt, material)
    for material in _scene_materials(rt):
        stripped += _strip_material_tree(rt, material)
    return stripped


def _strip_material_tree(rt: Any, material: Any, seen: set[int] | None = None) -> int:
    if material is None:
        return 0
    if seen is None:
        seen = set()
    key = id(material)
    if key in seen:
        return 0
    seen.add(key)
    stripped = _strip_props(rt, material)
    for child in _multi_material_entries(material):
        stripped += _strip_material_tree(rt, child, seen)
    return stripped


def _strip_props(rt: Any, obj: Any) -> int:
    count = 0
    for key in FORBIDDEN_IMPORT_METADATA:
        if _get_user_prop(rt, obj, key) is not None:
            _set_user_prop(rt, obj, key, "")
            count += 1
    return count


def _find_forbidden_metadata(rt: Any) -> list[str]:
    leaked: list[str] = []
    for obj in _scene_objects(rt):
        _collect_leaks(rt, obj, _object_name(obj), leaked)
        material = getattr(obj, "material", None)
        _collect_material_leaks(rt, material, f"{_object_name(obj)}.material", leaked)
    for material in _scene_materials(rt):
        _collect_material_leaks(rt, material, f"sceneMaterial.{_object_name(material)}", leaked)
    return leaked


def _collect_material_leaks(rt: Any, material: Any, label: str, leaked: list[str], seen: set[int] | None = None) -> None:
    if material is None:
        return
    if seen is None:
        seen = set()
    key = id(material)
    if key in seen:
        return
    seen.add(key)
    _collect_leaks(rt, material, label, leaked)
    for idx, child in enumerate(_multi_material_entries(material), start=1):
        _collect_material_leaks(rt, child, f"{label}[{idx}]", leaked, seen)


def _collect_leaks(rt: Any, obj: Any, label: str, leaked: list[str]) -> None:
    for key in FORBIDDEN_IMPORT_METADATA:
        if _get_user_prop(rt, obj, key) is not None:
            leaked.append(f"{label}:{key}")


def _scene_objects(rt: Any) -> list[Any]:
    try:
        return list(rt.objects)
    except Exception:
        return []


def _scene_materials(rt: Any) -> list[Any]:
    try:
        return list(rt.sceneMaterials)
    except Exception:
        return []


def _multi_material_entries(material: Any) -> list[Any]:
    if material is None:
        return []
    try:
        count = int(getattr(material, "numsubs"))
    except Exception:
        count = 0
    material_list = getattr(material, "materialList", None)
    out: list[Any] = []
    for idx in range(1, count + 1):
        child = _seq_get(material_list, idx)
        if child is not None:
            out.append(child)
    return out


def _seq_get(seq: Any, one_based_index: int) -> Any:
    if seq is None:
        return None
    for idx in (one_based_index, one_based_index - 1):
        try:
            return seq[idx]
        except Exception:
            pass
    try:
        values = getattr(seq, "values")
        return values[one_based_index - 1]
    except Exception:
        return None


def _get_user_prop(rt: Any, obj: Any, key: str) -> Any:
    try:
        value = rt.getUserProp(obj, key)
    except Exception:
        return None
    if value is None or str(value) == "":
        return None
    return value


def _set_user_prop(rt: Any, obj: Any, key: str, value: Any) -> None:
    try:
        rt.setUserProp(obj, key, value)
    except Exception:
        pass


def _object_name(obj: Any) -> str:
    try:
        return str(getattr(obj, "name", "") or obj)
    except Exception:
        return repr(obj)


def _ase_mismatch_report(expected: Path, actual: Path) -> str:
    expected_text = expected.read_text(encoding="utf-8", errors="replace").splitlines()
    actual_text = actual.read_text(encoding="utf-8", errors="replace").splitlines()
    diff = "\n".join(
        difflib.unified_diff(
            expected_text[:500],
            actual_text[:500],
            fromfile=str(expected),
            tofile=str(actual),
            lineterm="",
            n=3,
        )
    )
    return (
        f"Max current-scene ASE does not match direct ASE\n"
        f"expected: {expected}\n"
        f"actual:   {actual}\n"
        f"expected summary: {_ase_summary(expected)}\n"
        f"actual summary:   {_ase_summary(actual)}\n"
        f"first diff:\n{diff}\n"
    )


def _ase_outputs_match(expected: Path, actual: Path) -> tuple[bool, str]:
    expected_bytes = expected.read_bytes()
    actual_bytes = actual.read_bytes()
    if expected_bytes == actual_bytes:
        return True, "ASE byte parity passed\n"
    return _ase_texts_match(
        expected.read_text(encoding="utf-8", errors="replace"),
        actual.read_text(encoding="utf-8", errors="replace"),
    )


def _ase_texts_match(
    expected_text: str,
    actual_text: str,
    *,
    tolerance: float = ASE_FLOAT_TOLERANCE,
) -> tuple[bool, str]:
    expected_lines = expected_text.splitlines()
    actual_lines = actual_text.splitlines()
    if len(expected_lines) != len(actual_lines):
        return (
            False,
            "ASE line counts differ: expected %d, actual %d\n"
            % (len(expected_lines), len(actual_lines)),
        )

    tolerated_lines = 0
    max_delta = 0.0
    max_line = 0
    for line_number, (expected, actual) in enumerate(zip(expected_lines, actual_lines), start=1):
        if expected == actual:
            continue

        expected_numbers = ASE_NUMERIC_TOKEN.findall(expected)
        actual_numbers = ASE_NUMERIC_TOKEN.findall(actual)
        expected_shape = ASE_NUMERIC_TOKEN.sub("#", expected)
        actual_shape = ASE_NUMERIC_TOKEN.sub("#", actual)
        if expected_shape != actual_shape or len(expected_numbers) != len(actual_numbers):
            return (
                False,
                "ASE structural text differs at line %d\nexpected: %s\nactual:   %s\n"
                % (line_number, expected, actual),
            )

        line_max = 0.0
        for expected_number, actual_number in zip(expected_numbers, actual_numbers):
            delta = abs(float(expected_number) - float(actual_number))
            if delta > tolerance:
                return (
                    False,
                    "ASE numeric token differs beyond tolerance at line %d: %.9g > %.9g\n"
                    "expected: %s\nactual:   %s\n"
                    % (line_number, delta, tolerance, expected, actual),
                )
            line_max = max(line_max, delta)

        tolerated_lines += 1
        if line_max > max_delta:
            max_delta = line_max
            max_line = line_number

    return (
        True,
        "ASE semantic parity passed; byte diff is numeric drift only "
        "(%d lines, max delta %.9g at line %d, tolerance %.9g)\n"
        % (tolerated_lines, max_delta, max_line, tolerance),
    )


def _ase_summary(path: Path) -> dict[str, object]:
    from pyopennova import ase_ffi

    doc = ase_ffi.parse_file(str(path))
    try:
        return {
            "objects": int(doc.object_count),
            "lights": int(doc.light_count),
            "materials": int(doc.material_count),
            "flags": int(doc.flags),
            "skinned_flags": int(doc.skinned_flags),
            "object_names": [
                _cstr(doc.objects[i].name)
                for i in range(min(int(doc.object_count), 20))
            ],
            "light_names": [
                _cstr(doc.lights[i].name)
                for i in range(min(int(doc.light_count), 20))
            ],
            "material_names": [
                _cstr(doc.materials[i].name)
                for i in range(min(int(doc.material_count), 20))
            ],
        }
    finally:
        ase_ffi.free_document(doc)


def _cstr(value: Any) -> str:
    return bytes(value).split(b"\0", 1)[0].decode("utf-8", errors="replace")


def _max_material_diagnostics(rt: Any) -> str:
    lines: list[str] = []
    seen: set[int] = set()
    for obj in _scene_objects(rt):
        material = getattr(obj, "material", None)
        _collect_material_diagnostics(rt, material, f"{_object_name(obj)}.material", lines, seen)
    for material in _scene_materials(rt):
        _collect_material_diagnostics(rt, material, f"sceneMaterial.{_object_name(material)}", lines, seen)
    return "\n".join(lines[:200]) + ("\n" if lines else "")


def _collect_material_diagnostics(rt: Any, material: Any, label: str, lines: list[str], seen: set[int]) -> None:
    if material is None:
        return
    key = id(material)
    if key in seen:
        return
    seen.add(key)
    lines.append(
        "%s name=%r class=%r diffuse=%r selfIllum=%r map1=%r map2=%r map5=%r"
        % (
            label,
            _object_name(material),
            _class_name(rt, material),
            _bitmap_debug(getattr(material, "diffuseMap", None)),
            _bitmap_debug(getattr(material, "selfIllumMap", None)),
            _bitmap_debug(_seq_get(getattr(material, "maps", None), 1)),
            _bitmap_debug(_seq_get(getattr(material, "maps", None), 2)),
            _bitmap_debug(_seq_get(getattr(material, "maps", None), 5)),
        )
    )
    for idx, child in enumerate(_multi_material_entries(material), start=1):
        _collect_material_diagnostics(rt, child, f"{label}[{idx}]", lines, seen)


def _bitmap_debug(bitmap: Any) -> str:
    if bitmap is None:
        return ""
    for attr in ("filename", "fileName", "Filename"):
        try:
            value = getattr(bitmap, attr)
            if value:
                return os.path.basename(str(value))
        except Exception:
            pass
    return _object_name(bitmap)


def _class_name(rt: Any, obj: Any) -> str:
    try:
        return str(rt.classOf(obj))
    except Exception:
        return obj.__class__.__name__


def main() -> int:
    args = _parse_args()
    sys.path.insert(0, str(WORKTREE))

    from pyopennova.legacy_tools.stock_roundtrip import STOCK_THREEDI_FIXTURE_NAMES

    output_root = Path(args.output_root)
    output_root.mkdir(parents=True, exist_ok=True)
    trace_dir = output_root / "traces"
    trace_dir.mkdir(parents=True, exist_ok=True)

    fixtures = tuple(args.fixtures or _env_fixtures() or STOCK_THREEDI_FIXTURE_NAMES)
    _emit("OPENNOVA_MAX_ASE_PARITY_START")
    _emit(f"OPENNOVA_MAX_ASE_PARITY_OUTPUT_ROOT {output_root}")
    _emit(f"OPENNOVA_MAX_ASE_PARITY_FIXTURES {','.join(fixtures)}")

    failures: list[dict[str, object]] = []
    successes: list[dict[str, object]] = []
    for name in fixtures:
        _emit(f"OPENNOVA_MAX_ASE_PARITY_FIXTURE_START {name}")
        try:
            result = _run_fixture(name, output_root / name)
            successes.append(_result_record(result))
            _emit(
                "OPENNOVA_MAX_ASE_PARITY_FIXTURE_OK "
                f"{name} exported={result['exported']}"
            )
        except BaseException as exc:
            trace_path = trace_dir / f"{name}_trace.txt"
            trace_path.write_text(
                "".join(traceback.format_exception(type(exc), exc, exc.__traceback__)),
                encoding="utf-8",
                errors="replace",
            )
            failures.append(
                {
                    "fixture": name,
                    "error": f"{type(exc).__name__}: {exc}",
                    "trace": str(trace_path),
                }
            )
            _emit(
                "OPENNOVA_MAX_ASE_PARITY_FIXTURE_FAIL "
                f"{name} trace={trace_path}: {type(exc).__name__}: {exc}"
            )

    summary_path = output_root / "summary.json"
    summary_path.write_text(
        json.dumps(
            {
                "failed": bool(failures),
                "fixtures": fixtures,
                "successes": successes,
                "failures": failures,
            },
            indent=2,
        ),
        encoding="utf-8",
    )
    _emit(f"OPENNOVA_MAX_ASE_PARITY_SUMMARY {summary_path}")
    _emit(f"OPENNOVA_MAX_ASE_PARITY_RESULT {not failures}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
