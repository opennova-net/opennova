#!/usr/bin/env python3
"""Lay out a downloads host's files: each build under its stable name, and manifest.json.

The downloads host (ADR 0070; DEPLOY.md, "The downloads host") serves the current set alone, each
file under a name without its version, so the site's download buttons never change:

  python scripts/stage_downloads.py --from dist --to stage --version v0.0.21 \
      --source https://github.com/opennova-net/opennova/releases/tag/v0.0.21

--from holds a tagged release's assets (opennova-game-windows-v0.0.21.zip) or a CI run's
artifacts (opennova-game-windows-v0.0.20.zip, opennova-apps-windows.zip); every build the host
serves must be there, and anything else (the runtime's update package, a checksum file) is left
out. --to gets the stable names and manifest.json: the version, when it was staged, the source,
and each file's size and SHA-256. .github/workflows/publish.yml uploads --to as it is.
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import re
import shutil
import sys
from pathlib import Path

# The builds a downloads host serves, by their stable names (the site's download buttons name
# these): the three game zips, the editor, the Blender add-on, the apps and the Switcher.
BUILDS = (
    "opennova-game-windows.zip",
    "jointops-game-windows.zip",
    "dfx2-game-windows.zip",
    "opennova-editor-windows.zip",
    "opennova-blender-addon-windows.zip",
    "opennova-apps-windows.zip",
    "opennova-switcher-windows.zip",
)

# A build's name with its version: <stable stem>-v<digits and dots>.zip, as a release tag or
# the game's project version names it.
VERSIONED = re.compile(r"^(?P<stem>.+)-v\d+(?:\.\d+)*\.zip$")


class StageError(RuntimeError):
    pass


def stable_name(name: str) -> str:
    """The stable name of a build's file: its version taken out."""
    match = VERSIONED.match(name)
    return match.group("stem") + ".zip" if match else name


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def stage(source_dir: Path, out_dir: Path, version: str, source: str, now: datetime.datetime) -> dict:
    found: dict[str, Path] = {}
    for path in sorted(source_dir.iterdir()):
        if not path.is_file():
            continue
        name = stable_name(path.name)
        if name not in BUILDS:
            continue
        if name in found:
            raise StageError(f"two files are {name}: {found[name].name} and {path.name}")
        found[name] = path
    missing = [name for name in BUILDS if name not in found]
    if missing:
        raise StageError("missing " + ", ".join(missing) + f" in {source_dir}")
    out_dir.mkdir(parents=True, exist_ok=True)
    files = {}
    for name in BUILDS:
        target = out_dir / name
        shutil.copyfile(found[name], target)
        files[name] = {"size": target.stat().st_size, "sha256": sha256_of(target)}
    manifest = {
        "version": version,
        "published": now.strftime("%Y-%m-%dT%H:%M:%SZ"),
        "source": source,
        "files": files,
    }
    (out_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--from", dest="source_dir", required=True, type=Path)
    parser.add_argument("--to", dest="out_dir", required=True, type=Path)
    parser.add_argument("--version", required=True, help="what the site shows: v0.0.21, or a pull request's build")
    parser.add_argument("--source", required=True, help="where the builds came from (a release's or a run's URL)")
    args = parser.parse_args(argv)
    try:
        manifest = stage(args.source_dir, args.out_dir, args.version, args.source,
                         datetime.datetime.now(datetime.timezone.utc))
    except (StageError, OSError) as error:
        print(f"stage_downloads: {error}", file=sys.stderr)
        return 1
    for name, file in manifest["files"].items():
        print(f"{name}: {file['size']} bytes, sha256 {file['sha256']}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
