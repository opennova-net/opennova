# Build and package the OpenNova 3ds Max ASE exporter as an MZP installer.
#
# Produces:
#   dist\opennova_max-v<version>.mzp   (version read from pyproject.toml)
#
# This is a CI-facing package step. It does not require 3ds Max or
# 3dsmaxbatch.exe to assemble the package.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts\package_max_mzp.ps1

param(
    [string]$Configuration = "Release",
    [string]$SeriesMin = "2021",
    [string]$SeriesMax = "2026",
    [string]$Python = ""
)

$ErrorActionPreference = "Stop"

$ROOT = (Resolve-Path "$PSScriptRoot\..").Path
Set-Location $ROOT

$BUILD_ROOT = Join-Path $ROOT "build-max-mzp"
$NATIVE_BUILD = Join-Path $ROOT "build-max-mzp-native"
$STAGE_ROOT = Join-Path $BUILD_ROOT "stage"
$VALIDATE_ROOT = Join-Path $BUILD_ROOT "validate"
$DIST = Join-Path $ROOT "dist"

function Assert-UnderRoot {
    param([string]$Path)
    $rootFull = [System.IO.Path]::GetFullPath($ROOT).TrimEnd('\', '/')
    $pathFull = [System.IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
    $comparison = [System.StringComparison]::OrdinalIgnoreCase
    if ($pathFull -ne $rootFull -and -not $pathFull.StartsWith($rootFull + [System.IO.Path]::DirectorySeparatorChar, $comparison)) {
        throw "Refusing to modify path outside repository root: $pathFull"
    }
}

function Remove-TreeIfExists {
    param([string]$Path)
    if (Test-Path -LiteralPath $Path) {
        Assert-UnderRoot $Path
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
}

function Ensure-Dir {
    param([string]$Path)
    New-Item -ItemType Directory -Force -Path $Path | Out-Null
}

function Copy-PackageDir {
    param(
        [string]$Source,
        [string]$Destination
    )
    if (-not (Test-Path -LiteralPath $Source -PathType Container)) {
        throw "Missing package directory: $Source"
    }
    Ensure-Dir $Destination
    Get-ChildItem -LiteralPath $Source -Force | Copy-Item -Destination $Destination -Recurse -Force
    Get-ChildItem -LiteralPath $Destination -Recurse -Directory -Filter "__pycache__" -ErrorAction SilentlyContinue |
        Remove-Item -Recurse -Force
    Get-ChildItem -LiteralPath $Destination -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Extension -in @(".pyc", ".pyo") } |
        Remove-Item -Force
}

function Assert-File {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing required file: $Path"
    }
}

function Assert-Dir {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "Missing required directory: $Path"
    }
}

function Get-PythonExe {
    if ($Python) {
        return (Resolve-Path $Python).Path
    }
    $cmd = Get-Command python -ErrorAction SilentlyContinue
    if ($cmd) {
        return $cmd.Source
    }
    throw "Python was not found on PATH. Pass -Python <path> if needed."
}

$pyproject = Get-Content (Join-Path $ROOT "pyproject.toml") -Raw
if ($pyproject -notmatch '(?m)^version\s*=\s*"([^"]+)"') {
    throw "Could not extract version from pyproject.toml"
}
$Version = $Matches[1]
$BundleName = "OpenNovaMax-$Version.bundle"
$MzpName = "opennova_max-v$Version.mzp"
$MzpPath = Join-Path $DIST $MzpName
$BundleRoot = Join-Path $STAGE_ROOT $BundleName
$ContentsRoot = Join-Path $BundleRoot "Contents"
$PythonRoot = Join-Path $ContentsRoot "python"
$StartupRoot = Join-Path $ContentsRoot "startup"
$MacroRoot = Join-Path $ContentsRoot "macroscripts"

Write-Host "=== OpenNova Max ASE exporter version: $Version ==="

