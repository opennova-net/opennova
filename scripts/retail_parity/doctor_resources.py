#!/usr/bin/env python3
"""Fail-closed retail executable and packed mission checks for retail_parity.sh."""

from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path


PFF_MAGICS = {0x33464650, 0x34464650, 0x34460001}
PFF_HEADER = struct.Struct("<IIIII")
PFF_ENTRY = struct.Struct("<IIII16sI")
MAX_PFF_ENTRIES = 1_000_000


def emit(check: str, status: str, **fields: object) -> None:
    suffix = "".join(f" {name}={value}" for name, value in fields.items())
    print(f"CHECK={check} STATUS={status}{suffix}")


def find_case_insensitive(directory: Path, filename: str) -> Path | None:
    if not directory.is_dir():
        return None
    wanted = filename.casefold()
    try:
        for child in directory.iterdir():
            if child.name.casefold() == wanted:
                return child
    except OSError:
        return None
    return None


def pff_entry_names(path: Path) -> tuple[set[str] | None, str]:
    try:
        file_size = path.stat().st_size
        with path.open("rb") as stream:
            raw_header = stream.read(PFF_HEADER.size)
            if len(raw_header) != PFF_HEADER.size:
                return None, "short-header"
            _header_size, magic, count, entry_size, table_offset = PFF_HEADER.unpack(raw_header)
            if magic not in PFF_MAGICS:
                return None, "bad-magic"
            if entry_size != PFF_ENTRY.size:
                return None, "bad-entry-size"
            if count > MAX_PFF_ENTRIES:
                return None, "implausible-entry-count"
            table_size = count * PFF_ENTRY.size
            if table_offset > file_size or table_size > file_size - table_offset:
                return None, "truncated-table"
            stream.seek(table_offset)
            names: set[str] = set()
            for _ in range(count):
                raw_entry = stream.read(PFF_ENTRY.size)
                if len(raw_entry) != PFF_ENTRY.size:
                    return None, "truncated-entry"
                filename_bytes = PFF_ENTRY.unpack(raw_entry)[4]
                logical_name = filename_bytes.split(b"\0", 1)[0].rstrip(b" ")
                if logical_name:
                    names.add(logical_name.decode("ascii", errors="replace").casefold())
            return names, ""
    except OSError as exc:
        return None, f"io-{exc.__class__.__name__}"


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def check_install(game_root: Path, expected_sha: str, expansion: str, mission: str) -> int:
    failed = False
    executable = find_case_insensitive(game_root, "Jointops.exe")
    if executable is None or not executable.is_file():
        emit("JO_EXECUTABLE_SHA256", "missing", EXPECTED=expected_sha)
        failed = True
    else:
        try:
            actual_sha = sha256_file(executable)
        except OSError as exc:
            emit(
                "JO_EXECUTABLE_SHA256",
                "unreadable",
                EXPECTED=expected_sha,
                DETAIL=exc.__class__.__name__,
            )
            failed = True
        else:
            status = "ok" if actual_sha.casefold() == expected_sha.casefold() else "mismatch"
            emit(
                "JO_EXECUTABLE_SHA256",
                status,
                EXPECTED=expected_sha.casefold(),
                ACTUAL=actual_sha,
            )
            failed |= status != "ok"

    expansion_root = game_root / "expansion" / expansion
    main_archive = find_case_insensitive(expansion_root, f"{expansion}.pff")
    main_names: set[str] | None = None
    if not expansion_root.is_dir():
        emit("EXPANSION", "missing-root", NAME=expansion, ROOT=expansion_root)
        failed = True
    elif main_archive is None or not main_archive.is_file():
        emit("EXPANSION", "missing-archive", NAME=expansion, ROOT=expansion_root)
        failed = True
    else:
        main_names, archive_error = pff_entry_names(main_archive)
        if main_names is None:
            emit(
                "EXPANSION",
                "invalid-archive",
                NAME=expansion,
                ROOT=expansion_root,
                ARCHIVE=main_archive.name,
                DETAIL=archive_error,
            )
            failed = True
        else:
            emit(
                "EXPANSION",
                "ok",
                NAME=expansion,
                ROOT=expansion_root,
                ARCHIVE=main_archive.name,
            )

    archives: list[tuple[Path, set[str]]] = []
    local_archive = find_case_insensitive(expansion_root, f"{expansion}L.pff")
    if local_archive is not None and local_archive.is_file():
        local_names, _local_error = pff_entry_names(local_archive)
        if local_names is not None:
            archives.append((local_archive, local_names))
    if main_archive is not None and main_names is not None:
        archives.append((main_archive, main_names))
    for base_name in ("language.pff", "localres.pff", "resource.pff"):
        archive = find_case_insensitive(game_root, base_name)
        if archive is None or not archive.is_file():
            continue
        names, _archive_error = pff_entry_names(archive)
        if names is not None:
            archives.append((archive, names))

    mission_key = Path(mission).name.casefold()
    mission_archive = next(
        (archive for archive, names in archives if mission_key in names),
        None,
    )
    if mission_archive is not None:
        emit(
            "MISSION",
            "ok",
            NAME=Path(mission).name,
            SOURCE="packed",
            ARCHIVE=mission_archive.name,
            PATH=mission_archive,
        )
    else:
        loose_expansion = find_case_insensitive(expansion_root, Path(mission).name)
        loose_base = find_case_insensitive(game_root, Path(mission).name)
        loose = loose_expansion or loose_base
        if loose is not None:
            emit(
                "MISSION",
                "missing",
                NAME=Path(mission).name,
                SOURCE="packed",
                DETAIL="loose-only-requires-/d",
            )
        else:
            emit("MISSION", "missing", NAME=Path(mission).name, SOURCE="packed")
        failed = True

    return 1 if failed else 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--game-root", required=True, type=Path)
    parser.add_argument("--expected-sha", required=True)
    parser.add_argument("--expansion", default="revx02")
    parser.add_argument("--mission", required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    return check_install(
        args.game_root,
        args.expected_sha,
        args.expansion,
        args.mission,
    )


if __name__ == "__main__":
    raise SystemExit(main())
