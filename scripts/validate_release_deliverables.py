from __future__ import annotations

import argparse
from collections.abc import Collection
import re
import shutil
import sys
import tomllib
from pathlib import Path
import zipfile


class DeliverableValidationError(RuntimeError):
    pass


class DeliverableItem:
    def __init__(
        self,
        *,
        id: str,
        title: str,
        source_name: str,
        public_name: str,
        staged_path: Path,
    ) -> None:
        self.id = id
        self.title = title
        self.source_name = source_name
        self.public_name = public_name
        self.staged_path = staged_path


class DeliverableValidationResult:
    def __init__(self, items: list[DeliverableItem]) -> None:
        self.items = items


def _read_toml(path: Path) -> dict:
    with path.open("rb") as file:
        return tomllib.load(file)


def _read_godot_version(path: Path) -> str:
    text = path.read_text(encoding="utf-8")
    match = re.search(r'(?m)^config/version="([^"]+)"', text)
    if not match:
        raise DeliverableValidationError(f"Could not read Godot version from {path}")
    return match.group(1)


def _release_version(value: str) -> str:
    text = value.strip()
    if not text:
        raise DeliverableValidationError("Release version cannot be empty")
    return text if text.startswith("v") else f"v{text}"


def _version_context(repo_root: Path, release_version: str) -> dict[str, str]:
    pyproject = _read_toml(repo_root / "pyproject.toml")
    blender_manifest = _read_toml(repo_root / "blender" / "blender_manifest.toml")
    godot_version = _read_godot_version(repo_root / "godot" / "project.godot")
    return {
        "release_version": _release_version(release_version),
        "python_version": pyproject["project"]["version"],
        "blender_version": blender_manifest["version"],
        "godot_version": godot_version,
    }


def _format_template(value: str, context: dict[str, str]) -> str:
    try:
        return value.format(**context)
    except KeyError as exc:
        raise DeliverableValidationError(f"Unknown template variable {exc} in {value}") from exc


def _manifest_deliverables(manifest_path: Path, context: dict[str, str]) -> list[dict]:
    manifest = _read_toml(manifest_path)
    deliverables = manifest.get("deliverables", [])
    if not deliverables:
        raise DeliverableValidationError(f"No deliverables defined in {manifest_path}")

    rendered = []
    for item in deliverables:
        rendered_item = dict(item)
        rendered_item["source"] = _format_template(item["source"], context)
        rendered_item["public"] = _format_template(item["public"], context)
        rendered_item["required_entries"] = [
            _format_template(entry, context)
            for entry in item.get("required_entries", [])
        ]
        rendered.append(rendered_item)
    return rendered


def _assert_nonempty(path: Path) -> None:
    if not path.is_file():
        raise DeliverableValidationError(f"Missing deliverable source: {path.name}")
    if path.stat().st_size <= 0:
        raise DeliverableValidationError(f"Deliverable is empty: {path.name}")


def _validate_exe(path: Path) -> None:
    _assert_nonempty(path)
    with path.open("rb") as file:
        if file.read(2) != b"MZ":
            raise DeliverableValidationError(f"Windows exe missing MZ header: {path.name}")


def _normalize_archive_name(name: str) -> str:
    normalized = name.replace("\\", "/")
    while normalized.startswith("./"):
        normalized = normalized[2:]
    return normalized


def _validate_archive(path: Path, required_entries: list[str]) -> None:
    _assert_nonempty(path)
    if not zipfile.is_zipfile(path):
        raise DeliverableValidationError(f"Archive is not a valid zip file: {path.name}")

    with zipfile.ZipFile(path) as archive:
        names = {_normalize_archive_name(name) for name in archive.namelist()}

    missing = [entry for entry in required_entries if entry not in names]
    if missing:
        raise DeliverableValidationError(
            f"{path.name} is missing required entries: {', '.join(missing)}"
        )


def _validate_source(path: Path, item: dict) -> None:
    kind = item["kind"]
    if kind == "exe":
        _validate_exe(path)
    elif kind in {"zip", "mzp"}:
        _validate_archive(path, item.get("required_entries", []))
    else:
        raise DeliverableValidationError(f"Unknown deliverable kind for {item['id']}: {kind}")


def _assert_exact_dist_files(dist_dir: Path, expected_names: set[str]) -> None:
    actual_names = {path.name for path in dist_dir.iterdir() if path.is_file()}
    missing = sorted(expected_names - actual_names)
    unexpected = sorted(actual_names - expected_names)
    if missing:
        raise DeliverableValidationError(f"Missing files in dist: {', '.join(missing)}")
    if unexpected:
        raise DeliverableValidationError(f"Unexpected files in dist: {', '.join(unexpected)}")


def _reset_stage_dir(stage_dir: Path) -> None:
    if stage_dir.exists():
        shutil.rmtree(stage_dir)
    stage_dir.mkdir(parents=True)