Write-Host "=== Building opennova.dll ==="
cmake -S . -B $NATIVE_BUILD -DCMAKE_BUILD_TYPE=$Configuration -DBUILD_SHARED_LIB=ON
cmake --build $NATIVE_BUILD --config $Configuration --target opennova_shared
$NativeDll = Get-ChildItem -LiteralPath $NATIVE_BUILD -Recurse -Filter "opennova.dll" |
    Where-Object { $_.FullName -match "\\$Configuration\\" } |
    Select-Object -First 1
if (-not $NativeDll) {
    $NativeDll = Get-ChildItem -LiteralPath $NATIVE_BUILD -Recurse -Filter "opennova.dll" | Select-Object -First 1
}
if (-not $NativeDll) {
    throw "Could not find built opennova.dll under $NATIVE_BUILD"
}

Write-Host "=== Staging bundle ==="
Remove-TreeIfExists $BUILD_ROOT
Ensure-Dir $PythonRoot
Ensure-Dir $StartupRoot
Ensure-Dir $MacroRoot
Ensure-Dir $DIST

Copy-PackageDir (Join-Path $ROOT "opennova_max") (Join-Path $PythonRoot "opennova_max")
Copy-PackageDir (Join-Path $ROOT "pyopennova") (Join-Path $PythonRoot "pyopennova")
Copy-PackageDir (Join-Path $ROOT "opennova_jobs") (Join-Path $PythonRoot "opennova_jobs")

Set-Content -LiteralPath (Join-Path $PythonRoot "opennova_max\_packaged_version.py") `
    -Value "VERSION = `"$Version`"" -Encoding ASCII

