import fnmatch
import functools
import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
GODOT_ROOT = ROOT / "godot"
SHADER_DIR = GODOT_ROOT / "shaders"
PROVENANCE_PATH = SHADER_DIR / "provenance.json"
TECHNIQUE_VALIDATION_PATH = SHADER_DIR / "object" / "technique_validation.json"
AUXILIARY_TECHNIQUE_VALIDATION_PATH = (
    SHADER_DIR / "object" / "auxiliary_technique_validation.json"
)
RETAIL_EFFECT_INVENTORY_PATH = (
    SHADER_DIR / "object" / "retail_effect_inventory.json"
)
RETAIL_EFFECT_DIR = ROOT / "third_party" / "modsuperoed"
INCLUDE_RE = re.compile(r'^\s*#include\s+"res://([^\"]+)"', re.MULTILINE)
ADDRESS_RE = re.compile(r"^0x[0-9a-f]+$", re.IGNORECASE)
TECHNIQUE_RE = re.compile(
    r"\btechnique\s+(?P<name>[A-Za-z_]\w*)\s*"
    r"(?:<(?P<annotations>.*?)>)?\s*\{",
    re.DOTALL,
)
TECHNIQUE_TYPE_RE = re.compile(r"\bttype\s*=\s*(TECHNIQUE_[A-Z]+)")
PASS_RE = re.compile(r"(?m)^\s*pass\s+[A-Za-z_]\w*")


def shader_sources() -> set[Path]:
    return {
        path.resolve()
        for suffix in ("*.gdshader", "*.gdshaderinc")
        for path in SHADER_DIR.rglob(suffix)
    }


def relative(path: Path) -> str:
    return path.resolve().relative_to(SHADER_DIR.resolve()).as_posix()


def load_provenance() -> dict:
    return json.loads(PROVENANCE_PATH.read_text(encoding="utf-8"))


def rol32(value: int, shift: int) -> int:
    return ((value << shift) | (value >> (32 - shift))) & 0xFFFFFFFF


@functools.lru_cache
def decode_retail_scr(path: Path) -> str:
    """Decode the shipped SCR-wrapped .fx evidence without exporting it."""
    raw = path.read_bytes()
    assert raw[:3] == b"SCR", f"retail evidence is not SCR encoded: {path}"
    data = bytearray(raw[4:])
    data.reverse()
    key = 0xA55B1EED
    for index in range(len(data)):
        key = rol32((key + rol32(key, 11)) & 0xFFFFFFFF, 4) ^ 1
        data[index] ^= key & 0xFF
    return data.decode("latin-1")


def retail_effect_paths() -> list[Path]:
    """Return every .fx source on case-sensitive and case-insensitive platforms."""
    return sorted(
        path
        for path in RETAIL_EFFECT_DIR.rglob("*")
        if path.is_file() and path.suffix.lower() == ".fx"
    )


def retail_technique_body(source: str, declaration: re.Match[str]) -> str:
    """Extract one effect technique body, including all nested pass blocks."""
    start = declaration.end() - 1
    depth = 0
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"unterminated technique {declaration.group('name')}")


def matching_contracts(path: Path, manifest: dict) -> list[dict]:
    rel = relative(path)
    return [
        contract
        for contract in manifest["contracts"]
        if any(fnmatch.fnmatchcase(rel, pattern) for pattern in contract["patterns"])
    ]


def direct_includes(path: Path) -> list[Path]:
    source = path.read_text(encoding="utf-8")
    return [(GODOT_ROOT / match.group(1)).resolve() for match in INCLUDE_RE.finditer(source)]


def include_closure(wrapper: Path) -> set[Path]:
    visited: set[Path] = set()
    active: list[Path] = []

    def visit(path: Path) -> None:
        assert path not in active, (
            "shader include cycle: "
            + " -> ".join(relative(item) for item in active + [path])
        )
        if path in visited:
            return
        owner = relative(active[-1]) if active else relative(path)
        assert path.exists(), f"missing shader include referenced by {owner}"
        assert path.is_relative_to(SHADER_DIR.resolve()), (
            f"shader include leaves res://shaders: {path}"
        )
        active.append(path)
        for included in direct_includes(path):
            visit(included)
        active.pop()
        visited.add(path)

    visit(wrapper.resolve())
    return visited


def test_provenance_contract_covers_every_shader_resource_once() -> None:
    manifest = load_provenance()
    sources = shader_sources()
    assert manifest["schema"] == 2
    assert len(sources) == manifest["resource_count"] == 195
    assert len({contract["id"] for contract in manifest["contracts"]}) == len(
        manifest["contracts"]
    )
    for source in sorted(sources):
        contracts = matching_contracts(source, manifest)
        assert len(contracts) == 1, (
            f"{relative(source)} must have exactly one provenance contract; "
            f"got {[contract['id'] for contract in contracts]}"
        )


def test_validation_scope_is_the_locked_highest_quality_retail_profile() -> None:
    provenance = load_provenance()
    active_exceptions = {
        exception
        for contract in provenance["contracts"]
        for exception in contract["exceptions"]
    }
    assert "D-RMAT-8" not in active_exceptions
    assert "D-RORD-5" not in active_exceptions
    assert "D-RLIT-4" not in active_exceptions
    assert "D-RLIT-6" not in active_exceptions
    scope = provenance["quality_scope"]
    assert scope["profile"] == "highest-quality-retail-path"
    assert scope["shader_usage_level"] == 2
    assert scope["screenshots_allowed_only_after_validation"] is True
    assert scope["fixture_catalog"] == "docs/render/render-fixtures-retail-v5.json"
    assert (
        ROOT / scope["object_pass_class_validation"]
    ).resolve() == AUXILIARY_TECHNIQUE_VALIDATION_PATH.resolve()

    fixture_path = ROOT / scope["fixture_catalog"]
    fixture = json.loads(fixture_path.read_text(encoding="utf-8"))
    retail_profile = fixture["retail_video_profile_contract"]
    assert retail_profile["id"] == scope["fixture_contract"]
    assert retail_profile["required_values"]["shader_usage_level"] == 2

    policy_path = ROOT / scope["locked_menu_policy"]
    policy = policy_path.read_text(encoding="utf-8")
    for token in (
        'QualityControl.new(&"SHADERUSAGE", "2")',
        'QualityControl.new(&"TERRAINPOLY", "3")',
        'QualityControl.new(&"OBJECTTEX", "3")',
        'QualityControl.new(&"SHADOWQUALITY", "3")',
        "driver.set_widget_disabled(id, true)",
    ):
        assert token in policy


