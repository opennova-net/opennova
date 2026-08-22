#!/usr/bin/env python
"""Assemble a registered render-parity publication and generate its index.

The OpenNova probe, ``register_retail_capture.py`` and
``build_retail_side_by_side.py`` each leave their artifacts in a separate
scratch tree. This tool copies the publishable subset into the tracked
``screenshots/parity/`` layout, verifies every copied byte against the hash its
own manifest declares, and writes the publication ``README.md`` -- the identity
table, the inventory line, and the per-fixture descriptive metric table.

It is deliberately not a capture tool and never touches retail. It fails closed
on a missing fixture, a hash mismatch, a mixed source commit, or a mixed tool
version, because a publication that mixes provenance is not evidence.

Usage::

    uv run python scripts/render/publish_registered_comparisons.py \
      --catalog docs/render/render-fixtures-retail-v5.json \
      --opennova-root .scratch/golden/render/fixtures \
      --retail-root .scratch/retail/raw \
      --comparison-root .scratch/publication \
      --output screenshots/parity/render-lighting-2026-08/registered-<date>

See docs/render/render-parity-runbook.md section 4.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import sys
import textwrap
from pathlib import Path
from typing import Any

WRAP = 78

VARIANTS = (
    "beauty",
    "shadows_off",
    "lighting_only",
    "unshaded",
    "directional_shadow_atlas",
)
COMPARISON_OUTPUTS = (
    "side_by_side",
    "overlay_50",
    "absolute_diff",
    "opennova_normalized",
)
FILES_PER_FIXTURE = 19
NUMBER_WORDS = {
    1: "one", 2: "two", 3: "three", 4: "four", 5: "five", 6: "six",
    7: "seven", 8: "eight", 9: "nine", 10: "ten",
}

RESIDUALS = """\
- Surface-water reflection shape, wave/noise phase, clouds, brightness, and
  shoreline shading still diverge. Waterline/glare ordering and underwater
  murk remain non-identical.
- Fire rows retain unsynchronized flame/smoke phase, blur, warm spill, and
  particle differences. CP04 also contains foliage and live-actor phase.
- CP12 retains night exposure, residual road-marking contrast, vegetation and
  ground sampling, vehicle/material response, and live NPC or flag phase.
- Retail frames are SETTLED captures: >= 4 s after the fixture apply with the
  clock pinned by the hook's capture-frame fixture-binding witness, so the
  teleport transient the 2026-08-20 publication sampled is gone and the
  cameras are the ground-snapped settled poses (net-re section 5.40 ninth
  pass; runbook section 8). Deep-water rows keep the fast capture (float
  physics pins the pose; D-INF-3).
- The M16 and bare-arm identity and PLACEMENT are matched (bone-exact and
  counter-gated, D-INF-14 FIXED), but viewmodel lighting, material response,
  and animated phase remain different.
- Ground tire marks and other ordered .til overlay contributions diverge in
  placement and blend (the open D-TERRAIN-7 tile-composition producer gap,
  measured by the tire-marks fixture).
- Model-authored `LGHT` lamp delivery diverges: corona billboards are not
  drawn, Target/spot cones are dropped, and batched static buildings lose the
  authored subobject owner scope (the open D-RLIT-4 residual tail, measured by
  the armory-lght fixture).
