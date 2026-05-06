# Build and package the OpenNova 3ds Max plugin as an MZP installer.
#
# Produces:
#   dist\opennova_max-v<version>.mzp   (version read from pyproject.toml)
#
# This script is the CI-facing entrypoint for the Max plugin. It does not
# require 3ds Max or 3dsmaxbatch.exe; local Max validation remains optional.
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
    Get-ChildItem -LiteralPath $Source -Force |
        Copy-Item -Destination $Destination -Recurse -Force
    Get-ChildItem -LiteralPath $Destination -Recurse -Directory -Filter "__pycache__" -ErrorAction SilentlyContinue |
        Remove-Item -Recurse -Force
    Get-ChildItem -LiteralPath $Destination -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Extension -in @(".pyc", ".pyo") } |
        Remove-Item -Force
}

function Copy-PackageFile {
    param(
        [string]$Source,
        [string]$Destination
    )
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        throw "Missing package file: $Source"
    }
    Ensure-Dir (Split-Path -Parent $Destination)
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
}

function Assert-File {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Expected file missing: $Path"
    }
}

function Assert-Dir {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "Expected directory missing: $Path"
    }
}

function Resolve-Python {
    if ($Python) {
        $resolved = (Resolve-Path $Python).Path
        return $resolved
    }

    $pythonCmd = Get-Command python -ErrorAction SilentlyContinue
    if ($pythonCmd) {
        return $pythonCmd.Source
    }

    $pyCmd = Get-Command py -ErrorAction SilentlyContinue
    if ($pyCmd) {
        $resolved = (& $pyCmd.Source -3.11 -c "import sys; print(sys.executable)" 2>$null)
        if ($LASTEXITCODE -eq 0 -and $resolved) {
            return $resolved.Trim()
        }
    }

    throw "Could not find Python. Install Python 3.11 or pass -Python <path>."
}

function Find-NativeDll {
    $candidates = @(
        (Join-Path $NATIVE_BUILD "$Configuration\opennova.dll"),
        (Join-Path $NATIVE_BUILD "opennova.dll"),
        (Join-Path $ROOT "pyopennova\lib\windows-x64\opennova.dll")
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return (Resolve-Path $candidate).Path
        }
    }
    throw "Could not locate built opennova.dll."
}

$pyproject = Get-Content "$ROOT\pyproject.toml" -Raw
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

Write-Host "=== OpenNova Max version: $Version ==="

Write-Host "=== Building opennova.dll ==="
cmake -S $ROOT -B $NATIVE_BUILD -DCMAKE_BUILD_TYPE=$Configuration -DBUILD_SHARED_LIB=ON
cmake --build $NATIVE_BUILD --target opennova_shared --config $Configuration
$NativeDll = Find-NativeDll
Write-Host "Using native DLL: $NativeDll"

Write-Host "=== Staging $BundleName ==="
Remove-TreeIfExists $BUILD_ROOT
Ensure-Dir $PythonRoot
Ensure-Dir $StartupRoot
Ensure-Dir $MacroRoot
Ensure-Dir $DIST

Copy-PackageDir (Join-Path $ROOT "opennova_max") (Join-Path $PythonRoot "opennova_max")
Copy-PackageDir (Join-Path $ROOT "pyopennova") (Join-Path $PythonRoot "pyopennova")
Set-Content -LiteralPath (Join-Path $PythonRoot "opennova_max\_packaged_version.py") `
    -Value "VERSION = `"$Version`"" `
    -Encoding ASCII

$StagedPyLib = Join-Path $PythonRoot "pyopennova\lib"
Remove-TreeIfExists $StagedPyLib
Ensure-Dir (Join-Path $StagedPyLib "windows-x64")
Copy-PackageFile $NativeDll (Join-Path $StagedPyLib "windows-x64\opennova.dll")

Ensure-Dir (Join-Path $PythonRoot "apps")
Copy-PackageFile (Join-Path $ROOT "apps\__init__.py") (Join-Path $PythonRoot "apps\__init__.py")
Ensure-Dir (Join-Path $PythonRoot "apps\importer")
Copy-PackageFile (Join-Path $ROOT "apps\importer\__init__.py") (Join-Path $PythonRoot "apps\importer\__init__.py")
Copy-PackageFile (Join-Path $ROOT "apps\importer\jobs.py") (Join-Path $PythonRoot "apps\importer\jobs.py")
Copy-PackageFile (Join-Path $ROOT "apps\importer\resource_plan.py") (Join-Path $PythonRoot "apps\importer\resource_plan.py")
Copy-PackageFile (Join-Path $ROOT "opennova_max\maxscript\OpenNovaImporter.mcr") (Join-Path $MacroRoot "OpenNovaImporter.mcr")