def test_environment_cube_capture_is_live_and_highest_quality() -> None:
    shared = (SHADER_DIR / "object" / "shared.gdshaderinc").read_text(
        encoding="utf-8"
    )
    capture_header = (
        ROOT / "godot/src/env/nova_environment_cube_capture.h"
    ).read_text(encoding="utf-8")
    capture_source = (
        ROOT / "godot/src/env/nova_environment_cube_capture.cpp"
    ).read_text(encoding="utf-8")
    water = (SHADER_DIR / "water.gdshader").read_text(encoding="utf-8")
    project = (ROOT / "godot/project.godot").read_text(encoding="utf-8")

    for token in (
        "kCaptureSize = 256",
        "kRefreshFrames = 128",
        "kSkyDimByte = 0x60",
    ):
        assert token in capture_header
    for token in (
        "camera->set_fov(90.0f)",
        "camera->set_near(0.5f)",
        "camera->set_far(1000.0f)",
        "terrain_y + 10.0f",
        "capture_origin_.y += 1.0f",
        "Water::VISUAL_LAYER_ENVIRONMENT_CAPTURE",
        "camera->set_compositor(capture_compositor)",
    ):
        assert token in capture_source
    assert "image->linear_to_srgb()" not in capture_source
    for token in (
        "global uniform samplerCube opennova_environment_cube",
        "texture(opennova_environment_cube, direction)",
        "pow(aligned, 800.0)",
        "pow(aligned, 40.0)",
        "static_lobe * opennova_sun_light * 2.0",
    ):
        assert token in shared
    # The six capture cameras cull to the aliased water layer ALONE, so water
    # rejects them by exact camera mask (nova_is_q3_pass pattern), never by a
    # stale capture-origin distance.
    assert "NOVA_ENVIRONMENT_CAPTURE_CAMERA_MASK = 1024u" in water
    assert (
        "CAMERA_VISIBLE_LAYERS == NOVA_ENVIRONMENT_CAPTURE_CAMERA_MASK" in water
    )
    assert "opennova_environment_capture_origin" not in water
    assert '"type": "samplerCube"' in project


def test_retail_effect_inventory_covers_every_source_and_technique_declaration() -> None:
    inventory = json.loads(RETAIL_EFFECT_INVENTORY_PATH.read_text(encoding="utf-8"))
    paths = retail_effect_paths()
    relative_paths = [
        path.relative_to(RETAIL_EFFECT_DIR).as_posix() for path in paths
    ]

    assert inventory["schema"] == 1
    assert inventory["source_file_count"] == len(paths) == 44
    assert inventory["source_files"] == relative_paths

    decoded_inventory: dict[str, dict[str, list[str]]] = {}
    decoded_pass_counts: dict[str, int] = {}
    declaration_count = 0
    for path in paths:
        decoded = decode_retail_scr(path)
        assert decoded, f"empty decoded retail effect source: {path.name}"
        relative_path = path.relative_to(RETAIL_EFFECT_DIR).as_posix()
        for declaration in TECHNIQUE_RE.finditer(decoded):
            annotations = declaration.group("annotations") or ""
            technique_type = TECHNIQUE_TYPE_RE.search(annotations)
            class_name = technique_type.group(1) if technique_type else "UNSPECIFIED"
            decoded_inventory.setdefault(class_name, {}).setdefault(
                relative_path, []
            ).append(declaration.group("name"))
            decoded_pass_counts[class_name] = decoded_pass_counts.get(class_name, 0) + len(
                PASS_RE.findall(retail_technique_body(decoded, declaration))
            )
            declaration_count += 1

    audited_inventory = {
        class_name: entry["evidence"]
        for class_name, entry in inventory["classes"].items()
    }
    assert decoded_inventory == audited_inventory
    assert declaration_count == inventory["technique_declaration_count"] == 59
    typed_count = sum(
        entry["declaration_count"]
        for class_name, entry in inventory["classes"].items()
        if class_name != "UNSPECIFIED"
    )
    assert typed_count == inventory["typed_technique_declaration_count"] == 57
    assert sum(decoded_pass_counts.values()) == inventory["pass_declaration_count"] == 138
    assert set(inventory["classes"]) == {
        "TECHNIQUE_NORMAL",
        "TECHNIQUE_PROJSHAD",
        "TECHNIQUE_DEPTHMASK",
        "TECHNIQUE_CLIP",
        "TECHNIQUE_GLOW",
        "TECHNIQUE_MATCHTERRAIN",
        "UNSPECIFIED",
    }
    for class_name, entry in inventory["classes"].items():
        assert entry["declaration_count"] == sum(
            len(names) for names in entry["evidence"].values()
        )
        assert entry["pass_declaration_count"] == decoded_pass_counts[class_name]
        assert entry["disposition"]
        assert "deferred" not in entry["disposition"]

    normal = inventory["classes"]["TECHNIQUE_NORMAL"]
    decoded_normal_declarations = {
        (file_name, ordinal, name)
        for file_name, names in normal["evidence"].items()
        for ordinal, name in enumerate(names)
    }
    selected_normal_declarations = {
        (entry["file"], entry["ordinal"], entry["name"])
        for entry in normal["highest_quality_selected"]
    }
    excluded_normal_declarations = {
        (entry["file"], entry["ordinal"], entry["name"])
        for entry in normal["excluded_declarations"]
    }
    assert len(selected_normal_declarations) == 19
    assert len(excluded_normal_declarations) == 11
    assert selected_normal_declarations.isdisjoint(excluded_normal_declarations)
    assert (
        selected_normal_declarations | excluded_normal_declarations
        == decoded_normal_declarations
    )
    assert any(
        entry["file"] == "leaves.FX" and "not_registered" in entry["reason"]
        for entry in normal["excluded_declarations"]
    )

    pipeline = json.loads(
        (SHADER_DIR / "object" / "pipeline_manifest.json").read_text(
            encoding="utf-8"
        )
    )
    projected_runtime_techniques = {
        technique
        for entry in normal["highest_quality_selected"]
        for technique in entry["runtime_techniques"]
    } | {
        entry["runtime_technique"]
        for entry in normal["external_runtime_techniques"]
    }
    assert projected_runtime_techniques == {
        entry["engine_enum"] for entry in pipeline["techniques"]
    }


