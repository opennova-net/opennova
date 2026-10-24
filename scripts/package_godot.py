#!/usr/bin/env python3
"""Build, export and boot-test the Windows or Linux game and editor packages.

Usage: python scripts/package_godot.py --platform windows|linux
       [--export-mode release|debug] [--skip-build] [--project-cli PATH]

Windows ships ZIPs and Linux (Ubuntu 24.04+ x86_64) ships tar.gz archives in
dist/. Each game contains the editor's export of the tracked assets/ project
and expansions/, never local project files. The editor contains editor/ and
runtime/ side by side. Both archives are extracted and smoked before publishing
their final names, with all profile data kept outside the package (ADR 0046,
0048). --skip-build consumes the GDExtensions CI already built, but still builds
opennova-project unless --project-cli names one.
"""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile


ROOT = Path(__file__).resolve().parents[1]
GODOT_VERSION = "4.6.1-stable"
TEMPLATE_VERSION = "4.6.1.stable"
LFS_POINTER = b"version https://git-lfs.github.com/spec/v1"
LOAD_ERRORS = re.compile(
    r"SCRIPT ERROR|GDExtension dynamic library not found|No loader found for resource|"
    r"Failed to load script|Failed loading scene|Cannot open file 'res://|"
    r"Can't open dynamic library|Can't load GDExtension|Failed loading resource",
    re.IGNORECASE,
)


class PackageError(RuntimeError):
    pass


class Target:
    def __init__(self, name: str):
        if name not in ("windows", "linux"):
            raise PackageError(f"Unsupported package platform: {name}")
        self.name = name
        self.exe_suffix = ".exe" if name == "windows" else ""
        self.library_suffix = "dll" if name == "windows" else "so"
        self.archive_suffix = ".zip" if name == "windows" else ".tar.gz"
        self.godot_name = (f"Godot_v{GODOT_VERSION}_win64.exe" if name == "windows"
                           else f"Godot_v{GODOT_VERSION}_linux.x86_64")

    def executable(self, name: str) -> str:
        return name + self.exe_suffix

    def library(self, editor: bool, mode: str) -> str:
        product = "opennova_editor" if editor else "opennova"
        return f"lib{product}.{self.name}.template_{mode}.x86_64.{self.library_suffix}"

    def imgui_library(self, mode: str) -> str:
        return f"libimgui-godot-native.{self.name}.{mode}.x86_64.{self.library_suffix}"

    def preset(self, product: str) -> str:
        return f"OpenNova {product}" + (" Linux" if self.name == "linux" else "")

    def template_names(self) -> tuple[str, str]:
        if self.name == "windows":
            return ("windows_debug_x86_64.exe", "windows_release_x86_64.exe")
        return ("linux_debug.x86_64", "linux_release.x86_64")


def run(command, *, cwd=ROOT, capture=False, env=None, expected=0, timeout=None):
    """Wait for native GUI executables too; preserve argv containing spaces."""
    kwargs = {}
    if os.name == "nt":
        kwargs["creationflags"] = subprocess.CREATE_NO_WINDOW
    result = subprocess.run(
        [str(arg) for arg in command], cwd=cwd, env=env, timeout=timeout,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.STDOUT if capture else None,
        text=True, encoding="utf-8", errors="replace", **kwargs,
    )
    if result.returncode != expected:
        if result.stdout:
            print(result.stdout, flush=True)
        raise PackageError(f"{command[0]} exited {result.returncode}, expected {expected}: {command[1:]}")
    return result.stdout or ""


def require_file(path: Path) -> Path:
    if not path.is_file():
        raise PackageError(f"Required file missing: {path}")
    return path


def download(url: str, destination: Path):
    destination.parent.mkdir(parents=True, exist_ok=True)
    part = destination.with_name(destination.name + ".part")
    try:
        run(["curl", "--fail", "--location", "--retry", "3", "--output", part, url])
        part.replace(destination)
    finally:
        part.unlink(missing_ok=True)