$PackagedNative = Join-Path $PythonRoot "pyopennova\lib\windows-x64"
Remove-TreeIfExists (Join-Path $PythonRoot "pyopennova\lib")
Ensure-Dir $PackagedNative
Copy-Item -LiteralPath $NativeDll.FullName -Destination (Join-Path $PackagedNative "opennova.dll") -Force
Copy-Item -LiteralPath (Join-Path $ROOT "opennova_max\maxscript\OpenNovaExport.mcr") `
    -Destination (Join-Path $MacroRoot "OpenNovaExport.mcr") -Force

$PackageContents = @"
<?xml version="1.0" encoding="utf-8"?>
<ApplicationPackage
    SchemaVersion="1.0"
    AutodeskProduct="3ds Max"
    ProductType="Application"
    Name="OpenNova Max ASE Exporter"
    AppVersion="$Version"
    Description="OpenNova 3ds Max ASE exporter"
    ProductCode="{2D5C293C-DB15-4F87-A989-3D3B88C650D2}"
    UpgradeCode="{C13078E9-AC64-4075-8B6E-7FBE98C630C2}">
    <CompanyDetails Name="OpenNova" />
    <Components Description="macroscripts parts">
        <RuntimeRequirements OS="Win64" Platform="3ds Max" SeriesMin="$SeriesMin" SeriesMax="$SeriesMax" />
        <ComponentEntry AppName="OpenNovaMaxMacro" Version="$Version" ModuleName="./Contents/macroscripts/OpenNovaExport.mcr" />
    </Components>
    <Components Description="post-start-up scripts parts">
        <RuntimeRequirements OS="Win64" Platform="3ds Max" SeriesMin="$SeriesMin" SeriesMax="$SeriesMax" />
        <ComponentEntry AppName="OpenNovaMaxStartup" Version="$Version" ModuleName="./Contents/startup/opennova_max_startup.ms" />
    </Components>
</ApplicationPackage>
"@
Set-Content -LiteralPath (Join-Path $BundleRoot "PackageContents.xml") -Value $PackageContents -Encoding UTF8

$StartupMs = @'
(
    local startupDir = getFilenamePath (getSourceFileName())
    local macroPath = pathConfig.normalizePath (startupDir + "..\macroscripts\OpenNovaExport.mcr")
    try
    (
        fileIn macroPath
    )
    catch
    (
        print ("OpenNova package macro load failed: " + getCurrentException())
    )
    python.executeFile (startupDir + "opennova_max_startup.py")
)
'@
Set-Content -LiteralPath (Join-Path $StartupRoot "opennova_max_startup.ms") -Value $StartupMs -Encoding ASCII

$StartupPy = @"
from __future__ import annotations

import os
import sys
import traceback
from pathlib import Path


def _prepend(path: Path) -> None:
    text = str(path)
    if text not in sys.path:
        sys.path.insert(0, text)


def startup() -> None:
    try:
        contents_dir = Path(__file__).resolve().parents[1]
        python_dir = contents_dir / "python"
        _prepend(python_dir)
        os.environ["OPENNOVA_MAX_BUNDLE_DIR"] = str(contents_dir.parent)

        import opennova_max

        opennova_max.register_menu()
        print("OpenNova Max ASE exporter loaded " + opennova_max.get_version())
    except Exception:
        traceback.print_exc()


startup()
"@
Set-Content -LiteralPath (Join-Path $StartupRoot "opennova_max_startup.py") -Value $StartupPy -Encoding UTF8

$MzpRun = @"
name "OpenNova Max ASE Exporter $Version"
description "Installs the OpenNova 3ds Max ASE exporter"
version 1
run "install.ms"
drop "install.ms"
keep temp
"@
Set-Content -LiteralPath (Join-Path $STAGE_ROOT "mzp.run") -Value $MzpRun -Encoding ASCII

$InstallMs = @'
(
    local installerDir = getFilenamePath (getSourceFileName())
    python.executeFile (installerDir + "install.py")
)
'@
Set-Content -LiteralPath (Join-Path $STAGE_ROOT "install.ms") -Value $InstallMs -Encoding ASCII

$InstallDs = @'
macroScript OpenNovaMaxMzpInstaller category:"OpenNova"
(
    fn runOpenNovaMaxInstaller =
    (
        local installerDir = getFilenamePath (getSourceFileName())
        python.executeFile (installerDir + "install.py")
    )

    on droppable window node: point: do
    (
        true
    )

    on drop window node: point: do
    (
        runOpenNovaMaxInstaller()
    )

    on execute do
    (
        runOpenNovaMaxInstaller()
    )
)
'@
Set-Content -LiteralPath (Join-Path $STAGE_ROOT "install.ds") -Value $InstallDs -Encoding ASCII

$InstallPy = @"
from __future__ import annotations

import os
import shutil
import xml.etree.ElementTree as ET
from datetime import datetime
from pathlib import Path

VERSION = "$Version"
BUNDLE_NAME = "$BundleName"


def _install_root() -> Path:
    override = os.environ.get("OPENNOVA_MAX_INSTALL_ROOT")
    if override:
        return Path(override)

    appdata = os.environ.get("APPDATA")
    if not appdata:
        raise RuntimeError("APPDATA is not set; cannot locate Autodesk ApplicationPlugins.")
    return Path(appdata) / "Autodesk" / "ApplicationPlugins"


def _max_profile_roots():
    override = os.environ.get("OPENNOVA_MAX_PROFILE_ROOT")
    if override:
        return [Path(override)]

    roots = []
    for env_name in ("LOCALAPPDATA", "APPDATA"):
        base = os.environ.get(env_name)
        if base:
            roots.append(Path(base) / "Autodesk" / "3dsMax")
    return roots


def _message(text: str) -> None:
    print(text)
    try:
        import pymxs

        pymxs.runtime.messageBox(text, title="OpenNova Max")
    except Exception:
        pass


def _is_opennova_bundle(path: Path) -> bool:
    return (
        path.is_dir()
        and path.name.startswith("OpenNovaMax-")
        and path.name.endswith(".bundle")
    )


def _same_path(left: Path, right: Path) -> bool:
    return os.path.normcase(os.path.abspath(str(left))) == os.path.normcase(
        os.path.abspath(str(right))
    )


def _disable_bundle(path: Path) -> bool:
    package_contents = path / "PackageContents.xml"
    if not package_contents.exists():
        return True
    disabled = path / "PackageContents.xml.disabled-by-opennova-upgrade"
    if disabled.exists():
        disabled.unlink()
    package_contents.rename(disabled)
    return True


def _cleanup_existing_bundles(root: Path, destination: Path):
    removed = []
    disabled = []
    failed = []
    for candidate in root.glob("OpenNovaMax-*.bundle"):
        if not _is_opennova_bundle(candidate):
            continue
        if _same_path(candidate, destination):
            continue
        try:
            shutil.rmtree(str(candidate))
        except Exception:
            try:
                if _disable_bundle(candidate):
                    disabled.append(candidate)
            except Exception as disable_exc:
                failed.append((candidate, disable_exc))
        else:
            removed.append(candidate)
    return removed, disabled, failed


def _remove_matching_children(parent):
    changed = False
    for child in list(parent):
        if child.attrib.get("title") == "OpenNova":
            parent.remove(child)
            changed = True
            continue
        action_id = child.attrib.get("actionID", "")
        if "OpenNova" in action_id:
            parent.remove(child)
            changed = True
            continue
        if _remove_matching_children(child):
            changed = True
    return changed


def _cleanup_max_ui_files():
    cleaned = []
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    for root in _max_profile_roots():
        if not root.is_dir():
            continue
        for mnux in root.rglob("*.mnux"):
            try:
                text = mnux.read_text(encoding="utf-8", errors="ignore")
            except Exception:
                continue
            if "OpenNova" not in text:
                continue
            try:
                tree = ET.parse(str(mnux))
            except Exception:
                continue
            xml_root = tree.getroot()
            if not _remove_matching_children(xml_root):
                continue
            backup = mnux.with_name(mnux.name + ".opennova-cleanup-" + stamp + ".bak")
            shutil.copy2(str(mnux), str(backup))
            tree.write(str(mnux), encoding="utf-8", xml_declaration=True)
            cleaned.append(mnux)
    return cleaned


def install():
    installer_dir = Path(__file__).resolve().parent
    source = installer_dir / BUNDLE_NAME
    if not source.is_dir():
        raise RuntimeError(f"Bundled plugin directory missing: {source}")

    root = _install_root()
    destination = root / BUNDLE_NAME
    root.mkdir(parents=True, exist_ok=True)

    if destination.exists():
        shutil.rmtree(str(destination))
    shutil.copytree(str(source), str(destination))
    removed, disabled, failed = _cleanup_existing_bundles(root, destination)
    ui_cleaned = _cleanup_max_ui_files()
    return destination, removed, disabled, failed, ui_cleaned


if __name__ == "__main__":
    try:
        installed, removed, disabled, failed, ui_cleaned = install()
    except Exception as exc:
        _message(f"OpenNova Max {VERSION} ASE exporter install failed:\n{exc}")
        raise
    else:
        cleanup_parts = []
        if removed:
            cleanup_parts.append(
                "Removed old OpenNova Max bundles:\n"
                + "\n".join(f"- {path.name}" for path in removed)
            )
        if disabled:
            cleanup_parts.append(
                "Disabled locked old OpenNova Max bundles for next restart:\n"
                + "\n".join(f"- {path.name}" for path in disabled)
            )
        if failed:
            cleanup_parts.append(
                "Could not remove or disable these old OpenNova Max bundles:\n"
                + "\n".join(f"- {path.name}: {exc}" for path, exc in failed)
                + "\nClose 3ds Max and install this MZP again."
            )
        if ui_cleaned:
            cleanup_parts.append(
                "Removed stale OpenNova Max UI menu entries from:\n"
                + "\n".join(f"- {path}" for path in ui_cleaned)
            )
        cleanup = "\n\n".join(cleanup_parts) if cleanup_parts else "No old OpenNova Max bundles were found."
        _message(
            f"OpenNova Max {VERSION} ASE exporter installed to:\n{installed}\n\n"
            f"{cleanup}\n\n"
            "Restart 3ds Max to use this version."
        )
"@
Set-Content -LiteralPath (Join-Path $STAGE_ROOT "install.py") -Value $InstallPy -Encoding UTF8

Write-Host "=== Validating staged files ==="
Assert-File (Join-Path $STAGE_ROOT "mzp.run")
Assert-File (Join-Path $STAGE_ROOT "install.ms")
Assert-File (Join-Path $STAGE_ROOT "install.ds")
Assert-File (Join-Path $STAGE_ROOT "install.py")
Assert-File (Join-Path $BundleRoot "PackageContents.xml")
Assert-File (Join-Path $MacroRoot "OpenNovaExport.mcr")
Assert-File (Join-Path $StartupRoot "opennova_max_startup.ms")
Assert-File (Join-Path $StartupRoot "opennova_max_startup.py")
Assert-File (Join-Path $PythonRoot "opennova_max\__init__.py")
Assert-File (Join-Path $PythonRoot "opennova_max\_packaged_version.py")
Assert-File (Join-Path $PythonRoot "opennova_max\ase_scene_exporter.py")
Assert-File (Join-Path $PythonRoot "opennova_max\anim_scene_exporter.py")
Assert-File (Join-Path $PythonRoot "pyopennova\ase_ffi.py")
Assert-File (Join-Path $PythonRoot "pyopennova\bad_build.py")
Assert-File (Join-Path $PythonRoot "pyopennova\lib\windows-x64\opennova.dll")
Assert-Dir (Join-Path $PythonRoot "opennova_jobs")

Write-Host "=== Creating $MzpName ==="
$TempZip = Join-Path $DIST "opennova_max-v$Version.zip"
if (Test-Path -LiteralPath $TempZip) {
    Remove-Item -LiteralPath $TempZip -Force
}
if (Test-Path -LiteralPath $MzpPath) {
    Remove-Item -LiteralPath $MzpPath -Force
}
Compress-Archive -Path (Join-Path $STAGE_ROOT "*") -DestinationPath $TempZip -Force
Move-Item -LiteralPath $TempZip -Destination $MzpPath -Force

Write-Host "=== Extracting and validating MZP ==="
Remove-TreeIfExists $VALIDATE_ROOT
Ensure-Dir $VALIDATE_ROOT
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::ExtractToDirectory($MzpPath, $VALIDATE_ROOT)
$ExpandedBundle = Join-Path $VALIDATE_ROOT $BundleName
$ExpandedPython = Join-Path $ExpandedBundle "Contents\python"
Assert-File (Join-Path $VALIDATE_ROOT "mzp.run")
Assert-File (Join-Path $VALIDATE_ROOT "install.ms")
Assert-File (Join-Path $VALIDATE_ROOT "install.ds")
Assert-File (Join-Path $VALIDATE_ROOT "install.py")
Assert-File (Join-Path $ExpandedBundle "PackageContents.xml")
Assert-File (Join-Path $ExpandedBundle "Contents\macroscripts\OpenNovaExport.mcr")
Assert-File (Join-Path $ExpandedBundle "Contents\startup\opennova_max_startup.ms")
Assert-File (Join-Path $ExpandedBundle "Contents\startup\opennova_max_startup.py")
Assert-Dir (Join-Path $ExpandedPython "opennova_max")
Assert-Dir (Join-Path $ExpandedPython "pyopennova")
Assert-Dir (Join-Path $ExpandedPython "opennova_jobs")

$PythonExe = Get-PythonExe

Write-Host "=== Running installer cleanup smoke check ==="
$InstallSmokeRoot = Join-Path $VALIDATE_ROOT "install-smoke\ApplicationPlugins"
Ensure-Dir (Join-Path $InstallSmokeRoot "OpenNovaMax-0.0.1.bundle")
Ensure-Dir (Join-Path $InstallSmokeRoot "OpenNovaMax-9.9.9.bundle")
$LockedSmokeBundle = Join-Path $InstallSmokeRoot "OpenNovaMax-locked.bundle"
Ensure-Dir $LockedSmokeBundle
Set-Content -LiteralPath (Join-Path $LockedSmokeBundle "PackageContents.xml") -Value "<ApplicationPackage />" -Encoding ASCII
$LockedSmokeFile = Join-Path $LockedSmokeBundle "locked.dll"
Set-Content -LiteralPath $LockedSmokeFile -Value "locked" -Encoding ASCII
Ensure-Dir (Join-Path $InstallSmokeRoot "UnrelatedPlugin.bundle")
$ProfileSmokeRoot = Join-Path $VALIDATE_ROOT "profile-smoke\3dsMax"
$ProfileSmokeUi = Join-Path $ProfileSmokeRoot "2022 - 64bit\ENU\en-US\UI"
$ProfileSmokeWorkspace = Join-Path $ProfileSmokeUi "Workspaces\usersave"
Ensure-Dir $ProfileSmokeUi
Ensure-Dir $ProfileSmokeWorkspace
$StaleMnux = @'
<?xml version="1.0" encoding="utf-8"?>
<MenuFile>
    <Menu title="File-Export">
        <Item mode="2" modeName="AM_ITEM" actionTableID="647394" actionID="OpenNovaExportAse`OpenNova" customTitle="Novalogic ASE (.ase)" />
        <Item mode="2" modeName="AM_ITEM" actionTableID="647394" actionID="__OLD_ANIMS__`OpenNova" customTitle="__OLD_ANIM_TITLE__ (.adm + .bad)" />
        <Item mode="2" modeName="AM_ITEM" actionTableID="0" actionID="40488" />
    </Menu>
    <Menu title="OpenNova">
        <Item mode="2" modeName="AM_ITEM" actionTableID="647394" actionID="__OLD_IMPORTER__`OpenNova" customTitle="Importer..." />
    </Menu>
