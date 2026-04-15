# Build and export OpenNova Godot applications for Windows.
#
# Produces:
#   dist\opennova.exe             (Runtime)
#   dist\opennova-modtools.exe    (Mod Tools)
#
# Requires:
#   - MSVC toolchain + cmake (preinstalled on windows-latest)
#   - Git submodules already initialised (third_party/godot-cpp)
#
# Usage:
#   pwsh -File scripts\package_godot_windows.ps1

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"  # keeps Invoke-WebRequest fast on large files

$ROOT = (Resolve-Path "$PSScriptRoot\..").Path
Set-Location $ROOT

$GODOT_VERSION = "4.6.1-stable"
$GODOT_VERSION_PLAIN = "4.6.1"
$GODOT_ZIP_URL = "https://github.com/godotengine/godot/releases/download/$GODOT_VERSION/Godot_v${GODOT_VERSION}_win64.exe.zip"
$TEMPLATES_URL = "https://github.com/godotengine/godot/releases/download/$GODOT_VERSION/Godot_v${GODOT_VERSION}_export_templates.tpz"
$TEMPLATES_DIR = "$env:APPDATA\Godot\export_templates\${GODOT_VERSION_PLAIN}.stable"

$GODOT_DIR = "$ROOT\.godot-bin"
$GODOT_EXE = "$GODOT_DIR\Godot_v${GODOT_VERSION}_win64.exe"

# ---------------------------------------------------------------------------
# 1. Install Godot editor (skip if already present)
# ---------------------------------------------------------------------------
if (-not (Test-Path $GODOT_EXE)) {
    Write-Host "=== Downloading Godot $GODOT_VERSION ==="
    New-Item -ItemType Directory -Force -Path $GODOT_DIR | Out-Null
    $tempZip = "$GODOT_DIR\godot.zip"
    Invoke-WebRequest -Uri $GODOT_ZIP_URL -OutFile $tempZip
    Expand-Archive -Path $tempZip -DestinationPath $GODOT_DIR -Force
    Remove-Item $tempZip
}
if (-not (Test-Path $GODOT_EXE)) {
    throw "Godot executable missing after extraction: $GODOT_EXE"
}

# ---------------------------------------------------------------------------
# 2. Install export templates (skip if already present)
# ---------------------------------------------------------------------------
if (-not (Test-Path "$TEMPLATES_DIR\version.txt")) {
    Write-Host "=== Downloading export templates ==="
    New-Item -ItemType Directory -Force -Path $TEMPLATES_DIR | Out-Null
    $tempTpz = "$GODOT_DIR\templates.zip"
    Invoke-WebRequest -Uri $TEMPLATES_URL -OutFile $tempTpz
    $extractDir = "$GODOT_DIR\templates-extract"
    if (Test-Path $extractDir) { Remove-Item $extractDir -Recurse -Force }
    Expand-Archive -Path $tempTpz -DestinationPath $extractDir -Force
    # .tpz archives contain a "templates" root folder
    Copy-Item "$extractDir\templates\*" $TEMPLATES_DIR -Recurse -Force
    Remove-Item $tempTpz
    Remove-Item $extractDir -Recurse -Force
}

# ---------------------------------------------------------------------------
# 3. Build release GDExtension
# ---------------------------------------------------------------------------
Write-Host "=== Building release GDExtension ==="
cmake -S godot/engine -B build-godot-release -DGODOTCPP_TARGET=template_release
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed (exit $LASTEXITCODE)" }
cmake --build build-godot-release --config Release --target opennova
if ($LASTEXITCODE -ne 0) { throw "CMake build failed (exit $LASTEXITCODE)" }

$DLL = "$ROOT\godot\bin\libopennova.windows.template_release.x86_64.dll"
if (-not (Test-Path $DLL)) {
    throw "Release DLL missing after build: $DLL"
}

# ---------------------------------------------------------------------------
# 4. Run headless exports
# ---------------------------------------------------------------------------
$DIST = "$ROOT\dist"
New-Item -ItemType Directory -Force -Path $DIST | Out-Null

function Invoke-GodotExport {
    param([string]$PresetName, [string]$OutputPath)
    # Pass as a single command-line string so spaces in $PresetName survive.
    # Start-Process -Wait blocks until Godot exits; plain `&` has shown to return
    # before Godot finishes writing the .exe under some output-redirection setups.
    $arguments = "--headless --path godot --export-release `"$PresetName`" `"$OutputPath`""
    $proc = Start-Process -FilePath $GODOT_EXE -ArgumentList $arguments -NoNewWindow -Wait -PassThru
    if ($proc.ExitCode -ne 0) {
        throw "Godot export of '$PresetName' exited with code $($proc.ExitCode)"
    }
    if (-not (Test-Path $OutputPath)) {
        throw "Godot export of '$PresetName' produced no file at $OutputPath"
    }
}

Write-Host "=== Exporting opennova-modtools.exe ==="
Invoke-GodotExport -PresetName "OpenNova Mod Tools" -OutputPath "$DIST\opennova-modtools.exe"

Write-Host "=== Exporting opennova.exe ==="
Invoke-GodotExport -PresetName "OpenNova Runtime" -OutputPath "$DIST\opennova.exe"

# ---------------------------------------------------------------------------
# 5. Done
# ---------------------------------------------------------------------------
Write-Host "=== Done ==="
Get-Item "$DIST\opennova.exe","$DIST\opennova-modtools.exe" | Select-Object Name, Length
