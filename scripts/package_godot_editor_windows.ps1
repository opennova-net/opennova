# Build and export the OpenNova Mod Tools Godot package for Windows.

$ErrorActionPreference = "Stop"

& "$PSScriptRoot\package_godot_windows.ps1" -Target editor