$PackageContents = @"
<?xml version="1.0" encoding="utf-8"?>
<ApplicationPackage
    SchemaVersion="1.0"
    AutodeskProduct="3ds Max"
    ProductType="Application"
    Name="OpenNova Max"
    Description="OpenNova 3ds Max importer"
    AppVersion="$Version"
    UpgradeCode="{5CB72810-3C5F-48B6-9D11-5F661E76BEBB}">
    <CompanyDetails Name="OpenNova" />
    <Components Description="macroscripts parts">
        <RuntimeRequirements OS="Win64" Platform="3ds Max" SeriesMin="$SeriesMin" SeriesMax="$SeriesMax" />
        <ComponentEntry AppName="OpenNovaImporterMacro" Version="$Version" ModuleName="./Contents/macroscripts/OpenNovaImporter.mcr" />
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

VERSION = "$Version"


def _prepend(path: Path) -> None:
    value = str(path)
    normalized = os.path.normcase(os.path.abspath(value))
    existing = {
        os.path.normcase(os.path.abspath(item))
        for item in sys.path
        if item
    }
    if normalized not in existing:
        sys.path.insert(0, value)


def startup() -> None:
    try:
        contents_dir = Path(__file__).resolve().parents[1]
        python_dir = contents_dir / "python"
        _prepend(python_dir)
        os.environ["OPENNOVA_MAX_BUNDLE_DIR"] = str(contents_dir.parent)

        import opennova_max

        loaded_version = opennova_max.get_version()
        opennova_max.register_menu()
        print(f"OpenNova Max {loaded_version} loaded from {contents_dir.parent}")
    except BaseException as exc:
        print(f"OpenNova Max {VERSION} failed to load: {exc}")
        traceback.print_exc()


startup()
"@
Set-Content -LiteralPath (Join-Path $StartupRoot "opennova_max_startup.py") -Value $StartupPy -Encoding UTF8

$MzpRun = @"
name "OpenNova Max $Version"
description "Installs the OpenNova 3ds Max importer"
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


def _message(text: str) -> None:
    print(text)
    try:
        import pymxs

        pymxs.runtime.messageBox(text, title="OpenNova Max")
    except Exception:
        pass


def install() -> Path:
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
    return destination


if __name__ == "__main__":
    try:
        installed = install()
    except Exception as exc:
        _message(f"OpenNova Max {VERSION} install failed:\n{exc}")
        raise
    else:
        _message(
            f"OpenNova Max {VERSION} installed to:\n{installed}\n\n"
            "Restart 3ds Max to use this version."
        )
"@
Set-Content -LiteralPath (Join-Path $STAGE_ROOT "install.py") -Value $InstallPy -Encoding UTF8

Write-Host "=== Validating staged files ==="
$RequiredStageFiles = @(
    (Join-Path $STAGE_ROOT "mzp.run"),
    (Join-Path $STAGE_ROOT "install.ms"),
    (Join-Path $STAGE_ROOT "install.ds"),
    (Join-Path $STAGE_ROOT "install.py"),
    (Join-Path $BundleRoot "PackageContents.xml"),
    (Join-Path $MacroRoot "OpenNovaImporter.mcr"),
    (Join-Path $StartupRoot "opennova_max_startup.ms"),
    (Join-Path $StartupRoot "opennova_max_startup.py"),
    (Join-Path $PythonRoot "opennova_max\__init__.py"),
    (Join-Path $PythonRoot "opennova_max\_packaged_version.py"),
    (Join-Path $PythonRoot "opennova_max\animation.py"),
    (Join-Path $PythonRoot "opennova_max\qt_ui.py"),
    (Join-Path $PythonRoot "opennova_max\ui.py"),
    (Join-Path $PythonRoot "opennova_max\version.py"),
    (Join-Path $PythonRoot "pyopennova\__init__.py"),
    (Join-Path $PythonRoot "pyopennova\animation_build.py"),
    (Join-Path $PythonRoot "pyopennova\lib\windows-x64\opennova.dll"),
    (Join-Path $PythonRoot "apps\importer\resource_plan.py"),
    (Join-Path $PythonRoot "apps\importer\jobs.py")
)
foreach ($path in $RequiredStageFiles) {
    Assert-File $path
}

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
$ExpandedMacroRoot = Join-Path $ExpandedBundle "Contents\macroscripts"
$ExpandedPython = Join-Path $ExpandedBundle "Contents\python"
Assert-File (Join-Path $VALIDATE_ROOT "mzp.run")
Assert-File (Join-Path $VALIDATE_ROOT "install.ms")
Assert-File (Join-Path $VALIDATE_ROOT "install.ds")
Assert-File (Join-Path $VALIDATE_ROOT "install.py")
Assert-File (Join-Path $ExpandedBundle "PackageContents.xml")
Assert-File (Join-Path $ExpandedMacroRoot "OpenNovaImporter.mcr")
Assert-Dir (Join-Path $ExpandedPython "opennova_max")
Assert-Dir (Join-Path $ExpandedPython "pyopennova")
Assert-File (Join-Path $ExpandedPython "opennova_max\_packaged_version.py")
Assert-File (Join-Path $ExpandedPython "opennova_max\animation.py")
Assert-File (Join-Path $ExpandedPython "opennova_max\qt_ui.py")
Assert-File (Join-Path $ExpandedPython "opennova_max\ui.py")
Assert-File (Join-Path $ExpandedPython "opennova_max\version.py")
Assert-File (Join-Path $ExpandedPython "pyopennova\animation_build.py")
Assert-File (Join-Path $ExpandedPython "pyopennova\lib\windows-x64\opennova.dll")
Assert-File (Join-Path $ExpandedPython "apps\importer\resource_plan.py")
Assert-File (Join-Path $ExpandedPython "apps\importer\jobs.py")

