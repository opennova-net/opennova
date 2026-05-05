# Install or remove the startup hook used by editable Max Python installs.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts\install_max_editable_startup.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\install_max_editable_startup.ps1 -Uninstall

param(
    [string]$MaxVersion = "2022",
    [string]$Language = "ENU",
    [string]$StartupDir = "",
    [switch]$Uninstall
)

$ErrorActionPreference = "Stop"

if (-not $StartupDir) {
    if (-not $env:LOCALAPPDATA) {
        throw "LOCALAPPDATA is not set; cannot locate the 3ds Max user startup directory."
    }
    $StartupDir = Join-Path $env:LOCALAPPDATA "Autodesk\3dsMax\$MaxVersion - 64bit\$Language\scripts\startup"
}

$StartupMsPath = Join-Path $StartupDir "opennova_max_editable_startup.ms"
$StartupPyPath = Join-Path $StartupDir "opennova_max_editable_startup.py"

if ($Uninstall) {
    foreach ($path in @($StartupMsPath, $StartupPyPath)) {
        if (Test-Path -LiteralPath $path) {
            Remove-Item -LiteralPath $path -Force
        }
    }
    Write-Host "Removed OpenNova editable startup hook from: $StartupDir"
    return
}

New-Item -ItemType Directory -Force -Path $StartupDir | Out-Null

$StartupMs = @'
(
    local startupDir = getFilenamePath (getSourceFileName())
    python.executeFile (startupDir + "opennova_max_editable_startup.py")
)
'@

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

Write-Host "Installed OpenNova editable startup hook:"
Write-Host "  $StartupMsPath"
Write-Host "  $StartupPyPath"
Write-Host "Restart 3ds Max to register OpenNova > Importer..."
