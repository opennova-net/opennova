# Install or remove the startup hook used by editable Max Python installs.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts\install_max_editable_startup.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\install_max_editable_startup.ps1 -DisableInstalledBundles
#   powershell -ExecutionPolicy Bypass -File scripts\install_max_editable_startup.ps1 -Uninstall

param(
    [string]$MaxVersion = "2022",
    [string]$Language = "ENU",
    [string]$StartupDir = "",
    [string]$UserMacroDir = "",
    [string]$ApplicationPluginsDir = "",
    [switch]$DisableInstalledBundles,
    [switch]$Uninstall
)

$ErrorActionPreference = "Stop"

if (-not $StartupDir) {
    if (-not $env:LOCALAPPDATA) {
        throw "LOCALAPPDATA is not set; cannot locate the 3ds Max user startup directory."
    }
    $StartupDir = Join-Path $env:LOCALAPPDATA "Autodesk\3dsMax\$MaxVersion - 64bit\$Language\scripts\startup"
}
if (-not $UserMacroDir) {
    if (-not $env:LOCALAPPDATA) {
        throw "LOCALAPPDATA is not set; cannot locate the 3ds Max user macro directory."
    }
    $UserMacroDir = Join-Path $env:LOCALAPPDATA "Autodesk\3dsMax\$MaxVersion - 64bit\$Language\usermacros"
}
if (-not $ApplicationPluginsDir) {
    if ($env:APPDATA) {
        $ApplicationPluginsDir = Join-Path $env:APPDATA "Autodesk\ApplicationPlugins"
    }
}

$ROOT = (Resolve-Path "$PSScriptRoot\..").Path
$MacroSourcePath = Join-Path $ROOT "opennova_max\maxscript\OpenNovaImporter.mcr"
$MacroPath = Join-Path $UserMacroDir "OpenNova-OpenNovaImporter.mcr"
$StartupMsPath = Join-Path $StartupDir "opennova_max_editable_startup.ms"
$StartupPyPath = Join-Path $StartupDir "opennova_max_editable_startup.py"
$DisabledPackageName = "PackageContents.xml.disabled-by-opennova-editable"

function Disable-OpenNovaBundles {
    param([string]$Root)

    $disabled = @()
    if (-not $Root) {
        return $disabled
    }
    if (-not (Test-Path -LiteralPath $Root -PathType Container)) {
        return $disabled
    }

    $bundles = Get-ChildItem -LiteralPath $Root -Directory -Filter "OpenNovaMax-*.bundle" -ErrorAction SilentlyContinue
    foreach ($bundle in $bundles) {
        $packageContents = Join-Path $bundle.FullName "PackageContents.xml"
        if (-not (Test-Path -LiteralPath $packageContents -PathType Leaf)) {
            continue
        }

        $disabledPath = Join-Path $bundle.FullName $DisabledPackageName
        if (Test-Path -LiteralPath $disabledPath) {
            $timestamp = Get-Date -Format "yyyyMMddHHmmss"
            $disabledPath = "$disabledPath.$timestamp"
        }

        Move-Item -LiteralPath $packageContents -Destination $disabledPath -Force
        $disabled += $bundle.FullName
    }

    return $disabled
}

if ($Uninstall) {
    foreach ($path in @($MacroPath, $StartupMsPath, $StartupPyPath)) {
        if (Test-Path -LiteralPath $path) {
            Remove-Item -LiteralPath $path -Force
        }
    }
    Write-Host "Removed OpenNova editable startup hook from: $StartupDir"
    return
}

New-Item -ItemType Directory -Force -Path $StartupDir | Out-Null
New-Item -ItemType Directory -Force -Path $UserMacroDir | Out-Null
if (-not (Test-Path -LiteralPath $MacroSourcePath -PathType Leaf)) {
    throw "OpenNova macro source missing: $MacroSourcePath"
}
$MacroSource = Get-Content -LiteralPath $MacroSourcePath -Raw
foreach ($macroName in @("OpenNovaImporter", "OpenNovaExportAse", "OpenNovaExportAnims")) {
    $needle = "macroScript $macroName"
    if ($MacroSource -notlike "*$needle*") {
        throw "OpenNova macro source is missing required macro: $macroName"
    }
}
Copy-Item -LiteralPath $MacroSourcePath -Destination $MacroPath -Force

$StartupMs = @"
(
    local macroPath = @"$MacroPath"
    try
    (
        fileIn macroPath
    )
    catch
    (
        print ("OpenNova editable macro load failed: " + getCurrentException())
    )
    local startupDir = getFilenamePath (getSourceFileName())
    python.executeFile (startupDir + "opennova_max_editable_startup.py")
)
"@

$StartupPy = @'
from __future__ import annotations

import traceback
from pathlib import Path


def startup() -> None:
    try:
        import opennova_max

        loaded_version = opennova_max.get_version()
        opennova_max.register_menu()
        module_file = Path(getattr(opennova_max, "__file__", "")).resolve()
        print(f"OpenNova Max {loaded_version} loaded from editable install: {module_file.parent}")
    except BaseException as exc:
        print(f"OpenNova Max editable startup failed: {exc}")
        traceback.print_exc()


startup()
'@

Set-Content -LiteralPath $StartupMsPath -Value $StartupMs -Encoding ASCII
Set-Content -LiteralPath $StartupPyPath -Value $StartupPy -Encoding UTF8

$DisabledBundles = @()
if ($DisableInstalledBundles) {
    $DisabledBundles = Disable-OpenNovaBundles -Root $ApplicationPluginsDir
}

Write-Host "Installed OpenNova editable startup hook:"
Write-Host "  $MacroPath"
Write-Host "  $StartupMsPath"
Write-Host "  $StartupPyPath"
if ($DisableInstalledBundles) {
    if ($DisabledBundles.Count -gt 0) {
        Write-Host "Disabled OpenNova Max bundles for editable testing:"
        foreach ($bundle in $DisabledBundles) {
            Write-Host "  $bundle"
        }
    }
    else {
        Write-Host "No OpenNova Max bundles needed disabling for editable testing."
    }
}
Write-Host "Restart 3ds Max to register OpenNova importer and export commands."