def install_godot(target: Target) -> Path:
    cache = ROOT / ".godot-bin"
    binary = cache / target.godot_name
    release = f"https://github.com/godotengine/godot/releases/download/{GODOT_VERSION}"
    if not binary.is_file():
        archive = cache / f"godot-{target.name}.zip"
        download(f"{release}/{target.godot_name}.zip", archive)
        with zipfile.ZipFile(archive) as package:
            # The official Windows archive also contains its console companion.
            for name in package.namelist():
                if PurePosixPath(name).name == name and name.startswith(f"Godot_v{GODOT_VERSION}_"):
                    package.extract(name, cache)
        archive.unlink()
    require_file(binary)
    if target.name == "linux":
        binary.chmod(0o755)

    if target.name == "windows":
        data_dir = Path(os.environ["APPDATA"]) / "Godot"
    else:
        data_dir = Path(os.environ.get("XDG_DATA_HOME", str(Path.home() / ".local/share"))) / "godot"
    templates = data_dir / "export_templates" / TEMPLATE_VERSION
    # version.txt alone can survive a partial platform-specific CI cache restore.
    if not all((templates / name).is_file() for name in ("version.txt", *target.template_names())):
        archive = cache / "export_templates.tpz"
        download(f"{release}/Godot_v{GODOT_VERSION}_export_templates.tpz", archive)
        templates.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(archive) as package:
            for name in ("version.txt", "icudt_godot.dat", *target.template_names()):
                member = f"templates/{name}"
                if member not in package.namelist():
                    if name == "icudt_godot.dat":
                        continue
                    raise PackageError(f"Export templates archive is missing {member}")
                with package.open(member) as source, (templates / name).open("wb") as destination:
                    shutil.copyfileobj(source, destination)
                if target.name == "linux" and name.startswith("linux_"):
                    (templates / name).chmod(0o755)
        archive.unlink()
    return binary


def version_of(root: Path) -> str:
    match = re.search(r'^config/version\s*=\s*"([^"]+)"',
                      (root / "godot/project.godot").read_text(encoding="utf-8"), re.MULTILINE)
    if not match:
        raise PackageError("Could not extract config/version from godot/project.godot")
    return match[1]


@contextmanager
def windows_version(presets: Path, version: str, enabled: bool):
    """Stamp VERSIONINFO for export, restoring the checkout even after a failure."""
    if not enabled:
        yield
        return
    parts = version.split(".")
    if not 1 <= len(parts) <= 4 or not all(part.isdecimal() for part in parts):
        raise PackageError(f"Godot Windows VERSIONINFO needs a numeric version: {version}")
    padded = ".".join(parts + ["0"] * (4 - len(parts)))
    original = presets.read_bytes()
    stamped = re.sub(rb'application/(file_version|product_version)="[^"]*"',
                     lambda match: b'application/' + match[1] + b'="' + padded.encode("ascii") + b'"',
                     original)
    try:
        presets.write_bytes(stamped)
        yield
    finally:
        presets.write_bytes(original)


def build_extensions(target: Target, mode: str, skip: bool):
    for flavor in (["debug", "release"] if mode == "release" else ["debug"]):
        if not skip:
            # Linux CI's Release build avoids enormous debug artifacts. Windows
            # development packages match the optimized, symbol-bearing dev DLLs.
            config = "RelWithDebInfo" if target.name == "windows" and flavor == "debug" else "Release"
            # A Windows checkout can also be packaged through WSL: never reuse
            # the other host's compiler and generator cache.
            build = ROOT / f"build-godot-{target.name}-{flavor}"
            run(["cmake", "-S", ROOT / "godot/src", "-B", build,
                 f"-DGODOTCPP_TARGET=template_{flavor}", f"-DCMAKE_BUILD_TYPE={config}"])
            run(["cmake", "--build", build, "--config", config, "--target",
                 "opennova", "opennova_editor_gdext"])
        for editor in (False, True):
            require_file(ROOT / "godot/bin" / target.library(editor, flavor))


def project_cli(target: Target, supplied: str | None) -> Path:
    if supplied:
        return require_file(Path(supplied).resolve())
    build = ROOT / f"build-godot-{target.name}-project"
    if not (build / "CMakeCache.txt").is_file():
        run(["cmake", "-S", ROOT, "-B", build, "-DCMAKE_BUILD_TYPE=Release"])
    run(["cmake", "--build", build, "--config", "Release", "--target", "opennova_project"])
    for folder in (build / "apps/project", build / "apps/project/Release"):
        candidate = folder / target.executable("opennova-project")
        if candidate.is_file():
            return candidate
    raise PackageError("opennova-project not found (pass --project-cli, or let the script build it)")


def tracked_files(root: Path, pattern: str) -> list[str]:
    output = run(["git", "ls-files", "-z", "--", pattern], cwd=root, capture=True)
    return [name for name in output.split("\0") if name]