Write-Host "=== Running staged Python smoke check ==="
$PY = Resolve-Python
$SmokeFixture = Join-Path $ROOT "fixtures\threedi\3di3\Shed.3di"
Assert-File $SmokeFixture

$SmokeScript = @'
import os
import sys
from pathlib import Path

python_root = Path(os.environ["OPENNOVA_MAX_PYTHON_ROOT"]).resolve()
fixture = os.environ["OPENNOVA_MAX_SMOKE_3DI"]
expected_version = os.environ["OPENNOVA_MAX_EXPECTED_VERSION"]
sys.path.insert(0, str(python_root))

import opennova_max
import opennova_max.animation
import opennova_max.qt_ui
import opennova_max.ui
import pyopennova
import pyopennova.animation_build
from pyopennova._native import _lib_path, load_lib
from pyopennova.threedi_ffi import free_model_ir, read_model_ir

for module in (opennova_max, opennova_max.animation, opennova_max.qt_ui, opennova_max.ui, pyopennova, pyopennova.animation_build):
    module_file = Path(module.__file__).resolve()
    if not str(module_file).lower().startswith(str(python_root).lower()):
        raise RuntimeError(f"{module.__name__} imported from wrong path: {module_file}")

if opennova_max.get_version() != expected_version:
    raise RuntimeError(
        f"Packaged version mismatch: {opennova_max.get_version()} != {expected_version}"
    )

if opennova_max.qt_ui.dialog_title(expected_version) != f"OpenNova Importer v{expected_version}":
    raise RuntimeError("Qt UI helper returned an unexpected dialog title")

menu_script = opennova_max.ui.build_menu_script()
if 'menuMan.createActionItem "OpenNovaImporter" "OpenNova"' not in menu_script:
    raise RuntimeError("Max menu action item is missing from packaged UI script")
if 'macroScript OpenNovaImporter' in menu_script:
    raise RuntimeError("Menu registration should not dynamically define the OpenNova macro")

native_path = Path(_lib_path())
if not native_path.is_file():
    raise RuntimeError(f"Native DLL missing: {native_path}")
load_lib()

ir = read_model_ir(fixture)
try:
    if int(ir.lod_count) <= 0:
        raise RuntimeError("Smoke fixture produced zero LODs")
finally:
    free_model_ir(ir)

print(f"OpenNova Max package smoke check OK: {native_path}")
'@

$oldPythonPath = $env:PYTHONPATH
$oldSmokeRoot = $env:OPENNOVA_MAX_PYTHON_ROOT
$oldSmokeFixture = $env:OPENNOVA_MAX_SMOKE_3DI
$oldExpectedVersion = $env:OPENNOVA_MAX_EXPECTED_VERSION
$env:PYTHONPATH = $ExpandedPython
$env:OPENNOVA_MAX_PYTHON_ROOT = $ExpandedPython
$env:OPENNOVA_MAX_SMOKE_3DI = $SmokeFixture
$env:OPENNOVA_MAX_EXPECTED_VERSION = $Version
try {
    $SmokePath = Join-Path $VALIDATE_ROOT "opennova_max_package_smoke.py"
    Set-Content -LiteralPath $SmokePath -Value $SmokeScript -Encoding UTF8
    Push-Location $VALIDATE_ROOT
    & $PY $SmokePath
    if ($LASTEXITCODE -ne 0) {
        throw "Staged Python smoke check failed with exit code $LASTEXITCODE"
    }
}
finally {
    Pop-Location
    $env:PYTHONPATH = $oldPythonPath
    $env:OPENNOVA_MAX_PYTHON_ROOT = $oldSmokeRoot
    $env:OPENNOVA_MAX_SMOKE_3DI = $oldSmokeFixture
    $env:OPENNOVA_MAX_EXPECTED_VERSION = $oldExpectedVersion
}

Write-Host "=== Done ==="
Get-Item $MzpPath | Select-Object Name, Length