- Sun, sky-dome, and ambient response diverge at low sun: iris/ambient
  sampling (D-RLIT-2), the sun-glint/reflection stand-ins (D-RLIT-5), and the
  deferred overcast/TOD first-pass table (env #16), measured by the 03TR
  sun-sky fixture.
"""


class PublishError(RuntimeError):
    """A fail-closed publication error."""


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def load_json(path: Path) -> dict[str, Any]:
    # Some evidence writers emit a UTF-8 BOM; utf-8-sig reads both forms.
    if not path.is_file():
        raise PublishError(f"missing required record: {path}")
    with path.open("r", encoding="utf-8-sig") as handle:
        return json.load(handle)


def copy_verified(source: Path, target: Path, expected: str, label: str) -> None:
    if not source.is_file():
        raise PublishError(f"missing {label}: {source}")
    actual = sha256(source)
    if actual != expected:
        raise PublishError(
            f"{label} hash mismatch at {source}: "
            f"declared {expected}, found {actual}"
        )
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, target)


def sort_key(fixture_id: str) -> str:
    """Publication ordering: ids compared with separators removed.

    Keeps ``cp01-waterline-*`` ahead of ``cp01-water-oblique``, matching the
    ordering the first registered publication established.
    """
    return fixture_id.replace("-", "")


def collect_fixture(
    fixture_id: str,
    opennova_root: Path,
    retail_root: Path,
    comparison_root: Path,
    output_root: Path,
) -> dict[str, Any]:
    """Copy one fixture's publishable artifacts and return its index row data."""
    out_dir = output_root / fixture_id

    # --- OpenNova leg -----------------------------------------------------
    manifest_name = f"{fixture_id}-manifest.json"
    manifest_src = opennova_root / fixture_id / manifest_name
    manifest = load_json(manifest_src)
    if manifest.get("fixture", {}).get("id") != fixture_id:
        raise PublishError(f"{manifest_src} does not describe {fixture_id}")

    artifacts = manifest.get("artifacts", [])
    seen = [entry.get("variant") for entry in artifacts]
    if seen != list(VARIANTS):
        raise PublishError(
            f"{fixture_id}: variant order {seen} does not match {list(VARIANTS)}"
        )
    for entry in artifacts:
        copy_verified(
            manifest_src.parent / entry["png_path"],
            out_dir / "opennova" / entry["png_path"],
            entry["png_sha256"],
            f"{fixture_id} {entry['variant']} png",
        )
        copy_verified(
            manifest_src.parent / entry["state_path"],
            out_dir / "opennova" / entry["state_path"],
            entry["state_sha256"],
            f"{fixture_id} {entry['variant']} state",
        )
    (out_dir / "opennova").mkdir(parents=True, exist_ok=True)
    shutil.copyfile(manifest_src, out_dir / "opennova" / manifest_name)

    # --- retail leg -------------------------------------------------------
    registered_src = retail_root / fixture_id / "registered.json"
    registered = load_json(registered_src)
    if registered.get("fixture_id") != fixture_id:
        raise PublishError(f"{registered_src} does not describe {fixture_id}")
    capture = registered["capture"]
    raw = registered["raw_evidence"]
    copy_verified(
        registered_src.parent / capture["image_path"],
        out_dir / "retail" / capture["image_path"],
        capture["sha256"],
        f"{fixture_id} retail frame",
    )
    copy_verified(
        registered_src.parent / raw["retail_stage_manifest_name"],
        out_dir / "retail" / raw["retail_stage_manifest_name"],
        raw["retail_stage_manifest_sha256"],
        f"{fixture_id} retail stage record",
    )
    (out_dir / "retail").mkdir(parents=True, exist_ok=True)
    shutil.copyfile(registered_src, out_dir / "retail" / "registered.json")

    # --- comparison leg ---------------------------------------------------
    comparison_name = f"{fixture_id}-comparison.json"
    comparison_src = comparison_root / fixture_id / comparison_name
    comparison = load_json(comparison_src)
    if comparison.get("fixture_id") != fixture_id:
        raise PublishError(f"{comparison_src} does not describe {fixture_id}")
    for key in COMPARISON_OUTPUTS:
        entry = comparison["outputs"][key]
        copy_verified(
            comparison_src.parent / entry["path"],
            out_dir / "comparison" / entry["path"],
            entry["sha256"],
            f"{fixture_id} {key}",
        )
    (out_dir / "comparison").mkdir(parents=True, exist_ok=True)
    shutil.copyfile(comparison_src, out_dir / "comparison" / comparison_name)

    files = sorted(path for path in out_dir.rglob("*") if path.is_file())
    if len(files) != FILES_PER_FIXTURE:
        raise PublishError(
            f"{fixture_id}: published {len(files)} files, expected "
            f"{FILES_PER_FIXTURE}"
        )

    # The raw bundle sidecar is not publishable, but its schema names the
    # producer contract the registration was accepted under.
    bundle_state = retail_root / fixture_id / raw["state_name"]
    if sha256(bundle_state) != raw["state_sha256"]:
        raise PublishError(
            f"{fixture_id}: raw capture sidecar {bundle_state} does not match "
            "the hash the registration declares"
        )

    return {
        "id": fixture_id,
        "manifest": manifest,
        "registered": registered,
        "comparison": comparison,
        "bundle_schema": load_json(bundle_state)["schema"],
        "stage": load_json(
            out_dir / "retail" / raw["retail_stage_manifest_name"]
        ),
    }


def unique(rows: list[dict[str, Any]], reader, label: str) -> Any:
    values = {json.dumps(reader(row), sort_keys=True) for row in rows}
    if len(values) != 1:
        raise PublishError(
            f"{label} is not identical across the publication: "
            f"{sorted(values)}"
        )
    return reader(rows[0])


def metric(block: dict[str, Any]) -> str:
    return (
        f"{block['mean_abs_channel_delta']:.6f} / "
        f"{block['root_mean_square_channel_delta']:.6f}"
    )


def oxford(items: list[str]) -> str:
    if len(items) == 1:
        return items[0]
    if len(items) == 2:
        return f"{items[0]} and {items[1]}"
    return ", ".join(items[:-1]) + f", and {items[-1]}"


def region(comparison: dict[str, Any], name: str) -> dict[str, Any]:
    for entry in comparison["metrics"]["regions"]:
        if entry.get("name") == name:
            return entry
    raise PublishError(f"{comparison['fixture_id']}: missing ROI {name}")


def render_readme(
    rows: list[dict[str, Any]],
    catalog_sha: str,
    output_root: Path,
    source_notes: str | None = None,
) -> str:
    source_commit = unique(
        rows, lambda r: r["manifest"]["provenance"]["source_commit"],
        "OpenNova source commit",
    )
    godot_sha = unique(
        rows, lambda r: r["manifest"]["provenance"]["godot_executable_sha256"],
        "Godot executable hash",
    )
    gdext_sha = unique(
        rows, lambda r: r["manifest"]["provenance"]["gdextension_sha256"],
        "GDExtension hash",
    )
    retail_exe = unique(
        rows, lambda r: r["registered"]["build"]["retail_executable_sha256"],
        "retail executable hash",
    )
    profile = unique(
        rows, lambda r: r["registered"]["capture"]["video_profile_id"],
        "retail video profile",
    )
    hook = unique(
        rows,
        lambda r: (
            r["registered"]["raw_evidence"]["hook_version"],
            r["registered"]["raw_evidence"]["bridge_version_major"],
            r["registered"]["raw_evidence"]["bridge_version_minor"],
        ),
        "onHook producer",
    )
    registrar = unique(
        rows,
        lambda r: (r["registered"]["tool"]["version"], r["registered"]["schema"]),
        "retail registrar",
    )
    staging = unique(
        rows, lambda r: (r["stage"]["tool_version"], r["stage"]["schema"]),
        "retail staging tool",
    )
    builder = unique(
        rows,
        lambda r: (r["comparison"]["tool"]["version"], r["comparison"]["schema"]),
        "comparison builder",
    )
    original_cfg = unique(
        rows, lambda r: r["stage"]["game_config"]["original_sha256"],
        "original game.cfg hash",
    )
    staged_cfg = unique(
        rows, lambda r: r["stage"]["game_config"]["effective_sha256"],
        "staged game.cfg hash",
    )
    weapon_sav = unique(
        rows, lambda r: r["stage"]["weapon_profile"]["sha256"],
        "weapon.sav hash",
    )
    roi_names = [
        entry["name"] for entry in rows[0]["comparison"]["metrics"]["regions"]
    ]

    missions = sorted({row["manifest"]["mission"]["file"] for row in rows})
    file_count = sum(
        1 for path in output_root.rglob("*") if path.is_file()
    ) + 1  # + this README

    def span(reader) -> tuple[float, float]:
        values = [reader(row) for row in rows]
        return min(values), max(values)

    full_lo, full_hi = span(
        lambda r: r["comparison"]["metrics"]["full_frame"][
            "mean_abs_channel_delta"
        ]
    )
    world_lo, world_hi = span(
        lambda r: region(r["comparison"], "world_center")[
            "mean_abs_channel_delta"
        ]
    )
    arms_lo, arms_hi = span(
        lambda r: region(r["comparison"], "viewmodel_arms")[
            "mean_abs_channel_delta"
        ]
    )

    bundle_schema = unique(
        rows, lambda r: r["bundle_schema"], "raw capture bundle schema"
    )
    changed_keys = unique(
        rows, lambda r: r["stage"]["game_config"]["changed_keys"],
        "staged game.cfg changed keys",
    )
    hud_detail = unique(
        rows, lambda r: r["stage"]["game_config"]["hud_detail"],
        "staged hud_detail",
    )

    lines: list[str] = []
    add = lines.append

    def para(text: str) -> None:
        add(
            "\n".join(
                textwrap.wrap(
                    " ".join(text.split()),
                    width=WRAP,
                    break_on_hyphens=False,
                    break_long_words=False,
                )
            )
        )

    add("# Registered OpenNova / retail maximum-quality lighting review")
    add("")
    para(
        f"This publication contains {len(rows)} registered comparisons across "
        + oxford([f"`{name}`" for name in missions]) + ". Every pair keeps "
        "terrain and the first-person viewmodel active, uses the M16 Burst "
        "with plain bare `IndoArms.3di` arms, and omits gameplay HUD content "
        "from the saved images. OpenNova's runtime witness additionally "
        "records 30/270 ammunition."
    )
    add("")
    para(
        "Retail ran at the exhaustive highest-quality RevX02 profile. Its "
        "pre-launch stage changed only "
        + oxford([f"`{key}`" for key in changed_keys])
        + ", because the other video/effect values already matched the "
        "profile. The capture hook copies the backbuffer at a certified "
        "pre-HUD boundary, restores the D3D scene, and then lets retail render "
        "its UI unchanged. Normal on-screen HUD/FPS therefore remain visible; "
        "only the registered PNG is HUD-free."
    )
    add("")
    if source_notes:
        add(source_notes.strip("\n"))
        add("")
    add("## Immutable identities and inventory")
    add("")
    add("| Identity | Value |")
    add("|---|---|")
    add(f"| Frozen OpenNova source | `{source_commit}` |")
    add(f"| Retail fixture catalog SHA-256 | `{catalog_sha}` |")
    add(f"| Godot executable SHA-256 | `{godot_sha}` |")
    add(f"| GDExtension SHA-256 | `{gdext_sha}` |")
    add(f"| Retail executable SHA-256 | `{retail_exe}` |")
    add(f"| Retail video profile | `{profile}` |")
    add(
        f"| Retail capture producer | onHook {hook[0]}; bridge "
        f"{hook[1]}.{hook[2]}; `{bundle_schema}` |"
    )
    add(f"| Retail registrar | {registrar[0]}; `{registrar[1]}` |")
    add(f"| Retail staging | {staging[0]}; `{staging[1]}` |")
    add(f"| Comparison builder | {builder[0]}; `{builder[1]}` |")
    add("")
    para(
        f"Including this index, the package contains {file_count} files: "
        f"{len(rows) * 10} PNGs and {len(rows) * 9} JSON records plus this "
        "README. The PNGs comprise "
        f"{len(rows) * len(VARIANTS)} raw OpenNova diagnostic variants, "
        f"{len(rows)} retail frames, {len(rows)} normalized OpenNova frames, "
        f"{len(rows)} side-by-sides, {len(rows)} overlays, and {len(rows)} "
        "absolute differences. No backup, restore token, transcript, or "
        "absolute caller path is included."
    )
    add("")
    para(
        f"Every sanitized stage record binds original `game.cfg` SHA-256 "
        f"`{original_cfg}`, effective maximum-profile SHA-256 `{staged_cfg}`, "
        f"and approved `weapon.sav` SHA-256 `{weapon_sav}`. `hud_detail` "
        f"remains {hud_detail['original']} in both original and staged config. "
        "The original config was restored byte-for-byte after registration."
    )
    add("")
    add("## Review index and exact descriptive metrics")
    add("")
    para(
        "The full-frame policy is "
        "`qualitative_only_matched_hud_hidden_cross_engine`. The "
        f"{NUMBER_WORDS.get(len(roi_names), len(roi_names))} required ROI "
        "families are descriptive presentation regions: "
        + oxford([
            "`{}=[{}]`".format(
                name,
                ",".join(
                    str(value)
                    for value in region(rows[0]["comparison"], name)["region"]
                ),
            )
            for name in roi_names
        ])
        + f". Across all {len(rows)} fixtures, full-frame MAE spans "
        f"`{full_lo:.6f}`-`{full_hi:.6f}`, world-center MAE spans "
        f"`{world_lo:.6f}`-`{world_hi:.6f}`, and viewmodel-arms MAE spans "
        f"`{arms_lo:.6f}`-`{arms_hi:.6f}`. These are exact channel deltas from "
        "each comparison manifest, not thresholds or pixel-parity verdicts."
    )
    add("")
    add(
        "| Fixture | Mission / minute | Full MAE / RMS | World MAE / RMS "
        "| Arms MAE / RMS | Direct evidence |"
    )
    add("|---|---:|---:|---:|---:|---|")
    for row in rows:
        fid = row["id"]
        comparison = row["comparison"]
        minute = int(row["manifest"]["fixture"]["minutes_of_day"][0])
        evidence = " · ".join(
            [
                f"[side-by-side]({fid}/comparison/{fid}-side-by-side.png)",
                f"[overlay]({fid}/comparison/{fid}-overlay-50.png)",
                f"[diff]({fid}/comparison/{fid}-absolute-diff.png)",
                f"[comparison]({fid}/comparison/{fid}-comparison.json)",
                f"[OpenNova manifest]({fid}/opennova/{fid}-manifest.json)",
                f"[registration]({fid}/retail/registered.json)",
            ]
        )
        add(
            f"| `{fid}` | `{comparison['mission']['file']}` / {minute:04d} "
            f"| {metric(comparison['metrics']['full_frame'])} "
            f"| {metric(region(comparison, 'world_center'))} "
            f"| {metric(region(comparison, 'viewmodel_arms'))} "
            f"| {evidence} |"
        )
    add("")
    para(
        f"The {NUMBER_WORDS[len(VARIANTS)]} raw OpenNova variants in every row are "
        + oxford([f"`{name}`" for name in VARIANTS]) + "."
    )
    add("")
    add("## Honest visual residuals")
    add("")
    add(RESIDUALS.rstrip("\n"))
    add("")
    para(
        "This package supports qualitative scene-by-scene review. It does not "
        "claim pixel parity or complete water, sky, particles, vegetation, "
        "lighting, post-processing, or renderer parity."
    )
    add("")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--catalog", required=True, type=Path)
    parser.add_argument("--opennova-root", required=True, type=Path)
    parser.add_argument("--retail-root", required=True, type=Path)
    parser.add_argument("--comparison-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument(
        "--source-notes",
        type=Path,
        help=(
            "optional markdown fragment describing what this OpenNova source "
            "carries, inserted verbatim after the retail paragraph"
        ),
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="replace an existing output directory instead of refusing",
    )
    args = parser.parse_args(argv)

    catalog_path: Path = args.catalog
    catalog = load_json(catalog_path)
    catalog_sha = catalog["catalog_sha256"]
    fixture_ids = sorted(
        (entry["id"] for entry in catalog["fixtures"]), key=sort_key
    )

    output_root: Path = args.output
    if output_root.exists():
        if not args.force:
            raise PublishError(
                f"{output_root} already exists; pass --force to replace it"
            )
        shutil.rmtree(output_root)
    output_root.mkdir(parents=True)

    rows = [
        collect_fixture(
            fixture_id,
            args.opennova_root,
            args.retail_root,
            args.comparison_root,
            output_root,
        )
        for fixture_id in fixture_ids
    ]

    for row in rows:
        declared = row["registered"]["catalog_sha256"]
        if declared != catalog_sha:
            raise PublishError(
                f"{row['id']}: registration names catalog {declared}, "
                f"catalog file is {catalog_sha}"
            )
        declared = row["manifest"]["catalog_sha256"]
        if declared != catalog_sha:
            raise PublishError(
                f"{row['id']}: OpenNova manifest names catalog {declared}, "
                f"catalog file is {catalog_sha}"
            )

    source_notes = None
    if args.source_notes is not None:
        if not args.source_notes.is_file():
            raise PublishError(f"missing --source-notes file: {args.source_notes}")
        source_notes = args.source_notes.read_text(encoding="utf-8")

    readme = render_readme(rows, catalog_sha, output_root, source_notes)
    (output_root / "README.md").write_text(readme, encoding="utf-8", newline="\n")

    total = sum(1 for path in output_root.rglob("*") if path.is_file())
    print(
        f"published {len(rows)} fixtures / {total} files to {output_root}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except PublishError as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(2) from error
