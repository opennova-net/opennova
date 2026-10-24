#!/usr/bin/env python3
"""Exercise the shipped native binaries after extracting their actual archive."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tarfile
import tempfile
import tomllib
import zipfile


def run(*args, code=0, quiet=False):
    result = subprocess.run([str(arg) for arg in args], capture_output=True, text=True,
                            encoding="utf-8", errors="replace")
    if not quiet or result.returncode != code:
        print(result.stdout, end="")
        print(result.stderr, end="")
    if result.returncode != code:
        raise RuntimeError(f"{args[0]} exited {result.returncode}, expected {code}")
    return result.stdout


def dependencies(binary, platform):
    if platform == "windows":
        output = run("dumpbin", "/DEPENDENTS", binary)
        if re.search(r"\b(?:MSVCP|VCRUNTIME)\w*\.dll\b", output, re.I):
            raise RuntimeError(f"{binary.name} needs an unbundled Visual C++ runtime")
    else:
        if not os.access(binary, os.X_OK):
            raise RuntimeError(f"{binary.name} lost its executable permission")
        header = run("readelf", "-h", binary, quiet=True)
        if "Advanced Micro Devices X86-64" not in header:
            raise RuntimeError(f"{binary.name} is not Linux x86_64")
        if "not found" in run("ldd", binary):
            raise RuntimeError(f"{binary.name} has an unresolved dependency")
        versions = run("readelf", "--version-info", binary, quiet=True)
        if any(tuple(map(int, version.split("."))) > (2, 39)
               for version in re.findall(r"\bGLIBC_([0-9.]+)", versions)):
            raise RuntimeError(f"{binary.name} needs glibc newer than Ubuntu 24.04")


def smoke_apps(folder, platform):
    suffix = ".exe" if platform == "windows" else ""
    clis = {
        "opennova-3di": ([], 2),
        "opennova-extract": (["--help"], 2),
        "opennova-lan-probe": (["--help"], 0),
        "opennova-parity": ([], 2),
        "opennova-serve": (["--help"], 0),
        "opennova-update-tool": ([], 2),
        "opennova-wire": ([], 1),
    }
    expected = {name + suffix for name in clis} | {"README.txt", "example.host"}
    shipped = {path.relative_to(folder).as_posix() for path in folder.rglob("*")}
    if shipped != expected:
        raise RuntimeError(f"Package inventory differs: {shipped ^ expected}")
    for name, (args, code) in clis.items():
        binary = folder / (name + suffix)
        dependencies(binary, platform)
        run(binary, *args, code=code)
    print(f"Verified {len(clis)} packaged {platform} apps")


def smoke_blender(folder, platform):
    addon = folder / "opennova_3di"
    manifest = tomllib.loads((addon / "blender_manifest.toml").read_text(encoding="utf-8"))
    if manifest["platforms"] != [platform + "-x64"]:
        raise RuntimeError("Packaged manifest must name only the binary's platform")
    binary = addon / "bin" / ("opennova-3di.exe" if platform == "windows" else "opennova-3di")
    if list((addon / "bin").iterdir()) != [binary]:
        raise RuntimeError("The add-on must contain exactly its platform's CLI")
    dependencies(binary, platform)
    first, text, second = [folder / name for name in ("spinner.3di", "spinner.o3d", "roundtrip.3di")]
    run(binary, "build", "fixtures/threedi/o3d/spinner.o3d", "-o", first)
    run(binary, "scene", first, "-o", text)
    run(binary, "build", text, "-o", second)
    run(binary, "compare", first, second)
    preview = json.loads(run(binary, "weapon", "timing", "fixtures/anim/weapon_timing.txt",
                             "-o", folder / "weapon_actions.txt"))
    if preview["entries"][0]["cycle_ticks"] != 5 or preview["entries"][0]["rpm"] != 750:
        raise RuntimeError("Packaged weapon timing disagrees with the native FSM")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("product", choices=["apps", "blender"])
    parser.add_argument("platform", choices=["windows", "linux"])
    parser.add_argument("archive", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="opennova-package-") as scratch:
        folder = Path(scratch)
        if args.archive.name.endswith(".tar.gz"):
            with tarfile.open(args.archive) as archive:
                archive.extractall(folder, filter="data")
        else:
            with zipfile.ZipFile(args.archive) as archive:
                archive.extractall(folder)
                # This checks the archive's stored permissions. Blender's own
                # installer deliberately receives the original zip separately.
                if args.platform == "linux":
                    for entry in archive.infolist():
                        if not entry.is_dir():
                            (folder / entry.filename).chmod((entry.external_attr >> 16) & 0o777)
        (smoke_apps if args.product == "apps" else smoke_blender)(folder, args.platform)


if __name__ == "__main__":
    main()