def test_projected_shadow_state_contracts_match_decoded_retail_passes() -> None:
    pipeline = json.loads(
        (SHADER_DIR / "object" / "pipeline_manifest.json").read_text(
            encoding="utf-8"
        )
    )
    techniques = {entry["engine_enum"] for entry in pipeline["techniques"]}
    states = pipeline["projected_shadow_state_contracts"]
    assert states.keys() == techniques
    assert {name for name, state in states.items() if state == "material_blend"} == {
        "Fixed",
        "FixedDetail",
        "SelfLit",
        "SelfLitDetail",
    }
    assert {name for name, state in states.items() if state == "no_pass"} == {
        "Tracer",
        "Flag",
        "GlassFixed",
        "GlassSkinned",
    }
    assert sum(state == "opaque" for state in states.values()) == 16

    declarations: list[tuple[str, str]] = []
    decoded_by_name = {
        path.name: decode_retail_scr(path) for path in retail_effect_paths()
    }
    for file_name, decoded in decoded_by_name.items():
        for declaration in TECHNIQUE_RE.finditer(decoded):
            if "TECHNIQUE_PROJSHAD" in (declaration.group("annotations") or ""):
                declarations.append(
                    (file_name, retail_technique_body(decoded, declaration))
                )

    assert len(declarations) == 16
    ffp_bodies = [body for file_name, body in declarations if file_name == "_FFP.fx"]
    assert len(ffp_bodies) == 1
    ffp = ffp_bodies[0]
    for token in (
        "RSAlphaMode(FALSE, ONE, ZERO)",
        "RSAlphaMode(TRUE, SRCALPHA, INVSRCALPHA)",
        "RSAlphaMode(TRUE, ONE, ONE)",
        "RSAlphaMode(TRUE, DESTCOLOR, SRCCOLOR)",
    ):
        assert ffp.count(token) == 1

    opaque_files = {
        "BDiffT2.fx",
        "BmTxMirrT.fx",
        "BumpMirrT.fx",
        "Dot3DiffO.fx",
        "Dot3DiffT.fx",
        "EnvPhongT.fx",
        "PhongO.fx",
        "PhongT.fx",
        "SkBasic.fx",
        "SkBDiffO.fx",
        "SkBDiffO2.fx",
        "SkBDiffT.fx",
        "SkBDiffT2.fx",
        "SkBPhongO.fx",
        "SkBPhongT.fx",
    }
    opaque_bodies = [
        (file_name, body)
        for file_name, body in declarations
        if file_name != "_FFP.fx"
    ]
    assert {file_name for file_name, _ in opaque_bodies} == opaque_files
    for file_name, body in opaque_bodies:
        assert body.count("RSAlphaMode(FALSE, ONE, ZERO)") == 1, file_name
        assert "RSAlphaMode(TRUE" not in body, file_name
        assert "ZMODE_NOWRITE" not in body, file_name

    for file_name in ("Flag.fx", "Glass.fx", "SkGlass.fx"):
        assert not any(
            "TECHNIQUE_PROJSHAD" in (declaration.group("annotations") or "")
            for declaration in TECHNIQUE_RE.finditer(decoded_by_name[file_name])
        ), file_name
    material_re = (ROOT / "docs/render/render-material-re.md").read_text(
        encoding="utf-8"
    )
    assert "Tracer.fx exposes only TECHNIQUE_NORMAL" in material_re

    # The binary collector fills every selected ROBJ/bone matrix slot with one
    # entity transform. These source pins prove why the skinned projected path
    # then reduces to rigid geometry: all explicit weights plus the computed
    # final weight sum to exactly one over identical matrices.
    skinned_post = decoded_by_name["_vsSkPost.fx"]
    base = decoded_by_name["_BaseInc.fx"]
    for token in (
        "vsSkinPostBlackT1",
        "CalcSkinWorldPosAndNormal (In, pos, norm, indexvector, NumBones, false)",
        "Out.Pos = mul(float4(pos.xyz, 1.0f), MatViewProj)",
    ):
        assert token in skinned_post
    for token in (
        "lastweight = 1.0f - lastweight",
        "pos  += mul(In.Pos, SkinWorldMatrixArray[IndexArray[bone]]) * BlendWeightsArray[bone]",
        "pos  += (mul(In.Pos, SkinWorldMatrixArray[IndexArray[NumBones-1]]) * lastweight)",
    ):
        assert token in base


def test_every_reachable_object_technique_has_decoded_retail_evidence() -> None:
    provenance = load_provenance()
    validation_path = ROOT / provenance["quality_scope"]["object_technique_validation"]
    validation = json.loads(validation_path.read_text(encoding="utf-8"))
    pipeline = json.loads(
        (SHADER_DIR / "object" / "pipeline_manifest.json").read_text(encoding="utf-8")
    )

    assert validation["schema"] == 2
    assert validation["quality_scope"]["shader_usage_level"] == 2
    assert validation["quality_scope"]["retail_pass_class"] == "TECHNIQUE_NORMAL"
    assert (
        ROOT / validation["quality_scope"]["retail_effect_inventory"]
    ).resolve() == RETAIL_EFFECT_INVENTORY_PATH.resolve()
    assert set(validation["quality_scope"]["audited_pass_classes"]) == {
        "TECHNIQUE_NORMAL",
        "TECHNIQUE_PROJSHAD",
        "TECHNIQUE_DEPTHMASK",
        "TECHNIQUE_CLIP",
        "TECHNIQUE_GLOW",
        "TECHNIQUE_MATCHTERRAIN",
    }
    assert (
        ROOT / validation["quality_scope"]["pass_class_validation"]
    ).resolve() == AUXILIARY_TECHNIQUE_VALIDATION_PATH.resolve()
    assert re.fullmatch(r"[0-9a-f]{40}", validation["source_corpus"]["commit"])

    audited = {entry["engine_enum"]: entry for entry in validation["techniques"]}
    selected = {entry["engine_enum"]: entry for entry in pipeline["techniques"]}
    assert audited.keys() == selected.keys()
    assert len(audited) == len(validation["techniques"]), "techniques audit once"

    for engine_enum, entry in audited.items():
        selected_entry = selected[engine_enum]
        assert entry["implementation"] == selected_entry["implementation"]
        assert entry["policies"] == selected_entry["policies"]
        assert selected_entry["rgb_modulation"] in {"none", "self_lum"}
        assert selected_entry["alpha_modulation"] in {"none", "ffp"}
        assert selected_entry["coverage_source"] in {
            "diffuse_alpha",
            "normal_alpha",
            "vertex_diffuse_alpha",
            "reflect_alpha",
            "zero",
        }
        assert selected_entry["clip_class"] in {
            "explicit",
            "explicit_unskinned_or_submit_skip_skinned",
            "normal_fallback",
            "skinned_submit_skip",
        }
        for required_field in ("channels", "light_response", "state", "shader_tokens"):
            assert entry[required_field], f"{engine_enum} must explicitly audit {required_field}"
        assert entry["responses"].keys() == {
            "directional",
            "hemisphere",
            "ambient",
            "point",
        }
        assert all(isinstance(value, bool) for value in entry["responses"].values())
        raster_checks = entry.get("raster_checks", {})
        assert set(raster_checks).issubset(
            {"directional_axis_reversal", "hemisphere_axis_reversal"}
        )
        if any(value is False for value in raster_checks.values()):
            assert entry.get("raster_check_note")
        assert entry["parity_status"] in validation["parity_statuses"]
        if entry["parity_status"] != "matching":
            assert entry["exceptions"], f"{engine_enum} must bound every residual"

        evidence = entry["evidence"]
        if evidence["kind"] == "scr_fx":
            assert evidence["sources"]
            for source in evidence["sources"]:
                source_path = RETAIL_EFFECT_DIR / source["file"]
                assert source_path.is_file(), f"missing retail evidence {source_path}"
                decoded = decode_retail_scr(source_path)
                for symbol in source["symbols"]:
                    assert symbol in decoded, (
                        f"{engine_enum}: {symbol!r} absent from decoded {source['file']}"
                    )
        else:
            assert evidence["kind"] == "binary_documented"
            documentation = "\n".join(
                (ROOT / name).read_text(encoding="utf-8") for name in evidence["docs"]
            ).lower()
            for symbol in evidence["symbols"]:
                assert symbol.lower() in documentation

        for citation in evidence.get("binary_citations", []):
            assert ADDRESS_RE.fullmatch(citation["address"])
            assert citation["function"]
            cited_docs = "\n".join(
                (ROOT / name).read_text(encoding="utf-8").lower()
                for name in citation["docs"]
            )
            assert citation["address"].lower() in cited_docs
            assert citation["function"].lower() in cited_docs

        representative = (
            SHADER_DIR
            / "object"
            / selected_entry["directory"]
            / f"{entry['policies'][0]}.gdshader"
        )
        closure = "\n".join(
            path.read_text(encoding="utf-8") for path in include_closure(representative)
        )
        representative_source = representative.read_text(encoding="utf-8")
        for field, prefix in (
            ("rgb_modulation", "OBJ_RGB_MOD"),
            ("alpha_modulation", "OBJ_ALPHA_MOD"),
            ("coverage_source", "OBJ_COVERAGE"),
            ("clip_class", "OBJ_CLIP"),
        ):
            expected_macro = f'#define {prefix}_{selected_entry[field].upper()}'
            assert expected_macro in representative_source, (
                f"{engine_enum}: wrapper lost {field} contract {expected_macro}"
            )
        for token in entry["shader_tokens"]:
            assert token in closure, f"{engine_enum}: shader math lost {token!r}"

        auxiliary_resources = entry.get("auxiliary_resources", [])
        auxiliary_tokens = entry.get("auxiliary_shader_tokens", [])
        if auxiliary_resources or auxiliary_tokens:
            assert auxiliary_resources and auxiliary_tokens
            auxiliary_closure = "\n".join(
                path.read_text(encoding="utf-8")
                for resource in auxiliary_resources
                for path in include_closure(SHADER_DIR / resource)
            )
            for token in auxiliary_tokens:
                assert token in auxiliary_closure, (
                    f"{engine_enum}: auxiliary shader math lost {token!r}"
                )

        implementation = (
            SHADER_DIR
            / "object"
            / "technique"
            / f"{entry['implementation']}.gdshaderinc"
        ).read_text(encoding="utf-8")
        for token in entry.get("forbidden_shader_tokens", []):
            assert token not in implementation, (
                f"{engine_enum}: forbidden topology {token!r} reached its technique"
            )


