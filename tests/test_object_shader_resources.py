import hashlib
import json
import os
import re
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
SHADER_DIR = ROOT / "godot" / "shaders" / "object"
MANIFEST_PATH = SHADER_DIR / "pipeline_manifest.json"
HASH_GOLDEN_PATH = ROOT / "tests" / "object_shader_resource_hashes.golden.json"
INCLUDE_RE = re.compile(r'^\s*#include\s+"res://([^\"]+)"', re.MULTILINE)


def load_manifest() -> dict:
    return json.loads(MANIFEST_PATH.read_text())


def expected_shader_paths(manifest: dict) -> set[Path]:
    return {
        SHADER_DIR / technique["directory"] / f"{policy}{suffix}.gdshader"
        for technique in manifest["techniques"]
        for policy in technique["policies"]
        for suffix in ("", "_double_sided")
    }


def normalized_source(path: Path) -> str:
    return path.read_text(encoding="utf-8").replace("\r\n", "\n")


def transitive_sources(shader_path: Path) -> list[tuple[Path, str]]:
    """Return one wrapper and every recursively included source, in include order."""
    pending = [shader_path]
    seen: set[Path] = set()
    closure: list[tuple[Path, str]] = []
    while pending:
        path = pending.pop(0).resolve()
        if path in seen:
            continue
        seen.add(path)
        source = normalized_source(path)
        relative = path.relative_to(ROOT).as_posix()
        closure.append((path, source))
        includes = [
            (ROOT / "godot" / match.group(1)).resolve()
            for match in INCLUDE_RE.finditer(source)
        ]
        assert all(path.exists() for path in includes), f"missing include from {relative}"
        pending[0:0] = includes
    return closure


def transitive_source_hash(shader_path: Path) -> str:
    """Hash one wrapper and every recursively included source, in include order."""
    payload = "\n".join(
        f"@@ {path.relative_to(ROOT).as_posix()}\n{source}"
        for path, source in transitive_sources(shader_path)
    ).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def test_object_shader_runtime_does_not_compose_source_strings() -> None:
    engine_header = (ROOT / "engine/runtime/renderer/object_shader_template.h").read_text()
    engine_source = (ROOT / "engine/runtime/renderer/object_shader_template.cpp").read_text()
    godot_cache = (ROOT / "godot/src/object/nova_object_shader_cache.cpp").read_text()

    assert "compose_object_shader_glsl" not in engine_header + engine_source + godot_cache
    assert "set_code(" not in godot_cache


def test_object_shader_topology_is_compile_time_not_uniform_driven() -> None:
    implementation = "\n".join(
        path.read_text() for path in SHADER_DIR.rglob("*.gdshaderinc")
    )
    assert "u_cap_" not in implementation
    assert "u_object_family" not in implementation
    assert "u_environment_source" not in implementation
    assert "u_specular_source" not in implementation

    runtime_bindings = "\n".join(
        path.read_text()
        for path in (
            ROOT / "godot/src/object/nova_object_shader_cache.cpp",
            ROOT / "godot/src/object/nova_object_model_materials.cpp",
            ROOT / "godot/tests/render_swatch_probe.gd",
        )
    )
    for removed_uniform in (
        "u_cap_",
        "u_object_family",
        "u_environment_source",
        "u_specular_source",
        "u_emissive",
    ):
        assert removed_uniform not in runtime_bindings

    assert not (SHADER_DIR / "object_core.gdshaderinc").exists()
    assert not any((SHADER_DIR / "family").glob("*.gdshaderinc"))


def test_manifest_covers_every_resource_and_selector_technique() -> None:
    manifest = load_manifest()
    expected = expected_shader_paths(manifest)
    actual = set(SHADER_DIR.glob("*/*.gdshader"))
    assert actual == expected
    assert len(actual) == manifest["resource_count"] == 104
    assert not list(SHADER_DIR.glob("*.gdshader")), "no interim root-level wrappers"

    reachable_includes = {
        path
        for shader_path in expected
        for path, _source in transitive_sources(shader_path)
        if path.suffix == ".gdshaderinc" and path.is_relative_to(SHADER_DIR)
    }
    actual_includes = {path.resolve() for path in SHADER_DIR.rglob("*.gdshaderinc")}
    assert actual_includes == reachable_includes, "no orphan object shader includes"

    expected_uid_targets = {path.resolve() for path in actual | set(actual_includes)}
    actual_uid_targets = {
        Path(str(path.resolve())[:-4]) for path in SHADER_DIR.rglob("*.uid")
    }
    assert actual_uid_targets == expected_uid_targets

    cache_source = (ROOT / "godot/src/object/nova_object_shader_cache.cpp").read_text()
    engine_header = (ROOT / "engine/runtime/renderer/object_shader_template.h").read_text()
    for technique in manifest["techniques"]:
        assert f'ObjectShaderTechnique::{technique["engine_enum"]}' in cache_source
        assert technique["engine_enum"] in engine_header
        assert f'return "{technique["directory"]}"' in cache_source


def test_every_wrapper_matches_manifest_topology() -> None:
    manifest = load_manifest()
    policies = manifest["policies"]
    for technique in manifest["techniques"]:
        for policy_name in technique["policies"]:
            policy = policies[policy_name]
            for suffix in ("", "_double_sided"):
                path = (
                    SHADER_DIR
                    / technique["directory"]
                    / f"{policy_name}{suffix}.gdshader"
                )
                source = path.read_text()
                assert '#include "res://shaders/object/shared.gdshaderinc"' in source
                assert (
                    f'#include "res://shaders/object/sampling/'
                    f'{technique["sampling"]}.gdshaderinc"'
                ) in source
                assert (
                    f'#include "res://shaders/object/coverage/'
                    f'{policy["coverage"]}.gdshaderinc"'
                ) in source
                assert (
                    f'#include "res://shaders/object/normal/'
                    f'{technique["normal"]}.gdshaderinc"'
                ) in source
                assert (
                    f'#include "res://shaders/object/technique/'
                    f'{technique["implementation"]}.gdshaderinc"'
                ) in source
                expected_vertex = (
                    "vertex_flag" if technique["vertex"] == "flag" else "vertex_standard"
                )
                assert f'/object/{expected_vertex}.gdshaderinc"' in source
                expected_output = "output_alpha" if policy["writes_alpha"] else "output_opaque"
                assert f'/object/{expected_output}.gdshaderinc"' in source


def test_transitive_shader_sources_match_golden() -> None:
    """Keep the old composed-source regression sensitivity after static splitting.

    Deliberate witnessed shader changes regenerate this file with
    OPENNOVA_OBJECT_SHADER_HASHES_DUMP=1 and must review the resulting diff.
    Dump mode always fails, so it cannot be mistaken for a green validation.
    """
    manifest = load_manifest()
    hashes = {
        path.relative_to(ROOT).as_posix(): transitive_source_hash(path)
        for path in sorted(expected_shader_paths(manifest))
    }
    payload = {
        "schema": 1,
        "algorithm": "sha256-normalized-transitive-include-closure-v1",
        "resource_count": len(hashes),
        "resources": hashes,
    }

    if os.environ.get("OPENNOVA_OBJECT_SHADER_HASHES_DUMP"):
        HASH_GOLDEN_PATH.write_text(
            json.dumps(payload, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        pytest.fail(
            f"shader hash golden rewritten at {HASH_GOLDEN_PATH}; "
            "dump mode is deliberately red"
        )

    assert json.loads(HASH_GOLDEN_PATH.read_text(encoding="utf-8")) == payload
