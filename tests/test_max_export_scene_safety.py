from __future__ import annotations

import json
import subprocess
import sys
import textwrap
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]


def test_scene_safety_runner_scrubs_qt_test_environment(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    monkeypatch.setenv("QT_QPA_PLATFORM", "offscreen")
    monkeypatch.setenv("QT_PLUGIN_PATH", str(tmp_path / "pyside-plugins"))

    env = _scene_safety_runner_env(
        result_path=tmp_path / "result.json",
        output_dir=tmp_path / "out",
    )

    assert "QT_QPA_PLATFORM" not in env
    assert "QT_PLUGIN_PATH" not in env


def test_max_ase_export_preserves_skinned_scene_state(tmp_path: Path) -> None:
    from opennova_max.discovery import resolve_3dsmaxbatch

    maxbatch = resolve_3dsmaxbatch()
    if maxbatch is None:
        pytest.skip("3dsmaxbatch.exe not available; set OPENNOVA_3DSMAXBATCH to run scene safety test")

    fixture = ROOT / "fixtures" / "3dp" / "Bird1" / "Bird1.3di"
    if not fixture.is_file():
        pytest.skip(f"skinned fixture missing: {fixture}")

    runner = tmp_path / "max_scene_safety_runner.py"
    result_path = tmp_path / "result.json"
    output_dir = tmp_path / "out"
    runner.write_text(_MAX_SCENE_SAFETY_RUNNER, encoding="utf-8")

    env = _scene_safety_runner_env(result_path=result_path, output_dir=output_dir)

    process = subprocess.run(
        [str(maxbatch), str(runner)],
        cwd=str(ROOT),
        env=env,
        capture_output=True,
        text=True,
        errors="replace",
        timeout=60 * 10,
    )

    if process.returncode != 0:
        pytest.fail(
            "3dsmaxbatch scene-safety runner failed\n"
            f"stdout:\n{process.stdout[-4000:]}\n"
            f"stderr:\n{process.stderr[-4000:]}"
        )
    assert result_path.is_file(), "scene-safety runner did not write result.json"
    result = json.loads(result_path.read_text(encoding="utf-8"))
    assert result["ok"], result.get("diff", result)
    assert (output_dir / "Bird1.ase").is_file()


def _scene_safety_runner_env(*, result_path: Path, output_dir: Path) -> dict[str, str]:
    from opennova_max.runner import _max_subprocess_env

    env = _max_subprocess_env()
    env["OPENNOVA_TEST_WORKTREE"] = str(ROOT)
    env["OPENNOVA_TEST_RESULT"] = str(result_path)
    env["OPENNOVA_TEST_OUTPUT"] = str(output_dir)
    return env


_MAX_SCENE_SAFETY_RUNNER = textwrap.dedent(
    r'''
    from __future__ import annotations

    import json
    import os
    import sys
    import traceback
    from pathlib import Path


    def main() -> int:
        worktree = Path(os.environ["OPENNOVA_TEST_WORKTREE"])
        result_path = Path(os.environ["OPENNOVA_TEST_RESULT"])
        output_dir = Path(os.environ["OPENNOVA_TEST_OUTPUT"])
        sys.path.insert(0, str(worktree))

        import pymxs

        from opennova_max.ase_scene_exporter import AseSceneExporter
        from opennova_max.output_writers import reset_scene
        from opennova_max.scene_builder import MaxSceneBuilder
        from pyopennova.asset_resolver import AssetResolver
        from pyopennova.threedi_ffi import free_model_3di3, read_model

        rt = pymxs.runtime
        result_path.parent.mkdir(parents=True, exist_ok=True)
        output_dir.mkdir(parents=True, exist_ok=True)
        fixture = worktree / "fixtures" / "3dp" / "Bird1" / "Bird1.3di"

        reset_scene()
        model = read_model(str(fixture))
        try:
            with AssetResolver(str(fixture.parent)) as resolver:
                builder = MaxSceneBuilder(model, resolver=resolver)
                if not builder.build_basic_scene("Bird1"):
                    raise RuntimeError("MaxSceneBuilder produced no Bird1 scene objects")

            objects = _objects(rt)
            if objects:
                try:
                    rt.select(objects[0])
                except Exception:
                    pass
            try:
                rt.setCommandPanelTaskMode(rt.Name("modify"))
            except Exception:
                try:
                    rt.execute("setCommandPanelTaskMode #modify")
                except Exception:
                    pass

            before = _scene_signature(rt)
            exporter = AseSceneExporter(rt)
            exporter.export_textures = False
            exporter.export_scene(str(output_dir / "Bird1.ase"))
            after = _scene_signature(rt)

            result = {"ok": before == after}
            if before != after:
                result["diff"] = _first_mismatch(before, after)
                result["before"] = before
                result["after"] = after
            result_path.write_text(json.dumps(result, indent=2, sort_keys=True), encoding="utf-8")
            return 0 if result["ok"] else 1
        except BaseException:
            result_path.write_text(
                json.dumps({"ok": False, "error": traceback.format_exc()}, indent=2),
                encoding="utf-8",
            )
            return 1
        finally:
            try:
                free_model_3di3(model)
            finally:
                reset_scene()


    def _scene_signature(rt):
        return {
            "objects": [_object_signature(rt, obj) for obj in _objects(rt)],
            "selection": sorted(_name(obj) for obj in _selection(rt)),
            "command_panel_mode": _command_panel_mode(rt),
        }


    def _object_signature(rt, obj):
        return {
            "name": _name(obj),
            "parent": _name(getattr(obj, "parent", None)),
            "class": _class_name(rt, obj),
            "transform": _transform_signature(obj),
            "modifiers": _modifiers_signature(rt, obj),
            "material": _material_signature(rt, getattr(obj, "material", None)),
            "hidden": bool(getattr(obj, "isHidden", False)),
        }


    def _objects(rt):
        try:
            return sorted(list(rt.objects), key=_name)
        except Exception:
            return []


    def _selection(rt):
        try:
            return list(rt.selection)
        except Exception:
            return []


    def _command_panel_mode(rt):
        try:
            return str(rt.getCommandPanelTaskMode())
        except Exception:
            pass
        try:
            return str(rt.execute("getCommandPanelTaskMode()"))
        except Exception:
            return ""


    def _transform_signature(obj):
        try:
            tm = obj.transform
        except Exception:
            return ()
        return (
            _point_signature(getattr(tm, "row1", None)),
            _point_signature(getattr(tm, "row2", None)),
            _point_signature(getattr(tm, "row3", None)),
            _point_signature(getattr(tm, "position", None)),
        )


    def _point_signature(value):
        if value is None:
            return ()
        try:
            return (round(float(value.x), 5), round(float(value.y), 5), round(float(value.z), 5))
        except Exception:
            try:
                return (round(float(value[0]), 5), round(float(value[1]), 5), round(float(value[2]), 5))
            except Exception:
                return ()


    def _modifiers_signature(rt, obj):
        try:
            modifiers = list(getattr(obj, "modifiers", []))
        except Exception:
            return ()
        return tuple(_class_name(rt, mod) for mod in modifiers)


    def _material_signature(rt, material):
        if material is None:
            return None
        try:
            count = int(getattr(material, "numsubs"))
        except Exception:
            count = 0
        child_names = []
        material_list = getattr(material, "materialList", None)
        for index in range(1, count + 1):
            child = _seq_get(material_list, index)
            child_names.append(_name(child))
        return {
            "name": _name(material),
            "class": _class_name(rt, material),
            "children": tuple(child_names),
        }


    def _seq_get(seq, one_based_index):
        if seq is None:
            return None
        for index in (one_based_index, one_based_index - 1):
            try:
                return seq[index]
            except Exception:
                pass
        return None


    def _class_name(rt, obj):
        try:
            return str(rt.classOf(obj))
        except Exception:
            return obj.__class__.__name__


    def _name(obj):
        if obj is None:
            return ""
        try:
            return str(getattr(obj, "name", "") or obj)
        except Exception:
            return repr(obj)


    def _first_mismatch(left, right, path="signature"):
        if type(left) is not type(right):
            return "%s: type differs %s != %s" % (path, type(left).__name__, type(right).__name__)
        if isinstance(left, dict):
            if left.keys() != right.keys():
                return "%s: keys differ %r != %r" % (path, sorted(left.keys()), sorted(right.keys()))
            for key in left:
                if left[key] != right[key]:
                    return _first_mismatch(left[key], right[key], "%s.%s" % (path, key))
            return "%s: dictionaries differ" % path
        if isinstance(left, (list, tuple)):
            if len(left) != len(right):
                return "%s: length differs %s != %s" % (path, len(left), len(right))
            for index, (left_item, right_item) in enumerate(zip(left, right)):
                if left_item != right_item:
                    return _first_mismatch(left_item, right_item, "%s[%s]" % (path, index))
            return "%s: sequences differ" % path
        return "%s: %r != %r" % (path, left, right)


    if __name__ == "__main__":
        raise SystemExit(main())
    '''
).strip()
