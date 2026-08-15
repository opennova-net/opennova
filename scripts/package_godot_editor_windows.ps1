# Build and export the OpenNova Mod Tools Godot package for Windows.

param(
    [switch]$SkipBuild,
    [ValidateSet("debug", "release")]
    [string]$ExportMode = "release"
)

$ErrorActionPreference = "Stop"

& "$PSScriptRoot\package_godot_windows.ps1" -Target editor -SkipBuild:$SkipBuild -ExportMode $ExportMode
