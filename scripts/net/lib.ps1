# Shared helpers for the scripts/net/* network-iteration harness.
#
# Dot-source this from the other scripts:  . "$PSScriptRoot\lib.ps1"
#
# Provides: repo-root + .scratch path resolution, run timestamps, a Godot binary
# finder (same contract as the oned-run skill), and Npcap/dumpcap discovery used
# by detect_capture.ps1 / capture.ps1.
#
# Nothing here mutates tracked files; all run output belongs under <repo>\.scratch\.

Set-StrictMode -Version Latest

# Repo root = two levels up from this file (scripts\net\lib.ps1 -> repo).
$script:NetRepoRoot = (Resolve-Path "$PSScriptRoot\..\..").Path

function Get-RepoRoot {
    return $script:NetRepoRoot
}

# <repo>\.scratch (gitignored). Created on demand. Optional subdir.
function Get-ScratchDir {
    param([string] $SubDir = "")
    $dir = Join-Path (Get-RepoRoot) ".scratch"
    if ($SubDir) { $dir = Join-Path $dir $SubDir }
    if (-not (Test-Path $dir)) {
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
    }
    return (Resolve-Path $dir).Path
}

# Sortable run stamp, e.g. 20260626-143501.
function Get-RunStamp {
    return (Get-Date -Format 'yyyyMMdd-HHmmss')
}

# Godot 4.6.1 binary: $env:GODOT_BIN, else .godot-bin\ walking up from the repo
# (worktrees borrow the main checkout's copy). Prefer the _console build so stdout
# reaches the terminal. Returns $null if none found (caller decides whether fatal).
function Find-GodotBinary {
    if ($env:GODOT_BIN -and (Test-Path $env:GODOT_BIN)) {
        return (Resolve-Path $env:GODOT_BIN).Path
    }
    $dir = (Get-RepoRoot)
    while ($dir) {
        foreach ($name in @(
            "Godot_v4.6.1-stable_win64_console.exe",
            "Godot_v4.6.1-stable_win64.exe")) {
            $cand = Join-Path $dir ".godot-bin\$name"
            if (Test-Path $cand) { return (Resolve-Path $cand).Path }
        }
        $parent = Split-Path -Parent $dir
        if ($parent -eq $dir) { break }
        $dir = $parent
    }
    return $null
}

# Locate dumpcap.exe: PATH first, then the standard Wireshark install dirs.
# Returns $null if not found.
function Find-Dumpcap {
    $cmd = Get-Command dumpcap -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    foreach ($p in @(
        "$env:ProgramFiles\Wireshark\dumpcap.exe",
        "${env:ProgramFiles(x86)}\Wireshark\dumpcap.exe")) {
        if ($p -and (Test-Path $p)) { return $p }
    }
    return $null
}

# Is the Npcap (or legacy WinPcap NPF) driver service present and running?
function Test-NpcapRunning {
    $svc = Get-Service -Name npcap, npf -ErrorAction SilentlyContinue |
        Where-Object { $_.Status -eq 'Running' }
    return [bool]$svc
}

# Parse `dumpcap -D` into objects { Number; Name; Description; IsLoopback }.
# Loopback detection keys off Npcap's loopback adapter naming.
function Get-CaptureInterfaces {
    param([string] $Dumpcap)
    if (-not $Dumpcap) { return @() }
    $lines = & $Dumpcap -D 2>$null
    $out = @()
    foreach ($line in $lines) {
        # Format: "<n>. \Device\NPF_{guid} (Friendly Name)"
        if ($line -match '^\s*(\d+)\.\s+(\S+)(?:\s+\((.*)\))?\s*$') {
            $name = $Matches[2]
            $desc = if ($Matches[3]) { $Matches[3] } else { "" }
            $isLoop = ($name -match 'NPF_Loopback') -or
                      ($desc -match 'loopback') -or
                      ($name -match 'Loopback')
            $out += [pscustomobject]@{
                Number      = [int]$Matches[1]
                Name        = $name
                Description = $desc
                IsLoopback  = $isLoop
            }
        }
    }
    return $out
}
