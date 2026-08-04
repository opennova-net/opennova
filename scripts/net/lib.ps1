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

function Get-NetHarnessStringSha256 {
    param([Parameter(Mandatory = $true)] [string] $Value)

    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($Value)
        return ([BitConverter]::ToString($sha.ComputeHash($bytes)) -replace '-', '').ToLowerInvariant()
    }
    finally {
        $sha.Dispose()
    }
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

function Get-ParityMissionArtifactRole {
    param([Parameter(Mandatory = $true)] [string] $Mission)

    $leaf = [IO.Path]::GetFileName($Mission)
    if ([string]::IsNullOrWhiteSpace($leaf) -or $leaf -cne $Mission -or
            [IO.Path]::GetExtension($leaf) -cne '.bms') {
        throw "Parity mission must be one exact lowercase-extension BMS leaf: $Mission"
    }
    $stem = [IO.Path]::GetFileNameWithoutExtension($leaf).ToLowerInvariant()
    $token = ([regex]::Replace($stem, '[^a-z0-9]+', '_')).Trim('_')
    if ([string]::IsNullOrWhiteSpace($token)) {
        throw "Parity mission cannot derive a canonical artifact role: $Mission"
    }
    return "mission_$token"
}

function Get-ParityCaseRunSlug {
    param([Parameter(Mandatory = $true)] [string] $CaseId)

    if ($CaseId -notmatch '^[a-z0-9][a-z0-9-]*$') {
        throw "Parity case id is not canonical: $CaseId"
    }
    $slug = $CaseId.Split('-')[0]
    if ([string]::IsNullOrWhiteSpace($slug)) {
        throw "Parity case id cannot derive a run slug: $CaseId"
    }
    return $slug
}

