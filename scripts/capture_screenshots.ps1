# Regenerate the README editor and importer screenshots.
#
# Boots the OpenNova Editor (ONED) via the capture driver scene, which switches
# through each workspace, opens a representative asset from the resource dir, and
# writes PNGs to <repo>\screenshots\ (LFS-tracked). Then captures the standalone
# importer UI through its deterministic Qt screenshot driver. Re-run after UI
# changes to keep the README screenshots in sync with the tools.
#
# Requires a real rendering window (NOT --headless), so run on a desktop session.
#
# Expects $env:GODOT_BIN to point at Godot 4.6.1, or falls back to the binary in
# .godot-bin\.
#
# Assets are opened by name from $env:NOVA_RESOURCE_DIR (the external resource
# dir that also feeds the runtime/editor). Defaults to C:\Users\taylor\Desktop\
# revx02; override by setting NOVA_RESOURCE_DIR. The dir must hold Dvxi5.trn (+
# textures), full_00.env, the font, credits, the strings table, and the object.
#
# Usage:
#   pwsh -File scripts\capture_screenshots.ps1

$ErrorActionPreference = "Stop"

$ROOT = (Resolve-Path "$PSScriptRoot\..").Path

function Find-GodotBinary {
    param([string] $StartDir)

    $Dir = (Resolve-Path $StartDir).Path
    while ($Dir) {
        $Candidate = Join-Path $Dir ".godot-bin\Godot_v4.6.1-stable_win64.exe"
        if (Test-Path $Candidate) {
            return $Candidate
        }
        $Parent = Split-Path -Parent $Dir
        if (-not $Parent -or $Parent -eq $Dir) {
            break
        }
        $Dir = $Parent
    }
    return ""
}

$GODOT_BIN = $env:GODOT_BIN
if (-not $GODOT_BIN) {
    $GODOT_BIN = Find-GodotBinary $ROOT
}
if (-not (Test-Path $GODOT_BIN)) {
    Write-Error "set GODOT_BIN to a Godot 4.6.1 binary (or drop one in .godot-bin\)"
}

if (-not $env:NOVA_RESOURCE_DIR) {
    $env:NOVA_RESOURCE_DIR = "C:\Users\taylor\Desktop\revx02"
}
if (-not (Test-Path $env:NOVA_RESOURCE_DIR)) {
    Write-Error "NOVA_RESOURCE_DIR '$($env:NOVA_RESOURCE_DIR)' does not exist"
}

& $GODOT_BIN --path "$ROOT\godot" --resolution 1600x900 `
    "res://modtools/tools/screenshot_capture.tscn"
$EditorExitCode = $LASTEXITCODE
if ($null -ne $EditorExitCode -and $EditorExitCode -ne 0) {
    Write-Error "editor screenshot capture failed (exit $EditorExitCode)"
}

Push-Location $ROOT
try {
    & uv run python -m apps.importer.ui.screenshot_capture `
        --output "$ROOT\screenshots\importer.png"
    $ImporterExitCode = $LASTEXITCODE
    if ($null -ne $ImporterExitCode -and $ImporterExitCode -ne 0) {
        Write-Error "importer screenshot capture failed (exit $ImporterExitCode)"
    }
} finally {
    Pop-Location
}

Write-Host "Screenshots written to $ROOT\screenshots"
exit 0