def copy_tracked_project(root: Path, folder: str, stage: Path):
    names = tracked_files(root, folder)
    if not names:
        raise PackageError(f"No tracked files found under {folder}/")
    for name in names:
        source = root / name
        if source.is_symlink() or not source.is_file():
            raise PackageError(f"Tracked file missing or linked: {name}")
        with source.open("rb") as stream:
            if stream.read(len(LFS_POINTER)) == LFS_POINTER:
                raise PackageError(f"Tracked file is an unpulled LFS pointer: {name} (run git lfs pull)")
        destination = stage / Path(name).relative_to(folder)
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
    print(f"Staged {len(names)} tracked files of {folder}/", flush=True)


def export_base_game(cli: Path, stage: Path, game: Path):
    base = stage / "assets"
    copy_tracked_project(ROOT, "assets", base)
    run([cli, "export", base])
    exported = base / "build/export"
    for name in ("language.pff", "localres.pff", "resource.pff"):
        require_file(exported / name)
    shutil.copytree(exported, game, dirs_exist_ok=True)
    for project in tracked_files(ROOT, "expansions/*/project.opennova"):
        folder = PurePosixPath(project).parent.as_posix()
        expansion = stage / folder
        copy_tracked_project(ROOT, folder, expansion)
        output = stage / ("out-" + expansion.name)
        run([cli, "export", expansion, "--out", output])
        built = list((output / "expansion").iterdir())
        if len(built) != 1 or not built[0].is_dir():
            raise PackageError(f"The export of {folder} must hold one expansion folder")
        require_file(built[0] / f"{built[0].name}.pff")
        shutil.copytree(built[0], game / "expansion" / built[0].name)


def check_load_errors(output: str, label: str):
    if LOAD_ERRORS.search(output):
        print(output, flush=True)
        raise PackageError(f"{label} logged script, resource or GDExtension load errors")


def export_product(godot: Path, target: Target, mode: str, product: str, destination: Path):
    destination.parent.mkdir(parents=True, exist_ok=True)
    preset = target.preset(product)
    output = run([godot, "--headless", "--path", ROOT / "godot", f"--export-{mode}",
                  preset, destination], capture=True, timeout=900)
    print(output, flush=True)
    check_load_errors(output, f"Godot export of {preset}")
    require_file(destination)
    if target.name == "linux":
        destination.chmod(0o755)


def stage_product(source: Path, destination: Path, target: Target, mode: str, editor: bool):
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)
    library = ROOT / "godot/bin" / target.library(editor, mode)
    shutil.copy2(require_file(library), destination.parent / library.name)
    # Fresh per-product export directories prevent stale debug/foreign-platform
    # libraries from riding a later release package.
    if editor or mode == "debug":
        imgui = source.parent / target.imgui_library(mode)
        shutil.copy2(require_file(imgui), destination.parent / imgui.name)


def stage_files(stage: Path) -> set[str]:
    files = set()
    for path in stage.rglob("*"):
        if path.is_symlink():
            raise PackageError(f"Package stage contains a link: {path}")
        if path.is_file():
            files.add(path.relative_to(stage).as_posix())
        elif not path.is_dir():
            raise PackageError(f"Package stage contains a special file: {path}")
    return files


def assert_stage_holds_only(stage: Path, expected: set[str]):
    actual = stage_files(stage)
    allowed_dirs = {parent.as_posix() for name in expected for parent in PurePosixPath(name).parents
                    if parent.as_posix() != "."}
    unexpected_dirs = {path.relative_to(stage).as_posix() for path in stage.rglob("*")
                       if path.is_dir()} - allowed_dirs
    if actual != expected or unexpected_dirs:
        raise PackageError(f"Package contents changed: unexpected {sorted(actual - expected | unexpected_dirs)}, "
                           f"missing {sorted(expected - actual)}")


def write_archive(stage: Path, archive: Path, target: Target, expected: set[str]):
    assert_stage_holds_only(stage, expected)
    if target.name == "windows":
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as package:
            for name in sorted(expected):
                package.write(stage / name, name)
    else:
        with tarfile.open(archive, "w:gz") as package:
            for name in sorted(expected):
                info = package.gettarinfo(stage / name, arcname=name)
                # Do not publish the build runner's user/group name or ID.
                info.uid = info.gid = 0
                info.uname = info.gname = ""
                with (stage / name).open("rb") as source:
                    package.addfile(info, source)