def test_every_retail_pass_class_has_a_runtime_or_exclusion_disposition() -> None:
    audit = json.loads(
        AUXILIARY_TECHNIQUE_VALIDATION_PATH.read_text(encoding="utf-8")
    )
    inventory = json.loads(RETAIL_EFFECT_INVENTORY_PATH.read_text(encoding="utf-8"))
    pipeline = json.loads(
        (SHADER_DIR / "object" / "pipeline_manifest.json").read_text(
            encoding="utf-8"
        )
    )

    assert audit["schema"] == 1
    assert audit["quality_scope"]["shader_usage_level"] == 2
    assert audit["quality_scope"]["screenshots_allowed_only_after_validation"] is True
    assert re.fullmatch(r"[0-9a-f]{40}", audit["source_corpus"]["commit"])
    classes = audit["pass_classes"]
    assert classes.keys() == inventory["classes"].keys()

    expected_scopes = {
        "TECHNIQUE_NORMAL": "live",
        "TECHNIQUE_PROJSHAD": "live",
        "TECHNIQUE_DEPTHMASK": "unreachable",
        "TECHNIQUE_CLIP": "live",
        "TECHNIQUE_GLOW": "live",
        "TECHNIQUE_MATCHTERRAIN": "live",
        "UNSPECIFIED": "excluded_lower_quality",
    }
    expected_probes = {
        "TECHNIQUE_NORMAL": ["lighting", "channels"],
        "TECHNIQUE_PROJSHAD": ["projshadow"],
        "TECHNIQUE_DEPTHMASK": [],
        "TECHNIQUE_CLIP": ["clip"],
        "TECHNIQUE_GLOW": ["glow"],
        "TECHNIQUE_MATCHTERRAIN": ["matchterrain"],
        "UNSPECIFIED": [],
    }
    probe_source = (GODOT_ROOT / "tests" / "render_swatch_probe.gd").read_text(
        encoding="utf-8"
    )

    for class_name, entry in classes.items():
        inventory_entry = inventory["classes"][class_name]
        assert entry["scope"] == expected_scopes[class_name]
        assert entry["disposition"] == inventory_entry["disposition"]
        assert entry["declaration_count"] == inventory_entry["declaration_count"]
        assert (
            entry["pass_declaration_count"]
            == inventory_entry["pass_declaration_count"]
        )
        assert entry["parity_status"] in {"matching", "matching_with_bounded_residuals"}
        if entry["parity_status"] == "matching_with_bounded_residuals":
            assert entry["bounded_residuals"]
        else:
            assert entry["bounded_residuals"] == []
        assert entry["raster_probes"] == expected_probes[class_name]
        for mode in entry["raster_probes"]:
            assert f'"{mode}"' in probe_source

        assert entry["source_evidence"]
        for evidence in entry["source_evidence"]:
            decoded = decode_retail_scr(RETAIL_EFFECT_DIR / evidence["file"])
            for symbol in evidence["symbols"]:
                assert symbol in decoded, (
                    f"{class_name}: {symbol!r} absent from decoded "
                    f"{evidence['file']}"
                )

        assert entry["runtime_evidence"]
        for evidence in entry["runtime_evidence"]:
            path = ROOT / evidence["path"]
            assert path.is_file(), f"missing runtime evidence {path}"
            source = path.read_text(encoding="utf-8")
            for token in evidence["tokens"]:
                assert token in source, (
                    f"{class_name}: {token!r} absent from {evidence['path']}"
                )

    techniques = {entry["engine_enum"] for entry in pipeline["techniques"]}
    assert len(techniques) == 24
    for contract_name in (
        "projected_shadow_contracts",
        "projected_shadow_state_contracts",
        "match_terrain_contracts",
        "glow_contracts",
    ):
        assert pipeline[contract_name].keys() == techniques
    assert {
        entry["engine_enum"] for entry in pipeline["techniques"]
        if entry["clip_class"]
    } == techniques
    assert {
        resource["path"] for resource in pipeline["auxiliary_resources"]
    } == {
        "postmultiply/environment_textured.gdshader",
        "postmultiply/environment_textured_double_sided.gdshader",
        "postmultiply/environment_textured_cutout.gdshader",
        "postmultiply/environment_textured_cutout_double_sided.gdshader",
    }
    assert classes["TECHNIQUE_DEPTHMASK"]["unreachable_reason"]


def test_provenance_contracts_are_reviewable_and_citations_resolve_to_docs() -> None:
    manifest = load_provenance()
    allowed_statuses = set(manifest["parity_statuses"])
    for contract in manifest["contracts"]:
        assert contract["parity_status"] in allowed_statuses
        assert contract["light_behavior"]
        assert contract["docs"]
        assert contract["retail_citations"]
        if contract["parity_status"] != "matching":
            assert contract.get("exceptions"), (
                f"{contract['id']} must name every bounded parity exception"
            )

        documentation = ""
        for doc_name in contract["docs"]:
            doc_path = ROOT / doc_name
            assert doc_path.is_file(), f"missing provenance document {doc_name}"
            documentation += doc_path.read_text(encoding="utf-8").lower()

        for citation in contract["retail_citations"]:
            address = citation["address"].lower()
            start = citation["function_start"].lower()
            assert ADDRESS_RE.fullmatch(address)
            assert ADDRESS_RE.fullmatch(start)
            assert citation["function"]
            assert address in documentation, (
                f"{contract['id']} citation {address} is absent from its RE documents"
            )


