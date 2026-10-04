# Shared helpers for the scripts/net/* network-iteration harness.
#
# Dot-source this from the other scripts:  . "$PSScriptRoot\lib.ps1"
#
# Provides: repo-root + .scratch path resolution, run timestamps, a Godot binary
# finder (GODOT_BIN, then .godot-bin/), the retail-root getters, the proven
# OpenNova process launch/stop pair (Start-OpenNovaProcess with its MCP
# endpoint, Stop-OpenNovaProcess), the joiner readiness classifiers, and
# Npcap/dumpcap discovery used by detect_capture.ps1 / capture.ps1.
#
# Nothing here mutates tracked files; all run output belongs under <repo>\.scratch\.

Set-StrictMode -Version Latest

# Repo root = two levels up from this file (scripts\net\lib.ps1 -> repo).
$script:NetRepoRoot = (Resolve-Path "$PSScriptRoot\..\..").Path

# The opennova-game MCP client (Invoke-GameTool, Wait-GameMcpReady,
# Invoke-GameProbe, Stop-GameViaMcp, Get-StructuredResult; docs/mcp.md).
. (Join-Path $PSScriptRoot "..\mcp\game_mcp.ps1")

function Get-RepoRoot {
    return $script:NetRepoRoot
}

function Get-NetHarnessBytesSha256 {
    param([Parameter(Mandatory = $true)] [byte[]] $Bytes)

    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($sha.ComputeHash($Bytes)) -replace '-', '').ToLowerInvariant()
    }
    finally {
        $sha.Dispose()
    }
}

# Classify one parity-driver heartbeat without confusing a healthy post-death
# deployment wait with a dropped session. Retail leaves the local player row
# alive after S2C 0x13 and returns to the player-paced deployment state; it does
# not automatically emit another C2S 0x0E. The post-death class is opt-in so an
# initial join can never satisfy in-match readiness before it has entered play.
function Get-OpenNovaJoinerReadinessClass {
    param(
        [Parameter(Mandatory = $true)] $State,
        [Parameter(Mandatory = $true)]
        [ValidateSet('in_match', 'deploy_hold')] [string] $ReadinessMode,
        [switch] $AllowPostDeathRedeploy,
        # A scenario run deploys the held joiner itself (its steps press a
        # deploy key), so a deploy_hold run may end in the match.
        [switch] $AllowScenarioDeploy
    )

    $required = @(
        'readiness_mode', 'process_id', 'ticks_msec', 'heartbeat_sequence',
        'in_match', 'local_player', 'deploy_hold_ready',
        'deployment_pick_pending', 'deployment_pick_sent', 'auto_deploy',
        'self_handle', 'join_error', 'session_lost', 'joiner_phase',
        'join_admission_stage', 'deploy_presented', 'motion_complete'
    )
    if ($null -eq $State) { return 'invalid' }
    foreach ($name in $required) {
        if ($null -eq $State.PSObject.Properties[$name]) { return 'invalid' }
    }
    if ([string] $State.readiness_mode -cne $ReadinessMode -or
            [int] $State.process_id -le 0 -or
            [int64] $State.ticks_msec -le 0 -or
            [int64] $State.heartbeat_sequence -le 0 -or
            [int] $State.self_handle -le 0 -or
            -not [string]::IsNullOrEmpty([string] $State.join_error) -or
            [bool] $State.session_lost) {
        return 'invalid'
    }

    if ($ReadinessMode -eq 'deploy_hold') {
        # The witnessed deploy hold (net-re 5.61): the protocol InMatch phase is
        # open and the internal motor exists (the pre-pick C2S 0x0C source)
        # while the player-paced DEATH screen holds presentation. The class
        # asserts the raw observed split; the driver never projects fields.
        # The hold has two admission shapes: the post-auth stage still owes
        # the pick, or admission completed while the host still holds the
        # deploy overlay (the engine's joiner_deploy_hold_ready).
        $awaitingPick = [bool] $State.deployment_pick_pending -and
            [string] $State.join_admission_stage -ceq "awaiting the player's deployment pick"
        $initialOverlay = -not [bool] $State.deployment_pick_pending -and
            [string] $State.join_admission_stage -ceq "complete"
        if ([bool] $State.in_match -and
                [bool] $State.local_player -and
                [bool] $State.deploy_hold_ready -and
                -not [bool] $State.deployment_pick_sent -and
                -not [bool] $State.auto_deploy -and
                [bool] $State.deploy_presented -and
                [int] $State.joiner_phase -eq 4 -and
                ($awaitingPick -or $initialOverlay)) {
            return 'deploy_hold'
        }
        if ($AllowScenarioDeploy -and [bool] $State.in_match -and
                [bool] $State.local_player -and -not [bool] $State.deploy_presented) {
            return 'in_match'
        }
        return 'invalid'
    }

    if ([bool] $State.in_match -and [bool] $State.local_player -and
            -not [bool] $State.deploy_presented) {
        return 'in_match'
    }
    if ($AllowPostDeathRedeploy -and
            -not [bool] $State.in_match -and
            [bool] $State.local_player -and
            -not [bool] $State.deploy_hold_ready -and
            [bool] $State.deployment_pick_pending -and
            -not [bool] $State.deployment_pick_sent -and
            [bool] $State.deploy_presented -and
            [int] $State.joiner_phase -eq 3 -and
            [string] $State.join_admission_stage -ceq
                "awaiting the player's deployment pick") {
        return 'post_death_redeploy'
    }
    return 'invalid'
}