def extract_archive(archive: Path, destination: Path, target: Target, expected: set[str]):
    """Extract only the regular files we staged, including their archived modes."""
    def write_member(name, source, mode):
        relative = PurePosixPath(name)
        if relative.is_absolute() or ".." in relative.parts or "\\" in name or name not in expected:
            raise PackageError(f"Unexpected archive member: {name}")
        path = destination / name
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("wb") as output:
            shutil.copyfileobj(source, output)
        if target.name == "linux":
            path.chmod(mode & 0o777)

    if target.name == "windows":
        with zipfile.ZipFile(archive) as package:
            if len(package.infolist()) != len(expected):
                raise PackageError("Archive file count differs from the staged package")
            for member in package.infolist():
                with package.open(member) as source:
                    write_member(member.filename, source, member.external_attr >> 16)
    else:
        with tarfile.open(archive, "r:gz") as package:
            members = package.getmembers()
            if len(members) != len(expected):
                raise PackageError("Archive file count differs from the staged package")
            for member in members:
                if not member.isfile():
                    raise PackageError(f"Archive member is not a regular file: {member.name}")
                with package.extractfile(member) as source:
                    write_member(member.name, source, member.mode)
    assert_stage_holds_only(destination, expected)


def check_linux_dependencies(stage: Path):
    environment = os.environ.copy()
    environment["LC_ALL"] = "C"
    for name in sorted(stage_files(stage)):
        path = stage / name
        with path.open("rb") as stream:
            header = stream.read(20)
            if header[:4] != b"\x7fELF":
                continue
        # ELF64, little endian, EM_X86_64. Refuse a locally built archive that
        # silently targets another architecture or a newer glibc than Ubuntu
        # 24.04's 2.39, even if this packaging machine could run it.
        if len(header) != 20 or header[4:6] != b"\x02\x01" or int.from_bytes(header[18:20], "little") != 62:
            raise PackageError(f"Linux native file is not an x86_64 ELF: {name}")
        versions = run(["readelf", "--version-info", path], capture=True, env=environment)
        glibc_versions = [tuple(map(int, version)) for version in re.findall(r"\bGLIBC_(\d+)\.(\d+)\b", versions)]
        if any(version > (2, 39) for version in glibc_versions):
            raise PackageError(f"{name} needs a glibc newer than Ubuntu 24.04 (2.39)")
        output = run(["ldd", path], capture=True, env=environment)
        print(f"Dependencies of {name}:\n{output}", flush=True)
        if "not found" in output:
            raise PackageError(f"Unresolved Linux native dependencies in {name}")


def boot(executable: Path, arguments: list[str], expected: int) -> str:
    with tempfile.TemporaryDirectory(prefix="opennova-package-profile-") as profile:
        # user:// and every game's own save stay out of the archive and the
        # packager's real profile. Godot moves cwd to the exported exe's folder;
        # --working-dir is therefore necessary even with subprocess cwd set.
        environment = os.environ.copy()
        environment.update(APPDATA=profile, XDG_DATA_HOME=profile,
                           XDG_CONFIG_HOME=profile, XDG_CACHE_HOME=profile)
        output = run([executable, "--headless", "--disable-render-loop", "--disable-crash-handler",
                      "--quit-after", "120", "--verbose", "--", "--working-dir", profile, *arguments],
                     cwd=profile, env=environment, capture=True, expected=expected, timeout=120)
    check_load_errors(output, f"Boot smoke of {executable.name}")
    return output


def require_marker(output: str, marker: str):
    if not re.search(marker, output):
        print(output, flush=True)
        raise PackageError(f"Boot smoke did not report {marker}")


def smoke_game(stage: Path, target: Target):
    executable = stage / target.executable("opennova")
    output = boot(executable, [], 0)
    if re.search(r"bundled assets not found|no menu found in resource dir", output):
        raise PackageError(f"Boot smoke did not reach the bundled menu:\n{output}")
    require_marker(output, r"OpenNova: bundled menu up from .* \(build\)")
    require_marker(boot(executable, ["--resource-dir"], 2), r"--resource-dir needs a path")


def smoke_editor(stage: Path, target: Target):
    require_marker(boot(stage / "runtime" / target.executable("opennova"), ["--resource-dir"], 2),
                   r"--resource-dir needs a path")
    require_marker(boot(stage / "editor" / target.executable("opennova-editor"), ["--editor-smoke"], 0),
                   r"OpenNova Editor: smoke ok")


def publish_archive(stage: Path, archive: Path, target: Target, smoke):
    expected = stage_files(stage)
    pending = archive.with_name("." + archive.name + ".unverified")
    try:
        write_archive(stage, pending, target, expected)
        with tempfile.TemporaryDirectory(prefix=".package-extracted-", dir=archive.parent) as temporary:
            extracted = Path(temporary)
            extract_archive(pending, extracted, target, expected)
            if target.name == "linux":
                check_linux_dependencies(extracted)
            smoke(extracted, target)
            assert_stage_holds_only(extracted, expected)
        pending.replace(archive)
    finally:
        pending.unlink(missing_ok=True)
    print(f"Verified {archive.name} ({archive.stat().st_size} bytes)", flush=True)