def test_all_includes_resolve_without_cycles_or_orphans() -> None:
    sources = shader_sources()
    wrappers = {path for path in sources if path.suffix == ".gdshader"}
    includes = {path for path in sources if path.suffix == ".gdshaderinc"}
    reachable = {
        path
        for wrapper in wrappers
        for path in include_closure(wrapper)
        if path.suffix == ".gdshaderinc"
    }
    assert reachable == includes


def test_every_shader_resource_has_exactly_one_uid_sidecar() -> None:
    sources = shader_sources()
    uid_targets = {
        Path(str(path.resolve())[: -len(".uid")])
        for path in SHADER_DIR.rglob("*.uid")
    }
    assert uid_targets == sources


def test_every_wrapper_declares_one_shader_type_and_inherits_a_citation() -> None:
    manifest = load_provenance()
    wrappers = sorted(path for path in shader_sources() if path.suffix == ".gdshader")
    for wrapper in wrappers:
        closure = include_closure(wrapper)
        combined = "\n".join(path.read_text(encoding="utf-8") for path in closure)
        assert len(re.findall(r"(?m)^\s*shader_type\s+\w+\s*;", combined)) == 1, (
            f"{relative(wrapper)} must declare exactly one shader_type"
        )
        contract = matching_contracts(wrapper, manifest)[0]
        assert "[orig:" in combined.lower() or contract["retail_citations"], (
            f"{relative(wrapper)} has no retail provenance"
        )


def test_lighting_contracts_reach_the_shader_math() -> None:
    object_shared = (SHADER_DIR / "object" / "shared.gdshaderinc").read_text()
    for token in (
        "u_hemi_sky_color",
        "u_hemi_ground_color",
        "u_dir_light_dir",
        "u_dir_light_color",
        "u_point_light_count",
        "obj_ff_lighting",
        "obj_point_light_sum",
        "obj_bump_diffuse_lighting",
        "obj_phong_lighting",
        "obj_phong_map_specular",
        "obj_environment_cube_approx",
        "obj_apply_additive_fog",
        "v_dir_self_shadow",
        "v_pixel_point_factor",
        "obj_pixel_point_vertex_factors",
    ):
        assert token in object_shared

    terrain = (SHADER_DIR / "terrain_lighting.gdshaderinc").read_text()
    for token in ("u_sun_light", "u_sky_ambient", "terrain_point_light_pool"):
        assert token in terrain

    foliage = (SHADER_DIR / "foliage_detail.gdshaderinc").read_text()
    for token in (
        "opennova_sky_ambient",
        "opennova_sun_light",
        "opennova_sun_direction",
    ):
        assert token in foliage

    sky = (SHADER_DIR / "sky.gdshader").read_text()
    assert "dot(dome_normal, u_sun_dir)" in sky
    assert "dot(dome_normal, u_light_dir)" in sky


def test_highest_quality_foliage_rejects_the_inert_d3d_light_premise() -> None:
    foliage = (SHADER_DIR / "foliage_detail.gdshaderinc").read_text(
        encoding="utf-8"
    )
    assert "tile.a * opennova_sun_light + opennova_sky_ambient" in foliage
    assert "lit * u_emitter_color * 8.0" in foliage
    for forbidden in (
        "u_point_light_count",
        "u_point_light_posr_0",
        "opennova_static_point_light_rows",
        "terrain_point_light_pool",
    ):
        assert forbidden not in foliage
    for witness in (
        "Light_SelectAndEnableForDraw for each patch @ 0x60a5dc",
        "Foliage_WindSwayVS @ 0x60087a..0x600883",
        "writes oD0 = c6",
        "failed-VS fixed-function",
    ):
        assert witness in foliage

    contract = next(
        row for row in load_provenance()["contracts"] if row["id"] == "foliage"
    )
    assert contract["light_behavior"] == "terrain_tile_cache_environment_or_depth_only"
    citations = {row["address"].lower() for row in contract["retail_citations"]}
    assert {"0x5ff630", "0x60087a", "0x60a5dc"} <= citations


def test_retail_tile_set_atlas_is_carried_not_misclassified_as_a_lightmap() -> None:
    composer_header = (
        ROOT / "engine/runtime/terrain/terrain_tile_composer.h"
    ).read_text(encoding="utf-8")
    composer = (
        ROOT / "engine/runtime/terrain/terrain_tile_composer.cpp"
    ).read_text(encoding="utf-8")
    cache_device = (
        GODOT_ROOT / "src/terrain/nova_terrain_tile_cache_device.cpp"
    ).read_text(encoding="utf-8")
    terrain = (SHADER_DIR / "terrain.gdshader").read_text(encoding="utf-8")
    foliage = (SHADER_DIR / "foliage_detail.gdshaderinc").read_text(
        encoding="utf-8"
    )

    for token in (
        "mission .til RGB",
        "const Rgba8Image *tilestrip",
        "compose_terrain_tile_page",
    ):
        assert token in composer_header
    for token in (
        "til_build_entry_render_uv_quad",
        "sample_tilestrip",
        "compose_overlay_rgba",
        "add_dot3_alpha(output, dot3_alpha)",
    ):
        assert token in composer
    for token in (
        "get_tilestrip_tex()",
        "snapshot->tilestrip",
        "result.tilestrip = &tilestrip",
    ):
        assert token in cache_device
    for shader in (terrain, foliage):
        assert "uniform sampler2DArray u_tile_cache" in shader
        assert "texture(u_tile_cache" in shader
    assert "terrain_surface_color_from_colormap" in terrain
    assert "tile.a * opennova_sun_light + opennova_sky_ambient" in foliage

    contract = next(
        row
        for row in load_provenance()["contracts"]
        if row["id"] == "terrain-surface"
    )
    citations = {row["address"].lower() for row in contract["retail_citations"]}
    assert {"0x604a90", "0x60ddd4"} <= citations
    assert "D-RLIT-6" not in contract["exceptions"]


def test_terrain_static_shadow_final_composite_is_explicit_and_cited() -> None:
    header = (
        ROOT / "engine/runtime/terrain/terrain_static_shadow_alpha.h"
    ).read_text(encoding="utf-8")
    source = (
        ROOT / "engine/runtime/terrain/terrain_static_shadow_alpha.cpp"
    ).read_text(encoding="utf-8")
    native_test = (
        ROOT / "tests/terrain/terrain_static_shadow_alpha_test.cpp"
    ).read_text(encoding="utf-8")

    for token in (
        "RGBA writes and additive",
        "ONE/ONE",
        "PSDepthAlpha",
        "composite_terrain_static_shadow_pixel",
        "temporary_blue",
    ):
        assert token in header
    for token in (
        "destination_rgba[0]",
        "destination_rgba[1]",
        "destination_rgba[2]",
        "static_cast<int>(destination_rgba[3])",
        "composite_terrain_static_shadow_pixel(",
    ):
        assert token in source
    for token in (
        "test_retail_additive_composite_contract",
        "zero-RGB ONE/ONE source preserves RGB",
        "saturates like RGBA8 retail",
    ):
        assert token in native_test

    contract = next(
        row
        for row in load_provenance()["contracts"]
        if row["id"] == "terrain-surface"
    )
    citations = {row["address"].lower() for row in contract["retail_citations"]}
    assert "0x60e0c6" in citations


