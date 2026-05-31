# Build and export the OpenNova Runtime Godot package for Windows.

$ErrorActionPreference = "Stop"

& "$PSScriptRoot\package_godot_windows.ps1" -Target runtime