def readme(target: Target, editor: bool) -> str:
    exe = target.executable("opennova")
    prefix = "./" if target.name == "linux" else ""
    platform_note = "Requires Ubuntu 24.04 or newer, x86_64.\n\n" if target.name == "linux" else ""
    if editor:
        return ("OpenNova Editor (experimental).\n\n" + platform_note +
                f"Run {prefix}editor/{target.executable('opennova-editor')} to create or open a project, "
                "build it, and play.\n"
                f"The editor's Play uses runtime/{exe}. Keep editor/ and runtime/ side by side,\n"
                "or choose another runtime in File > Project settings...\n")
    retail = '"$HOME/Games/Joint Operations"' if target.name == "linux" else '"C:\\Games\\Joint Operations"'
    loose = '"$HOME/MyGameData"' if target.name == "linux" else '"C:\\MyGameData"'
    return ("OpenNova\n\n" + platform_note +
            f"Run {prefix}{exe}. It plays OpenNova's own small game from the files beside it.\n"
            "To play Joint Operations itself, press PLAY RETAIL and choose your Joint Operations\n"
            "folder. The folder is remembered; CHANGE FOLDER picks another.\n\n"
            "Online: open NovaWorld from Joint Operations' multiplayer menu and choose OpenNova.\n"
            f"{exe} reaches OpenNova's NovaWorld by itself; the OpenNova Switcher is only for\n"
            "the original Jointops.exe and dfx2.exe on Windows.\n\n"
            f"Command line (advanced):\n  {prefix}{exe} -- --resource-dir {retail}\n"
            f"Loose data:\n  {prefix}{exe} -- --resource-dir {loose} --loose-root /d\n\n"
            "Use /game <code> and /exp <name> to select a game or expansion. OpenNova's own\n"
            "expansions ship in expansion/; the main menu's MODS lists them, or start one with:\n"
            f"  {prefix}{exe} -- /exp onjo1\n")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--platform", choices=("windows", "linux"), required=True)
    parser.add_argument("--export-mode", choices=("release", "debug"), default="release")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--project-cli")
    args = parser.parse_args(argv)
    target = Target(args.platform)
    host = "windows" if sys.platform == "win32" else "linux" if sys.platform == "linux" else "unsupported"
    if host != target.name or platform.machine().lower() not in ("amd64", "x86_64"):
        raise PackageError(f"Package {target.name} on a native x86_64 {target.name} host")
    version = version_of(ROOT)
    print(f"Packaging {target.name} v{version} ({args.export_mode})", flush=True)
    run(["bash", "scripts/bootstrap_imgui_godot.sh"])
    godot = install_godot(target)
    build_extensions(target, args.export_mode, args.skip_build)
    cli = project_cli(target, args.project_cli)
    dist = ROOT / "dist"
    dist.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".stage-godot-", dir=dist) as temporary:
        work = Path(temporary)
        runtime = work / "exports/game" / target.executable("opennova")
        play = work / "exports/play" / target.executable("opennova")
        editor = work / "exports/editor" / target.executable("opennova-editor")
        with windows_version(ROOT / "godot/export_presets.cfg", version, target.name == "windows"):
            for product, destination in (("Runtime", runtime), ("Play Runtime", play), ("Editor", editor)):
                export_product(godot, target, args.export_mode, product, destination)
        game_stage = work / "game"
        export_base_game(cli, work / "projects", game_stage)
        stage_product(runtime, game_stage / runtime.name, target, args.export_mode, False)
        (game_stage / "README.txt").write_text(readme(target, False), encoding="utf-8")
        publish_archive(game_stage, dist / f"opennova-game-{target.name}-v{version}{target.archive_suffix}",
                        target, smoke_game)
        editor_stage = work / "editor"
        stage_product(editor, editor_stage / "editor" / editor.name, target, args.export_mode, True)
        stage_product(play, editor_stage / "runtime" / play.name, target, args.export_mode, False)
        (editor_stage / "README.txt").write_text(readme(target, True), encoding="utf-8")
        publish_archive(editor_stage, dist / f"opennova-editor-{target.name}-v{version}{target.archive_suffix}",
                        target, smoke_editor)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (PackageError, OSError, subprocess.SubprocessError, zipfile.BadZipFile, tarfile.TarError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