def test_max_quality_tile_page_projection_is_one_c7_c8_cutover() -> None:
    cache_header = (
        ROOT / "engine/runtime/terrain/terrain_tile_composition_cache.h"
    ).read_text(encoding="utf-8")
    cache_source = (
        ROOT / "engine/runtime/terrain/terrain_tile_composition_cache.cpp"
    ).read_text(encoding="utf-8")
    shadow = (
        ROOT / "engine/runtime/terrain/terrain_static_shadow_raster.cpp"
    ).read_text(encoding="utf-8")
    terrain = (SHADER_DIR / "terrain.gdshader").read_text(encoding="utf-8")
    foliage = (SHADER_DIR / "foliage_detail.gdshaderinc").read_text(
        encoding="utf-8"
    )
    match_terrain = (SHADER_DIR / "object/match_terrain.gdshaderinc").read_text(
        encoding="utf-8"
    )
    object_shared = (SHADER_DIR / "object/shared.gdshaderinc").read_text(
        encoding="utf-8"
    )
    runtime_consumers = "\n".join(
        path.read_text(encoding="utf-8")
        for path in (
            GODOT_ROOT / "src/terrain/nova_terrain.cpp",
            GODOT_ROOT / "src/terrain/nova_foliage_dispatcher.cpp",
            GODOT_ROOT / "src/object/nova_object_model.cpp",
        )
    )

    for token in (
        "TerrainTilePageProjection",
        "Foliage_RenderFarPatches @0x60A1DE..0x60A34F",
        "c7/c8 uploads",
        "failed-vertex-",
        "page_projection(",
    ):
        assert token in cache_header
    for token in (
        "world_x - world_origin_x",
        "world_z - world_origin_z",
        "1.0f / static_cast<float>(span)",
    ):
        assert token in cache_source
    assert "TerrainTileCompositionCache::page_projection(input.page)" in shadow

    for shader in (terrain, foliage):
        assert "u_instance_tile_cache_projection" in shader
        assert "u_instance_tile_cache_origin_span" not in shader
    for shader in (object_shared, match_terrain):
        assert "u_match_terrain_page_projection" in shader
        assert "u_match_terrain_page_origin_span" not in shader
    assert runtime_consumers.count("TerrainTileCompositionCache::page_projection(") == 3
    assert "tile_cache_origin_span" not in runtime_consumers
    assert "match_terrain_page_origin_span" not in runtime_consumers

    contracts = {row["id"]: row for row in load_provenance()["contracts"]}
    terrain_citations = {
        row["address"].lower()
        for row in contracts["terrain-surface"]["retail_citations"]
    }
    foliage_citations = {
        row["address"].lower()
        for row in contracts["foliage"]["retail_citations"]
    }
    assert "0x60db67" in terrain_citations
    assert {"0x6006f0", "0x60a220"} <= foliage_citations


def test_object_point_lights_preserve_the_retail_stage_split() -> None:
    shared = (SHADER_DIR / "object" / "shared.gdshaderinc").read_text(
        encoding="utf-8"
    )
    fixed_point = shared.split("vec3 obj_point_light_one", 1)[1].split(
        "vec3 obj_point_light_sum", 1
    )[0]
    pixel_vertex_factor = shared.split(
        "float obj_pixel_point_vertex_factor_one", 1
    )[1].split("vec4 obj_pixel_point_vertex_factors", 1)[0]
    pixel_vertex_attenuation = shared.split(
        "float obj_pixel_point_vertex_attenuation_one", 1
    )[1].split("vec4 obj_pixel_point_vertex_factors", 1)[0]
    pixel_fragment_points = shared.split("vec3 obj_bump_diffuse_point_one", 1)[
        1
    ].split("vec3 obj_environment_cube_approx", 1)[0]

    # Fixed-function lights are D3D vertex lights and keep the D3D range gate.
    assert "if (dist >= col.w)" in fixed_point
    assert "ndotl" in fixed_point

    # DOT3/Phong authored effects interpolate attenuation+self-shadow from
    # their vertex programs; only mapped-normal N.L/N.H remains per fragment.
    assert "obj_self_shadow(geom_normal_ws, to_light)" in pixel_vertex_factor
    assert "1.0 + posr.w * distance_to_light * distance_to_light" in pixel_vertex_factor
    assert "col.w" not in pixel_vertex_factor
    assert "obj_self_shadow" not in pixel_vertex_attenuation
    assert "1.0 + posr.w * distance_to_light * distance_to_light" in (
        pixel_vertex_attenuation
    )
    assert "obj_self_shadow" not in pixel_fragment_points
    assert "dist >=" not in pixel_fragment_points
    assert "vertex_factor" in pixel_fragment_points
    assert "v_pixel_point_factor" in pixel_fragment_points
    assert "v_pixel_point_attenuation" in pixel_fragment_points

    vertex_standard = (SHADER_DIR / "object" / "vertex_standard.gdshaderinc").read_text(
        encoding="utf-8"
    )
    vertex_flag = (SHADER_DIR / "object" / "vertex_flag.gdshaderinc").read_text(
        encoding="utf-8"
    )
    assert "obj_pixel_point_vertex_factors" in vertex_standard
    assert "obj_pixel_point_vertex_attenuations" in vertex_standard
    assert "v_pixel_point_factor = vec4(0.0)" in vertex_flag
    assert "v_pixel_point_attenuation = vec4(0.0)" in vertex_flag

    validation = json.loads(TECHNIQUE_VALIDATION_PATH.read_text(encoding="utf-8"))
    audited = {entry["engine_enum"]: entry for entry in validation["techniques"]}
    assert audited["PhongObjectSpecularPhongMap"]["responses"] == {
        "directional": True,
        "hemisphere": False,
        "ambient": True,
        "point": True,
    }
    for name in ("Flag", "GlassFixed", "GlassSkinned"):
        assert audited[name]["responses"]["point"] is False


def test_static_multimeshes_share_the_per_robj_point_light_contract() -> None:
    shared = (SHADER_DIR / "object" / "shared.gdshaderinc").read_text(
        encoding="utf-8"
    )
    vertex_standard = (
        SHADER_DIR / "object" / "vertex_standard.gdshaderinc"
    ).read_text(encoding="utf-8")
    vertex_flag = (SHADER_DIR / "object" / "vertex_flag.gdshaderinc").read_text(
        encoding="utf-8"
    )
    project = (GODOT_ROOT / "project.godot").read_text(encoding="utf-8")
    placer = (
        GODOT_ROOT / "src" / "mission" / "nova_mission_object_placer.cpp"
    ).read_text(encoding="utf-8")
    light_scene = (
        GODOT_ROOT / "src" / "lights" / "nova_light_scene.cpp"
    ).read_text(encoding="utf-8")

    for token in (
        "global uniform sampler2D opennova_static_point_light_rows",
        "varying float v_static_point_light_row",
        "float obj_point_light_count()",
        "vec4 obj_point_light_posr(int light_index)",
        "vec4 obj_point_light_color(int light_index)",
        "texelFetch(opennova_static_point_light_rows",
    ):
        assert token in shared
    # Direct instance-uniform reads belong only to the live-model fallback in
    # the accessors. Every fixed/DOT3/Phong/environment helper calls those
    # accessors, so static rows cannot silently skip one technique family.
    assert shared.count("u_point_light_count") == 2
    for index in range(4):
        assert shared.count(f"u_point_light_posr_{index}") == 2
        assert shared.count(f"u_point_light_color_{index}") == 2
    for vertex in (vertex_standard, vertex_flag):
        assert "v_static_point_light_row = INSTANCE_CUSTOM.x" in vertex

    assert "opennova_static_point_light_rows={" in project
    assert '"type": "sampler2D"' in project
    for token in (
        "mm->set_use_custom_data(true)",
        "batch.robj_index",
        "static_light_draw_key",
        "_append_static_light_draw_source",
        "static_cast<float>(*row + 1)",
    ):
        assert token in placer
    for token in (
        "STATIC_LIGHT_ROW_TEXELS",
        "Image::FORMAT_RGBAF",
        "scene_.select_for_draws",
        'global_shader_parameter_set("opennova_static_point_light_rows"',
        "atlas[0] = static_cast<float>(selection.count)",
        "1 + light * 2",
    ):
        assert token in light_scene


