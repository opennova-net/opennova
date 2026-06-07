# Build and export the OpenNova Runtime Godot package for Windows.

param([switch]$SkipBuild)

$ErrorActionPreference = "Stop"

& "$PSScriptRoot\package_godot_windows.ps1" -Target runtime -SkipBuild:$SkipBuild
