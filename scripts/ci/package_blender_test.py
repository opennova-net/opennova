#!/usr/bin/env python3
"""Download verified Blender, then test the add-on as an installed extension."""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import urllib.error
import urllib.request
import zipfile

VERSION = "5.1.2"
PACKAGES = {
    "windows": ("zip", "345bedea7b0acf7cc9666423d8553f9129622aea34ded65c23e8cb70f83f14ff"),
    "linux": ("tar.xz", "aaccb355f50183979b698bcce7467103a76261b5fa59f4972295842662a285fb"),
}
MIRRORS = (
    "https://download.blender.org/release/Blender5.1",
    "https://ftp.nluug.nl/pub/graphics/blender/release/Blender5.1",
    "https://mirrors.ocf.berkeley.edu/blender/release/Blender5.1",
)


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def download(platform):
    extension, checksum = PACKAGES[platform]
    path = Path("build/blender-download") / f"blender-{VERSION}-{platform}-x64.{extension}"
    if path.is_file() and digest(path) == checksum:
        return path
    path.parent.mkdir(parents=True, exist_ok=True)
    partial = path.with_suffix(path.suffix + ".partial")
    for mirror in MIRRORS:
        try:
            with urllib.request.urlopen(f"{mirror}/{path.name}", timeout=60) as response, partial.open("wb") as output:
                while block := response.read(1024 * 1024):
                    output.write(block)
            if digest(partial) == checksum:
                partial.replace(path)
                return path
            print(f"Checksum mismatch: {mirror}/{path.name}")
        except (OSError, urllib.error.URLError) as error:
            print(f"Download failed: {mirror}/{path.name}: {error}")
        partial.unlink(missing_ok=True)
    raise RuntimeError(f"Could not obtain verified {path.name}")


def exercise_installed(platform, archive, root):
    if platform == "windows":
        with zipfile.ZipFile(archive) as package:
            package.extractall(root)
    else:
        with tarfile.open(archive) as package:
            package.extractall(root, filter="data")
    blender = root / f"blender-{VERSION}-{platform}-x64" / ("blender.exe" if platform == "windows" else "blender")
    environment = dict(os.environ, BLENDER_USER_RESOURCES=str(root / "user"))

    def run(*arguments):
        subprocess.run([str(blender), "--background", "--factory-startup", *map(str, arguments)],
                       env=environment, check=True)

    run("--command", "extension", "install-file", "-r", "user_default", "-e",
        f"dist/opennova-blender-addon-{platform}.zip")
    # No shell chmod: this exercises the installed add-on's permission repair.
    run("--python-exit-code", "1", "--python", "scripts/ci/smoke_installed_blender.py")
    tests = sorted(Path("tests/blender").glob("*_test.py"))
    if not tests:
        raise RuntimeError("No Blender tests found")
    for test in tests:
        print(f"Testing installed add-on: {test.name}", flush=True)
        run("--python-exit-code", "1", "--python", test, "--", "--installed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("platform", choices=PACKAGES)
    parser.add_argument("--download-only", action="store_true")
    args = parser.parse_args()
    archive = download(args.platform)
    if not args.download_only:
        # Fresh extraction and user resources exercise the install from scratch.
        # On WSL this also keeps POSIX mode checks on the native filesystem.
        with tempfile.TemporaryDirectory(prefix="opennova-blender-") as scratch:
            exercise_installed(args.platform, archive, Path(scratch))


if __name__ == "__main__":
    main()
