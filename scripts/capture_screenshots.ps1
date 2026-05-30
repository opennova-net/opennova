# Regenerate the README editor screenshots.
#
# Boots the OpenNova Editor (ONED) via the capture driver scene, which switches
# through each workspace, opens a representative asset from the resource dir, and
# writes PNGs to <repo>\screenshots\ (LFS-tracked). Re-run after UI changes to
# keep the README screenshots in sync with the editor.
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

$GODOT_BIN = $env:GODOT_BIN
if (-not $GODOT_BIN) {
    $GODOT_BIN = "$ROOT\.godot-bin\Godot_v4.6.1-stable_win64.exe"
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
if ($LASTEXITCODE -ne 0) {
    Write-Error "screenshot capture failed (exit $LASTEXITCODE)"
}

Write-Host "Screenshots written to $ROOT\screenshots"
