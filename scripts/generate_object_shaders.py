"""Generate the finite, reviewable object-shader wrapper matrix.

The manifest owns topology and render state. Technique math remains handwritten
in checked-in includes; this generator only performs the mechanical Cartesian
product over technique, policy, and cull mode.

Procedure: edit ``godot/shaders/object/pipeline_manifest.json`` or a
``*.gdshaderinc`` include, run ``uv run --frozen python
scripts/generate_object_shaders.py``, then regenerate the transitive hash
golden with ``OPENNOVA_OBJECT_SHADER_HASHES_DUMP=1 uv run --frozen pytest
tests/test_object_shader_resources.py -q -k transitive`` (dump mode is
deliberately red) and confirm the normal run is green.

The wrapper's compile-time contract includes ``OBJ_VERTEX_POINT_LIGHTS_*``: the
EffectWorld point-light product each technique's vertex stage must publish
(the D3D vertex-light diffuse sum for the fixed-function family, the oD0
attenuation x self-shadow factors for the pixel-shader families, attenuation
alone for the encoded-vector DOT3 families, nothing for unlit/glass/flag).
``vertex_standard.gdshaderinc`` evaluates only the product the technique
consumes, so no wrapper pays for another family's per-vertex light loops.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SHADER_ROOT = ROOT / "godot" / "shaders" / "object"
MANIFEST_PATH = SHADER_ROOT / "pipeline_manifest.json"


def uid_for(path: Path) -> str:
    """Return a stable positive 63-bit UID in Godot's base-36 text alphabet."""
    value = int.from_bytes(hashlib.sha256(path.as_posix().encode()).digest()[:8], "big")
    value &= (1 << 63) - 1
    alphabet = "0123456789abcdefghijklmnopqrstuvwxyz"
    encoded = ""
    while value:
        value, digit = divmod(value, len(alphabet))
        encoded = alphabet[digit] + encoded
    return "uid://" + (encoded or "1")


def wrapper_source(
    technique: dict,
    policy: dict,
    projected_shadow: str,
    match_terrain: str,
    glow: str,
    double_sided: bool,
) -> str:
    cull = "cull_disabled" if double_sided else "cull_back"
    blend = "blend_add" if policy["blend"] == "add" else "blend_mix"
    depth = "depth_draw_opaque" if policy["depth"] == "opaque" else "depth_draw_never"
    vertex = "vertex_flag" if technique["vertex"] == "flag" else "vertex_standard"
    output = "output_alpha" if policy["writes_alpha"] else "output_opaque"
    fog = technique.get("fog", policy["fog"])
    includes = (
        "shared.gdshaderinc",
        f"sampling/{technique['sampling']}.gdshaderinc",
        f"coverage/{policy['coverage']}.gdshaderinc",
        f"normal/{technique['normal']}.gdshaderinc",
        "surface.gdshaderinc",
        "match_terrain.gdshaderinc",
        "glow.gdshaderinc",
        f"{vertex}.gdshaderinc",
        f"fog/{fog}.gdshaderinc",
        f"technique/{technique['implementation']}.gdshaderinc",
        f"{output}.gdshaderinc",
    )
    include_lines = "\n".join(
        f'#include "res://shaders/object/{include}"' for include in includes
    )
    compile_time_contract = "\n".join(
        (
            f"#define OBJ_RGB_MOD_{technique['rgb_modulation'].upper()}",
            f"#define OBJ_ALPHA_MOD_{technique['alpha_modulation'].upper()}",
            f"#define OBJ_COVERAGE_{technique['coverage_source'].upper()}",
            f"#define OBJ_CLIP_{technique['clip_class'].upper()}",
            f"#define OBJ_VERTEX_POINT_LIGHTS_{technique['vertex_point_lights'].upper()}",
            f"#define OBJ_PROJSHAD_{projected_shadow.upper()}",
            f"#define OBJ_MATCHTERRAIN_{match_terrain.upper()}",
            f"#define OBJ_GLOW_{glow.upper()}",
            (
                "#define OBJ_Q3_DEPTH_OCCLUDER"
                if policy["depth"] == "opaque"
                else "#define OBJ_Q3_TRANSPARENT"
            ),
        )
    )
    return (
        "shader_type spatial;\n"
        f"render_mode unshaded, {blend}, {depth}, {cull};\n\n"
        f"{compile_time_contract}\n\n"
        f"{include_lines}\n"
    )


def main() -> None:
    manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
    generated = 0
    for technique in manifest["techniques"]:
        projected_shadow = manifest["projected_shadow_contracts"][
            technique["engine_enum"]
        ]
        match_terrain = manifest["match_terrain_contracts"][technique["engine_enum"]]
        glow = manifest["glow_contracts"][technique["engine_enum"]]
        directory = SHADER_ROOT / technique["directory"]
        directory.mkdir(parents=True, exist_ok=True)
        for policy_name in technique["policies"]:
            policy = manifest["policies"][policy_name]
            for double_sided in (False, True):
                suffix = "_double_sided" if double_sided else ""
                path = directory / f"{policy_name}{suffix}.gdshader"
                source = wrapper_source(
                    technique, policy, projected_shadow, match_terrain, glow,
                    double_sided
                )
                if not path.exists() or path.read_text(encoding="utf-8") != source:
                    path.write_text(source, encoding="utf-8", newline="\n")
                uid_path = Path(str(path) + ".uid")
                if not uid_path.exists():
                    uid_path.write_text(uid_for(path) + "\n", encoding="utf-8", newline="\n")
                generated += 1

    if generated != manifest["resource_count"]:
        raise SystemExit(
            f"manifest resource_count={manifest['resource_count']} but generated {generated}"
        )

    # Auxiliary passes and handwritten includes are part of the same finite
    # resource graph. Give every checked-in shader source a deterministic UID
    # so a fresh checkout does not depend on an editor import to complete it.
    for pattern in ("*.gdshader", "*.gdshaderinc"):
        for path in SHADER_ROOT.rglob(pattern):
            uid_path = Path(str(path) + ".uid")
            if not uid_path.exists():
                uid_path.write_text(
                    uid_for(path) + "\n", encoding="utf-8", newline="\n"
                )
    print(f"generated {generated} object shader wrappers")


if __name__ == "__main__":
    main()
