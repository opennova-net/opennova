#!/usr/bin/env python3
"""Build one product's update package from its OpenNova Editor project, for upload in the admin.

Game data is never built or shipped by CI (ADR 0067): each product's project lives on the
maintainer's disk, and its release is this script's package, uploaded in the admin's Game data
(Releases: upload, validate, Publish) or with publish_update.py.

  Our own game, a full package carrying the last published one's removals:
    python scripts/package_release.py --product ACIDGAMBIT --project PROJECTS/ACIDGAMBIT \
        --version 0.2.0 --previous PROJECTS/RELEASES/ACIDGAMBIT-0.1.0.sfs
  A retail game's patch, only what the project changed against the stock install:
    python scripts/package_release.py --product JO --project PROJECTS/JO \
        --install "D:/SteamLibrary/steamapps/common/Joint Operations Combined Arms" --version 1.7.5.8

A retail patch is the difference between two exports by our own writer: the edited project's, and
a fresh import of the stock install's. The package builder compares archive entries as stored, and
our archives store entries otherwise than the stock ones do, so a diff against the stock install
itself would rewrite every entry. Only the files the project changed get rows, each into the
archive the stock install holds it in (the import's archive record); an unedited project gives a
package of no rows, which the stock updater accepts. An expansion's patch exports its base game's
project first, and its pristine import stands on a pristine base.

The version and the product's name go into the command file's header (--version, the product's
name); no version file is read or written. The package and its inspection report land in --out
(default: RELEASES beside the project), named <PRODUCT>-<version>.sfs.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


@dataclass(frozen=True)
class Product:
    """A product the update service serves, as its data set is seeded (backend/migrations 0011, 0013)."""

    key: str
    login: str
    command_file: str
    name: str  # the command file's PRODUCT, and the package's registry name
    exe: str  # the command file's EXE, launched at DONE under /RUN ("" for an expansion)
    retail: bool  # a difference package against the stock install, else a full package
    game: str = ""  # the project's target game, for a pristine import
    expansion: str = ""  # its expansion code ("" for a base game)
    base: str = ""  # an expansion's base product


PRODUCTS = {
    p.key: p
    for p in (
        Product("JO", "JOTRus", "JOPTR.CMD", "Joint Operations", "Jointops.exe", True, game="jo"),
        Product(
            "JOX01",
            "JOEus",
            "JOEPTR.CMD",
            "Joint Operations: Escalation",
            "",
            True,
            game="jo",
            expansion="jox01",
            base="JO",
        ),
        Product("DFX2", "DFX2", "DFX2.CMD", "Delta Force Xtreme 2", "dfx2.exe", True, game="dfx2"),
        Product("ACIDGAMBIT", "ACIDGAMBIT", "ACIDGAMBIT.CMD", "OP: Acid Gambit", "opennova.exe", False),
        Product("NIFTY", "NIFTY", "NIFTY.CMD", "OP: Nifty Package", "", False, expansion="nifty"),
    )
}


def run(args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, **kwargs)


def quiet(args):
    """A tool run whose findings are printed only when it fails."""
    done = subprocess.run([str(a) for a in args], capture_output=True)
    if done.returncode != 0:
        sys.stderr.write(
            done.stdout.decode(errors="replace")[-4000:]
            + done.stderr.decode(errors="replace")[-4000:]
        )
        raise subprocess.CalledProcessError(done.returncode, done.args)
    return done


def default_tool(name: str) -> Path:
    exe = name + (".exe" if sys.platform == "win32" else "")
    for candidate in (
        ROOT / "build/apps" / name.split("-", 1)[1].replace("-", "_") / "Release" / exe,
        ROOT / "build/apps" / name.split("-", 1)[1].replace("-", "_") / exe,
    ):
        if candidate.is_file():
            return candidate
    return Path(exe)


def project_document(project: Path) -> dict:
    try:
        return json.loads((project / "project.opennova").read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise ValueError(f"{project} is no OpenNova Editor project ({error})") from error


def export(tool: Path, project: Path, out: Path) -> Path:
    quiet([tool, "export", project, "--out", out])
    return out


def base_project_of(project: Path) -> Path:
    base = project_document(project).get("expansion", {}).get("base_project", "")
    if not base:
        raise ValueError(
            f"{project} builds on no base game's project (its expansion's base_project)"
        )
    return (project / base).resolve()


def pristine_export(tool: Path, product: Product, install: Path, work: Path) -> Path:
    """The stock install imported afresh and exported: what a stock player holds, in our writer's form."""
    base = PRODUCTS[product.base] if product.base else product
    base_dir = work / "pristine" / base.key
    quiet([tool, "new", base_dir, "--title", base.name, "--game", base.game, "--install", install])
    quiet([tool, "import", base_dir, "--all", "--install", install])
    if not product.base:
        return export(tool, base_dir, work / "pristine-export")
    quiet([tool, "export", base_dir])  # the expansion's base, at its own output
    exp_dir = work / "pristine" / product.key
    quiet(
        [
            tool,
            "new",
            exp_dir,
            "--title",
            product.name,
            "--game",
            product.game,
            "--expansion",
            product.expansion,
            "--base-project",
            "../" + base.key,
        ]
    )
    quiet(
        [tool, "import", exp_dir, install, "--all", "--expansion", product.expansion, "--replace"]
    )
    return export(tool, exp_dir, work / "pristine-export")