def _release_body(items: list[dict], context: dict[str, str]) -> str:
    lines = [
        "## Release Assets",
        "",
        "| Asset | Platform | Purpose |",
        "|---|---|---|",
    ]
    for item in items:
        lines.append(f"| `{item['public']}` | {item['platform']} | {item['purpose']} |")

    lines.extend(
        [
            "",
            "## Install and Use",
            "",
        ]
    )
    for item in items:
        lines.extend(
            [
                f"### `{item['public']}`",
                "",
                f"Purpose: {item['purpose']}",
                "",
                f"Install: {item['install']}",
                "",
                f"Use: {item['use']}",
                "",
            ]
        )

    lines.extend(
        [
            "## Component Versions",
            "",
            "| Component | Version |",
            "|---|---|",
            f"| Release assets | {context['release_version']} |",
            f"| Blender addon | {context['blender_version']} |",
            f"| Python tools / importer / Max exporter | {context['python_version']} |",
            f"| Godot apps | {context['godot_version']} |",
            "",
        ]
    )
    return "\n".join(lines)


def validate_release_deliverables(
    *,
    repo_root: str | Path,
    dist_dir: str | Path,
    stage_dir: str | Path,
    release_body: str | Path,
    release_version: str,
    manifest_path: str | Path | None = None,
    deliverable_ids: Collection[str] | None = None,
) -> DeliverableValidationResult:
    root = Path(repo_root)
    dist = Path(dist_dir)
    stage = Path(stage_dir)
    body_path = Path(release_body)
    manifest = Path(manifest_path) if manifest_path is not None else root / "release" / "deliverables.toml"

    if not dist.is_dir():
        raise DeliverableValidationError(f"dist directory does not exist: {dist}")

    context = _version_context(root, release_version)
    deliverables = _manifest_deliverables(manifest, context)

    if deliverable_ids is not None:
        requested_ids = list(deliverable_ids)
        if not requested_ids:
            raise DeliverableValidationError("At least one deliverable ID must be selected")

        seen_ids: set[str] = set()
        duplicate_ids: list[str] = []
        for deliverable_id in requested_ids:
            if deliverable_id in seen_ids and deliverable_id not in duplicate_ids:
                duplicate_ids.append(deliverable_id)
            seen_ids.add(deliverable_id)
        if duplicate_ids:
            raise DeliverableValidationError(
                f"Duplicate deliverable IDs: {', '.join(duplicate_ids)}"
            )

        known_ids = {item["id"] for item in deliverables}
        unknown_ids = [
            deliverable_id for deliverable_id in requested_ids
            if deliverable_id not in known_ids
        ]
        if unknown_ids:
            raise DeliverableValidationError(
                f"Unknown deliverable IDs: {', '.join(unknown_ids)}"
            )

        requested_id_set = set(requested_ids)
        deliverables = [
            item for item in deliverables
            if item["id"] in requested_id_set
        ]

    expected_sources = {item["source"] for item in deliverables}
    _assert_exact_dist_files(dist, expected_sources)

    for item in deliverables:
        _validate_source(dist / item["source"], item)

    _reset_stage_dir(stage)
    result_items = []
    for item in deliverables:
        source = dist / item["source"]
        destination = stage / item["public"]
        shutil.copy2(source, destination)
        result_items.append(
            DeliverableItem(
                id=item["id"],
                title=item["title"],
                source_name=item["source"],
                public_name=item["public"],
                staged_path=destination,
            )
        )

    body_path.parent.mkdir(parents=True, exist_ok=True)
    body_path.write_text(_release_body(deliverables, context), encoding="utf-8")
    return DeliverableValidationResult(result_items)


def _parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Validate and stage OpenNova release deliverables.")
    parser.add_argument("--repo-root", default=Path(__file__).resolve().parents[1])
    parser.add_argument("--dist", default="dist")
    parser.add_argument("--stage-dir", default="dist/release-assets")
    parser.add_argument("--release-body", default="release-body.md")
    parser.add_argument("--release-version", required=True)
    parser.add_argument("--manifest", default=None)
    parser.add_argument(
        "--only-id",
        action="append",
        dest="deliverable_ids",
        metavar="ID",
        help="Validate only this manifest deliverable ID; may be repeated.",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = _parse_args(sys.argv[1:] if argv is None else argv)
    try:
        result = validate_release_deliverables(
            repo_root=Path(args.repo_root),
            dist_dir=Path(args.dist),
            stage_dir=Path(args.stage_dir),
            release_body=Path(args.release_body),
            release_version=args.release_version,
            manifest_path=Path(args.manifest) if args.manifest else None,
            deliverable_ids=args.deliverable_ids,
        )
    except DeliverableValidationError as exc:
        print(f"release deliverable validation failed: {exc}", file=sys.stderr)
        return 1

    print("Validated release deliverables:")
    for item in result.items:
        print(f"  {item.source_name} -> {item.staged_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
