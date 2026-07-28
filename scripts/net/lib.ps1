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
# Cross-language twin: scripts/godot_bin.sh is the sh-side resolver - change both.
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

# Launch the OpenNova game project with an isolated set of NW_LAN_* variables.
# Start-Process inherits this process's environment, so temporarily install only
# the requested role, then restore the caller's exact LAN environment even when
# process creation fails. The returned Process is already detached and visible.
function Start-OpenNovaLanProcess {
    param(
        [Parameter(Mandatory = $true)] [hashtable] $LanEnvironment
    )

    $godot = Find-GodotBinary
    if (-not $godot) {
        throw "Godot 4.6.1 was not found. Set GODOT_BIN or populate a .godot-bin directory above the repo."
    }

    $projectDir = Join-Path (Get-RepoRoot) "godot"
    if (-not (Test-Path (Join-Path $projectDir "project.godot"))) {
        throw "OpenNova Godot project not found at: $projectDir"
    }

    foreach ($name in $LanEnvironment.Keys) {
        if (-not ([string] $name).StartsWith("NW_LAN_", [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "Start-OpenNovaLanProcess accepts only NW_LAN_* variables (got '$name')."
        }
    }

    # Keep this child on the local-LAN entry path even when the calling shell was
    # previously used for online-gate, replay, or single-player development. Those
    # alternate-mode variables are suppressed for the child and restored below.
    $isolatedEntries = {
        Get-ChildItem Env: | Where-Object {
            $_.Name -like "NW_LAN_*" -or
            $_.Name -like "NW_GATE_*" -or
            $_.Name -like "NW_REPLAY*" -or
            $_.Name -eq "NW_SP_MISSION"
        }
    }

    $saved = @{}
    foreach ($entry in @(& $isolatedEntries)) {
        $saved[$entry.Name] = $entry.Value
    }

    try {
        foreach ($entry in @(& $isolatedEntries)) {
            [Environment]::SetEnvironmentVariable(
                $entry.Name, $null, [EnvironmentVariableTarget]::Process)
        }
        foreach ($name in $LanEnvironment.Keys) {
            [Environment]::SetEnvironmentVariable(
                [string] $name, [string] $LanEnvironment[$name], [EnvironmentVariableTarget]::Process)
        }

        # No --headless/hidden flags: each child is an ordinary, visible game instance.
        # Quote the project path because Start-Process flattens ArgumentList on Windows.
        return Start-Process -FilePath $godot `
            -WorkingDirectory $projectDir `
            -ArgumentList @("--path", ('"{0}"' -f $projectDir)) `
            -PassThru
    }
    finally {
        foreach ($entry in @(& $isolatedEntries)) {
            [Environment]::SetEnvironmentVariable(
                $entry.Name, $null, [EnvironmentVariableTarget]::Process)
        }
        foreach ($name in $saved.Keys) {
            [Environment]::SetEnvironmentVariable(
                [string] $name, [string] $saved[$name], [EnvironmentVariableTarget]::Process)
        }
    }
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