function Get-ParityMatrixBindings {
    param([Parameter(Mandatory = $true)] [object[]] $Cases)

    $caseBindings = @{}
    $missionBindings = [System.Collections.Generic.List[object]]::new()
    $seenMissionRoles = @{}
    $seenCaseSlugs = @{}
    foreach ($case in $Cases) {
        $caseId = [string]$case.id
        $mission = [string]$case.mission
        $slug = Get-ParityCaseRunSlug -CaseId $caseId
        # Preserve the historical short slugs for the canonical four. A
        # crossover/holdout whose first token is already occupied falls back
        # to its full canonical case id, keeping every run id unambiguous.
        if ($seenCaseSlugs.ContainsKey($slug)) { $slug = $caseId }
        $missionRole = Get-ParityMissionArtifactRole -Mission $mission
        if ($caseBindings.ContainsKey($caseId)) {
            throw "Manifest template repeats parity case id: $caseId"
        }
        if ($seenMissionRoles.ContainsKey($missionRole) -and
                -not [string]::Equals(
                    [string]$seenMissionRoles[$missionRole], $mission,
                    [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "Manifest template mission artifact role collision: $missionRole"
        }
        if ($seenCaseSlugs.ContainsKey($slug)) {
            throw "Manifest template run slug collision: $slug"
        }
        $newMissionBinding = -not $seenMissionRoles.ContainsKey($missionRole)
        $seenMissionRoles[$missionRole] = $mission
        $seenCaseSlugs[$slug] = $caseId
        $binding = [pscustomobject]@{
            Id = $caseId
            Slug = $slug
            Mission = $mission
            MissionRole = $missionRole
        }
        $caseBindings[$caseId] = $binding
        if ($newMissionBinding) { $missionBindings.Add($binding) }
    }
    return [pscustomobject]@{
        CaseBindings = $caseBindings
        MissionBindings = @($missionBindings.ToArray())
    }
}

function Test-ParityMatrixAutomationPorts {
    param(
        [Parameter(Mandatory = $true)] [object[]] $Cases,
        [int] $Minimum = 32786,
        [int] $Maximum = 32789
    )
    $ports = @($Cases | ForEach-Object { [int]$_.Port })
    $requiredDistinct = $Maximum - $Minimum + 1
    return @($ports | Where-Object {
        $_ -lt $Minimum -or $_ -gt $Maximum
    }).Count -eq 0 -and
        @($ports | Sort-Object -Unique).Count -ge $requiredDistinct
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
        [switch] $AllowPostDeathRedeploy
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
        if ([bool] $State.in_match -and
                [bool] $State.local_player -and
                [bool] $State.deploy_hold_ready -and
                [bool] $State.deployment_pick_pending -and
                -not [bool] $State.deployment_pick_sent -and
                -not [bool] $State.auto_deploy -and
                [bool] $State.deploy_presented -and
                [int] $State.joiner_phase -eq 4 -and
                [string] $State.join_admission_stage -ceq
                    "awaiting the player's deployment pick") {
            return 'deploy_hold'
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
        -AllowPostDeathRedeploy:$AllowPostDeathRedeploy
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

function New-OpenNovaJoinerReadinessWitness {
    param(
        [Parameter(Mandatory = $true)] $Initial,
        [Parameter(Mandatory = $true)] $Final,
        [Parameter(Mandatory = $true)]
        [ValidateSet('in_match', 'deploy_hold')] [string] $ReadinessMode,
        [switch] $AllowPostDeathRedeploy,
        [switch] $RequireMotionComplete
    )

    if (-not (Test-OpenNovaJoinerReadinessTransition -Initial $Initial `
            -Final $Final -ReadinessMode $ReadinessMode `
            -AllowPostDeathRedeploy:$AllowPostDeathRedeploy `
            -RequireMotionComplete:$RequireMotionComplete)) {
        return $null
    }
    $finalClass = Get-OpenNovaJoinerReadinessClass -State $Final `
        -ReadinessMode $ReadinessMode `
        -AllowPostDeathRedeploy:$AllowPostDeathRedeploy
    return [pscustomobject][ordered]@{
        schema = 'opennova.parity-joiner-readiness.v2'
        readiness_mode = $ReadinessMode
        initial = ConvertTo-OpenNovaJoinerReadinessSnapshot $Initial
        final = ConvertTo-OpenNovaJoinerReadinessSnapshot $Final
        final_class = $finalClass
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

function Get-BmsMissionDisplayName {
    param([Parameter(Mandatory = $true)] [string] $Path)

    return [string] (Get-BmsHeaderIdentity -Path $Path).MissionDisplayName
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

# Hash the complete nonignored repository worktree as content, independent of
# whether an unchanged byte image is staged.  This closes the provenance gap
# between the separately bound native DLL/tool binaries and the source that a
# later commit/rebuild will use.  Gitignored captures and build products never
# enter the inventory.  Clean gitlinks are represented by their exact commit;
# a dirty or mismatched submodule fails closed instead of hashing an ambiguous
# mixture of parent and nested worktrees.
function Get-NetHarnessRepositorySourceState {
    param([string] $RepoRoot = (Get-RepoRoot))

    $repo = (Resolve-Path -LiteralPath $RepoRoot -ErrorAction Stop).Path
    $gitRoot = @(& git -C $repo rev-parse --show-toplevel)
    if ($LASTEXITCODE -ne 0 -or $gitRoot.Count -ne 1 -or
            -not ([IO.Path]::GetFullPath([string]$gitRoot[0])).Equals(
                $repo, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Repository source-state root is not one exact Git worktree: $repo"
    }

    $listed = @(& git -C $repo -c core.quotepath=false `
        ls-files --cached --others --exclude-standard)
    if ($LASTEXITCODE -ne 0) {
        throw "Could not enumerate nonignored repository source state: $repo"
    }
    $relativePaths = @($listed | ForEach-Object {
        ([string]$_).Replace('\', '/')
    } | Where-Object {
        $_ -and $_ -notmatch '^(?:\.scratch|\.godot-bin|build)(?:/|$)'
    } | Sort-Object -Unique)

    $prefix = $repo.TrimEnd('\') + '\'
    $entries = [System.Collections.Generic.List[string]]::new()
    $submoduleCount = 0
    foreach ($relative in $relativePaths) {
        if ($relative.IndexOfAny([char[]]@("`r", "`n", [char]0)) -ge 0 -or
                [IO.Path]::IsPathRooted($relative)) {
            throw "Repository source-state path is not canonical: $relative"
        }
        $absolute = [IO.Path]::GetFullPath((Join-Path $repo $relative))
        if (-not $absolute.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Repository source-state path escaped the worktree: $relative"
        }
        if (Test-Path -LiteralPath $absolute -PathType Leaf) {
            $hash = (Get-FileHash -LiteralPath $absolute -Algorithm SHA256).Hash.ToLowerInvariant()
            $entries.Add("file:$relative=$hash")
            continue
        }
        if (-not (Test-Path -LiteralPath $absolute -PathType Container)) {
            # An unstaged deletion and the same deletion after staging describe
            # one identical worktree byte image: the path is absent in both.
            continue
        }

        $stage = @(& git -C $repo ls-files --stage -- $relative)
        if ($LASTEXITCODE -ne 0 -or $stage.Count -ne 1 -or
                [string]$stage[0] -notmatch '^160000 ([0-9a-fA-F]{40,64}) 0\s') {
            throw "Repository source-state directory is not one Git submodule: $relative"
        }
        $expectedCommit = $Matches[1].ToLowerInvariant()
        $actualCommit = @(& git -C $absolute rev-parse HEAD)
        if ($LASTEXITCODE -ne 0 -or $actualCommit.Count -ne 1 -or
                [string]$actualCommit[0] -cne $expectedCommit) {
            throw "Repository source-state submodule commit drift: $relative"
        }
        $dirty = @(& git -C $absolute status --porcelain=v1 --untracked-files=all)
        if ($LASTEXITCODE -ne 0 -or $dirty.Count -ne 0) {
            throw "Repository source-state submodule is dirty: $relative"
        }
        $entries.Add("submodule:$relative=$expectedCommit")
        $submoduleCount++
    }
    if ($entries.Count -eq 0) {
        throw "Repository source-state inventory is empty: $repo"
    }
    $canonical = ((@($entries.ToArray()) | Sort-Object) -join "`n") + "`n"
    return [pscustomobject][ordered]@{
        schema = 'opennova.repository-source-state.v1'
        mode = 'git-nonignored-worktree-content'
        sha256 = Get-NetHarnessStringSha256 $canonical
        entry_count = [int]$entries.Count
        submodule_count = [int]$submoduleCount
    }
}

# Return every repo-native input that the ordinary LAN launch can load through
# res://, in addition to the separately hash-bound native DLL. Roles derive
# from the repo-relative path, so adding, removing, or renaming a resource also
# changes the suite fingerprint rather than merely shifting an ordinal.
function Get-GodotRuntimeArtifacts {
    param([string] $RepoRoot = (Get-RepoRoot))

    $repo = (Resolve-Path -LiteralPath $RepoRoot -ErrorAction Stop).Path
    $project = Join-Path $repo 'godot'
    $fixed = @(
        Join-Path $project 'project.godot'
        Join-Path $project 'bin\opennova.gdextension'
    )
    $roots = @('addons', 'engine', 'game', 'server', 'shaders') |
        ForEach-Object { Join-Path $project $_ }
    $runtimeExtensions = @(
        '.cfg', '.font', '.gd', '.gdshader', '.godot', '.import', '.jpeg',
        '.jpg', '.json', '.material', '.mp3', '.ogg', '.otf', '.png',
        '.res', '.svg', '.theme', '.tres', '.tscn', '.ttf', '.uid',
        '.wav', '.webp', '.xml'
    )

    $candidates = [System.Collections.Generic.List[System.IO.FileInfo]]::new()
    foreach ($path in $fixed) {
        $candidates.Add((Get-Item -LiteralPath $path -ErrorAction Stop))
    }
    foreach ($root in $roots) {
        if (-not (Test-Path -LiteralPath $root -PathType Container)) { continue }
        foreach ($file in Get-ChildItem -LiteralPath $root -Recurse -File) {
            if ($file.FullName -match '[\\/]build([\\/]|$)') { continue }
            if ($runtimeExtensions -contains $file.Extension.ToLowerInvariant()) {
                $candidates.Add($file)
            }
        }
    }

    $prefix = $repo.TrimEnd('\') + '\'
    $byRelative = @{}
    foreach ($file in $candidates) {
        $absolute = $file.FullName
        if (-not $absolute.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Godot runtime artifact escaped the repository: $absolute"
        }
        $relative = $absolute.Substring($prefix.Length).Replace('\', '/')
        $byRelative[$relative.ToLowerInvariant()] = [pscustomobject]@{
            Relative = $relative
            Path = $absolute
        }
    }

    $artifacts = @{}
    foreach ($key in @($byRelative.Keys | Sort-Object)) {
        $entry = $byRelative[$key]
        $role = 'godot_resource_' + (Get-NetHarnessStringSha256 $entry.Relative)
        if ($artifacts.ContainsKey($role)) {
            throw "Godot runtime artifact role collision: $($entry.Relative)"
        }
        $artifacts[$role] = $entry.Path
    }
    return $artifacts
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

# Read the hook's simple `Key Value` / `Key=Value` configuration format. Unknown
# keys are intentionally retained: the retail DLL ignores them, which lets one
# per-role onhook.cfg also carry OpenNova comparison-launch settings.
function Read-OnHookConfig {
    param([Parameter(Mandatory = $true)] [string] $Path)

    $resolved = Resolve-Path -LiteralPath $Path -ErrorAction Stop
    $values = @{}
    foreach ($line in Get-Content -LiteralPath $resolved.Path) {
        $trimmed = $line.Trim()
        if (-not $trimmed -or $trimmed.StartsWith('#') -or $trimmed.StartsWith(';')) {
            continue
        }
        if ($line -match '^\s*([A-Za-z][A-Za-z0-9_]*)(?:\s*=\s*|\s+)(.*?)\s*$') {
            $values[$Matches[1]] = $Matches[2]
        }
    }
    return [pscustomobject]@{
        Path = $resolved.Path
        Directory = Split-Path -Parent $resolved.Path
        Values = $values
    }
}

# onhook-mcp rewrites only these owned role keys and its marker comment. Remove
# that dynamic surface and hash the remaining bytes with normalized newlines;
# the result proves every fixed launch/hook setting came from the tracked role
# template while still allowing per-case mission/port/name/game-type values.
function Get-OnHookConfigBase {
    param([Parameter(Mandatory = $true)] [string] $Path)

    $resolved = (Resolve-Path -LiteralPath $Path -ErrorAction Stop).Path
    $dynamicKeys = @{
        LanHostMission = $true
        LanHostPort = $true
        LanHostCallsign = $true
        LanHostGameType = $true
        LanHostMaxPlayers = $true
        LanJoinAddress = $true
        LanJoinPort = $true
        LanJoinCallsign = $true
        LanJoinDiscoveryTimeoutMs = $true
    }
    $seenKeys = @{}
    $settings = @{}
    $kept = [System.Collections.Generic.List[string]]::new()
    $raw = [IO.File]::ReadAllText($resolved).Replace("`r`n", "`n").Replace("`r", "`n")
    foreach ($line in $raw.Split("`n")) {
        $trimmed = $line.Trim()
        if ($trimmed -eq '// Managed by onhook-mcp for an owned LAN parity run.') {
            continue
        }
        if ($line -match '^\s*([A-Za-z][A-Za-z0-9_]*)(?:\s*=\s*|\s+)(.*?)\s*$') {
            $key = $Matches[1]
            if ($seenKeys.ContainsKey($key)) {
                throw "Duplicate onHook config key '$key' in $resolved"
            }
            $seenKeys[$key] = $true
            $settings[$key] = $Matches[2]
            if ($dynamicKeys.ContainsKey($key)) { continue }
        }
        $kept.Add($line)
    }
    while ($kept.Count -gt 0 -and $kept[$kept.Count - 1] -eq '') {
        $kept.RemoveAt($kept.Count - 1)
    }
    $canonical = (@($kept.ToArray()) -join "`n") + "`n"
    return [pscustomobject]@{
        Path = $resolved
        Canonical = $canonical
        Sha256 = Get-NetHarnessStringSha256 $canonical
        Settings = $settings
    }
}

# Rewrite only the per-run LAN keys that onhook-mcp owns. The replacement is
# create-new + atomic so a failed matrix setup cannot leave a partially written
# retail cfg, and every fixed byte remains available to Get-OnHookConfigBase.
function Set-OnHookOwnedConfigValues {
    param(
        [Parameter(Mandatory = $true)] [string] $Path,
        [Parameter(Mandatory = $true)] [hashtable] $Values
    )

    $resolved = (Resolve-Path -LiteralPath $Path -ErrorAction Stop).Path
    $ownedKeys = @{
        LanHostMission = $true
        LanHostPort = $true
        LanHostCallsign = $true
        LanHostGameType = $true
        LanHostMaxPlayers = $true
        LanJoinAddress = $true
        LanJoinPort = $true
        LanJoinCallsign = $true
        LanJoinDiscoveryTimeoutMs = $true
    }
    $raw = [IO.File]::ReadAllText($resolved)
    foreach ($key in @($Values.Keys | Sort-Object)) {
        $value = [string] $Values[$key]
        if (-not $ownedKeys.ContainsKey($key)) {
            throw "Cannot rewrite non-owned onHook key '$key'."
        }
        if ($value -notmatch '^[A-Za-z0-9._:-]+$') {
            throw "Unsafe onHook value for ${key}: '$value'"
        }
        $pattern = [regex]::new(
            "(?m)^[ \t]*$([regex]::Escape($key))(?:[ \t]*=[ \t]*|[ \t]+)" +
            "[^\r\n]*(?=\r?$)"
        )
        if ($pattern.Matches($raw).Count -ne 1) {
            throw "Expected one owned onHook key '$key' in $resolved"
        }
        $raw = $pattern.Replace($raw, "$key $value", 1)
    }

    $temp = "$resolved.parity-$PID.tmp"
    $backup = "$resolved.parity-$PID.bak"
    if ((Test-Path -LiteralPath $temp) -or (Test-Path -LiteralPath $backup)) {
        throw "Create-new onHook config temp/backup already exists for $resolved"
    }
    try {
        [IO.File]::WriteAllText($temp, $raw, [Text.UTF8Encoding]::new($false))
        [IO.File]::Replace($temp, $resolved, $backup)
        Remove-Item -LiteralPath $backup -Force
    }
    finally {
        if (Test-Path -LiteralPath $temp) {
            Remove-Item -LiteralPath $temp -Force
        }
        if (Test-Path -LiteralPath $backup) {
            Remove-Item -LiteralPath $backup -Force
        }
    }
}

function Resolve-OnHookConfigPath {
    param(
        [Parameter(Mandatory = $true)] [string] $ConfigDirectory,
        [Parameter(Mandatory = $true)] [string] $Value
    )
    $candidate = $Value.Trim()
    if (-not [System.IO.Path]::IsPathRooted($candidate)) {
        $candidate = Join-Path $ConfigDirectory $candidate
    }
    return (Resolve-Path -LiteralPath $candidate -ErrorAction Stop).Path
}

function ConvertFrom-OnHookBoolean {
    param(
        [Parameter(Mandatory = $true)] [string] $Value,
        [Parameter(Mandatory = $true)] [string] $Key
    )
    switch ($Value.Trim().ToLowerInvariant()) {
        { $_ -in @('1', 'true', 'yes', 'on') } { return $true }
        { $_ -in @('0', 'false', 'no', 'off') } { return $false }
        default { throw "$Key must be 0/1, true/false, yes/no, or on/off." }
    }
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
# when discovering a Windows installation; Start-OpenNovaLanProcess resolves that
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
    if ($env:NW_LAN_PROBE -and (Test-Path -LiteralPath $env:NW_LAN_PROBE -PathType Leaf)) {
        return (Resolve-Path -LiteralPath $env:NW_LAN_PROBE).Path
    }
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

# Launch the OpenNova game project with an isolated set of NW_LAN_* variables.
# Start-Process inherits this process's environment, so temporarily install only
# the requested role, then restore the caller's exact LAN environment even when
# process creation fails. The official console wrapper remains the lifecycle
# owner; the returned Process is its proven runtime child and owns the window,
# readiness witness, and LAN socket.
function Start-OpenNovaLanProcess {
    param(
        [Parameter(Mandatory = $true)] [hashtable] $LanEnvironment,
        [string[]] $GodotArguments = @(),
        [string[]] $GameArguments = @()
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

    $environmentMutex = [System.Threading.Mutex]::new(
        $false, "Local\OpenNovaLanEnvironment-$PID")
    $environmentLockTaken = $false
    $saved = @{}
    try {
        try { $environmentLockTaken = $environmentMutex.WaitOne() }
        catch [System.Threading.AbandonedMutexException] {
            # The abandoned owner is gone and this thread now owns the mutex.
            $environmentLockTaken = $true
        }
        if (-not $environmentLockTaken) {
            throw "Could not acquire the OpenNova launch-environment mutex."
        }
        foreach ($entry in @(& $isolatedEntries)) {
            $saved[$entry.Name] = $entry.Value
        }
        foreach ($entry in @(& $isolatedEntries)) {
            [Environment]::SetEnvironmentVariable(
                $entry.Name, $null, [EnvironmentVariableTarget]::Process)
        }
        foreach ($name in $LanEnvironment.Keys) {
            [Environment]::SetEnvironmentVariable(
                [string] $name, [string] $LanEnvironment[$name], [EnvironmentVariableTarget]::Process)
        }

        # No --headless/hidden flags: each child is an ordinary, visible game instance.
        # Start-Process flattens ArgumentList on Windows, so quote every token that
        # contains whitespace. Reject embedded quotes instead of producing an
        # ambiguous command line.
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
            $launcher = Start-Process -FilePath $godotCandidate `
                -WorkingDirectory $projectDir `
                -ArgumentList ($arguments -join ' ') `
                -PassThru
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
        try {
            if ($environmentLockTaken) {
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
        finally {
            if ($environmentLockTaken) {
                $environmentMutex.ReleaseMutex()
            }
            $environmentMutex.Dispose()
        }
    }
}

# Close one exact OpenNova launch. Graceful close targets the runtime window;
# forced cleanup targets the official console wrapper so its KILL_ON_JOB_CLOSE
# job tears down the complete engine tree. Direct non-wrapper launches retain
# the same exact-Process fallback.
function Stop-OpenNovaLanProcess {
    param(
        [Parameter(Mandatory = $true)]
        [System.Diagnostics.Process] $Process,
        [ValidateRange(0, 60000)] [int] $GracefulTimeoutMs = 8000,
        [ValidateRange(100, 30000)] [int] $ForceTimeoutMs = 5000,
        [switch] $PassThruShutdownEvidence,
        [switch] $RequireCleanExit
    )

    $requestedUtc = [DateTime]::UtcNow.ToString('o')
    $closeRequested = $false
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

    # Start-OpenNovaLanProcess materializes the runtime handle while it is alive,
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