def write_wizard(tree: Path, product: Product) -> None:
    """Our own product's UPDATE.WIZ in its tree, which marks the folder it installs into as that
    product's: the updater installs OpenNova content only into a folder with no game data or one its
    wizard names (apps/update client.cpp, ADR 0067 d2). A retail product's players keep their stock one."""
    folder = tree / "expansion" / product.expansion if product.expansion else tree
    folder.mkdir(parents=True, exist_ok=True)
    lines = [
        "[HEADER]",
        f"CMD={product.command_file}",
        "URL=update.opennova.net",
        f"PRODUCT_LOGIN={product.login}",
    ]
    (folder / "UPDATE.WIZ").write_bytes(("\r\n".join(lines) + "\r\n").encode("ascii"))


def build(args) -> Path:
    product = PRODUCTS[args.product]
    project = args.project.resolve()
    project_document(project)
    project_tool, update_tool = args.project_tool, args.update_tool
    if product.retail and not args.install:
        raise ValueError(f"{product.key} is a retail game: name its stock install with --install")
    if not product.retail and args.install:
        raise ValueError(f"{product.key} is our own game: its package is full, with no --install")
    previous = args.previous.resolve() if args.previous else None
    if previous and not previous.is_file():
        raise ValueError(f"no preceding package at {previous}")
    out = (args.out or project.parent / "RELEASES").resolve()
    out.mkdir(parents=True, exist_ok=True)
    package = out / f"{product.key}-{args.version}.sfs"
    with tempfile.TemporaryDirectory(prefix="opennova-release-") as temporary:
        work = Path(temporary)
        if product.expansion:
            # The expansion's base game, exported where the expansion's build reads it.
            quiet([project_tool, "export", base_project_of(project)])
        edited = export(project_tool, project, work / "export")
        if not product.retail:
            write_wizard(edited, product)
        if product.retail:
            source = pristine_export(project_tool, product, args.install.resolve(), work)
        else:
            source = work / "empty"
            source.mkdir()
        command = [
            update_tool,
            "build",
            "--from",
            source,
            "--to",
            edited,
            "--out",
            package,
            "--product-name",
            product.name,
            "--product",
            product.name,
            "--version",
            args.version,
            "--exclude",
            "export.json",
        ]
        if product.expansion:
            command += ["--expansion", f"{product.expansion}={product.command_file}"]
        else:
            command += ["--base", product.command_file]
            if product.exe:
                command += ["--exe", product.exe]
        if not product.retail:
            command += ["--full"]
            if previous:
                command += ["--previous", previous]
        run(command)
    inspected = run([update_tool, "inspect", package], capture_output=True)
    (out / f"{product.key}-{args.version}.txt").write_bytes(inspected.stdout)
    return package


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument(
        "--product", required=True, choices=sorted(PRODUCTS), help="the product to package"
    )
    parser.add_argument("--project", type=Path, required=True, help="its OpenNova Editor project")
    parser.add_argument(
        "--version", required=True, help="the release's version, e.g. 0.2.0 or 1.7.5.8"
    )
    parser.add_argument(
        "--install", type=Path, help="a retail game's stock install, its patch's baseline"
    )
    parser.add_argument(
        "--previous", type=Path, help="our own game's last published package (a full one)"
    )
    parser.add_argument(
        "--out", type=Path, help="where the package goes (default: RELEASES beside the project)"
    )
    parser.add_argument("--project-tool", type=Path, default=default_tool("opennova-project"))
    parser.add_argument("--update-tool", type=Path, default=default_tool("opennova-update-tool"))
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9]+(?:\.[0-9]+){0,3}", args.version):
        parser.error("--version must have one to four numeric components")
    if args.previous and PRODUCTS[args.product].retail:
        parser.error(
            "--previous belongs to our own game's full packages; a retail patch is a difference"
        )
    try:
        package = build(args)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"package_release: {error}", file=sys.stderr)
        return 1
    if not PRODUCTS[args.product].retail and not args.previous:
        print(
            "package_release: no --previous: right only for the product's first release",
            file=sys.stderr,
        )
    print(f"package_release: wrote {package}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