</MenuFile>
'@
$StaleMnux = $StaleMnux.Replace("__OLD_ANIMS__", "OpenNovaExport" + "Anims")
$StaleMnux = $StaleMnux.Replace("__OLD_ANIM_TITLE__", "Novalogic " + "Anims")
$StaleMnux = $StaleMnux.Replace("__OLD_IMPORTER__", "OpenNova" + "Importer")
Set-Content -LiteralPath (Join-Path $ProfileSmokeUi "MaxStartUI.mnux") -Value $StaleMnux -Encoding UTF8
Set-Content -LiteralPath (Join-Path $ProfileSmokeWorkspace "Workspace1__usersave__.mnux") -Value $StaleMnux -Encoding UTF8
$OldInstallRoot = $env:OPENNOVA_MAX_INSTALL_ROOT
$OldProfileRoot = $env:OPENNOVA_MAX_PROFILE_ROOT
$LockStream = $null
$PushedInstallSmokeLocation = $false
try {
    $LockStream = [System.IO.File]::Open(
        $LockedSmokeFile,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::ReadWrite,
        [System.IO.FileShare]::None
    )
    $env:OPENNOVA_MAX_INSTALL_ROOT = $InstallSmokeRoot
    $env:OPENNOVA_MAX_PROFILE_ROOT = $ProfileSmokeRoot
    Push-Location $VALIDATE_ROOT
    $PushedInstallSmokeLocation = $true
    & $PythonExe (Join-Path $VALIDATE_ROOT "install.py")
    if ($LASTEXITCODE -ne 0) {
        throw "Installer cleanup smoke check failed with exit code $LASTEXITCODE"
    }
}
finally {
    if ($PushedInstallSmokeLocation) {
        Pop-Location
    }
    if ($LockStream) {
        $LockStream.Dispose()
    }
    $env:OPENNOVA_MAX_INSTALL_ROOT = $OldInstallRoot
    $env:OPENNOVA_MAX_PROFILE_ROOT = $OldProfileRoot
}
Assert-Dir (Join-Path $InstallSmokeRoot $BundleName)
if (Test-Path -LiteralPath (Join-Path $InstallSmokeRoot "OpenNovaMax-0.0.1.bundle")) {
    throw "Installer did not remove stale OpenNovaMax-0.0.1.bundle"
}
if (Test-Path -LiteralPath (Join-Path $InstallSmokeRoot "OpenNovaMax-9.9.9.bundle")) {
    throw "Installer did not remove stale OpenNovaMax-9.9.9.bundle"
}
Assert-Dir $LockedSmokeBundle
if (Test-Path -LiteralPath (Join-Path $LockedSmokeBundle "PackageContents.xml")) {
    throw "Installer did not disable locked OpenNovaMax-locked.bundle"
}
Assert-Dir (Join-Path $InstallSmokeRoot "UnrelatedPlugin.bundle")
$ActiveProfileHits = Get-ChildItem -LiteralPath $ProfileSmokeRoot -Recurse -File -Filter "*.mnux" |
    Select-String -Pattern "OpenNova" -SimpleMatch
