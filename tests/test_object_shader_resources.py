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


def expected_generated_shader_paths(manifest: dict) -> set[Path]:
    return {
        SHADER_DIR / technique["directory"] / f"{policy}{suffix}.gdshader"
        for technique in manifest["techniques"]
        for policy in technique["policies"]
        for suffix in ("", "_double_sided")
    }


def expected_auxiliary_shader_paths(manifest: dict) -> set[Path]:
    return {
        SHADER_DIR / resource["path"]
        for resource in manifest["auxiliary_resources"]
    }


def expected_shader_paths(manifest: dict) -> set[Path]:
    return expected_generated_shader_paths(manifest) | expected_auxiliary_shader_paths(
        manifest
    )


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
    generated = expected_generated_shader_paths(manifest)
    auxiliary = expected_auxiliary_shader_paths(manifest)
    expected = expected_shader_paths(manifest)
    actual = set(SHADER_DIR.rglob("*.gdshader"))
    assert actual == expected
    assert generated.isdisjoint(auxiliary)
    assert len(generated) == manifest["resource_count"] == 128
    assert len(auxiliary) == manifest["auxiliary_resource_count"] == 4
    assert len(actual) == manifest["total_resource_count"] == 132
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


def test_auxiliary_shader_resources_match_their_manifest_contracts() -> None:
    manifest = load_manifest()
    resources = {resource["path"]: resource for resource in manifest["auxiliary_resources"]}
    assert resources == {
        "postmultiply/environment_textured.gdshader": {
            "path": "postmultiply/environment_textured.gdshader",
            "pass_class": "TECHNIQUE_NORMAL_P3",
            "contract": "diffuse1_gamma_postmultiply",
            "coverage": "full",
            "cull": "back",
        },
        "postmultiply/environment_textured_double_sided.gdshader": {
            "path": "postmultiply/environment_textured_double_sided.gdshader",
            "pass_class": "TECHNIQUE_NORMAL_P3",
            "contract": "diffuse1_gamma_postmultiply",
            "coverage": "full",
            "cull": "disabled",
        },
        "postmultiply/environment_textured_cutout.gdshader": {
            "path": "postmultiply/environment_textured_cutout.gdshader",
            "pass_class": "TECHNIQUE_NORMAL_P3",
            "contract": "diffuse1_gamma_postmultiply",
            "coverage": "diffuse_alpha",
            "cull": "back",
        },
        "postmultiply/environment_textured_cutout_double_sided.gdshader": {
            "path": "postmultiply/environment_textured_cutout_double_sided.gdshader",
            "pass_class": "TECHNIQUE_NORMAL_P3",
            "contract": "diffuse1_gamma_postmultiply",
            "coverage": "diffuse_alpha",
            "cull": "disabled",
        },
    }
    for relative_path, resource in resources.items():
        source = (SHADER_DIR / relative_path).read_text(encoding="utf-8")
        expected_cull = "cull_back" if resource["cull"] == "back" else "cull_disabled"
        assert "unshaded" in source
        assert "depth_draw_never" in source
        assert expected_cull in source
        assert '#include "res://shaders/object/postmultiply/environment_textured_body.gdshaderinc"' in source
        if resource["coverage"] == "diffuse_alpha":
            assert "#define OBJ_POSTMULTIPLY_CUTOUT" in source

    assert not (SHADER_DIR / "glow_proxy").exists()
    glow = (SHADER_DIR / "glow.gdshaderinc").read_text(encoding="utf-8")
    output = (SHADER_DIR / "output_opaque.gdshaderinc").read_text(encoding="utf-8")
    for token in (
        "obj_glass_glow(CAMERA_POSITION_WORLD)",
        "obj_apply_additive_fog",
        "OBJ_GLOW_ROTATED_SPECULAR",
        "OBJ_GLOW_NORMAL_COPY",
        "OBJ_GLOW_NO_PASS",
        "nova_is_q3_pass",
    ):
        assert token in glow + output
    assert "u_glow_hdr_scale" not in glow + output

    postmultiply_body = (
        SHADER_DIR / "postmultiply" / "environment_textured_body.gdshaderinc"
    ).read_text(encoding="utf-8")
    for token in (
        "texture(u_diffuse, v_raw_uv)",
        "textureLod(u_post_screen, SCREEN_UV, 0.0)",
        "2.0 * source_gamma * destination_gamma",
        "u_alpha_test_invert",
    ):
        assert token in postmultiply_body


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
                assert (
                    f'#define OBJ_RGB_MOD_{technique["rgb_modulation"].upper()}'
                    in source
                )
                assert (
                    f'#define OBJ_ALPHA_MOD_{technique["alpha_modulation"].upper()}'
                    in source
                )
                assert (
                    f'#define OBJ_COVERAGE_{technique["coverage_source"].upper()}'
                    in source
                )
                assert (
                    f'#define OBJ_CLIP_{technique["clip_class"].upper()}'
                    in source
                )
                assert (
                    f'#define OBJ_VERTEX_POINT_LIGHTS_'
                    f'{technique["vertex_point_lights"].upper()}'
                    in source
                )
                assert (
                    f'#define OBJ_PROJSHAD_'
                    f'{manifest["projected_shadow_contracts"][technique["engine_enum"]].upper()}'
                    in source
                )
                assert (
                    f'#define OBJ_MATCHTERRAIN_'
                    f'{manifest["match_terrain_contracts"][technique["engine_enum"]].upper()}'
                    in source
                )
                assert (
                    f'#define OBJ_GLOW_'
                    f'{manifest["glow_contracts"][technique["engine_enum"]].upper()}'
                    in source
                )
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
                assert '#include "res://shaders/object/match_terrain.gdshaderinc"' in source
                assert '#include "res://shaders/object/glow.gdshaderinc"' in source
                expected_fog = technique.get("fog", policy["fog"])
                assert (
                    f'#include "res://shaders/object/fog/'
                    f'{expected_fog}.gdshaderinc"'
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

                expected_depth = (
                    "depth_draw_opaque"
                    if policy["depth"] == "opaque"
                    else "depth_draw_never"
                )
                assert expected_depth in source
                assert "depth_prepass_alpha" not in source


FUNCTION_HEAD_RE = re.compile(r"^\s*(?:void|bool|int|float|vec[234]|mat[34])\s+(\w+)\s*\(", re.MULTILINE)
IDENTIFIER_RE = re.compile(r"\b[A-Za-z_]\w*\b")
VERTEX_POINT_LIGHT_VARYINGS = {
    "v_point_light_diffuse": "ffp_diffuse",
    "v_pixel_point_factor": "self_shadowed_attenuation",
    "v_pixel_point_attenuation": "attenuation",
}


def glsl_function_bodies(source: str) -> dict[str, str]:
    """Map every top-level function name to its brace-delimited body text."""
    source = re.sub(r"//[^\n]*", "", source)
    bodies: dict[str, str] = {}
    for match in FUNCTION_HEAD_RE.finditer(source):
        open_brace = source.find("{", match.end())
        if open_brace < 0:
            continue
        depth = 0
        for index in range(open_brace, len(source)):
            if source[index] == "{":
                depth += 1
            elif source[index] == "}":
                depth -= 1
                if depth == 0:
                    bodies[match.group(1)] = source[open_brace : index + 1]
                    break
    return bodies


def varyings_reachable_from(bodies: dict[str, str], root: str, varyings: set[str]) -> set[str]:
    reached: set[str] = set()
    pending = [root]
    visited: set[str] = set()
    while pending:
        name = pending.pop()
        if name in visited or name not in bodies:
            continue
        visited.add(name)
        identifiers = set(IDENTIFIER_RE.findall(bodies[name]))
        reached |= identifiers & varyings
        pending.extend(identifier for identifier in identifiers if identifier in bodies)
    return reached


def test_vertex_point_light_products_match_technique_consumption() -> None:
    """The vertex stage evaluates only the point-light product its technique reads.

    Every retail vertex program publishes one EffectWorld point-light product
    (D3D vertex diffuse, oD0 attenuation x self-shadow, or attenuation alone).
    The manifest names it per technique so vertex_standard skips the other
    <=4-light loops; this proves the named product is exactly the varying the
    technique's fragment math can observe, so gating never zeroes a live term.
    """
    manifest = load_manifest()
    shared = normalized_source(SHADER_DIR / "shared.gdshaderinc")
    vertex_standard = normalized_source(SHADER_DIR / "vertex_standard.gdshaderinc")
    vertex_flag = normalized_source(SHADER_DIR / "vertex_flag.gdshaderinc")

    for varying, product in VERTEX_POINT_LIGHT_VARYINGS.items():
        define = f"#ifdef OBJ_VERTEX_POINT_LIGHTS_{product.upper()}"
        assert define in vertex_standard, f"vertex_standard must gate {varying} on {define}"
        gated = vertex_standard.split(define, 1)[1].split("#endif", 1)[0]
        assert f"{varying} = obj_" in gated.split("#else", 1)[0]
        zero = "vec3(0.0)" if varying == "v_point_light_diffuse" else "vec4(0.0)"
        assert f"{varying} = {zero};" in gated.split("#else", 1)[1]
        assert f"{varying} = {zero};" in vertex_flag

    for technique in manifest["techniques"]:
        product = technique["vertex_point_lights"]
        assert product in {*VERTEX_POINT_LIGHT_VARYINGS.values(), "none"}
        if technique["vertex"] == "flag":
            assert product == "none", "vertex_flag publishes no point-light product"
            continue
        implementation = normalized_source(
            SHADER_DIR / "technique" / f"{technique['implementation']}.gdshaderinc"
        )
        bodies = glsl_function_bodies(shared + "\n" + implementation)
        assert "obj_evaluate_surface" in bodies
        consumed = varyings_reachable_from(
            bodies, "obj_evaluate_surface", set(VERTEX_POINT_LIGHT_VARYINGS)
        )
        expected = {VERTEX_POINT_LIGHT_VARYINGS[varying] for varying in consumed} or {"none"}
        assert expected == {product}, (
            f"{technique['engine_enum']} declares vertex_point_lights={product!r} "
            f"but its technique math reads {sorted(consumed)}"
        )


def test_slot_capture_camera_signature_pins_the_water_layer_table() -> None:
    """PROJSHAD capture is a camera-mask signature, not twelve eye distances.

    SlotShadow gives each capture camera one reserved visual layer; the object
    shaders recognise a camera that culls to those layers alone. Pin the shader
    constant to the Water layer table and keep the signature disjoint from the
    beauty/Q3 device masks so a layer reallocation cannot silently re-route the
    NORMAL pass into the silhouette pass.
    """
    water_header = (ROOT / "godot/src/env/nova_water.h").read_text(encoding="utf-8")
    table = re.search(
        r"VISUAL_LAYER_SLOT_CAPTURE_MASK\s*=\s*([^,]+?),\n", water_header, re.DOTALL
    )
    assert table, "Water must publish the slot capture layer table"
    expected_mask = 0
    for bit in re.findall(r"1\s*<<\s*(\d+)", table.group(1)):
        expected_mask |= 1 << int(bit)
    assert expected_mask == 0x000E03FE

    shared = normalized_source(SHADER_DIR / "shared.gdshaderinc")
    declared = re.search(
        r"const uint NOVA_SLOT_CAPTURE_LAYER_MASK = (\d+)u;", shared
    )
    assert declared, "shared.gdshaderinc must declare the capture signature"
    assert int(declared.group(1)) == expected_mask
    assert "bool obj_is_slot_shadow_capture(uint camera_visible_layers," in shared
    assert "~NOVA_SLOT_CAPTURE_LAYER_MASK" in shared

    retail_pass = normalized_source(ROOT / "godot/shaders/nova_frame_pass.gdshaderinc")
    for name in ("NOVA_BEAUTY_CAMERA_MASK", "NOVA_Q3_CAMERA_MASK"):
        device_mask = re.search(rf"const uint {name} = (\d+)u;", retail_pass)
        assert device_mask, name
        assert int(device_mask.group(1)) & expected_mask == 0

    slot_shadow = (ROOT / "godot/src/env/nova_slot_shadow.cpp").read_text(encoding="utf-8")
    assert "camera->set_cull_mask(kCaptureLayerBits[i]);" in slot_shadow
    assert "viewport->set_transparent_background(true);" in slot_shadow
    assert "capture_environment->set_bg_color(Color(1.0f, 1.0f, 1.0f));" in slot_shadow
    assert "viewport->set_msaa_3d(Viewport::MSAA_4X);" in slot_shadow

    for output in ("output_opaque.gdshaderinc", "output_alpha.gdshaderinc"):
        source = normalized_source(SHADER_DIR / output)
        assert "obj_is_slot_shadow_capture(CAMERA_VISIBLE_LAYERS," in source
    for path in SHADER_DIR.rglob("*.gdshaderinc"):
        assert "opennova_slot_shadow_capture" not in normalized_source(path), (
            f"{path.name} reintroduced the per-fragment capture-eye gate"
        )


def test_projshadow_capture_is_retail_black_over_the_white_clear() -> None:
    """The live slot RT stores the fixed-function black silhouette itself."""
    for output in ("output_opaque.gdshaderinc", "output_alpha.gdshaderinc"):
        source = normalized_source(SHADER_DIR / output)
        capture = source.split(
            "} else if (obj_is_slot_shadow_capture(CAMERA_VISIBLE_LAYERS,", 1
        )[1].split("\n\t} else {", 1)[0]
        assert "obj_apply_coverage(coverage);" in capture
        assert "ALBEDO = vec3(0.0);" in capture
        assert "obj_evaluate_surface" not in capture
        assert "capture_lit" not in capture


def test_wrapper_generator_is_idempotent_and_manifest_owned() -> None:
    generator = (ROOT / "scripts" / "generate_object_shaders.py").read_text()
    assert "pipeline_manifest.json" in generator
    assert "manifest[\"resource_count\"]" in generator


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
