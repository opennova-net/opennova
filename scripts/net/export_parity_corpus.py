#!/usr/bin/env python3
"""Create or verify parity inputs from the engine-faithful retail VFS.

The wire verifier classifies item/ammo rows and binds mission bytes.  Those
inputs must be the files retail actually resolves after mounting its expansion,
not similarly named loose copies from another install.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any

from pyopennova.vfs_ffi import Vfs


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def parse_expected_archive(value: str) -> tuple[str, Path]:
    if "=" not in value:
        raise argparse.ArgumentTypeError("expected archive must be LOGICAL=RELATIVE/PATH.pff")
    logical, relative = value.split("=", 1)
    if not logical or not relative:
        raise argparse.ArgumentTypeError("expected archive must have non-empty sides")
    path = Path(relative)
    if path.is_absolute():
        raise argparse.ArgumentTypeError("expected archive path must be relative to each game root")
    return logical.casefold(), path


def mounted_files(root: Path, expansion: str, logical_names: list[str]) -> dict[str, dict[str, Any]]:
    wanted = {name.casefold(): name for name in logical_names}
    with Vfs() as vfs:
        if not vfs.mount_game(str(root), expansion):
            raise RuntimeError(f"could not mount {root}: {vfs.last_error()}")
        resolution: dict[str, tuple[str, str]] = {}
        for index in range(vfs.file_count()):
            row = vfs.file_at(index)
            if row is None:
                continue
            logical, source, archive = row
            key = logical.casefold()
            if key in wanted:
                resolution[key] = (source, archive)
        result: dict[str, dict[str, Any]] = {}
        for requested in logical_names:
            key = requested.casefold()
            data = vfs.read_file(requested)
            if data is None:
                raise RuntimeError(f"{root} does not resolve required VFS file {requested}")
            source, archive = resolution.get(key, ("", ""))
            if not source:
                raise RuntimeError(f"{root} read {requested} but exposed no resolution owner")
            archive_relative = ""
            if archive:
                try:
                    archive_relative = Path(archive).resolve().relative_to(root.resolve()).as_posix()
                except ValueError as exc:
                    raise RuntimeError(
                        f"{root} resolves {requested} through archive outside its game root: {archive}"
                    ) from exc
            result[key] = {
                "logical_name": requested,
                "source": source,
                "archive": archive_relative,
                "size": len(data),
                "sha256": sha256(data),
                "bytes": data,
            }
        return result


def destination(output: Path, logical_name: str, mission_names: set[str]) -> Path:
    if logical_name.casefold() in mission_names:
        return output / "missions" / logical_name
    return output / logical_name.casefold()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server-root", type=Path, required=True)
    parser.add_argument("--client-root", type=Path, required=True)
    parser.add_argument("--expansion", required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--mode", choices=("create", "verify"), required=True)
    parser.add_argument("--mission", action="append", default=[], required=True)
    parser.add_argument(
        "--expect-archive",
        action="append",
        default=[],
        type=parse_expected_archive,
        metavar="LOGICAL=RELATIVE/PATH.pff",
    )
    args = parser.parse_args()

    server_root = args.server_root.resolve(strict=True)
    client_root = args.client_root.resolve(strict=True)
    output = args.output_dir.resolve()
    missions = list(dict.fromkeys(args.mission))
    logical_names = ["items.def", "ammo.def", *missions]
    if len({name.casefold() for name in logical_names}) != len(logical_names):
        raise RuntimeError("logical parity corpus names must be unique case-insensitively")
    expected_archives = dict(args.expect_archive)
    unknown_expectations = sorted(set(expected_archives) - {name.casefold() for name in logical_names})
    if unknown_expectations:
        raise RuntimeError(f"archive expectations name unknown files: {unknown_expectations}")

    server = mounted_files(server_root, args.expansion, logical_names)
    client = mounted_files(client_root, args.expansion, logical_names)
    mission_keys = {name.casefold() for name in missions}
    report_files: list[dict[str, Any]] = []
    for logical in logical_names:
        key = logical.casefold()
        left = server[key]
        right = client[key]
        for field in ("source", "archive", "size", "sha256"):
            if left[field] != right[field]:
                raise RuntimeError(
                    f"SERVER/CLIENT mounted corpus differs for {logical} field {field}: "
                    f"{left[field]!r} != {right[field]!r}"
                )
        if left["bytes"] != right["bytes"]:
            raise RuntimeError(f"SERVER/CLIENT mounted bytes differ for {logical}")
        if key in expected_archives:
            expected = expected_archives[key].as_posix().casefold()
            if left["source"] != "pff" or left["archive"].casefold() != expected:
                raise RuntimeError(
                    f"{logical} resolved from {left['source']}:{left['archive']}, expected pff:{expected}"
                )
        path = destination(output, logical, mission_keys)
        if args.mode == "create":
            if path.exists():
                raise RuntimeError(f"create-new corpus destination already exists: {path}")
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(left["bytes"])
        else:
            if not path.is_file():
                raise RuntimeError(f"verified corpus file is missing: {path}")
            existing = path.read_bytes()
            if existing != left["bytes"]:
                raise RuntimeError(
                    f"verified corpus file is not the mounted {logical}: "
                    f"actual={sha256(existing)} mounted={left['sha256']}"
                )
        report_files.append(
            {
                "logical_name": logical,
                "path": path.as_posix(),
                "source": left["source"],
                "archive": left["archive"],
                "size": left["size"],
                "sha256": left["sha256"],
            }
        )

    print(
        json.dumps(
            {
                "schema": 1,
                "expansion": args.expansion,
                "server_root": server_root.as_posix(),
                "client_root": client_root.as_posix(),
                "mode": args.mode,
                "files": report_files,
            },
            sort_keys=True,
            separators=(",", ":"),
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