if ($ActiveProfileHits) {
    throw "Installer did not remove stale OpenNova Max UI menu entries"
}
$UiBackups = Get-ChildItem -LiteralPath $ProfileSmokeRoot -Recurse -File -Filter "*.opennova-cleanup-*.bak"
if ($UiBackups.Count -lt 2) {
    throw "Installer did not create backups for cleaned Max UI menu files"
}

Write-Host "=== Running staged Python smoke check ==="
$OldPythonPath = $env:PYTHONPATH
$OldSmokeVersion = $env:OPENNOVA_MAX_SMOKE_VERSION
$OldPythonRoot = $env:OPENNOVA_MAX_PYTHON_ROOT
$env:PYTHONPATH = $ExpandedPython
$env:OPENNOVA_MAX_SMOKE_VERSION = $Version
$env:OPENNOVA_MAX_PYTHON_ROOT = $ExpandedPython
try {
    & $PythonExe -c @'
import os
import sys
from pathlib import Path

python_root = Path(os.environ['OPENNOVA_MAX_PYTHON_ROOT']).resolve()
sys.path.insert(0, str(python_root))

import opennova_max
from opennova_max import ui
import opennova_max.ase_scene_exporter
import opennova_max.anim_scene_exporter
import pyopennova.ase_ffi
import pyopennova.bad_build

for module in (opennova_max, ui, opennova_max.ase_scene_exporter,
               opennova_max.anim_scene_exporter, pyopennova.ase_ffi, pyopennova.bad_build):
    module_file = Path(module.__file__).resolve()
    if not str(module_file).lower().startswith(str(python_root).lower()):
        raise RuntimeError(f'{module.__name__} imported from wrong path: {module_file}')

assert opennova_max.get_version() == os.environ['OPENNOVA_MAX_SMOKE_VERSION']
menu_script = ui.build_menu_script()
assert 'Novalogic ASE (.ase)' in menu_script
assert 'Novalogic Anims (.adm + .bad)' in menu_script
assert 'maxOps.GetICuiMenuMgr()' in menu_script
assert '#cuiRegisterMenus' in menu_script
assert 'OpenNovaExportAse`OpenNova' in menu_script
assert 'OpenNovaExportAnims`OpenNova' in menu_script
assert 'menuMgr.GetMenuById' in menu_script
assert 'eed3eaef-ea24-4342-aacc-9dfd87f9a4f4' in menu_script
assert 'exportMenu.addItem aseItem -1' in menu_script
assert 'exportMenu.addItem animsItem -1' in menu_script
assert 'OpenNovaExportMenu' not in menu_script
assert 'createSubMenuItem "OpenNova"' not in menu_script
print('staged Max ASE + Anim exporter smoke ok')
'@
    if ($LASTEXITCODE -ne 0) {
        throw "Staged Python smoke check failed"
    }
}
finally {
    $env:PYTHONPATH = $OldPythonPath
    $env:OPENNOVA_MAX_SMOKE_VERSION = $OldSmokeVersion
    $env:OPENNOVA_MAX_PYTHON_ROOT = $OldPythonRoot
}

Write-Host "=== Done ==="
Write-Host "  -> $MzpPath"
Get-Item $MzpPath | Select-Object Name, Length