def test_material_rgb_alpha_and_coverage_channels_are_technique_specific() -> None:
    pipeline = json.loads(
        (SHADER_DIR / "object" / "pipeline_manifest.json").read_text(
            encoding="utf-8"
        )
    )
    assert pipeline["specular_alpha_contracts"] == {
        "PhongTangentSpecular": "pow8_brightness",
        "PhongTangentSpecularSkinned": "pow8_brightness",
        "PhongObjectSpecular": "pow8_brightness",
        "PhongObjectSpecularPhongMap": "phong_map_brightness_and_exponent",
        "EnvironmentPhong": "pow8_brightness",
    }

    single = (SHADER_DIR / "object" / "sampling" / "single.gdshaderinc").read_text(
        encoding="utf-8"
    )
    detail = (SHADER_DIR / "object" / "sampling" / "detail.gdshaderinc").read_text(
        encoding="utf-8"
    )
    surface = (SHADER_DIR / "object" / "surface.gdshaderinc").read_text(
        encoding="utf-8"
    )
    self_lit = (SHADER_DIR / "object" / "technique" / "self_lit.gdshaderinc").read_text(
        encoding="utf-8"
    )

    for sampling in (single, detail):
        assert "#ifdef OBJ_RGB_MOD_SELF_LUM" in sampling
        assert "base.rgb *= u_rgb_mod" in sampling
        assert "#ifdef OBJ_ALPHA_MOD_FFP" in sampling
        assert "base.a *= u_alpha_mod" in sampling
    assert "OBJ_COVERAGE_VERTEX_DIFFUSE_ALPHA" in surface
    assert "coverage_alpha = v_dir_self_shadow" in surface
    assert "OBJ_COVERAGE_NORMAL_ALPHA" in surface
    assert "coverage_alpha = obj_normal_alpha()" in surface
    assert "OBJ_COVERAGE_REFLECT_ALPHA" in surface
    assert "coverage_alpha = u_reflect_color.a" in surface
    assert "OBJ_COVERAGE_ZERO" in surface
    assert "coverage_alpha = 0.0" in surface
    assert "output_alpha = 0.0" in self_lit

    wrapper_macros = {
        "Fixed": ("fixed", "OBJ_ALPHA_MOD_FFP", "OBJ_RGB_MOD_NONE"),
        "FixedSkinned": ("fixed_skinned", "OBJ_ALPHA_MOD_NONE", "OBJ_RGB_MOD_NONE"),
        "SelfLit": ("self_lit", "OBJ_ALPHA_MOD_NONE", "OBJ_RGB_MOD_SELF_LUM"),
        "PhongObjectSpecular": (
            "phong_object_specular",
            "OBJ_ALPHA_MOD_NONE",
            "OBJ_RGB_MOD_NONE",
        ),
    }
    for technique, (directory, alpha_macro, rgb_macro) in wrapper_macros.items():
        wrapper = (SHADER_DIR / "object" / directory / "opaque.gdshader").read_text(
            encoding="utf-8"
        )
        assert f"#define {alpha_macro}" in wrapper, technique
        assert f"#define {rgb_macro}" in wrapper, technique


def test_water_reflection_clip_class_matches_every_reachable_retail_effect() -> None:
    pipeline = json.loads(
        (SHADER_DIR / "object" / "pipeline_manifest.json").read_text(
            encoding="utf-8"
        )
    )
    actual = {
        entry["engine_enum"]: entry["clip_class"]
        for entry in pipeline["techniques"]
    }
    assert actual == {
        "Fixed": "explicit",
        "FixedSkinned": "skinned_submit_skip",
        "FixedDetail": "explicit",
        "SelfLit": "explicit",
        "SelfLitDetail": "explicit",
        "Tracer": "normal_fallback",
        "Flag": "normal_fallback",
        "PhongTangentDiffuse": "explicit_unskinned_or_submit_skip_skinned",
        "PhongTangentSpecular": "explicit",
        "PhongTangentSpecularSkinned": "skinned_submit_skip",
        "PhongObjectDiffuse": "explicit_unskinned_or_submit_skip_skinned",
        "PhongObjectSpecular": "normal_fallback",
        "PhongObjectSpecularPhongMap": "skinned_submit_skip",
        "Dot3Tangent": "explicit",
        "Dot3TangentDetail": "explicit",
        "Dot3TangentSkinned": "skinned_submit_skip",
        "Dot3TangentDetailSkinned": "skinned_submit_skip",
        "Dot3Object": "skinned_submit_skip",
        "Dot3ObjectDetail": "skinned_submit_skip",
        "EnvironmentMirror": "normal_fallback",
        "EnvironmentMirrorTextured": "normal_fallback",
        "EnvironmentPhong": "normal_fallback",
        "GlassFixed": "explicit",
        "GlassSkinned": "skinned_submit_skip",
    }

    inventory = json.loads(RETAIL_EFFECT_INVENTORY_PATH.read_text(encoding="utf-8"))
    assert inventory["classes"]["TECHNIQUE_CLIP"]["evidence"] == {
        "_FFP.fx": ["TBoringFFPClip"],
        "BDiffT2.fx": ["TSegTanDiff_Clip"],
        "Dot3DiffO.fx": ["TSegObjDiff_clip"],
        "Dot3DiffT.fx": ["TSegTanDiff_Clip"],
        "Glass.fx": ["TGlassFFP_clip"],
        "PhongT.fx": ["TSegTanDiff_Clip"],
    }

    shared = (SHADER_DIR / "object" / "shared.gdshaderinc").read_text(
        encoding="utf-8"
    )
    surface = (SHADER_DIR / "object" / "surface.gdshaderinc").read_text(
        encoding="utf-8"
    )
    for token in (
        "opennova_water_reflection_clip_active",
        "opennova_water_reflection_eye",
        "opennova_water_height - 0.1",
        "OBJ_CLIP_EXPLICIT",
        "OBJ_CLIP_EXPLICIT_UNSKINNED_OR_SUBMIT_SKIP_SKINNED",
        "discard",
    ):
        assert token in shared
    assert "obj_apply_water_reflection_clip(camera_position_world)" in surface

    water = (GODOT_ROOT / "src" / "env" / "nova_water.cpp").read_text(
        encoding="utf-8"
    )
    assert '"opennova_water_reflection_eye"' in water
    assert '"opennova_water_reflection_clip_active"' in water
    assert "!view.below_water" in water


