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
    <Components Description="macro scripts">
        <RuntimeRequirements OS="Win64" Platform="3ds Max" SeriesMin="$SeriesMin" SeriesMax="$SeriesMax" />
        <ComponentEntry AppName="OpenNovaMaxMacro" Version="$Version" ModuleName="./Contents/macroscripts/OpenNovaExport.mcr" />
    </Components>
    <Components Description="post-start-up scripts">
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
    on execute do runOpenNovaMaxInstaller()
)
'@
Set-Content -LiteralPath (Join-Path $STAGE_ROOT "install.ds") -Value $InstallDs -Encoding ASCII

$InstallPy = @"
from __future__ import annotations

import os
import shutil
from pathlib import Path


BUNDLE_GLOB = "OpenNovaMax-*.bundle"


def _install_root() -> Path:
    override = os.environ.get("OPENNOVA_MAX_INSTALL_ROOT", "").strip()
    if override:
        return Path(override)
    appdata = os.environ.get("APPDATA")
    if not appdata:
        raise RuntimeError("APPDATA is not set")
    return Path(appdata) / "Autodesk" / "ApplicationPlugins"


def _disable_bundle(path: Path) -> bool:
    package_contents = path / "PackageContents.xml"
    if not package_contents.exists():
        return True
    disabled = path / "PackageContents.xml.disabled-by-opennova-upgrade"
    if disabled.exists():
        disabled.unlink()
    package_contents.rename(disabled)
    return True


def _remove_or_disable_bundle(path: Path) -> None:
    try:
        shutil.rmtree(path)
    except Exception:
        _disable_bundle(path)


def install() -> None:
    source_root = Path(__file__).resolve().parent
    candidates = list(source_root.glob(BUNDLE_GLOB))
    if len(candidates) != 1:
        raise RuntimeError("Expected exactly one OpenNovaMax bundle next to installer")

    install_root = _install_root()
    install_root.mkdir(parents=True, exist_ok=True)

    for old_bundle in install_root.glob(BUNDLE_GLOB):
        _remove_or_disable_bundle(old_bundle)

    source = candidates[0]
    destination = install_root / source.name
    if destination.exists():
        _remove_or_disable_bundle(destination)
    shutil.copytree(source, destination)
    print("Installed OpenNova Max ASE exporter to " + str(destination))


install()
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
Assert-File (Join-Path $PythonRoot "pyopennova\ase_ffi.py")
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
$ValidationZip = Join-Path $BUILD_ROOT "opennova_max-validate.zip"
if (Test-Path -LiteralPath $ValidationZip) {
    Remove-Item -LiteralPath $ValidationZip -Force
}
Copy-Item -LiteralPath $MzpPath -Destination $ValidationZip -Force
Expand-Archive -LiteralPath $ValidationZip -DestinationPath $VALIDATE_ROOT -Force
Remove-Item -LiteralPath $ValidationZip -Force
$ExpandedBundle = Join-Path $VALIDATE_ROOT $BundleName
$ExpandedPython = Join-Path $ExpandedBundle "Contents\python"
Assert-File (Join-Path $VALIDATE_ROOT "mzp.run")
Assert-File (Join-Path $VALIDATE_ROOT "install.ms")
Assert-File (Join-Path $VALIDATE_ROOT "install.py")
Assert-File (Join-Path $ExpandedBundle "PackageContents.xml")
Assert-File (Join-Path $ExpandedBundle "Contents\macroscripts\OpenNovaExport.mcr")
Assert-File (Join-Path $ExpandedBundle "Contents\startup\opennova_max_startup.ms")
Assert-File (Join-Path $ExpandedBundle "Contents\startup\opennova_max_startup.py")
Assert-Dir (Join-Path $ExpandedPython "opennova_max")
Assert-Dir (Join-Path $ExpandedPython "pyopennova")
Assert-Dir (Join-Path $ExpandedPython "opennova_jobs")

Write-Host "=== Running staged Python smoke check ==="
$PythonExe = Get-PythonExe
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
import pyopennova.ase_ffi

for module in (opennova_max, ui, opennova_max.ase_scene_exporter, pyopennova.ase_ffi):
    module_file = Path(module.__file__).resolve()
    if not str(module_file).lower().startswith(str(python_root).lower()):
        raise RuntimeError(f'{module.__name__} imported from wrong path: {module_file}')

assert opennova_max.get_version() == os.environ['OPENNOVA_MAX_SMOKE_VERSION']
menu_script = ui.build_menu_script()
assert 'Novalogic ASE (.ase)' in menu_script
assert 'exportMenu.addItem aseItem -1' in menu_script
assert 'OpenNovaExportMenu' not in menu_script
assert 'createSubMenuItem "OpenNova"' not in menu_script
print('staged Max ASE exporter smoke ok')
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