function Test-OpenNovaJoinerReadinessTransition {
    param(
        [Parameter(Mandatory = $true)] $Initial,
        [Parameter(Mandatory = $true)] $Final,
        [Parameter(Mandatory = $true)]
        [ValidateSet('in_match', 'deploy_hold')] [string] $ReadinessMode,
        [switch] $AllowPostDeathRedeploy,
        [switch] $AllowScenarioDeploy,
        [switch] $RequireMotionComplete
    )

    $initialExpected = if ($ReadinessMode -eq 'in_match') {
        'in_match'
    } else {
        'deploy_hold'
    }
    if ((Get-OpenNovaJoinerReadinessClass -State $Initial `
                -ReadinessMode $ReadinessMode) -cne $initialExpected) {
        return $false
    }
    $finalClass = Get-OpenNovaJoinerReadinessClass -State $Final `
        -ReadinessMode $ReadinessMode `
        -AllowPostDeathRedeploy:$AllowPostDeathRedeploy `
        -AllowScenarioDeploy:$AllowScenarioDeploy
    if ($finalClass -eq 'invalid' -or
            [int] $Final.process_id -ne [int] $Initial.process_id -or
            [int] $Final.self_handle -ne [int] $Initial.self_handle -or
            [int64] $Final.ticks_msec -le [int64] $Initial.ticks_msec -or
            [int64] $Final.heartbeat_sequence -le
                [int64] $Initial.heartbeat_sequence -or
            ($RequireMotionComplete -and -not [bool] $Final.motion_complete)) {
        return $false
    }
    return $true
}

function ConvertTo-OpenNovaJoinerReadinessSnapshot {
    param([Parameter(Mandatory = $true)] $State)

    return [pscustomobject][ordered]@{
        process_id = [int] $State.process_id
        ticks_msec = [int64] $State.ticks_msec
        heartbeat_sequence = [int64] $State.heartbeat_sequence
        readiness_mode = [string] $State.readiness_mode
        in_match = [bool] $State.in_match
        local_player = [bool] $State.local_player
        deploy_hold_ready = [bool] $State.deploy_hold_ready
        deployment_pick_pending = [bool] $State.deployment_pick_pending
        deployment_pick_sent = [bool] $State.deployment_pick_sent
        auto_deploy = [bool] $State.auto_deploy
        self_handle = [int] $State.self_handle
        join_error = [string] $State.join_error
        session_lost = [bool] $State.session_lost
        joiner_phase = [int] $State.joiner_phase
        join_admission_stage = [string] $State.join_admission_stage
        deploy_presented = [bool] $State.deploy_presented
        motion_complete = [bool] $State.motion_complete
    }
}

# Return the exact 616-byte BMS header and its authoritative printable mission
# title from an already hash-bound artifact. Retail's 0x7B serializer chooses
# between a mission-title path and a mode-specific mission-file fallback, while
# its map field is always the mission file. The same header is sent as S2C 0x0B,
# so callers can bind both identities without a per-map exception table.
function Get-BmsHeaderIdentity {
    param([Parameter(Mandatory = $true)] [string] $Path)

    $resolved = (Resolve-Path -LiteralPath $Path -ErrorAction Stop).Path
    [byte[]] $raw = [IO.File]::ReadAllBytes($resolved)
    if ($raw.Length -lt 616) {
        throw "BMS is shorter than its 616-byte header: $resolved"
    }
    if ($raw[0] -ne [byte][char]'B' -or
            $raw[1] -ne [byte][char]'M' -or
            $raw[2] -ne [byte][char]'S' -or
            $raw[3] -lt 19) {
        throw "BMS has invalid magic/version: $resolved"
    }

    $length = 0
    while ($length -lt 32 -and $raw[4 + $length] -ne 0) {
        $value = [int] $raw[4 + $length]
        if ($value -lt 0x20 -or $value -gt 0x7e) {
            throw "BMS mission_name is not printable ASCII: $resolved"
        }
        $length++
    }
    if ($length -eq 0) {
        throw "BMS mission_name is empty: $resolved"
    }
    $title = [Text.Encoding]::ASCII.GetString($raw, 4, $length)
    if ([string]::IsNullOrWhiteSpace($title)) {
        throw "BMS mission_name is whitespace-only: $resolved"
    }
    [byte[]] $header = [byte[]] $raw[0..615]
    return [pscustomobject]@{
        Path = $resolved
        Bytes = $header
        Sha256 = Get-NetHarnessBytesSha256 $header
        MissionDisplayName = $title
    }
}

function Test-ParityAdvertisedMissionName {
    param(
        [Parameter(Mandatory = $true)] [string] $Advertised,
        [Parameter(Mandatory = $true)] [string] $MissionFile,
        [Parameter(Mandatory = $true)] [string] $MissionDisplayName
    )

    return $Advertised.Equals($MissionFile, [StringComparison]::Ordinal) -or
        $Advertised.Equals($MissionDisplayName, [StringComparison]::Ordinal)
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

# Normalize Godot's `--resolution WIDTHxHEIGHT` value before any process is
# launched. Keeping this shared prevents a pair run from starting its host and
# only then discovering that the joiner's viewport value is malformed.
function ConvertTo-OpenNovaResolution {
    param([Parameter(Mandatory = $true)] [string] $Value)

    $candidate = $Value.Trim()
    if ($candidate -notmatch '^(?<width>[1-9][0-9]*)[xX](?<height>[1-9][0-9]*)$') {
        throw "-Resolution must have the form WIDTHxHEIGHT using positive integers."
    }
    $width = 0
    $height = 0
    if (-not [int]::TryParse($Matches['width'], [ref] $width) -or
            -not [int]::TryParse($Matches['height'], [ref] $height) -or
            $width -gt 16384 -or $height -gt 16384) {
        throw "-Resolution dimensions must each be from 1 through 16384."
    }
    return ('{0}x{1}' -f $width, $height)
}

# Godot 4.6.1 binary: $env:GODOT_BIN, else .godot-bin\ walking up from the repo
# (worktrees borrow the main checkout's copy). Prefer the official _console entry
# when discovering a Windows installation; Start-OpenNovaProcess resolves that
# wrapper to its sibling runtime so its returned PID owns the window and sockets.
# Returns $null if none is found (the caller decides whether that is fatal).
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

# The documented machine roots (docs/dev-env-vars.md): a packed retail install
# (OPENNOVA_JO_DIR) and an extracted asset tree (OPENNOVA_JO_ASSETS, which also
# carries the shipped .bms missions loose at its root). Scripts resolve them
# here and nowhere else. Both getters return $null when the variable is unset
# or the directory is missing; the caller decides whether that is fatal.
# Cross-language twins: tests/common/retail_paths.h (C++) and
# godot/tests/support/retail_data.gd (GUT) - change all three together.
function Get-OpenNovaRetailInstall {
    if ($env:OPENNOVA_JO_DIR -and (Test-Path -LiteralPath $env:OPENNOVA_JO_DIR -PathType Container)) {
        return (Resolve-Path -LiteralPath $env:OPENNOVA_JO_DIR).Path
    }
    return $null
}

function Get-OpenNovaRetailAssets {
    if ($env:OPENNOVA_JO_ASSETS -and (Test-Path -LiteralPath $env:OPENNOVA_JO_ASSETS -PathType Container)) {
        return (Resolve-Path -LiteralPath $env:OPENNOVA_JO_ASSETS).Path
    }
    return $null
}

# A file under the extracted asset tree by case-insensitive name (retail
# archives mix ITEMS.DEF and items.def). $null when the tree or the file is absent.
function Get-OpenNovaRetailAssetFile {
    param([Parameter(Mandatory = $true)] [string] $Name)
    $root = Get-OpenNovaRetailAssets
    if (-not $root) { return $null }
    $hit = Get-ChildItem -LiteralPath $root -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -ieq $Name } | Select-Object -First 1
    if ($hit) { return $hit.FullName }
    return $null
}

# The official Windows `_console.exe` is a small wrapper that starts the real
# Godot executable as a child. Keep resolving the exact sibling separately: the
# wrapper remains the launch/lifecycle owner, while callers receive the runtime
# Process that owns the window and LAN socket.
function Resolve-GodotRuntimeBinary {
    param([Parameter(Mandatory = $true)] [string] $Path)

    $resolved = (Resolve-Path -LiteralPath $Path -ErrorAction Stop).Path
    $leaf = [IO.Path]::GetFileName($resolved)
    if ($leaf -notmatch '^(?<stem>.+)_console\.exe$') {
        return $resolved
    }

    $runtime = Join-Path ([IO.Path]::GetDirectoryName($resolved)) `
        ($Matches['stem'] + '.exe')
    if (-not (Test-Path -LiteralPath $runtime -PathType Leaf)) {
        throw "Godot console wrapper has no sibling runtime executable: $resolved"
    }
    return (Resolve-Path -LiteralPath $runtime -ErrorAction Stop).Path
}

function Test-OpenNovaPathEqual {
    param(
        [Parameter(Mandatory = $true)] [string] $Left,
        [Parameter(Mandatory = $true)] [string] $Right
    )

    $leftFull = [IO.Path]::GetFullPath($Left)
    $rightFull = [IO.Path]::GetFullPath($Right)
    return $leftFull.Equals($rightFull, [StringComparison]::OrdinalIgnoreCase)
}

function ConvertFrom-OpenNovaWindowsCommandLine {
    param([Parameter(Mandatory = $true)] [string] $CommandLine)

    if (-not ('OpenNova.NetHarness.WindowsCommandLine' -as [type])) {
        $null = Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

namespace OpenNova.NetHarness {
    public static class WindowsCommandLine {
        [DllImport("shell32.dll", SetLastError = true)]
        private static extern IntPtr CommandLineToArgvW(
            [MarshalAs(UnmanagedType.LPWStr)] string commandLine,
            out int argumentCount);

        [DllImport("kernel32.dll")]
        private static extern IntPtr LocalFree(IntPtr memory);

        public static string[] Split(string commandLine) {
            int count;
            IntPtr argv = CommandLineToArgvW(commandLine, out count);
            if (argv == IntPtr.Zero) {
                throw new Win32Exception(Marshal.GetLastWin32Error());
            }
            try {
                string[] result = new string[count];
                for (int i = 0; i < count; ++i) {
                    IntPtr value = Marshal.ReadIntPtr(argv, i * IntPtr.Size);
                    result[i] = Marshal.PtrToStringUni(value);
                }
                return result;
            } finally {
                LocalFree(argv);
            }
        }
    }
}
'@
    }

    return [OpenNova.NetHarness.WindowsCommandLine]::Split($CommandLine)
}

function Assert-OpenNovaProcessCommandLine {
    param(
        [Parameter(Mandatory = $true)] [string] $Label,
        [Parameter(Mandatory = $true)] [string] $CommandLine,
        [Parameter(Mandatory = $true)] [string] $ExpectedExecutable,
        [Parameter(Mandatory = $true)] [string[]] $ExpectedArguments
    )

    $actual = @(ConvertFrom-OpenNovaWindowsCommandLine -CommandLine $CommandLine)
    if ($actual.Count -ne ($ExpectedArguments.Count + 1)) {
        throw "$Label command line has $($actual.Count) token(s); expected $($ExpectedArguments.Count + 1)."
    }
    if (-not (Test-OpenNovaPathEqual -Left $actual[0] -Right $ExpectedExecutable)) {
        throw "$Label command line executable is not the selected binary: $($actual[0])"
    }
    for ($index = 0; $index -lt $ExpectedArguments.Count; ++$index) {
        if (-not [string]::Equals(
                [string] $actual[$index + 1],
                [string] $ExpectedArguments[$index],
                [StringComparison]::Ordinal)) {
            throw "$Label command line argument $index differs from the requested launch."
        }
    }
}

function ConvertTo-OpenNovaUtc {
    param([Parameter(Mandatory = $true)] [DateTime] $Value)

    if ($Value.Kind -eq [DateTimeKind]::Unspecified) {
        return [TimeZoneInfo]::ConvertTimeToUtc($Value)
    }
    return $Value.ToUniversalTime()
}

function Test-OpenNovaProcessExecutionStarted {
    param([Parameter(Mandatory = $true)] [System.Diagnostics.Process] $Process)

    try {
        $Process.Refresh()
        if ($Process.HasExited) { return $false }
        foreach ($thread in @($Process.Threads)) {
            try {
                if ($thread.ThreadState -ne [System.Diagnostics.ThreadState]::Wait) {
                    return $true
                }
                if ($thread.WaitReason -ne [System.Diagnostics.ThreadWaitReason]::Suspended) {
                    return $true
                }
            }
            catch {
                # A thread can disappear while its state is sampled. Retry the
                # process on the next discovery pass instead of treating that
                # transient as proof that the suspended child was resumed.
            }
        }
    }
    catch {
        return $false
    }
    return $false
}

function Get-OpenNovaProcessRecord {
    param([Parameter(Mandatory = $true)] [int] $ProcessId)

    $records = @(Get-CimInstance -ClassName Win32_Process `
        -Filter "ProcessId = $ProcessId" -ErrorAction Stop)
    if ($records.Count -gt 1) {
        throw "Process identity query returned duplicate PID $ProcessId records."
    }
    if ($records.Count -eq 0) { return $null }
    return $records[0]
}

function Wait-OpenNovaProcessRecord {
    param(
        [Parameter(Mandatory = $true)] [System.Diagnostics.Process] $Process,
        [Parameter(Mandatory = $true)] [string] $ExpectedExecutable,
        [Parameter(Mandatory = $true)] [string[]] $ExpectedArguments,
        [Parameter(Mandatory = $true)] [DateTime] $NotBeforeUtc,
        [Parameter(Mandatory = $true)] [DateTime] $DeadlineUtc,
        [Parameter(Mandatory = $true)] [string] $Label
    )

    do {
        $Process.Refresh()
        if ($Process.HasExited) {
            throw "$Label exited before its process identity could be proven."
        }
        $record = Get-OpenNovaProcessRecord -ProcessId $Process.Id
        if ($record -and $record.ExecutablePath -and $record.CommandLine) {
            if (-not (Test-OpenNovaPathEqual `
                    -Left ([string] $record.ExecutablePath) `
                    -Right $ExpectedExecutable)) {
                throw "$Label PID $($Process.Id) executable differs from the selected binary."
            }
            $createdUtc = ConvertTo-OpenNovaUtc -Value ([DateTime] $record.CreationDate)
            if ($createdUtc -lt $NotBeforeUtc.AddSeconds(-1)) {
                throw "$Label PID $($Process.Id) predates the current launch."
            }
            Assert-OpenNovaProcessCommandLine `
                -Label $Label `
                -CommandLine ([string] $record.CommandLine) `
                -ExpectedExecutable $ExpectedExecutable `
                -ExpectedArguments $ExpectedArguments
            return [pscustomobject]@{
                Record = $record
                CreatedUtc = $createdUtc
            }
        }
        Start-Sleep -Milliseconds 50
    } while ([DateTime]::UtcNow -lt $DeadlineUtc)

    throw "$Label process identity was not observable before the launch deadline."
}

function Stop-OpenNovaFailedLaunch {
    param(
        [System.Diagnostics.Process] $LauncherProcess,
        [string] $ExpectedRuntime,
        [DateTime] $LauncherStartUtc
    )

    if (-not $LauncherProcess) { return }
    $launcherId = $LauncherProcess.Id
    $cleanupDeadline = [DateTime]::UtcNow.AddSeconds(8)

    do {
        $LauncherProcess.Refresh()
        if ($LauncherProcess.HasExited) { break }
        try { $LauncherProcess.Kill() }
        catch {
            $LauncherProcess.Refresh()
            if (-not $LauncherProcess.HasExited -and
                    [DateTime]::UtcNow -ge $cleanupDeadline) {
                throw "Could not stop failed Godot launcher PID ${launcherId}: $($_.Exception.Message)"
            }
        }
        $null = $LauncherProcess.WaitForExit(250)
    } while ([DateTime]::UtcNow -lt $cleanupDeadline)
    $LauncherProcess.Refresh()
    if (-not $LauncherProcess.HasExited) {
        throw "Failed Godot launcher PID $launcherId remained alive after bounded cleanup."
    }

    if (-not $ExpectedRuntime) { return }

    # A wrapper that fails between CreateProcess(CREATE_SUSPENDED) and
    # AssignProcessToJobObject can leave its exact child outside the job. After
    # proving the wrapper exited, poll until CIM has been quiet for one second;
    # this catches a child whose record becomes visible after the first query.
    $quietSinceUtc = $null
    $lastAuditError = $null
    do {
        try {
            $children = @(Get-CimInstance -ClassName Win32_Process `
                -Filter "ParentProcessId = $launcherId" -ErrorAction Stop)
            $lastAuditError = $null
        }
        catch {
            $lastAuditError = $_.Exception.Message
            $quietSinceUtc = $null
            Start-Sleep -Milliseconds 100
            continue
        }

        $incompleteChildren = @($children | Where-Object {
            -not $_.ExecutablePath -or -not $_.CreationDate
        })
        $runtimeChildren = [System.Collections.Generic.List[object]]::new()
        foreach ($child in $children) {
            if (-not $child.ExecutablePath -or -not $child.CreationDate -or
                    -not (Test-OpenNovaPathEqual `
                        -Left ([string] $child.ExecutablePath) `
                        -Right $ExpectedRuntime)) {
                continue
            }
            $createdUtc = ConvertTo-OpenNovaUtc -Value ([DateTime] $child.CreationDate)
            if ($createdUtc -ge $LauncherStartUtc.AddSeconds(-1)) {
                $runtimeChildren.Add($child)
            }
        }

        foreach ($child in $runtimeChildren) {
            $orphan = Get-Process -Id ([int] $child.ProcessId) -ErrorAction SilentlyContinue
            if (-not $orphan) { continue }
            $orphan.Refresh()
            if (-not $orphan.HasExited) {
                $orphan.Kill()
                if (-not $orphan.WaitForExit(2000)) {
                    throw "Failed Godot runtime PID $($child.ProcessId) remained alive after exact cleanup."
                }
            }
        }

        if ($incompleteChildren.Count -eq 0 -and $runtimeChildren.Count -eq 0) {
            if (-not $quietSinceUtc) { $quietSinceUtc = [DateTime]::UtcNow }
            if ([DateTime]::UtcNow -ge $quietSinceUtc.AddSeconds(1)) { return }
        }
        else {
            $quietSinceUtc = $null
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $cleanupDeadline)

    if ($lastAuditError) {
        throw "Could not complete failed-launch child audit: $lastAuditError"
    }
    throw "Failed-launch child audit did not reach a stable orphan-free state."
}

function Test-OpenNovaConsoleHostRecord {
    param(
        [Parameter(Mandatory = $true)] $Record,
        [Parameter(Mandatory = $true)] [DateTime] $LauncherStartUtc
    )

    if (-not $Record.ExecutablePath -or
            -not [string]::Equals(
                [string] $Record.Name,
                'conhost.exe',
                [StringComparison]::OrdinalIgnoreCase)) {
        return $false
    }
    $expected = Join-Path $env:SystemRoot 'System32\conhost.exe'
    if (-not (Test-OpenNovaPathEqual `
            -Left ([string] $Record.ExecutablePath) -Right $expected)) {
        return $false
    }
    $createdUtc = ConvertTo-OpenNovaUtc -Value ([DateTime] $Record.CreationDate)
    return $createdUtc -ge $LauncherStartUtc.AddSeconds(-1)
}

function Wait-OpenNovaRuntimeChild {
    param(
        [Parameter(Mandatory = $true)] [System.Diagnostics.Process] $LauncherProcess,
        [Parameter(Mandatory = $true)] [string] $ExpectedRuntime,
        [Parameter(Mandatory = $true)] [string[]] $ExpectedArguments,
        [Parameter(Mandatory = $true)] [DateTime] $LauncherStartUtc,
        [Parameter(Mandatory = $true)] [DateTime] $DeadlineUtc
    )

    do {
        $LauncherProcess.Refresh()
        if ($LauncherProcess.HasExited) {
            throw "Godot console launcher exited before its runtime child could be proven."
        }
        $children = @(Get-CimInstance -ClassName Win32_Process `
            -Filter "ParentProcessId = $($LauncherProcess.Id)" -ErrorAction Stop)
        $incompleteChildren = @($children | Where-Object {
            -not $_.ExecutablePath -or -not $_.CreationDate
        })
        if ($incompleteChildren.Count -gt 0) {
            Start-Sleep -Milliseconds 50
            continue
        }
        $runtimeChildren = @($children | Where-Object {
            $_.ExecutablePath -and (Test-OpenNovaPathEqual `
                -Left ([string] $_.ExecutablePath) -Right $ExpectedRuntime)
        })
        $runtimeChildPids = @($runtimeChildren | ForEach-Object {
            [int] $_.ProcessId
        })
        $auxiliaryChildren = @($children | Where-Object {
            -not ($runtimeChildPids -contains [int] $_.ProcessId)
        })
        $unexpectedChildren = @($auxiliaryChildren | Where-Object {
            -not (Test-OpenNovaConsoleHostRecord `
                -Record $_ -LauncherStartUtc $LauncherStartUtc)
        })
        if ($unexpectedChildren.Count -gt 0) {
            $unexpected = @($unexpectedChildren | ForEach-Object {
                "$($_.Name):$($_.ProcessId)"
            }) -join ','
            throw "Godot console launcher created unexpected direct child process(es): $unexpected"
        }
        if ($runtimeChildren.Count -gt 1) {
            throw "Godot console launcher created more than one sibling runtime child."
        }
        if ($runtimeChildren.Count -eq 1) {
            $child = $runtimeChildren[0]
            if ($child.ExecutablePath -and $child.CommandLine) {
                if (-not (Test-OpenNovaPathEqual `
                        -Left ([string] $child.ExecutablePath) `
                        -Right $ExpectedRuntime)) {
                    throw "Godot console launcher child executable is not its selected sibling runtime."
                }
                if ([int] $child.ParentProcessId -ne $LauncherProcess.Id) {
                    throw "Godot runtime parent PID changed during discovery."
                }
                $createdUtc = ConvertTo-OpenNovaUtc -Value ([DateTime] $child.CreationDate)
                if ($createdUtc -lt $LauncherStartUtc) {
                    throw "Godot runtime child predates its console launcher."
                }
                Assert-OpenNovaProcessCommandLine `
                    -Label 'Godot runtime' `
                    -CommandLine ([string] $child.CommandLine) `
                    -ExpectedExecutable $ExpectedRuntime `
                    -ExpectedArguments $ExpectedArguments
                $runtime = Get-Process -Id ([int] $child.ProcessId) -ErrorAction Stop
                $runtimeStartUtc = $runtime.StartTime.ToUniversalTime()
                if ($runtimeStartUtc -lt $LauncherStartUtc) {
                    throw "Godot runtime Process start time predates its console launcher."
                }
                if (-not (Test-OpenNovaPathEqual `
                        -Left $runtime.MainModule.FileName `
                        -Right $ExpectedRuntime)) {
                    throw "Godot runtime Process handle resolves to a different executable."
                }
                if (Test-OpenNovaProcessExecutionStarted -Process $runtime) {
                    return [pscustomobject]@{
                        Process = $runtime
                        Record = $child
                        CreatedUtc = $createdUtc
                        StartUtc = $runtimeStartUtc
                        ConsoleHostPids = @($auxiliaryChildren | ForEach-Object {
                            [int] $_.ProcessId
                        })
                    }
                }
            }
        }
        Start-Sleep -Milliseconds 50
    } while ([DateTime]::UtcNow -lt $DeadlineUtc)

    throw "Godot runtime child was not uniquely proven active before the launch deadline."
}

# Native stateless 0x41/0x81 LAN responder probe. An explicit override is
# useful for non-default build directories; otherwise prefer the Release tool
# produced by the ordinary CMake build, then Debug.
function Find-NwLanProbe {
    $root = Get-RepoRoot
    foreach ($candidate in @(
        (Join-Path $root "build\apps\nw_lan_probe\Release\nw-lan-probe.exe"),
        (Join-Path $root "build\apps\nw_lan_probe\Debug\nw-lan-probe.exe"),
        (Join-Path $root "build\apps\nw_lan_probe\nw-lan-probe")
    )) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    return $null
}

# Launch the OpenNova game project. Every role setting is a launch flag
# (--lan-host/--lan-join/--callsign/..., docs/dev-env-vars.md) passed after
# `--`; nothing rides on the environment. The official console wrapper
# remains the lifecycle owner; the returned Process is its proven runtime
# child and owns the window, the LAN socket and, with -McpPort, the
# opennova-game endpoint (`--mcp-port`, awaited before returning).
# The window starts behind every other window and never takes the foreground
# ([OpenNova.Scripts.BehindLaunch], scripts/mcp/game_mcp.ps1; the wrapper's
# console gets no window); -Front is an ordinary start (Start-Process).
function Start-OpenNovaProcess {
    param(
        [string[]] $GodotArguments = @(),
        [string[]] $GameArguments = @(),
        [ValidateRange(0, 65535)] [int] $McpPort = 0,
        [ValidateRange(1, 600)] [int] $McpReadyTimeoutSeconds = 240,
        [switch] $Front
    )

    $godotCandidate = Find-GodotBinary
    if (-not $godotCandidate) {
        throw "Godot 4.6.1 was not found. Set GODOT_BIN or populate a .godot-bin directory above the repo."
    }
    $godotRuntime = Resolve-GodotRuntimeBinary -Path $godotCandidate
    $wrapperUsed = -not (Test-OpenNovaPathEqual `
        -Left $godotCandidate -Right $godotRuntime)

    $projectDir = Join-Path (Get-RepoRoot) "godot"
    if (-not (Test-Path (Join-Path $projectDir "project.godot"))) {
        throw "OpenNova Godot project not found at: $projectDir"
    }
    foreach ($token in @($GameArguments)) {
        if ([string] $token -eq '--mcp-port') {
            throw "Pass the endpoint port through -McpPort, not --mcp-port."
        }
    }
    if ($McpPort -gt 0) {
        if (Test-GameMcpPortOpen -Port $McpPort) {
            throw "MCP port $McpPort is already answering; another game owns it."
        }
        $GameArguments = @($GameArguments) + @('--mcp-port', [string] $McpPort)
    }

    $behind = [OpenNova.Scripts.BehindLaunch]::new(-not $Front)
    try {
        # No --headless/hidden flags: each child is an ordinary, visible game instance.
        # The command line is one string (Start-Process flattens ArgumentList on
        # Windows), so quote every token that contains whitespace. Reject embedded
        # quotes instead of producing an ambiguous command line.
        $tokens = @('--path', $projectDir) + @($GodotArguments)
        if ($GameArguments.Count -gt 0) {
            $tokens += '--'
            $tokens += @($GameArguments)
        }
        $arguments = foreach ($token in $tokens) {
            $value = [string] $token
            if ($value.Contains('"')) {
                throw "OpenNova launch argument contains an unsupported quote: $value"
            }
            if ($value.Length -eq 0 -or $value -match '\s') {
                # Windows treats backslashes before a closing quote specially.
                # Embedded quotes are rejected above, so doubling only the
                # trailing run gives CommandLineToArgvW the exact token back.
                $trailingBackslashes = [regex]::Match($value, '\\+$').Value
                if ($trailingBackslashes.Length -gt 0) {
                    $value += $trailingBackslashes
                }
                '"{0}"' -f $value
            } else {
                $value
            }
        }
        $launchNotBeforeUtc = [DateTime]::UtcNow
        $launcher = $null
        try {
            if ($behind.Active) {
                $behind.Prepare()
                $launcher = $behind.Start($godotCandidate, ($arguments -join ' '), $projectDir, $wrapperUsed)
                $behind.Watch($launcher)
            }
            else {
                $launcher = Start-Process -FilePath $godotCandidate `
                    -WorkingDirectory $projectDir `
                    -ArgumentList ($arguments -join ' ') `
                    -PassThru
            }
            $launcherStartUtc = $launcher.StartTime.ToUniversalTime()
            if ($launcherStartUtc -lt $launchNotBeforeUtc.AddSeconds(-1)) {
                throw "Godot launcher Process predates the current launch."
            }
            $deadlineUtc = [DateTime]::UtcNow.AddSeconds(15)
            $launcherIdentity = Wait-OpenNovaProcessRecord `
                -Process $launcher `
                -ExpectedExecutable $godotCandidate `
                -ExpectedArguments @($tokens) `
                -NotBeforeUtc $launchNotBeforeUtc `
                -DeadlineUtc $deadlineUtc `
                -Label 'Godot launcher'

            if ($wrapperUsed) {
                $runtimeIdentity = Wait-OpenNovaRuntimeChild `
                    -LauncherProcess $launcher `
                    -ExpectedRuntime $godotRuntime `
                    -ExpectedArguments @($tokens) `
                    -LauncherStartUtc $launcherStartUtc `
                    -DeadlineUtc $deadlineUtc
                $runtime = $runtimeIdentity.Process
                $runtimeRecord = $runtimeIdentity.Record
                $runtimeCreatedUtc = $runtimeIdentity.CreatedUtc
                $runtimeStartUtc = $runtimeIdentity.StartUtc
                $consoleHostPids = @($runtimeIdentity.ConsoleHostPids)
            }
            else {
                $runtime = $launcher
                $runtimeRecord = $launcherIdentity.Record
                $runtimeCreatedUtc = $launcherIdentity.CreatedUtc
                $runtimeStartUtc = $launcherStartUtc
                $consoleHostPids = @()
                do {
                    if (Test-OpenNovaProcessExecutionStarted -Process $runtime) { break }
                    $runtime.Refresh()
                    if ($runtime.HasExited) {
                        throw "Direct Godot runtime exited before active execution could be proven."
                    }
                    Start-Sleep -Milliseconds 50
                } while ([DateTime]::UtcNow -lt $deadlineUtc)
                if (-not (Test-OpenNovaProcessExecutionStarted -Process $runtime)) {
                    throw "Direct Godot runtime was not proven active before the launch deadline."
                }
            }

            # Get-Process does not retain a queryable exit code unless its
            # native handle is materialized while the process is alive. Keep
            # that handle so cooperative parity shutdown can distinguish a
            # clean exit from an after-ACK teardown crash.
            $runtime.EnableRaisingEvents = $true
            $null = $runtime.Handle

            $proof = [pscustomobject]@{
                schema = 'opennova.godot-process-ownership.v1'
                wrapper_used = [bool] $wrapperUsed
                launcher_pid = [int] $launcher.Id
                launcher_path = [string] $launcherIdentity.Record.ExecutablePath
                launcher_start_utc = $launcherStartUtc.ToString('o')
                launcher_created_utc = $launcherIdentity.CreatedUtc.ToString('o')
                launcher_command_line = [string] $launcherIdentity.Record.CommandLine
                runtime_pid = [int] $runtime.Id
                runtime_parent_pid = [int] $runtimeRecord.ParentProcessId
                runtime_path = [string] $runtimeRecord.ExecutablePath
                runtime_start_utc = $runtimeStartUtc.ToString('o')
                runtime_created_utc = $runtimeCreatedUtc.ToString('o')
                runtime_command_line = [string] $runtimeRecord.CommandLine
                launcher_console_host_pids = @($consoleHostPids)
                arguments = @($tokens | ForEach-Object { [string] $_ })
                parent_verified = [bool] $(
                    -not $wrapperUsed -or
                    [int] $runtimeRecord.ParentProcessId -eq [int] $launcher.Id)
                argv_verified = $true
                active_execution_verified = $true
                proven_utc = [DateTime]::UtcNow.ToString('o')
            }
            $runtime | Add-Member -NotePropertyName OpenNovaLauncherProcess `
                -NotePropertyValue $launcher -Force
            $runtime | Add-Member -NotePropertyName OpenNovaLaunchProof `
                -NotePropertyValue $proof -Force
            if ($McpPort -gt 0) {
                $null = Wait-GameMcpReady -Port $McpPort `
                    -TimeoutSeconds $McpReadyTimeoutSeconds -Process $runtime
                $runtime | Add-Member -NotePropertyName OpenNovaMcpPort `
                    -NotePropertyValue $McpPort -Force
            }
            $behind.Settle()
            return $runtime
        }
        catch {
            $launchFailure = $_
            $launcherStartForCleanup = if ($launcher) {
                try { $launcher.StartTime.ToUniversalTime() }
                catch { $launchNotBeforeUtc }
            } else {
                $launchNotBeforeUtc
            }
            try {
                Stop-OpenNovaFailedLaunch `
                    -LauncherProcess $launcher `
                    -ExpectedRuntime $(if ($wrapperUsed) { $godotRuntime } else { '' }) `
                    -LauncherStartUtc $launcherStartForCleanup
            }
            catch {
                throw "Godot launch failed: $($launchFailure.Exception.Message) Cleanup also failed: $($_.Exception.Message)"
            }
            throw $launchFailure
        }
    }
    finally {
        # The foreground lock is never held past the start, however it ends.
        $behind.Dispose()
    }
}

# Close one exact OpenNova launch. Graceful close targets the runtime window;
# forced cleanup targets the official console wrapper so its KILL_ON_JOB_CLOSE
# job tears down the complete engine tree. Direct non-wrapper launches retain
# the same exact-Process fallback.
function Stop-OpenNovaProcess {
    param(
        [Parameter(Mandatory = $true)]
        [System.Diagnostics.Process] $Process,
        [ValidateRange(0, 60000)] [int] $GracefulTimeoutMs = 8000,
        [ValidateRange(100, 30000)] [int] $ForceTimeoutMs = 5000,
        [switch] $PassThruShutdownEvidence,
        [switch] $RequireCleanExit,
        # The game was already asked to quit through its MCP endpoint
        # (Stop-GameViaMcp acknowledged): that request is the graceful close.
        [switch] $QuitRequested
    )

    $requestedUtc = [DateTime]::UtcNow.ToString('o')
    $closeRequested = [bool] $QuitRequested
    $exactFallbackRequired = $false
    $launcherKilled = $false
    $runtimeKilled = $false
    $proofProperty = $Process.PSObject.Properties['OpenNovaLaunchProof']
    $launcherProperty = $Process.PSObject.Properties['OpenNovaLauncherProcess']
    $wrapperUsed = $proofProperty -and [bool] $proofProperty.Value.wrapper_used
    $launcher = if ($launcherProperty) { $launcherProperty.Value } else { $Process }

    $Process.Refresh()
    if (-not $Process.HasExited) {
        $closeRequested = [bool] $Process.CloseMainWindow()
        if ($GracefulTimeoutMs -gt 0) {
            $null = $Process.WaitForExit($GracefulTimeoutMs)
        }
    }

    $Process.Refresh()
    if ($wrapperUsed -and $launcher) {
        $launcher.Refresh()
        if ($Process.HasExited -and -not $launcher.HasExited) {
            $null = $launcher.WaitForExit($ForceTimeoutMs)
            $launcher.Refresh()
        }
        if (-not $Process.HasExited -or -not $launcher.HasExited) {
            if (-not $launcher.HasExited) {
                $exactFallbackRequired = $true
                $launcherKilled = $true
                try { $launcher.Kill() }
                catch {
                    $launcher.Refresh()
                    if (-not $launcher.HasExited) { throw }
                }
                $null = $launcher.WaitForExit($ForceTimeoutMs)
            }
        }
    }
    # The wrapper job should remove the runtime when the wrapper closes. Keep an
    # exact-process fallback for a wrapper that exited before assigning the job,
    # or for a failed job teardown, so cleanup never reports failure while
    # knowingly leaving the already-proven runtime alive.
    $Process.Refresh()
    if (-not $Process.HasExited) {
        $exactFallbackRequired = $true
        $runtimeKilled = $true
        try { $Process.Kill() }
        catch {
            $Process.Refresh()
            if (-not $Process.HasExited) { throw }
        }
    }

    $Process.Refresh()
    if (-not $Process.HasExited) {
        $null = $Process.WaitForExit($ForceTimeoutMs)
        $Process.Refresh()
    }
    if (-not $Process.HasExited) {
        throw "OpenNova runtime PID $($Process.Id) remained alive after exact cleanup."
    }
    if ($wrapperUsed -and $launcher) {
        $launcher.Refresh()
        if (-not $launcher.HasExited) {
            $null = $launcher.WaitForExit($ForceTimeoutMs)
            $launcher.Refresh()
        }
        if (-not $launcher.HasExited) {
            throw "OpenNova launcher PID $($launcher.Id) remained alive after exact cleanup."
        }
    }

    # Start-OpenNovaProcess materializes the runtime handle while it is alive,
    # but keep the evidence nullable and fail closed if a non-centralized caller
    # supplies a Process whose exit code can no longer be queried.
    $exitCode = $null
    try { $exitCode = [int] $Process.ExitCode }
    catch { $exitCode = $null }
    $clean = $closeRequested -and -not $exactFallbackRequired -and
        $null -ne $exitCode -and [int] $exitCode -eq 0
    $evidence = [pscustomobject][ordered]@{
        schema = 'opennova.process-shutdown.v1'
        process_id = [int] $Process.Id
        requested_utc = $requestedUtc
        completed_utc = [DateTime]::UtcNow.ToString('o')
        close_requested = [bool] $closeRequested
        exact_fallback_required = [bool] $exactFallbackRequired
        launcher_killed = [bool] $launcherKilled
        runtime_killed = [bool] $runtimeKilled
        exit_code = $(if ($null -eq $exitCode) { $null } else { [int] $exitCode })
        clean = [bool] $clean
    }
    if ($RequireCleanExit -and -not $clean) {
        $exitCodeText = if ($null -eq $exitCode) { '<unavailable>' } else { [string] $exitCode }
        throw "OpenNova runtime PID $($Process.Id) did not prove a clean exit: close_requested=$closeRequested exact_fallback_required=$exactFallbackRequired exit_code=$exitCodeText"
    }
    if ($PassThruShutdownEvidence) {
        return $evidence
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