def test_gamma_encoded_retail_effect_math_crosses_godot_linear_boundary_once() -> None:
    sources = {
        relative(path): path.read_text(encoding="utf-8") for path in shader_sources()
    }
    assert not any(": source_color" in source for source in sources.values())
    assert not any("nova_gamma_to_linear" in source for source in sources.values())
    assert not any("nova_linear_to_gamma" in source for source in sources.values())

    color_math_particles = {
        "particle/particle_blend_additive.gdshader",
        "particle/particle_blend_blend.gdshader",
        "particle/particle_blend_bump.gdshader",
        "particle/particle_blend_bumpadd.gdshader",
        "particle/particle_blend_mod.gdshader",
        "particle/particle_blend_mod2x.gdshader",
        "particle/particle_blend_premult.gdshader",
    }
    for name in color_math_particles:
        source = sources[name]
        assert '#include "res://shaders/nova_color.gdshaderinc"' in source
        assert "nova_scene_output" in source

    distort = sources["particle/particle_blend_distort.gdshader"]
    assert "nova_scene_output" not in distort
    assert "nova_display_decode_gamma" not in distort

    nvg = sources["nvg_view.gdshader"]
    assert "nova_display_encode_gamma" in nvg
    assert "nova_display_decode_gamma" in nvg

    drape = sources["slot_shadow_drape.gdshader"]
    assert "nova_scene_output(factor)" in drape
    assert "pow(factor" not in drape
    assert "vec3 ambient = vec3(1.0) - (1.0 - fade) * u_slot_term[i].rgb;" in drape
    assert "vec3 shadowed = s.rgb + ambient;" in drape
    assert "u_slot_term[i].rgb * s.a" not in drape

    color_contract = sources["nova_color.gdshaderinc"]
    assert "nova_scene_output" in color_contract
    assert "nova_scene_input" in color_contract
    assert "nova_display_decode_gamma" in color_contract
    assert "nova_display_encode_gamma" in color_contract
    assert "nova_gamma_to_linear" not in color_contract
    assert "nova_linear_to_gamma" not in color_contract

    frame_renderer = (
        GODOT_ROOT / "src" / "render" / "nova_framefx.cpp"
    ).read_text(encoding="utf-8")
    assert "FramePass::GammaDecode" in frame_renderer
    assert "EFFECT_CALLBACK_TYPE_POST_TRANSPARENT" in frame_renderer
    assert "framebuffer_blend_domain\"] = \"gamma\"" in frame_renderer
    for token in (
        "kBeautyCameraMask = 0x00018C01u",
        "kQ3CameraMask = 0x00010401u",
        "DATA_FORMAT_R8G8B8A8_UNORM",
        "direction_for_degrees(30.0f, 1.0f / 1024.0f)",
        "1.0f / 2048.0f",
        "* 0.50",
        "* 0.46",
        "* 0.35",
        "* 0.19",
        "Vector2i(kFrameFxSide, kFrameFxSide), 90.0f",
        "Vector2i(kFrameFxSide, kFrameFxSide), 0.0f",
        "direction_for_degrees(45.0f, 0.0027621093f)",
        "BlendMode::SourceAlphaAdd, FramePass::FinalAverage",
        'result["capture_filter"] = "linear_rgba8_highest_quality"',
    ):
        assert token in frame_renderer

    # The first-person viewmodel draws inside the beauty pass through the
    # shader-side renderfov projection + depth band; no composite shader.
    assert "viewmodel_composite.gdshader" not in sources
    viewmodel_pass = sources["nova_viewmodel_pass.gdshaderinc"]
    assert "global uniform vec4 opennova_viewmodel_projection" in viewmodel_pass
    assert "instance uniform bool u_viewmodel_pass" in viewmodel_pass
    assert "NOVA_VIEWMODEL_DEPTH_WINDOW = 0.1" in viewmodel_pass

    probe = (GODOT_ROOT / "tests" / "render_swatch_probe.gd").read_text(
        encoding="utf-8"
    )
    assert '["SRCALPHA/INVSRCALPHA", -0.3, 128]' in probe
    assert '["ONE/ONE", 0.3, 96]' in probe


def test_environment_techniques_do_not_invent_fresnel_or_diffuse_terms() -> None:
    technique_dir = SHADER_DIR / "object" / "technique"
    mirror = (technique_dir / "environment.gdshaderinc").read_text(encoding="utf-8")
    textured = (technique_dir / "environment_textured.gdshaderinc").read_text(
        encoding="utf-8"
    )
    glass = (technique_dir / "glass.gdshaderinc").read_text(encoding="utf-8")
    glass_skinned = (technique_dir / "glass_skinned.gdshaderinc").read_text(
        encoding="utf-8"
    )
    executable = "\n".join(
        line for line in (mirror + textured + glass + glass_skinned).splitlines()
        if not line.lstrip().startswith("//")
    ).lower()
    assert "fresnel" not in executable
    assert "obj_ff_lighting" not in executable
    assert "obj_bump_diffuse_lighting" not in executable


def test_windowed_probe_exercises_direction_hemisphere_and_gameplay_points() -> None:
    probe = (GODOT_ROOT / "tests" / "render_swatch_probe.gd").read_text()
    for state in (
        '"direction_a"',
        '"direction_b"',
        '"hemi_sky"',
        '"hemi_ground"',
        '"ambient_off"',
        '"ambient_on"',
        '"point_off"',
        '"point_on"',
        '"point_static"',
    ):
        assert state in probe
    assert 'set_instance_shader_parameter("u_point_light_count"' in probe
    assert "MultiMeshInstance3D.new()" in probe
    assert "use_custom_data = true" in probe
    assert "set_instance_custom_data" in probe
    assert '"opennova_static_point_light_rows", static_point_atlas' in probe
    assert "static atlas point-light route was not pixel-identical" in probe
    assert '"res://shaders/object/technique_validation.json"' in probe
    assert "did not reverse its directional lobe" in probe
    assert "did not reverse sky/ground response" in probe
    assert "invented a directional response" in probe
    assert "invented a hemisphere response" in probe
    assert "invented an ambient response" in probe
    assert "invented a gameplay point-light response" in probe
    for state in (
        '"rgb_low"',
        '"rgb_high"',
        '"specular_alpha_low"',
        '"specular_alpha_high"',
        '"coverage_low"',
        '"coverage_high"',
        '"alpha_gen_low"',
        '"alpha_gen_high"',
    ):
        assert state in probe
    assert 'parsed.get("specular_alpha_contracts", {})' in probe
    assert '"specular alpha"' in probe
    assert '"coverage source"' in probe
    assert '"RgbGen"' in probe
    assert '"AlphaGen"' in probe
    assert "did not react to %s" in probe
    assert "invented a %s response" in probe
    for state in ('"inactive"', '"disarmed"', '"wrong_eye"', '"reflection"'):
        assert state in probe
    assert '"object-water-reflection-clip"' in probe
    assert '"reflection CLIP"' in probe
    assert "clipped above waterHeight-0.1" in probe
    assert "clipped an unrelated camera" in probe
