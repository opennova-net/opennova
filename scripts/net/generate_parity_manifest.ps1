[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]{0,30}$')]
    [string] $SuitePrefix,
    [string] $Output = "",

    [Parameter(Mandatory = $true)]
    [string] $RetailServerRoot,

    [Parameter(Mandatory = $true)]
    [string] $RetailClientRoot,

    [Parameter(Mandatory = $true)]
    [string] $OnHookMcpPath,

    [Parameter(Mandatory = $true)]
    [string] $GodotLauncher
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$repo = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
. (Join-Path $repo "scripts\net\lib.ps1")
$serverDir = (Resolve-Path -LiteralPath $RetailServerRoot).Path
$clientDir = (Resolve-Path -LiteralPath $RetailClientRoot).Path
$onHookMcp = (Resolve-Path -LiteralPath $OnHookMcpPath).Path
if (-not (Test-Path -LiteralPath $serverDir -PathType Container) -or
        -not (Test-Path -LiteralPath $clientDir -PathType Container)) {
    throw "RetailServerRoot and RetailClientRoot must both be directories."
}
if (-not (Test-Path -LiteralPath $onHookMcp -PathType Leaf)) {
    throw "OnHookMcpPath must be a file."
}
if ([string]::Equals(
        $serverDir, $clientDir, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "RetailServerRoot and RetailClientRoot must be distinct installations."
}
$corpusRoot = Join-Path $repo ".scratch\parity\corpus"
$corpusExporter = Join-Path $repo "scripts\net\export_parity_corpus.py"
$godotLauncher = (Resolve-Path -LiteralPath $GodotLauncher).Path
if (-not (Test-Path -LiteralPath $godotLauncher -PathType Leaf)) {
    throw "GodotLauncher must be a file."
}
$godotRuntime = Resolve-GodotRuntimeBinary -Path $godotLauncher
$templatePath = Join-Path $repo "scripts\net\parity-matrix.example.json"
$manifest = Get-Content -LiteralPath $templatePath -Raw | ConvertFrom-Json
$templateCases = @($manifest.cases)
$requiredCaseIds = @($manifest.required_case_ids | ForEach-Object { [string]$_ })
$matrixBindings = Get-ParityMatrixBindings -Cases $templateCases
$caseBindings = $matrixBindings.CaseBindings
$missionBindings = @($matrixBindings.MissionBindings)
if ($templateCases.Count -eq 0 -or
        $requiredCaseIds.Count -ne $templateCases.Count -or
        ($requiredCaseIds -join "`n") -cne
            (@($templateCases | ForEach-Object { [string]$_.id }) -join "`n")) {
    throw "Manifest template required_case_ids must exactly order every runnable case."
}
if (-not (Test-ParityMatrixAutomationPorts -Cases $templateCases)) {
    throw "Manifest template must cover all four retail automation ports 32786-32789; additional sequential cases may reuse them."
}
if ($manifest.PSObject.Properties.Name -contains "retail_input_witness_examples") {
    $manifest.PSObject.Properties.Remove("retail_input_witness_examples")
}
if ($manifest.PSObject.Properties.Name -contains "opennova_joiner_shutdown_witness_examples") {
    $manifest.PSObject.Properties.Remove("opennova_joiner_shutdown_witness_examples")
}
if ($manifest.PSObject.Properties.Name -contains "opennova_host_shutdown_witness_examples") {
    $manifest.PSObject.Properties.Remove("opennova_host_shutdown_witness_examples")
}
$outputPath = if ($Output) {
    if ([System.IO.Path]::IsPathRooted($Output)) { $Output } else { Join-Path $repo $Output }
} else {
    Join-Path $repo ".scratch\parity-final\$SuitePrefix.manifest.json"
}
if (Test-Path -LiteralPath $outputPath) {
    throw "Manifest output must be create-new: $outputPath"
}

function Get-Sha256([string] $Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-RepoRelative([string] $Path) {
    $absolute = (Resolve-Path -LiteralPath $Path).Path
    $prefix = $repo.TrimEnd('\') + '\'
    if (-not $absolute.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Path is outside the repository: $absolute"
    }
    return "repo:" + $absolute.Substring($prefix.Length).Replace('\', '/')
}

function ConvertFrom-StrictUtcIsoTimestamp([string] $Value) {
    if ($Value -notmatch '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d{1,7})?Z$') {
        throw "Timestamp is not strict ISO-8601 UTC: $Value"
    }
    [string[]] $formats = @("yyyy-MM-dd'T'HH:mm:ss'Z'")
    foreach ($digits in 1..7) {
        $formats += "yyyy-MM-dd'T'HH:mm:ss.$('f' * $digits)'Z'"
    }
    $styles = [Globalization.DateTimeStyles]::AssumeUniversal -bor
        [Globalization.DateTimeStyles]::AdjustToUniversal
    return [DateTimeOffset]::ParseExact(
        $Value, $formats, [Globalization.CultureInfo]::InvariantCulture, $styles)
}

function Assert-ExactJsonProperties {
    param(
        [Parameter(Mandatory = $true)] $Value,
        [Parameter(Mandatory = $true)] [string[]] $Expected,
        [Parameter(Mandatory = $true)] [string] $Context
    )
    if (-not $Value) { throw "$Context is missing." }
    $actual = @($Value.PSObject.Properties.Name | Sort-Object)
    $wanted = @($Expected | Sort-Object)
    if (($actual -join ',') -cne ($wanted -join ',')) {
        throw "$Context property set drifted: actual=$($actual -join ',') expected=$($wanted -join ',')"
    }
}

function Get-FilePrefixSha256 {
    param(
        [Parameter(Mandatory = $true)] [string] $Path,
        [Parameter(Mandatory = $true)] [int64] $Length
    )
    $item = Get-Item -LiteralPath $Path -ErrorAction Stop
    if ($Length -le 0 -or $Length -gt $item.Length) {
        throw "Invalid hash prefix length $Length for $Path ($($item.Length) bytes)."
    }
    $stream = [IO.File]::Open(
        $item.FullName, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        [IO.FileShare]::Read)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $buffer = [byte[]]::new(1024 * 1024)
        [int64] $remaining = $Length
        while ($remaining -gt 0) {
            $requested = [int][Math]::Min([int64] $buffer.Length, $remaining)
            $read = $stream.Read($buffer, 0, $requested)
            if ($read -le 0) { throw "File ended before readiness prefix ${Length}: $Path" }
            $remaining -= $read
            if ($remaining -eq 0) {
                $null = $sha.TransformFinalBlock($buffer, 0, $read)
            }
            else {
                $null = $sha.TransformBlock($buffer, 0, $read, $buffer, 0)
            }
        }
        return ([BitConverter]::ToString($sha.Hash) -replace '-', '').ToLowerInvariant()
    }
    finally {
        $sha.Dispose()
        $stream.Dispose()
    }
}

function Get-ValidatedWireReadyWitness {
    param(
        [Parameter(Mandatory = $true)] $Witness,
        [Parameter(Mandatory = $true)] [string] $ExpectedRunRoot,
        [Parameter(Mandatory = $true)] [string] $ExpectedCapture,
        [Parameter(Mandatory = $true)] [string] $ExpectedLiveSourceCapture,
        [Parameter(Mandatory = $true)] [ValidateSet("RR", "RO", "OR", "OO")]
        [string] $Topology,
        [Parameter(Mandatory = $true)] [string] $ExpectedMission,
        [Parameter(Mandatory = $true)] [string] $ExpectedMissionDisplayName,
        [Parameter(Mandatory = $true)] [string] $ExpectedMissionHeaderSha256,
        [Parameter(Mandatory = $true)] [uint32] $ExpectedGameType,
        [Parameter(Mandatory = $true)] [string] $ExpectedServerName,
        [Parameter(Mandatory = $true)] [string] $ExpectedHostCallsign,
        [Parameter(Mandatory = $true)] [string] $ExpectedJoinerCallsign,
        [Parameter(Mandatory = $true)] [string] $ExpectedExpansion,
        [Parameter(Mandatory = $true)] [string] $ExpectedNwPp,
        [Parameter(Mandatory = $true)] [string] $ExpectedItems,
        [Parameter(Mandatory = $true)] [DateTimeOffset] $SteadyStarted
    )
    $witnessPath = (Resolve-Path -LiteralPath ([string] $Witness.witness_path) `
        -ErrorAction Stop).Path
    $expectedWitnessPath = (Resolve-Path -LiteralPath (
        Join-Path $ExpectedRunRoot "wire-ready.json") -ErrorAction Stop).Path
    if ($witnessPath -ne $expectedWitnessPath) {
        throw "Wire-ready witness path drift: topology=$Topology"
    }
    $disk = Get-Content -LiteralPath $witnessPath -Raw | ConvertFrom-Json
    if (($Witness | ConvertTo-Json -Depth 12 -Compress) -cne
            ($disk | ConvertTo-Json -Depth 12 -Compress)) {
        throw "Run summary wire-ready witness differs from its create-new artifact: topology=$Topology"
    }
    Assert-ExactJsonProperties $disk @(
        "schema", "ready", "created_utc", "witness_path", "expected",
        "thresholds", "source", "evidence"
    ) "wire-ready witness"
    Assert-ExactJsonProperties $disk.expected @(
        "host_callsign", "joiner_callsign", "host_session_name", "mission",
        "mission_display_name", "mission_header_sha256", "map", "game_type", "expansion"
    ) "wire-ready expected binding"
    Assert-ExactJsonProperties $disk.thresholds @(
        "decoded_c2s_0c", "decoded_local_s2c_0a", "state_span_ns",
        "matched_c2s_0x2c_s2c_0x57"
    ) "wire-ready thresholds"
    Assert-ExactJsonProperties $disk.source @(
        "capture", "capture_prefix_bytes", "capture_prefix_sha256",
        "capture_last_write_utc", "nw_pp", "nw_pp_sha256", "items",
        "items_sha256", "loaded_item_count", "parity_packet_count",
        "parity_event_count"
    ) "wire-ready source"
    Assert-ExactJsonProperties $disk.evidence @(
        "session", "client_sid", "server_sid", "joiner_player_info_frame",
        "advertised_mission", "session_config_frame", "mission_header_frame",
        "host_player_sync_frame",
        "joiner_player_sync_frame", "mission_status_frame", "world_load_frame",
        "gameplay_not_before_ts_ns", "player_handle", "player_type",
        "player_entity_first_frame", "c2s_0c_count", "c2s_0c_first_ts_ns",
        "c2s_0c_last_ts_ns", "c2s_0c_span_ns", "s2c_0a_count",
        "s2c_0a_first_ts_ns", "s2c_0a_last_ts_ns", "s2c_0a_span_ns",
        "matched_rtt_count", "first_rtt_token", "last_rtt_token"
    ) "wire-ready evidence"

    if ([string] $disk.schema -cne "opennova.parity-wire-readiness.v2" -or
            $disk.ready -isnot [bool] -or -not [bool] $disk.ready -or
            [string] $disk.witness_path -ne $witnessPath) {
        throw "Wire-ready witness header is invalid: topology=$Topology"
    }
    $expected = $disk.expected
    if (-not [string]::Equals([string] $expected.host_callsign,
                $ExpectedHostCallsign, [StringComparison]::Ordinal) -or
            -not [string]::Equals([string] $expected.joiner_callsign,
                $ExpectedJoinerCallsign, [StringComparison]::Ordinal) -or
            -not [string]::Equals([string] $expected.host_session_name,
                $ExpectedServerName, [StringComparison]::Ordinal) -or
            -not [string]::Equals([string] $expected.mission,
                $ExpectedMission, [StringComparison]::Ordinal) -or
            -not [string]::Equals([string] $expected.mission_display_name,
                $ExpectedMissionDisplayName, [StringComparison]::Ordinal) -or
            [string] $expected.mission_header_sha256 -cne
                $ExpectedMissionHeaderSha256 -or
            -not [string]::Equals([string] $expected.map,
                $ExpectedMission, [StringComparison]::Ordinal) -or
            [uint32] $expected.game_type -ne $ExpectedGameType -or
            -not [string]::Equals([string] $expected.expansion,
                $ExpectedExpansion, [StringComparison]::Ordinal)) {
        throw "Wire-ready exact case binding drifted: topology=$Topology"
    }
    $thresholds = $disk.thresholds
    if ([int] $thresholds.decoded_c2s_0c -ne 40 -or
            [int] $thresholds.decoded_local_s2c_0a -ne 40 -or
            [uint64] $thresholds.state_span_ns -ne [uint64]10000000000 -or
            [int] $thresholds.matched_c2s_0x2c_s2c_0x57 -ne 20) {
        throw "Wire-ready thresholds were weakened or drifted: topology=$Topology"
    }

    $capturePath = (Resolve-Path -LiteralPath $ExpectedCapture -ErrorAction Stop).Path
    $source = $disk.source
    $sourceCapture = (Resolve-Path -LiteralPath ([string] $source.capture) `
        -ErrorAction Stop).Path
    if ($sourceCapture -ne (Resolve-Path -LiteralPath $ExpectedLiveSourceCapture).Path) {
        throw "Wire-ready live source capture is not the exact owned dumpcap stream: topology=$Topology"
    }
    [int64] $prefixBytes = $source.capture_prefix_bytes
    if ($prefixBytes -le 24 -or $prefixBytes -gt (Get-Item -LiteralPath $capturePath).Length -or
            [string] $source.capture_prefix_sha256 -cnotmatch '^[0-9a-f]{64}$' -or
            (Get-FilePrefixSha256 $capturePath $prefixBytes) -cne
                [string] $source.capture_prefix_sha256) {
        throw "Wire-ready prefix is not an exact prefix of the final capture: topology=$Topology"
    }
    $nwPpPath = (Resolve-Path -LiteralPath ([string] $source.nw_pp `
        ) -ErrorAction Stop).Path
    $itemsPath = (Resolve-Path -LiteralPath ([string] $source.items `
        ) -ErrorAction Stop).Path
    if ($nwPpPath -ne (Resolve-Path -LiteralPath $ExpectedNwPp).Path -or
            $itemsPath -ne (Resolve-Path -LiteralPath $ExpectedItems).Path -or
            [string] $source.nw_pp_sha256 -cne (Get-Sha256 $nwPpPath) -or
            [string] $source.items_sha256 -cne (Get-Sha256 $itemsPath) -or
            [int] $source.loaded_item_count -le 0 -or
            [int] $source.parity_packet_count -le 0 -or
            [int] $source.parity_event_count -le 0) {
        throw "Wire-ready decoder/corpus source binding is invalid: topology=$Topology"
    }

    $created = ConvertFrom-StrictUtcIsoTimestamp ([string] $disk.created_utc)
    $captureWrite = ConvertFrom-StrictUtcIsoTimestamp `
        ([string] $source.capture_last_write_utc)
    $witnessItem = Get-Item -LiteralPath $witnessPath
    $fileWrite = [DateTimeOffset]::new($witnessItem.LastWriteTimeUtc)
    if ($created -gt $SteadyStarted -or
            $created -lt $SteadyStarted.AddSeconds(-10) -or
            $captureWrite -gt $created.AddSeconds(1) -or
            $fileWrite -lt $created.AddSeconds(-1) -or
            $fileWrite -gt $SteadyStarted.AddSeconds(2)) {
        throw "Wire-ready witness was not freshly published immediately before steady: topology=$Topology"
    }

    $evidence = $disk.evidence
    foreach ($frameField in @(
        "joiner_player_info_frame", "session_config_frame",
        "mission_header_frame", "host_player_sync_frame", "joiner_player_sync_frame",
        "mission_status_frame", "world_load_frame", "player_entity_first_frame"
    )) {
        if ([int64] $evidence.$frameField -le 0) {
            throw "Wire-ready evidence has invalid $frameField`: topology=$Topology"
        }
    }
    if ([int] $evidence.session -lt 0 -or [int] $evidence.session -gt 65535 -or
            -not (Test-ParityAdvertisedMissionName `
                ([string] $evidence.advertised_mission) $ExpectedMission `
                $ExpectedMissionDisplayName) -or
            [string] $evidence.client_sid -cnotmatch '^0x(?!00000000)[0-9a-f]{8}$' -or
            [string] $evidence.server_sid -cnotmatch '^0x(?!00000000)[0-9a-f]{8}$' -or
            [uint32] $evidence.player_handle -eq 0 -or
            [uint32] $evidence.player_type -eq 0 -or
            [int] $evidence.c2s_0c_count -lt 40 -or
            [int] $evidence.s2c_0a_count -lt 40 -or
            [uint64] $evidence.c2s_0c_span_ns -lt [uint64]10000000000 -or
            [uint64] $evidence.s2c_0a_span_ns -lt [uint64]10000000000 -or
            [int] $evidence.matched_rtt_count -lt 20 -or
            [string] $evidence.first_rtt_token -cnotmatch '^[0-9a-f]{8}$' -or
            [string] $evidence.last_rtt_token -cnotmatch '^[0-9a-f]{8}$') {
        throw "Wire-ready decoded evidence is below the acceptance floor: topology=$Topology"
    }
    if ([uint64] $evidence.c2s_0c_last_ts_ns -
                [uint64] $evidence.c2s_0c_first_ts_ns -ne
                [uint64] $evidence.c2s_0c_span_ns -or
            [uint64] $evidence.s2c_0a_last_ts_ns -
                [uint64] $evidence.s2c_0a_first_ts_ns -ne
                [uint64] $evidence.s2c_0a_span_ns -or
            [uint64] $evidence.gameplay_not_before_ts_ns -gt
                [uint64] $evidence.c2s_0c_first_ts_ns -or
            [uint64] $evidence.gameplay_not_before_ts_ns -gt
                [uint64] $evidence.s2c_0a_first_ts_ns) {
        throw "Wire-ready decoded timestamp evidence is inconsistent: topology=$Topology"
    }

    return [pscustomobject]@{
        path = Get-RepoRelative $witnessPath
        sha256 = Get-Sha256 $witnessPath
        schema = [string] $disk.schema
        ready = $true
        created_utc = [string] $disk.created_utc
        file_last_write_utc = $witnessItem.LastWriteTimeUtc.ToString("o")
        expected = $disk.expected
        thresholds = $disk.thresholds
        source = [pscustomobject]@{
            capture = Get-RepoRelative $capturePath
            capture_sha256 = Get-Sha256 $capturePath
            capture_prefix_bytes = $prefixBytes
            capture_prefix_sha256 = [string] $source.capture_prefix_sha256
            capture_last_write_utc = [string] $source.capture_last_write_utc
            nw_pp = Get-RepoRelative $nwPpPath
            nw_pp_sha256 = [string] $source.nw_pp_sha256
            items = Get-RepoRelative $itemsPath
            items_sha256 = [string] $source.items_sha256
            loaded_item_count = [int] $source.loaded_item_count
            parity_packet_count = [int] $source.parity_packet_count
            parity_event_count = [int] $source.parity_event_count
        }
        evidence = $disk.evidence
    }
}

function Assert-RetailLifecycleState {
    param(
        [Parameter(Mandatory = $true)] $State,
        [Parameter(Mandatory = $true)] [ValidateSet("host", "joiner")] [string] $Role,
        [Parameter(Mandatory = $true)] [int] $ExpectedPort,
        [Parameter(Mandatory = $true)] [ValidateSet("in_match", "deploy_hold")] [string] $Mode
    )
    foreach ($counter in @("dropped", "truncated", "write_errors", "unsupported_async")) {
        if ([int64] $State.capture_counters.$counter -ne 0) {
            throw "Retail $Role lifecycle has nonzero capture counter '$counter'."
        }
    }
    if ([string] $State.role -ne $Role -or [string] $State.failure -ne "none" -or
            -not [bool] $State.active -or -not [bool] $State.peer -or
            [string] $State.capture_state -ne "recording" -or
            [int] $State.network_type -ne 2 -or
            [int] $State.requested_port -ne $ExpectedPort -or
            [int64] $State.capture_counters.packets -lt 40) {
        throw "Retail $Role lifecycle is not an active captured peer."
    }
    if ($Role -eq "host") {
        if (-not [bool] $State.authority -or -not [bool] $State.responder_ready -or
                [int] $State.socket_mode -ne 3 -or [int] $State.connection_mode -ne 3 -or
                [int] $State.local_port -ne $ExpectedPort) {
            throw "Retail host lifecycle lacks exact responder/port ownership."
        }
    }
    else {
        if ([bool] $State.authority -or [int] $State.socket_mode -ne 2 -or
                [int] $State.connection_mode -ne 2 -or [int] $State.local_port -le 0) {
            throw "Retail joiner lifecycle lacks an exact client connection."
        }
        if ($Mode -eq "deploy_hold" -and
                ([string] $State.phase -ne "loading" -or [bool] $State.local_player)) {
            throw "Retail deploy-hold joiner did not remain in the player-paced loading state."
        }
    }
    if ($Mode -eq "in_match" -and
            ([string] $State.phase -ne "in_match" -or -not [bool] $State.local_player)) {
        throw "Retail $Role did not reach in-match/local-player readiness."
    }
}

function Get-ValidatedOpenNovaLaunchProof {
    param(
        [Parameter(Mandatory = $true)] $Proof,
        [Parameter(Mandatory = $true)]
        [ValidateSet("host", "joiner")] [string] $Role,
        [Parameter(Mandatory = $true)] [int] $ExpectedRuntimePid,
        [Parameter(Mandatory = $true)] [string] $ExpectedLauncherPath,
        [Parameter(Mandatory = $true)] [string] $ExpectedRuntimePath,
        [Parameter(Mandatory = $true)] [string[]] $ExpectedArguments,
        [Parameter(Mandatory = $true)] [DateTimeOffset] $SteadyStarted
    )
    $requiredProperties = @(
        "schema", "wrapper_used",
        "launcher_pid", "launcher_path", "launcher_start_utc",
        "launcher_created_utc", "launcher_command_line",
        "runtime_pid", "runtime_parent_pid", "runtime_path",
        "runtime_start_utc", "runtime_created_utc", "runtime_command_line",
        "launcher_console_host_pids", "arguments",
        "parent_verified", "argv_verified", "active_execution_verified",
        "proven_utc"
    )
    $actualProperties = @($Proof.PSObject.Properties.Name | Sort-Object)
    if (($actualProperties -join ',') -cne
            (@($requiredProperties | Sort-Object) -join ',')) {
        throw "OpenNova $Role launch-proof schema fields drifted."
    }
    if ([string] $Proof.schema -cne 'opennova.godot-process-ownership.v1') {
        throw "OpenNova $Role launch-proof schema version drifted."
    }
    foreach ($field in @(
        "wrapper_used", "parent_verified", "argv_verified",
        "active_execution_verified")) {
        $value = $Proof.$field
        if ($value -isnot [bool] -or -not [bool] $value) {
            throw "OpenNova $Role launch proof lacks true boolean '$field'."
        }
    }
    foreach ($field in @("launcher_pid", "runtime_pid", "runtime_parent_pid")) {
        if ([string] $Proof.$field -notmatch '^[1-9][0-9]*$' -or
                [int64] $Proof.$field -gt [int]::MaxValue) {
            throw "OpenNova $Role launch proof has invalid PID '$field'."
        }
    }
    $launcherPid = [int] $Proof.launcher_pid
    $runtimePid = [int] $Proof.runtime_pid
    if ($runtimePid -ne $ExpectedRuntimePid -or
            $runtimePid -eq $launcherPid -or
            [int] $Proof.runtime_parent_pid -ne $launcherPid) {
        throw "OpenNova $Role runtime/launcher PID ownership drifted."
    }
    $launcherPath = (Resolve-Path -LiteralPath ([string] $Proof.launcher_path) `
        -ErrorAction Stop).Path
    $runtimePath = (Resolve-Path -LiteralPath ([string] $Proof.runtime_path) `
        -ErrorAction Stop).Path
    $expectedLauncher = (Resolve-Path -LiteralPath $ExpectedLauncherPath `
        -ErrorAction Stop).Path
    $expectedRuntime = (Resolve-Path -LiteralPath $ExpectedRuntimePath `
        -ErrorAction Stop).Path
    if (-not [string]::Equals(
            $launcherPath, $expectedLauncher,
            [System.StringComparison]::OrdinalIgnoreCase) -or
            -not [string]::Equals(
                $runtimePath, $expectedRuntime,
                [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "OpenNova $Role launch proof is not bound to the hash-bound Godot launcher/runtime."
    }
    foreach ($field in @("launcher_command_line", "runtime_command_line")) {
        if ([string]::IsNullOrWhiteSpace([string] $Proof.$field)) {
            throw "OpenNova $Role launch proof has an empty '$field'."
        }
    }
    if ($Proof.arguments -isnot [System.Array]) {
        throw "OpenNova $Role launch-proof arguments are not a JSON array."
    }
    $actualArguments = @($Proof.arguments | ForEach-Object { [string] $_ })
    if (@($actualArguments | Where-Object { [string]::IsNullOrEmpty($_) }).Count -ne 0 -or
            $actualArguments.Count -ne $ExpectedArguments.Count -or
            ($actualArguments -join "`n") -cne ($ExpectedArguments -join "`n")) {
        throw "OpenNova $Role launch-proof argv drifted from the exact matrix invocation."
    }
    if ($Proof.launcher_console_host_pids -isnot [System.Array]) {
        throw "OpenNova $Role launch-proof console-host PIDs are not a JSON array."
    }
    $consoleHostPids = @($Proof.launcher_console_host_pids)
    foreach ($consoleHostPid in $consoleHostPids) {
        if ([string] $consoleHostPid -notmatch '^[1-9][0-9]*$' -or
                [int64] $consoleHostPid -gt [int]::MaxValue -or
                [int] $consoleHostPid -in @($launcherPid, $runtimePid)) {
            throw "OpenNova $Role launch proof has an invalid console-host PID."
        }
    }
    if (@($consoleHostPids | ForEach-Object { [int] $_ } |
            Sort-Object -Unique).Count -ne $consoleHostPids.Count) {
        throw "OpenNova $Role launch proof repeats a console-host PID."
    }
    $launcherStart = ConvertFrom-StrictUtcIsoTimestamp `
        ([string] $Proof.launcher_start_utc)
    $launcherCreated = ConvertFrom-StrictUtcIsoTimestamp `
        ([string] $Proof.launcher_created_utc)
    $runtimeStart = ConvertFrom-StrictUtcIsoTimestamp `
        ([string] $Proof.runtime_start_utc)
    $runtimeCreated = ConvertFrom-StrictUtcIsoTimestamp `
        ([string] $Proof.runtime_created_utc)
    $proven = ConvertFrom-StrictUtcIsoTimestamp ([string] $Proof.proven_utc)
    if ([Math]::Abs(($launcherStart - $launcherCreated).TotalMilliseconds) -gt 2000 -or
            [Math]::Abs(($runtimeStart - $runtimeCreated).TotalMilliseconds) -gt 2000 -or
            $launcherStart -gt $runtimeStart -or
            $launcherCreated -gt $runtimeCreated -or
            $runtimeStart -gt $proven -or
            $runtimeCreated -gt $proven -or
            $proven -gt $SteadyStarted) {
        throw "OpenNova $Role launch-proof timestamps are inconsistent with process creation and the steady window."
    }
    return $Proof
}

function Get-HexSha256([string] $Hex) {
    if ($Hex -notmatch '^(?:[0-9a-f]{2}){8,}$') {
        throw "Tail marker is not lowercase even-length hex of at least eight bytes."
    }
    [byte[]] $bytes = New-Object byte[] ($Hex.Length / 2)
    for ($i = 0; $i -lt $bytes.Length; $i++) {
        $bytes[$i] = [Convert]::ToByte($Hex.Substring($i * 2, 2), 16)
    }
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($bytes)) -replace '-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}

function ConvertFrom-StrictHexUtf8([string] $Hex) {
    if ($Hex -notmatch '^(?:[0-9a-f]{2}){8,}$') {
        throw "UTF-8 hex payload is not lowercase even-length hex of at least eight bytes."
    }
    [byte[]] $bytes = New-Object byte[] ($Hex.Length / 2)
    for ($i = 0; $i -lt $bytes.Length; $i++) {
        $bytes[$i] = [Convert]::ToByte($Hex.Substring($i * 2, 2), 16)
    }
    $strictUtf8 = New-Object System.Text.UTF8Encoding($false, $true)
    return $strictUtf8.GetString($bytes)
}

function Get-ValidatedPcapTailMarkerText {
    param(
        [Parameter(Mandatory = $true)] [string] $Hex,
        [Parameter(Mandatory = $true)] [string] $ExpectedRunId
    )
    $lowerHex = $Hex.ToLowerInvariant()
    if ($Hex -cne $lowerHex) {
        throw "Tail marker hex must be canonical lowercase."
    }
    $text = ConvertFrom-StrictHexUtf8 $lowerHex
    $expectedPattern = '^OPENNOVA-PCAPNG-TAIL\|' +
        [regex]::Escape($ExpectedRunId) + '\|[0-9a-f]{32}$'
    if ($text -cnotmatch $expectedPattern) {
        throw "Tail marker text is not bound to run '$ExpectedRunId'."
    }
    return $text
}

function Get-ValidatedMotionGateWitness {
    param(
        [Parameter(Mandatory = $true)] $Witness,
        [Parameter(Mandatory = $true)] [string] $ExpectedRunRoot,
        [Parameter(Mandatory = $true)] [string] $ExpectedRunId,
        [Parameter(Mandatory = $true)]
        [ValidateSet("RO", "OO")] [string] $ExpectedTopology,
        [Parameter(Mandatory = $true)] [int] $ExpectedJoinerPid,
        [Parameter(Mandatory = $true)] [DateTimeOffset] $SteadyStarted,
        [Parameter(Mandatory = $true)] [DateTimeOffset] $SteadyCompleted
    )
    $gatePath = (Resolve-Path -LiteralPath ([string] $Witness.path) `
        -ErrorAction Stop).Path
    $expectedGatePath = (Resolve-Path -LiteralPath (
        Join-Path $ExpectedRunRoot "joiner\motion-gate.json") `
        -ErrorAction Stop).Path
    $gateHash = Get-Sha256 $gatePath
    $gateItem = Get-Item -LiteralPath $gatePath
    $gate = Get-Content -LiteralPath $gatePath -Raw | ConvertFrom-Json
    $created = ConvertFrom-StrictUtcIsoTimestamp ([string] $gate.created_utc)
    $gateSteadyStarted = ConvertFrom-StrictUtcIsoTimestamp `
        ([string] $gate.steady_started_utc)
    $declaredFileWrite = ConvertFrom-StrictUtcIsoTimestamp `
        ([string] $Witness.file_last_write_utc)
    $actualFileWrite = [DateTimeOffset]::new($gateItem.LastWriteTimeUtc)
    if ($gatePath -ne $expectedGatePath -or
            [string] $Witness.sha256 -ne $gateHash -or
            [string] $Witness.run_id -ne $ExpectedRunId -or
            [string] $Witness.topology -ne $ExpectedTopology -or
            [int] $Witness.joiner_pid -ne $ExpectedJoinerPid -or
            [string] $Witness.created_utc -ne [string] $gate.created_utc -or
            [string] $Witness.steady_started_utc -ne [string] $gate.steady_started_utc -or
            [int] $gate.schema -ne 1 -or
            [string] $gate.run_id -ne $ExpectedRunId -or
            [string] $gate.topology -ne $ExpectedTopology -or
            [int] $gate.joiner_pid -ne $ExpectedJoinerPid -or
            $gateSteadyStarted -ne $SteadyStarted -or
            $declaredFileWrite -ne $actualFileWrite -or
            [int] $gate.pre_roll_ms -ne 250 -or
            [int] $gate.inter_phase_gap_ms -ne 250 -or
            [int] $gate.look_samples_per_phase -ne 20 -or
            [int] $gate.forward_ms -ne 1800 -or
            [int] $gate.forward_look -ne 320 -or
            [int] $gate.strafe_ms -ne 1200 -or
            [int] $gate.strafe_look -ne -320 -or
            [int] $gate.return_ms -ne 900 -or
            [int] $gate.return_look -ne 160) {
        throw "Motion-gate path/hash/run metadata drift: run=$ExpectedRunId topology=$ExpectedTopology"
    }
    if ($created -lt $SteadyStarted -or
            $created -gt $SteadyStarted.AddMilliseconds(2000) -or
            $created -ge $SteadyCompleted -or
            $actualFileWrite -lt $created.AddMilliseconds(-1000) -or
            $actualFileWrite -gt $SteadyStarted.AddMilliseconds(3000)) {
        throw "Motion gate was not published at steady-window start: run=$ExpectedRunId topology=$ExpectedTopology"
    }
    return [pscustomobject]@{
        path = Get-RepoRelative $gatePath
        sha256 = $gateHash
        run_id = $ExpectedRunId
        topology = $ExpectedTopology
        joiner_pid = $ExpectedJoinerPid
        created_utc = [string] $gate.created_utc
        steady_started_utc = [string] $gate.steady_started_utc
        file_last_write_utc = [string] $Witness.file_last_write_utc
        pre_roll_ms = [int] $gate.pre_roll_ms
        inter_phase_gap_ms = [int] $gate.inter_phase_gap_ms
        look_samples_per_phase = [int] $gate.look_samples_per_phase
        forward_ms = [int] $gate.forward_ms
        forward_look = [int] $gate.forward_look
        strafe_ms = [int] $gate.strafe_ms
        strafe_look = [int] $gate.strafe_look
        return_ms = [int] $gate.return_ms
        return_look = [int] $gate.return_look
    }
}

function Get-ValidatedRetailInputWitness {
    param(
        [Parameter(Mandatory = $true)] $Witness,
        [Parameter(Mandatory = $true)] [string] $ExpectedRunRoot,
        [Parameter(Mandatory = $true)] [string] $ExpectedRunId,
        [Parameter(Mandatory = $true)]
        [ValidateSet("RR", "OR")] [string] $ExpectedTopology,
        [Parameter(Mandatory = $true)] [int] $ExpectedPid,
        [Parameter(Mandatory = $true)] [DateTimeOffset] $SteadyStarted,
        [Parameter(Mandatory = $true)] [DateTimeOffset] $SteadyCompleted
    )
    Assert-ExactJsonProperties $Witness @("path", "sha256", "pid", "detail") `
        "retail-input run-summary witness"
    $inputPath = (Resolve-Path -LiteralPath ([string] $Witness.path) `
        -ErrorAction Stop).Path
    $expectedInputPath = (Resolve-Path -LiteralPath (
        Join-Path $ExpectedRunRoot "retail-input.json") -ErrorAction Stop).Path
    $inputHash = Get-Sha256 $inputPath
    $disk = Get-Content -LiteralPath $inputPath -Raw | ConvertFrom-Json
    Assert-ExactJsonProperties $disk @(
        "schema", "complete", "run_id", "topology", "pid",
        "process_start_utc", "window_handle", "started_utc", "completed_utc",
        "forward_ms", "strafe_ms", "return_ms", "turn_pixels",
        "injected_events", "focus_attempts", "foreground_checks"
    ) "retail-input artifact"
    if (($Witness.detail | ConvertTo-Json -Depth 5 -Compress) -cne
            ($disk | ConvertTo-Json -Depth 5 -Compress)) {
        throw "Run-summary retail-input detail differs from its create-new artifact: run=$ExpectedRunId topology=$ExpectedTopology"
    }
    $processStarted = ConvertFrom-StrictUtcIsoTimestamp ([string] $disk.process_start_utc)
    $started = ConvertFrom-StrictUtcIsoTimestamp ([string] $disk.started_utc)
    $completed = ConvertFrom-StrictUtcIsoTimestamp ([string] $disk.completed_utc)
    if ($inputPath -ne $expectedInputPath -or
            [string] $Witness.sha256 -cne $inputHash -or
            [string] $Witness.sha256 -cnotmatch '^[0-9a-f]{64}$' -or
            [int] $Witness.pid -ne $ExpectedPid -or
            [string] $disk.schema -cne "opennova.retail-input.v1" -or
            $disk.complete -isnot [bool] -or -not [bool] $disk.complete -or
            [string] $disk.run_id -cne $ExpectedRunId -or
            [string] $disk.topology -cne $ExpectedTopology -or
            [int] $disk.pid -ne $ExpectedPid -or
            [int64] $disk.window_handle -le 0 -or
            [int] $disk.forward_ms -ne 1800 -or
            [int] $disk.strafe_ms -ne 1200 -or
            [int] $disk.return_ms -ne 900 -or
            [int] $disk.turn_pixels -ne 320 -or
            [int] $disk.injected_events -ne 66 -or
            [int] $disk.focus_attempts -lt 3 -or
            [int] $disk.focus_attempts -gt 30 -or
            [int] $disk.foreground_checks -lt 195 -or
            [int] $disk.foreground_checks -gt 222) {
        throw "Retail-input path/hash/run/topology/PID/trajectory drift: run=$ExpectedRunId topology=$ExpectedTopology"
    }
    if ($processStarted -gt $SteadyStarted -or $processStarted -gt $started -or
            $started -lt $SteadyStarted -or
            $completed -le $started -or
            $completed -gt $SteadyCompleted) {
        throw "Retail-input timestamps are not wholly within the steady window: run=$ExpectedRunId topology=$ExpectedTopology"
    }
    return [pscustomobject]@{
        path = Get-RepoRelative $inputPath
        sha256 = $inputHash
        schema = [string] $disk.schema
        complete = [bool] $disk.complete
        run_id = [string] $disk.run_id
        topology = [string] $disk.topology
        pid = [int] $disk.pid
        process_start_utc = [string] $disk.process_start_utc
        window_handle = [int64] $disk.window_handle
        started_utc = [string] $disk.started_utc
        completed_utc = [string] $disk.completed_utc
        forward_ms = [int] $disk.forward_ms
        strafe_ms = [int] $disk.strafe_ms
        return_ms = [int] $disk.return_ms
        turn_pixels = [int] $disk.turn_pixels
        injected_events = [int] $disk.injected_events
        focus_attempts = [int] $disk.focus_attempts
        foreground_checks = [int] $disk.foreground_checks
    }
}

function Get-ValidatedOpenNovaJoinerShutdownWitness {
    param(
        [Parameter(Mandatory = $true)] $Witness,
        [Parameter(Mandatory = $true)] [string] $ExpectedRunRoot,
        [Parameter(Mandatory = $true)] [string] $ExpectedRunId,
        [Parameter(Mandatory = $true)]
        [ValidateSet("RO", "OO")] [string] $ExpectedTopology,
        [Parameter(Mandatory = $true)] [int] $ExpectedPid,
        [Parameter(Mandatory = $true)] [DateTimeOffset] $SteadyCompleted
    )
    Assert-ExactJsonProperties $Witness @(
        "request_path", "request_sha256", "path", "sha256", "schema",
        "run_id", "topology", "process_id", "requested_utc",
        "completed_utc", "clean"
    ) "OpenNova joiner shutdown run-summary witness"
    $requestPath = (Resolve-Path -LiteralPath ([string] $Witness.request_path) `
        -ErrorAction Stop).Path
    $ackPath = (Resolve-Path -LiteralPath ([string] $Witness.path) `
        -ErrorAction Stop).Path
    $expectedRequestPath = (Resolve-Path -LiteralPath (
        Join-Path $ExpectedRunRoot "joiner\joiner-stop-request.json") `
        -ErrorAction Stop).Path
    $expectedAckPath = (Resolve-Path -LiteralPath (
        Join-Path $ExpectedRunRoot "joiner\joiner-shutdown.json") `
        -ErrorAction Stop).Path
    $requestHash = Get-Sha256 $requestPath
    $ackHash = Get-Sha256 $ackPath
    [byte[]] $requestRaw = [IO.File]::ReadAllBytes($requestPath)
    [byte[]] $ackRaw = [IO.File]::ReadAllBytes($ackPath)
    foreach ($artifact in @(
        [pscustomobject]@{name="request";raw=$requestRaw},
        [pscustomobject]@{name="ack";raw=$ackRaw}
    )) {
        if ($artifact.raw.Length -eq 0 -or ($artifact.raw.Length -ge 3 -and
                $artifact.raw[0] -eq 0xef -and $artifact.raw[1] -eq 0xbb -and
                $artifact.raw[2] -eq 0xbf)) {
            throw "OpenNova joiner shutdown $($artifact.name) is empty or has a UTF-8 BOM"
        }
    }
    $request = [Text.Encoding]::UTF8.GetString($requestRaw) | ConvertFrom-Json
    $ack = [Text.Encoding]::UTF8.GetString($ackRaw) | ConvertFrom-Json
    Assert-ExactJsonProperties $request @(
        "schema", "run_id", "topology", "pid", "requested_utc"
    ) "OpenNova joiner stop request"
    Assert-ExactJsonProperties $ack @(
        "schema", "run_id", "topology", "process_id", "requested_utc",
        "completed_utc", "clean"
    ) "OpenNova joiner shutdown acknowledgement"
    $requested = ConvertFrom-StrictUtcIsoTimestamp ([string] $ack.requested_utc)
    $completed = ConvertFrom-StrictUtcIsoTimestamp ([string] $ack.completed_utc)
    if ($requestPath -ne $expectedRequestPath -or $ackPath -ne $expectedAckPath -or
            $Witness.request_path -isnot [string] -or
            $Witness.request_sha256 -isnot [string] -or
            $Witness.path -isnot [string] -or $Witness.sha256 -isnot [string] -or
            $Witness.schema -isnot [string] -or $Witness.run_id -isnot [string] -or
            $Witness.topology -isnot [string] -or
            -not ($Witness.process_id -is [int] -or $Witness.process_id -is [long]) -or
            $Witness.requested_utc -isnot [string] -or
            $Witness.completed_utc -isnot [string] -or
            $Witness.clean -isnot [bool] -or -not [bool] $Witness.clean -or
            [string] $Witness.request_sha256 -cne $requestHash -or
            [string] $Witness.sha256 -cne $ackHash -or
            [string] $Witness.request_sha256 -cnotmatch '^[0-9a-f]{64}$' -or
            [string] $Witness.sha256 -cnotmatch '^[0-9a-f]{64}$' -or
            $request.schema -isnot [string] -or $request.run_id -isnot [string] -or
            $request.topology -isnot [string] -or
            -not ($request.pid -is [int] -or $request.pid -is [long]) -or
            $request.requested_utc -isnot [string] -or
            [string] $request.schema -cne "opennova.parity-joiner-stop-request.v1" -or
            [string] $request.run_id -cne $ExpectedRunId -or
            [string] $request.topology -cne $ExpectedTopology -or
            [int] $request.pid -ne $ExpectedPid -or
            $ack.schema -isnot [string] -or $ack.run_id -isnot [string] -or
            $ack.topology -isnot [string] -or
            -not ($ack.process_id -is [int] -or $ack.process_id -is [long]) -or
            $ack.requested_utc -isnot [string] -or $ack.completed_utc -isnot [string] -or
            [string] $ack.schema -cne "opennova.parity-joiner-shutdown.v1" -or
            [string] $ack.run_id -cne $ExpectedRunId -or
            [string] $ack.topology -cne $ExpectedTopology -or
            [int] $ack.process_id -ne $ExpectedPid -or
            [string] $ack.requested_utc -cne [string] $request.requested_utc -or
            $ack.clean -isnot [bool] -or -not [bool] $ack.clean) {
        throw "OpenNova joiner shutdown path/hash/request/ack identity drift: run=$ExpectedRunId topology=$ExpectedTopology"
    }
    foreach ($field in @(
        "schema", "run_id", "topology", "process_id", "requested_utc",
        "completed_utc", "clean"
    )) {
        if ("$($Witness.$field)" -cne "$($ack.$field)") {
            throw "OpenNova joiner shutdown run summary differs from ack field '$field'"
        }
    }
    if ($requested -lt $SteadyCompleted -or $completed -lt $requested -or
            $completed -gt $requested.AddSeconds(20)) {
        throw "OpenNova joiner shutdown timestamps do not follow steady completion within 20 seconds"
    }
    return [pscustomobject]@{
        request_path = Get-RepoRelative $requestPath
        request_sha256 = $requestHash
        path = Get-RepoRelative $ackPath
        sha256 = $ackHash
        schema = [string] $ack.schema
        run_id = [string] $ack.run_id
        topology = [string] $ack.topology
        process_id = [int] $ack.process_id
        requested_utc = [string] $ack.requested_utc
        completed_utc = [string] $ack.completed_utc
        clean = [bool] $ack.clean
    }
}

function Get-ValidatedOpenNovaHostShutdownWitness {
    param(
        [Parameter(Mandatory = $true)] $Witness,
        [Parameter(Mandatory = $true)] [string] $ExpectedRunRoot,
        [Parameter(Mandatory = $true)] [string] $ExpectedRunId,
        [Parameter(Mandatory = $true)]
        [ValidateSet("OR", "OO")] [string] $ExpectedTopology,
        [Parameter(Mandatory = $true)] [int] $ExpectedPid,
        [Parameter(Mandatory = $true)] [DateTimeOffset] $RuntimeStarted,
        [Parameter(Mandatory = $true)] [DateTimeOffset] $SteadyCompleted
    )
    Assert-ExactJsonProperties $Witness @(
        "schema", "run_id", "topology", "process_id", "requested_utc",
        "completed_utc", "close_requested", "exit_code", "clean",
        "log_path", "log_sha256", "forbidden_diagnostics_absent"
    ) "OpenNova host shutdown run-summary witness"

    $logPath = (Resolve-Path -LiteralPath ([string] $Witness.log_path) `
        -ErrorAction Stop).Path
    $expectedLogPath = (Resolve-Path -LiteralPath (
        Join-Path $ExpectedRunRoot "host\godot.log") -ErrorAction Stop).Path
    [byte[]] $logRaw = [IO.File]::ReadAllBytes($logPath)
    $hasBom = ($logRaw.Length -ge 3 -and
            $logRaw[0] -eq 0xef -and $logRaw[1] -eq 0xbb -and
            $logRaw[2] -eq 0xbf) -or
        ($logRaw.Length -ge 2 -and
            (($logRaw[0] -eq 0xff -and $logRaw[1] -eq 0xfe) -or
             ($logRaw[0] -eq 0xfe -and $logRaw[1] -eq 0xff))) -or
        ($logRaw.Length -ge 4 -and
            $logRaw[0] -eq 0x00 -and $logRaw[1] -eq 0x00 -and
            $logRaw[2] -eq 0xfe -and $logRaw[3] -eq 0xff)
    if ($logRaw.Length -eq 0 -or $hasBom -or
            [Array]::IndexOf($logRaw, [byte]0) -ge 0) {
        throw "OpenNova host log is empty, BOM-prefixed, or contains NUL bytes"
    }
    try {
        $logText = [Text.UTF8Encoding]::new($false, $true).GetString($logRaw)
    }
    catch {
        throw "OpenNova host log is not strict BOM-free UTF-8: $($_.Exception.Message)"
    }
    if ([string]::IsNullOrWhiteSpace($logText)) {
        throw "OpenNova host log contains no diagnostic text"
    }
    $forbiddenDiagnostics = @(
        [pscustomobject]@{
            Name = "ObjectDB leak"
            Pattern = '(?im)\bObjectDB\b[^\r\n]*\bleak(?:ed|s)?\b'
        },
        [pscustomobject]@{
            Name = "resource leak"
            Pattern = '(?im)\bResources?\b[^\r\n]*(?:\bstill in use\b|\bleak(?:ed|s)?\b)'
        },
        [pscustomobject]@{
            Name = "RenderingServer leak"
            Pattern = '(?im)\bRenderingServer\b[^\r\n]*(?:\bstill in use\b|\bleak(?:ed|s)?\b|get_singleton\(\))'
        },
        [pscustomobject]@{
            Name = "RID leak"
            Pattern = '(?im)\bRIDs?\b[^\r\n]*\bleak(?:ed|s)?\b'
        }
    )
    $forbiddenMatches = @($forbiddenDiagnostics | Where-Object {
        $logText -match $_.Pattern
    } | ForEach-Object { $_.Name })
    if ($forbiddenMatches.Count -ne 0) {
        throw "OpenNova host log contains forbidden teardown diagnostics: $($forbiddenMatches -join ', ')"
    }

    $logHash = Get-Sha256 $logPath
    $requested = ConvertFrom-StrictUtcIsoTimestamp ([string] $Witness.requested_utc)
    $completed = ConvertFrom-StrictUtcIsoTimestamp ([string] $Witness.completed_utc)
    if (-not $logPath.Equals($expectedLogPath,
                [StringComparison]::OrdinalIgnoreCase) -or
            $ExpectedPid -le 0 -or
            $Witness.schema -isnot [string] -or
            $Witness.run_id -isnot [string] -or
            $Witness.topology -isnot [string] -or
            -not ($Witness.process_id -is [int] -or
                $Witness.process_id -is [long]) -or
            $Witness.requested_utc -isnot [string] -or
            $Witness.completed_utc -isnot [string] -or
            $Witness.close_requested -isnot [bool] -or
            -not [bool] $Witness.close_requested -or
            -not ($Witness.exit_code -is [int] -or
                $Witness.exit_code -is [long]) -or
            [int] $Witness.exit_code -ne 0 -or
            $Witness.clean -isnot [bool] -or -not [bool] $Witness.clean -or
            $Witness.log_path -isnot [string] -or
            $Witness.log_sha256 -isnot [string] -or
            $Witness.forbidden_diagnostics_absent -isnot [bool] -or
            -not [bool] $Witness.forbidden_diagnostics_absent -or
            [string] $Witness.schema -cne "opennova.parity-host-shutdown.v1" -or
            [string] $Witness.run_id -cne $ExpectedRunId -or
            [string] $Witness.topology -cne $ExpectedTopology -or
            [int] $Witness.process_id -ne $ExpectedPid -or
            [string] $Witness.log_sha256 -cnotmatch '^[0-9a-f]{64}$' -or
            [string] $Witness.log_sha256 -cne $logHash) {
        throw "OpenNova host shutdown identity/exit/log contract drift: run=$ExpectedRunId topology=$ExpectedTopology"
    }
    if ($requested -lt $RuntimeStarted -or $requested -lt $SteadyCompleted -or
            $completed -lt $requested -or
            $completed -gt $requested.AddSeconds(20)) {
        throw "OpenNova host shutdown timestamps do not follow process/steady start within 20 seconds"
    }
    return [pscustomobject]@{
        schema = [string] $Witness.schema
        run_id = [string] $Witness.run_id
        topology = [string] $Witness.topology
        process_id = [int] $Witness.process_id
        requested_utc = [string] $Witness.requested_utc
        completed_utc = [string] $Witness.completed_utc
        close_requested = [bool] $Witness.close_requested
        exit_code = [int] $Witness.exit_code
        clean = [bool] $Witness.clean
        log_path = Get-RepoRelative $logPath
        log_sha256 = $logHash
        forbidden_diagnostics_absent = $true
    }
}

$corpusArguments = @(
    "run", "python", $corpusExporter,
    "--server-root", $serverDir,
    "--client-root", $clientDir,
    "--expansion", "revx02",
    "--output-dir", $corpusRoot,
    "--mode", "verify"
)
foreach ($binding in $missionBindings) {
    $corpusArguments += @("--mission", [string]$binding.Mission)
}
foreach ($binding in @(
    "items.def=expansion/revx02/RevX02.pff",
    "ammo.def=expansion/revx02/RevX02.pff"
)) {
    $corpusArguments += @("--expect-archive", $binding)
}
foreach ($binding in $missionBindings) {
    $corpusArguments += @(
        "--expect-archive", "$($binding.Mission)=localres.pff")
}
$runtimeCorpusLines = @(& uv @corpusArguments)
if ($LASTEXITCODE -ne 0) { throw "Runtime VFS corpus verification failed: $LASTEXITCODE" }
$runtimeCorpus = ($runtimeCorpusLines -join "`n") | ConvertFrom-Json

$artifactPaths = @{
    items = Join-Path $corpusRoot "items.def"
    ammo = Join-Path $corpusRoot "ammo.def"
    retail_exe = Join-Path $serverDir "Jointops.exe"
    hook = Join-Path $serverDir "binkw32.dll"
    resource = Join-Path $serverDir "resource.pff"
    localres = Join-Path $serverDir "localres.pff"
    language = Join-Path $serverDir "language.pff"
    med = Join-Path $serverDir "med.pff"
    expansion_pff = Join-Path $serverDir "expansion\revx02\RevX02.pff"
    expansion_language_pff = Join-Path $serverDir "expansion\revx02\RevX02L.pff"
    expansion_bin = Join-Path $serverDir "expansion\revx02\revx02.bin"
    opennova_runtime = Join-Path $repo "godot\bin\libopennova.windows.template_debug.x86_64.dll"
    godot_launcher = $godotLauncher
    godot_runtime = $godotRuntime
    nw_pp = Join-Path $repo "build\apps\nw_pp\Release\nw_pp.exe"
    lan_probe = Join-Path $repo "build\apps\nw_lan_probe\Release\nw-lan-probe.exe"
    onhook_mcp = $onHookMcp
    parity_runner = Join-Path $repo "scripts\net\run_parity_topology.ps1"
    matrix_runner = Join-Path $repo "scripts\net\run_parity_matrix.ps1"
    manifest_generator = $PSCommandPath
    auto_joiner = Join-Path $repo "godot\tests\net\parity_joiner_driver.gd"
    capture_script = Join-Path $repo "scripts\net\capture.ps1"
    host_script = Join-Path $repo "scripts\net\host_opennova.ps1"
    join_script = Join-Path $repo "scripts\net\join_opennova.ps1"
    net_lib = Join-Path $repo "scripts\net\lib.ps1"
    retail_launcher = Join-Path $repo "scripts\net\launch_retail_cfg.ps1"
    verifier = Join-Path $repo "scripts\net\verify_parity_matrix.ps1"
    manifest_template = $templatePath
    corpus_exporter = $corpusExporter
    retail_input_script = Join-Path $repo "scripts\net\exercise_retail_input.ps1"
    wire_ready_script = Join-Path $repo "scripts\net\wait_parity_wire_ready.ps1"
    server_cfg_template = Join-Path $repo "scripts\net\configs\retail-lan-server.onhook.cfg"
    client_cfg_template = Join-Path $repo "scripts\net\configs\retail-lan-client.onhook.cfg"
}
foreach ($binding in $missionBindings) {
    $role = [string]$binding.MissionRole
    if ($artifactPaths.ContainsKey($role)) {
        throw "Duplicate artifact role while binding parity mission: $role"
    }
    $artifactPaths[$role] = Join-Path $corpusRoot "missions\$($binding.Mission)"
}
$godotRuntimeArtifacts = Get-GodotRuntimeArtifacts -RepoRoot $repo
foreach ($role in $godotRuntimeArtifacts.Keys) {
    if ($artifactPaths.ContainsKey($role)) {
        throw "Duplicate artifact role while binding Godot project: $role"
    }
    $artifactPaths[$role] = $godotRuntimeArtifacts[$role]
}
$artifactHashes = @{}
foreach ($role in $artifactPaths.Keys) {
    $artifactHashes[$role] = Get-Sha256 $artifactPaths[$role]
}
$clientMirrorPaths = @{
    retail_exe = Join-Path $clientDir "Jointops.exe"
    hook = Join-Path $clientDir "binkw32.dll"
    resource = Join-Path $clientDir "resource.pff"
    localres = Join-Path $clientDir "localres.pff"
    language = Join-Path $clientDir "language.pff"
    med = Join-Path $clientDir "med.pff"
    expansion_pff = Join-Path $clientDir "expansion\revx02\RevX02.pff"
    expansion_language_pff = Join-Path $clientDir "expansion\revx02\RevX02L.pff"
    expansion_bin = Join-Path $clientDir "expansion\revx02\revx02.bin"
}
foreach ($role in $clientMirrorPaths.Keys) {
    $clientHash = Get-Sha256 $clientMirrorPaths[$role]
    if ($clientHash -ne $artifactHashes[$role]) {
        throw "Retail SERVER/CLIENT artifact mismatch for role '$role'."
    }
}
$repositorySourceState = Get-NetHarnessRepositorySourceState -RepoRoot $repo
$inventory = (@($artifactHashes.Keys | ForEach-Object {
    "$_=$($artifactHashes[$_])"
}) + @("repo_source_state=$($repositorySourceState.sha256)")) | Sort-Object
$inventoryBytes = [System.Text.Encoding]::UTF8.GetBytes(($inventory -join "`n") + "`n")
$hasher = [System.Security.Cryptography.SHA256]::Create()
try {
    $fingerprint = ([BitConverter]::ToString($hasher.ComputeHash($inventoryBytes)) -replace '-', '').ToLowerInvariant()
}
finally {
    $hasher.Dispose()
}
$suiteId = "$SuitePrefix-$($fingerprint.Substring(0, 12))"
$suiteRoot = Join-Path $repo ".scratch\parity-final\suites\$SuitePrefix"
$suiteProvenancePath = Join-Path $suiteRoot "suite-provenance.json"
if (-not (Test-Path -LiteralPath $suiteProvenancePath -PathType Leaf)) {
    throw "Missing create-new suite provenance: $suiteProvenancePath"
}
$suiteProvenance = Get-Content -LiteralPath $suiteProvenancePath -Raw | ConvertFrom-Json
if ([string] $suiteProvenance.suite_id -ne $suiteId -or
        [string] $suiteProvenance.suite_prefix -ne $SuitePrefix -or
        [string] $suiteProvenance.corpus_fingerprint -ne $fingerprint) {
    throw "Suite provenance does not match the current endpoint/artifact corpus."
}
Assert-ExactJsonProperties $suiteProvenance.source_state @(
    'schema','mode','sha256','entry_count','submodule_count'
) 'suite provenance source_state'
$declaredSourceState = $suiteProvenance.source_state |
    ConvertTo-Json -Depth 5 -Compress
$currentSourceState = $repositorySourceState |
    ConvertTo-Json -Depth 5 -Compress
if ($declaredSourceState -cne $currentSourceState) {
    throw "Suite provenance does not match the current repository source state."
}
$suiteRuntimeCorpus = $suiteProvenance.runtime_corpus | ConvertTo-Json -Depth 20 -Compress
$currentRuntimeCorpus = $runtimeCorpus | ConvertTo-Json -Depth 20 -Compress
if ($suiteRuntimeCorpus -ne $currentRuntimeCorpus) {
    throw "Suite provenance does not match the currently mounted runtime VFS corpus."
}
foreach ($role in $artifactHashes.Keys) {
    $declaredHash = [string] $suiteProvenance.artifact_hashes.$role
    if ($declaredHash -ne $artifactHashes[$role]) {
        throw "Suite provenance artifact drift for role '$role'."
    }
}

$manifest.suite_id = $suiteId
$manifest.corpus.fingerprint = $fingerprint
$manifest.corpus.source_state = $repositorySourceState
$declaredRoles = @($manifest.corpus.artifacts | ForEach-Object { [string] $_.role })
foreach ($role in @($artifactPaths.Keys | Sort-Object)) {
    if ($role -in $declaredRoles) { continue }
    $manifest.corpus.artifacts += [pscustomobject]@{
        role = $role
        path = [string] $artifactPaths[$role]
        sha256 = [string] $artifactHashes[$role]
    }
}
foreach ($artifact in $manifest.corpus.artifacts) {
    $role = [string] $artifact.role
    if (-not $artifactPaths.ContainsKey($role)) {
        throw "Unexpected artifact role in template: $role"
    }
    $artifactPath = (Resolve-Path -LiteralPath $artifactPaths[$role]).Path
    $repoPrefix = $repo.TrimEnd('\') + '\'
    $artifact.path = if ($artifactPath.StartsWith(
            $repoPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        Get-RepoRelative $artifactPath
    } else {
        $artifactPath
    }
    $artifact.sha256 = $artifactHashes[$role]
}

$topologyExpectations = @{
    RR = [pscustomobject]@{ Perspective = "retail-client"; Role = "joiner"; RelativeCapture = "joiner\net.pcap" }
    RO = [pscustomobject]@{ Perspective = "retail-host"; Role = "host"; RelativeCapture = "host\net.pcap" }
    OR = [pscustomobject]@{ Perspective = "retail-client"; Role = "joiner"; RelativeCapture = "joiner\traffic.pcap" }
    OO = [pscustomobject]@{ Perspective = "external"; Role = "external"; RelativeCapture = "external.pcapng" }
}
$observedRunIds = [System.Collections.Generic.List[string]]::new()

foreach ($case in $manifest.cases) {
    $caseId = [string] $case.id
    if (-not $caseBindings.ContainsKey($caseId)) {
        throw "Unexpected case in template: $caseId"
    }
    if (@($case.exceptions).Count -ne 0) {
        throw "Manifest template case=$caseId declares unsupported exceptions."
    }
    if (-not [string]::Equals(
                [string] $case.server_name,
                "Untitled ",
                [System.StringComparison]::Ordinal) -or
            -not [string]::Equals(
                [string] $case.host_session_name,
                [string] $case.server_name,
                [System.StringComparison]::Ordinal) -or
            [string]::IsNullOrWhiteSpace([string] $case.host_session_name) -or
            ([string] $case.host_session_name).Length -gt 31 -or
            [string] $case.host_session_name -notmatch '^[\x20-\x7e]+$' -or
            [string]::Equals(
                [string] $case.host_session_name,
                [string] $case.host_callsign,
                [System.StringComparison]::Ordinal)) {
        throw "Invalid or conflated host session name in template: case=$caseId"
    }
    $readinessMode = [string] $case.readiness_mode
    if ($readinessMode -notin @("in_match", "deploy_hold") -or
            ($readinessMode -eq "deploy_hold" -and [bool] $case.exercise_input)) {
        throw "Invalid readiness contract in template: case=$caseId"
    }
    $caseBinding = $caseBindings[$caseId]
    $slug = [string]$caseBinding.Slug
    $missionRole = [string]$caseBinding.MissionRole
    $missionPath = (Resolve-Path -LiteralPath $artifactPaths[$missionRole]).Path
    $missionHash = [string] $artifactHashes[$missionRole]
    $missionIdentity = Get-BmsHeaderIdentity -Path $missionPath
    $missionDisplayName = [string] $missionIdentity.MissionDisplayName
    $missionBinding = @($suiteProvenance.mission_bindings | Where-Object {
        [string] $_.mission -eq [string] $case.mission -and
        [string] $_.role -eq $missionRole -and
        [string] $_.sha256 -eq $missionHash
    })
    if ($missionBinding.Count -ne 1) {
        throw "Suite provenance lacks one exact mission binding for case=$caseId."
    }
    $case.mission_artifact.path = Get-RepoRelative $missionPath
    $case.mission_artifact.sha256 = $missionHash
    foreach ($topology in @("RR", "RO", "OR", "OO")) {
        $runId = "$SuitePrefix-$slug-$($topology.ToLowerInvariant())"
        $summaryPath = Join-Path $repo ".scratch\runs\$runId\run-summary.json"
        if (-not (Test-Path -LiteralPath $summaryPath)) {
            throw "Missing run summary: $summaryPath"
        }
        $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
        $expectedRunRoot = (Resolve-Path -LiteralPath (Split-Path -Parent $summaryPath)).Path
        $expected = $topologyExpectations[$topology]
        if ([string] $summary.topology -ne $topology -or
            [string] $summary.run_id -ne $runId -or
            [string] $summary.suite_id -ne $suiteId -or
            [string] $summary.corpus_fingerprint -ne $fingerprint -or
            [bool] $summary.wire_only -or
            [bool] $summary.exercise_input -ne [bool] $case.exercise_input -or
            (Resolve-Path -LiteralPath ([string] $summary.run_root)).Path -ne $expectedRunRoot -or
            [string] $summary.mission -ne [string] $case.mission -or
            (Resolve-Path -LiteralPath ([string] $summary.mission_path)).Path -ne $missionPath -or
            [string] $summary.mission_sha256 -ne $missionHash -or
            [int64] $summary.game_type -ne [int64] $case.game_type -or
            [int] $summary.port -ne [int] $case.port -or
            [string] $summary.resolution -ne [string] $case.resolution -or
            [string] $summary.expansion -ne [string] $manifest.corpus.expansion -or
            -not [string]::Equals(
                [string] $summary.host_session_name,
                [string] $case.host_session_name,
                [System.StringComparison]::Ordinal) -or
            -not [string]::Equals(
                [string] $summary.server_name,
                [string] $case.server_name,
                [System.StringComparison]::Ordinal) -or
            [string] $summary.host_callsign -ne [string] $case.host_callsign -or
            [string] $summary.joiner_callsign -ne [string] $case.joiner_callsign -or
            [string] $summary.readiness_mode -ne $readinessMode -or
            [int] $summary.steady_seconds -ne [int] $case.runs.$topology.steady_seconds -or
            [string] $summary.integrity_profile -ne "retail-revx02-024f56f2-2d087374" -or
            [string] $summary.evidence_perspective -ne [string] $expected.Perspective -or
            [string] $summary.evidence_role -ne [string] $expected.Role) {
            throw "Run summary metadata drift: case=$caseId topology=$topology"
        }
        $capture = (Resolve-Path -LiteralPath ([string] $summary.evidence_capture)).Path
        $relativeCapture = if ($readinessMode -eq "deploy_hold" -and
                $topology -in @("RR", "RO")) {
            if ($topology -eq "RR") { "joiner\traffic.pcap" } else { "host\traffic.pcap" }
        } else {
            [string] $expected.RelativeCapture
        }
        $expectedCapture = (Resolve-Path -LiteralPath (
            Join-Path $expectedRunRoot $relativeCapture)).Path
        if ($capture -ne $expectedCapture) {
            throw "Evidence capture ownership drift: case=$caseId topology=$topology"
        }
        $wireCapture = (Resolve-Path -LiteralPath `
            ([string] $summary.external_capture) -ErrorAction Stop).Path
        $expectedWireCapture = (Resolve-Path -LiteralPath (
            Join-Path $expectedRunRoot "external.pcapng") -ErrorAction Stop).Path
        $wireCaptureProof = $summary.external_capture_proof
        $wireArtifactPath = (Resolve-Path -LiteralPath `
            ([string] $wireCaptureProof.artifact_path) -ErrorAction Stop).Path
        $wireLiveSource = (Resolve-Path -LiteralPath `
            ([string] $wireCaptureProof.validated_source) -ErrorAction Stop).Path
        if ($wireCapture -ne $expectedWireCapture -or
                $wireArtifactPath -ne $wireCapture -or
                [string] $wireCaptureProof.artifact_sha256 -ne
                    (Get-Sha256 $wireCapture) -or
                -not [bool] $wireCaptureProof.validation.valid -or
                -not [bool] $wireCaptureProof.validation.exact_eof -or
                [int] $wireCaptureProof.validation.packet_block_count -le 0) {
            throw "Rolling external wire capture completion proof is invalid: case=$caseId topology=$topology"
        }
        $run = $case.runs.$topology
        if ([string] $run.perspective -ne [string] $summary.evidence_perspective) {
            throw "Manifest perspective drift: case=$caseId topology=$topology"
        }
        if (-not [string]::Equals(
                [string] $run.host_session_name,
                [string] $case.host_session_name,
                [System.StringComparison]::Ordinal)) {
            throw "Manifest host session name drift: case=$caseId topology=$topology"
        }
        $run | Add-Member -NotePropertyName run_id -NotePropertyValue $runId -Force
        $run | Add-Member -NotePropertyName suite_id -NotePropertyValue $suiteId -Force
        $run | Add-Member -NotePropertyName host_session_name `
            -NotePropertyValue ([string] $summary.host_session_name) -Force
        $run | Add-Member -NotePropertyName server_name `
            -NotePropertyValue ([string] $summary.server_name) -Force
        $run | Add-Member -NotePropertyName readiness_mode `
            -NotePropertyValue $readinessMode -Force
        foreach ($binding in ([ordered]@{
                mission=[string]$case.mission
                resolution=[string]$case.resolution
                port=[int]$case.port
                expansion=[string]$manifest.corpus.expansion
                host_callsign=[string]$case.host_callsign
                joiner_callsign=[string]$case.joiner_callsign
            }).GetEnumerator()) {
            $run | Add-Member -NotePropertyName $binding.Key `
                -NotePropertyValue $binding.Value -Force
        }
        if ($runId -notin @($suiteProvenance.run_ids | ForEach-Object { [string] $_ })) {
            throw "Run is absent from suite provenance: $runId"
        }
        $steadyStarted = ConvertFrom-StrictUtcIsoTimestamp ([string] $summary.steady_started_utc)
        $steadyCompleted = ConvertFrom-StrictUtcIsoTimestamp ([string] $summary.steady_completed_utc)
        $steadyDurationMs = ($steadyCompleted - $steadyStarted).TotalMilliseconds
        $declaredSteadyMs = 1000.0 * [double] $summary.steady_seconds
        if ($steadyDurationMs -lt $declaredSteadyMs -or
                $steadyDurationMs -gt $declaredSteadyMs + 15000.0) {
            throw "Run summary steady interval is outside its declared bound: case=$caseId topology=$topology actual_ms=$steadyDurationMs"
        }
        $run | Add-Member -NotePropertyName steady_started_utc `
            -NotePropertyValue ([string] $summary.steady_started_utc) -Force
        $run | Add-Member -NotePropertyName steady_completed_utc `
            -NotePropertyValue ([string] $summary.steady_completed_utc) -Force
        if (-not $summary.wire_ready_witness -or
                -not $summary.acceptance.wire_ready -or
                ($summary.wire_ready_witness | ConvertTo-Json -Depth 12 -Compress) -cne
                    ($summary.acceptance.wire_ready | ConvertTo-Json -Depth 12 -Compress)) {
            throw "Run summary lacks one internally consistent wire-ready witness: case=$caseId topology=$topology"
        }
        $wireReadyWitness = Get-ValidatedWireReadyWitness `
            -Witness $summary.wire_ready_witness `
            -ExpectedRunRoot $expectedRunRoot `
            -ExpectedCapture $wireCapture `
            -ExpectedLiveSourceCapture $wireLiveSource `
            -Topology $topology `
            -ExpectedMission ([string] $case.mission) `
            -ExpectedMissionDisplayName $missionDisplayName `
            -ExpectedMissionHeaderSha256 ([string] $missionIdentity.Sha256) `
            -ExpectedGameType ([uint32] $case.game_type) `
            -ExpectedServerName ([string] $case.server_name) `
            -ExpectedHostCallsign ([string] $case.host_callsign) `
            -ExpectedJoinerCallsign ([string] $case.joiner_callsign) `
            -ExpectedExpansion ([string] $manifest.corpus.expansion) `
            -ExpectedNwPp ([string] $artifactPaths.nw_pp) `
            -ExpectedItems ([string] $artifactPaths.items) `
            -SteadyStarted $steadyStarted
        $run | Add-Member -NotePropertyName wire_ready_witness `
            -NotePropertyValue $wireReadyWitness -Force
        $expectedLaunchProofRoles = switch ($topology) {
            "RR" { @() }
            "RO" { @("joiner") }
            "OR" { @("host") }
            "OO" { @("host", "joiner") }
        }
        $summaryLaunchProofs = $summary.opennova_launch_proofs
        $actualLaunchProofRoles = if ($summaryLaunchProofs) {
            @($summaryLaunchProofs.PSObject.Properties.Name | Sort-Object)
        } else {
            @()
        }
        if (($actualLaunchProofRoles -join ',') -cne
                (@($expectedLaunchProofRoles | Sort-Object) -join ',')) {
            throw "OpenNova launch-proof role set drift: case=$caseId topology=$topology"
        }
        $validatedLaunchProofs = [ordered]@{}
        foreach ($launchRole in $expectedLaunchProofRoles) {
            if ($launchRole -eq "host") {
                $expectedRuntimePid = [int] $summary.acceptance.opennova_host.pid
                $portOwnerPid = [int] $summary.acceptance.opennova_host.port_ownership.pid
                if ($expectedRuntimePid -le 0 -or $portOwnerPid -ne $expectedRuntimePid -or
                        ($topology -eq "OO" -and
                            [int] $summary.acceptance.host_pid -ne $expectedRuntimePid)) {
                    throw "OpenNova host acceptance PID ownership drift: case=$caseId topology=$topology"
                }
                $expectedResourceDir = $serverDir
            }
            else {
                $expectedRuntimePid = [int] $summary.acceptance.opennova_joiner.process_id
                if ($expectedRuntimePid -le 0 -or
                        ($topology -eq "OO" -and
                            [int] $summary.acceptance.joiner_pid -ne $expectedRuntimePid)) {
                    throw "OpenNova joiner readiness PID ownership drift: case=$caseId topology=$topology"
                }
                $expectedResourceDir = $clientDir
            }
            $expectedArguments = @(
                "--path", (Join-Path $repo "godot"),
                "--windowed", "--resolution", [string] $case.resolution,
                "--log-file", (Join-Path $expectedRunRoot "$launchRole\godot.log")
            )
            if ($launchRole -eq "joiner") {
                $expectedArguments += @(
                    "--script",
                    (Join-Path $repo "godot\tests\net\parity_joiner_driver.gd"))
            }
            $expectedArguments += @(
                "--", "--resource-dir", $expectedResourceDir,
                "/exp", [string] $manifest.corpus.expansion)
            $proofProperty = $summaryLaunchProofs.PSObject.Properties[$launchRole]
            if (-not $proofProperty -or -not $proofProperty.Value) {
                throw "Missing OpenNova $launchRole launch proof: case=$caseId topology=$topology"
            }
            $validatedProof = Get-ValidatedOpenNovaLaunchProof `
                -Proof $proofProperty.Value `
                -Role $launchRole `
                -ExpectedRuntimePid $expectedRuntimePid `
                -ExpectedLauncherPath ([string] $artifactPaths.godot_launcher) `
                -ExpectedRuntimePath ([string] $artifactPaths.godot_runtime) `
                -ExpectedArguments $expectedArguments `
                -SteadyStarted $steadyStarted
            $validatedLaunchProofs.Add($launchRole, $validatedProof)
        }
        $run | Add-Member -NotePropertyName opennova_launch_proofs `
            -NotePropertyValue ([pscustomobject] $validatedLaunchProofs) -Force
        $expectsJoinerShutdown = $topology -in @("RO", "OO")
        if ($expectsJoinerShutdown) {
            if ($summary.opennova_joiner_exact_fallback_required -isnot [bool] -or
                    [bool] $summary.opennova_joiner_exact_fallback_required -or
                    -not $summary.opennova_joiner_shutdown_witness) {
                throw "OpenNova joiner did not prove clean cooperative shutdown without exact fallback: case=$caseId topology=$topology"
            }
            $expectedJoinerPid = [int] $summary.acceptance.opennova_joiner.process_id
            $shutdownWitness = Get-ValidatedOpenNovaJoinerShutdownWitness `
                -Witness $summary.opennova_joiner_shutdown_witness `
                -ExpectedRunRoot $expectedRunRoot `
                -ExpectedRunId $runId `
                -ExpectedTopology $topology `
                -ExpectedPid $expectedJoinerPid `
                -SteadyCompleted $steadyCompleted
            $run | Add-Member -NotePropertyName opennova_joiner_shutdown_witness `
                -NotePropertyValue $shutdownWitness -Force
            $run | Add-Member -NotePropertyName opennova_joiner_exact_fallback_required `
                -NotePropertyValue $false -Force
        }
        else {
            if ($summary.opennova_joiner_exact_fallback_required -isnot [bool] -or
                    [bool] $summary.opennova_joiner_exact_fallback_required -or
                    $summary.opennova_joiner_shutdown_witness) {
                throw "Unexpected OpenNova joiner shutdown evidence: case=$caseId topology=$topology"
            }
            if ($run.PSObject.Properties.Name -contains "opennova_joiner_shutdown_witness" -or
                    $run.PSObject.Properties.Name -contains "opennova_joiner_exact_fallback_required") {
                throw "Template predeclares OpenNova joiner shutdown evidence for topology $topology"
            }
        }
        $expectsHostShutdown = $topology -in @("OR", "OO")
        if ($expectsHostShutdown) {
            $summaryHostShutdownProperties = @($summary.PSObject.Properties.Name)
            if ($summaryHostShutdownProperties -notcontains
                    "opennova_host_exact_fallback_required" -or
                    $summaryHostShutdownProperties -notcontains
                    "opennova_host_shutdown_witness" -or
                    $summary.opennova_host_exact_fallback_required -isnot [bool] -or
                    [bool] $summary.opennova_host_exact_fallback_required -or
                    -not $summary.opennova_host_shutdown_witness) {
                throw "OpenNova host did not prove clean shutdown without exact fallback: case=$caseId topology=$topology"
            }
            $expectedHostPid = [int] $summary.acceptance.opennova_host.pid
            $hostLaunchProof = $validatedLaunchProofs["host"]
            if (-not $hostLaunchProof -or
                    [int] $hostLaunchProof.runtime_pid -ne $expectedHostPid) {
                throw "OpenNova host shutdown PID differs from launch proof/acceptance: case=$caseId topology=$topology"
            }
            $hostShutdownWitness = Get-ValidatedOpenNovaHostShutdownWitness `
                -Witness $summary.opennova_host_shutdown_witness `
                -ExpectedRunRoot $expectedRunRoot `
                -ExpectedRunId $runId `
                -ExpectedTopology $topology `
                -ExpectedPid $expectedHostPid `
                -RuntimeStarted (ConvertFrom-StrictUtcIsoTimestamp `
                    ([string] $hostLaunchProof.runtime_start_utc)) `
                -SteadyCompleted $steadyCompleted
            $run | Add-Member -NotePropertyName opennova_host_shutdown_witness `
                -NotePropertyValue $hostShutdownWitness -Force
            $run | Add-Member -NotePropertyName opennova_host_exact_fallback_required `
                -NotePropertyValue $false -Force
        }
        else {
            if ($summary.PSObject.Properties.Name -contains
                    "opennova_host_exact_fallback_required" -or
                    $summary.PSObject.Properties.Name -contains
                    "opennova_host_shutdown_witness") {
                throw "Unexpected OpenNova host shutdown evidence: case=$caseId topology=$topology"
            }
            if ($run.PSObject.Properties.Name -contains "opennova_host_shutdown_witness" -or
                    $run.PSObject.Properties.Name -contains "opennova_host_exact_fallback_required") {
                throw "Template predeclares OpenNova host shutdown evidence for topology $topology"
            }
        }
        $expectedConfigRoles = switch ($topology) {
            "RR" { @("client", "server") }
            "RO" { @("server") }
            "OR" { @("client") }
            default { @() }
        }
        $summaryConfigs = @($summary.effective_retail_configs)
        $actualConfigRoles = @($summaryConfigs | ForEach-Object { [string] $_.role } | Sort-Object)
        if (($actualConfigRoles -join ',') -ne (@($expectedConfigRoles | Sort-Object) -join ',')) {
            throw "Effective retail cfg role set drift: case=$caseId topology=$topology"
        }
        $manifestConfigs = @()
        foreach ($config in $summaryConfigs) {
            $role = [string] $config.role
            $configPath = (Resolve-Path -LiteralPath ([string] $config.path)).Path
            $expectedConfigPath = (Resolve-Path -LiteralPath (
                Join-Path $expectedRunRoot "effective-$role.onhook.cfg")).Path
            if ($configPath -ne $expectedConfigPath -or
                    [string] $config.sha256 -ne (Get-Sha256 $configPath)) {
                throw "Effective retail cfg ownership/hash drift: case=$caseId topology=$topology role=$role"
            }
            $templateRole = "${role}_cfg_template"
            $templatePathForRole = [string] $artifactPaths[$templateRole]
            $templateBase = Get-OnHookConfigBase -Path $templatePathForRole
            $effectiveBase = Get-OnHookConfigBase -Path $configPath
            if ([string] $effectiveBase.Sha256 -ne [string] $templateBase.Sha256 -or
                    [string] $effectiveBase.Canonical -ne [string] $templateBase.Canonical) {
                throw "Effective retail cfg fixed settings differ from the tracked template: case=$caseId topology=$topology role=$role"
            }
            $cfgText = Get-Content -LiteralPath $configPath -Raw
            $requiredSettings = if ($role -eq "server") {
                @{
                    LanHostMission = [string] $case.mission
                    LanHostPort = [string] $case.port
                    LanHostCallsign = [string] $case.host_callsign
                    LanHostGameType = [string] $case.game_type
                    LanHostMaxPlayers = "4"
                    OpenNovaExpansion = [string] $manifest.corpus.expansion
                }
            } else {
                @{
                    LanJoinAddress = "127.0.0.1"
                    LanJoinPort = [string] $case.port
                    LanJoinCallsign = [string] $case.joiner_callsign
                    OpenNovaExpansion = [string] $manifest.corpus.expansion
                }
            }
            foreach ($key in $requiredSettings.Keys) {
                $pattern = "(?m)^\s*$([regex]::Escape($key))\s+$([regex]::Escape($requiredSettings[$key]))\s*$"
                if ([regex]::Matches($cfgText, $pattern).Count -ne 1) {
                    throw "Effective retail cfg lacks one exact $key setting: case=$caseId topology=$topology role=$role"
                }
            }
            $manifestConfigs += [pscustomobject]@{
                role = $role
                path = Get-RepoRelative $configPath
                sha256 = Get-Sha256 $configPath
                template_role = $templateRole
                base_sha256 = [string] $effectiveBase.Sha256
            }
        }
        $run | Add-Member -NotePropertyName effective_configs `
            -NotePropertyValue @($manifestConfigs) -Force
        $observedRunIds.Add($runId)
		$joinerReadinessWitness = $null
		if ($topology -in @('RO', 'OO')) {
			$joinerReadinessWitness = New-OpenNovaJoinerReadinessWitness `
				-Initial $summary.acceptance.opennova_joiner_initial `
				-Final $summary.acceptance.opennova_joiner `
				-ReadinessMode $readinessMode `
				-AllowPostDeathRedeploy:($readinessMode -eq 'in_match') `
				-RequireMotionComplete
			if ($null -eq $joinerReadinessWitness) {
				throw "OpenNova joiner readiness transition is invalid: case=$caseId topology=$topology"
			}
			$run | Add-Member -NotePropertyName `
				opennova_joiner_readiness_witness `
				-NotePropertyValue $joinerReadinessWitness -Force
		}
        switch ($topology) {
            "RR" {
                Assert-RetailLifecycleState -State $summary.acceptance.host `
                    -Role host -ExpectedPort ([int] $case.port) -Mode $readinessMode
                Assert-RetailLifecycleState -State $summary.acceptance.joiner `
                    -Role joiner -ExpectedPort ([int] $case.port) -Mode $readinessMode
            }
            "RO" {
                Assert-RetailLifecycleState `
                    -State $summary.acceptance.retail_host.instance.run_state `
                    -Role host -ExpectedPort ([int] $case.port) -Mode $readinessMode
                $joiner = $summary.acceptance.opennova_joiner
                if ([string] $joiner.readiness_mode -ne $readinessMode -or
                        [int] $joiner.process_id -le 0 -or
                        -not [bool] $joiner.motion_complete) {
                    throw "RO OpenNova joiner readiness metadata is invalid: case=$caseId"
                }
                if ($readinessMode -eq "in_match" -and
                        [string] $joinerReadinessWitness.final_class -notin
							@('in_match', 'post_death_redeploy')) {
                    throw "RO OpenNova joiner did not retain a healthy admitted state: case=$caseId"
                }
                if ($readinessMode -eq "deploy_hold" -and
						[string] $joinerReadinessWitness.final_class -cne
							'deploy_hold') {
                    throw "RO OpenNova joiner did not prove player-paced deploy hold: case=$caseId"
                }
            }
            "OR" {
                Assert-RetailLifecycleState -State $summary.acceptance.retail_joiner `
                    -Role joiner -ExpectedPort ([int] $case.port) -Mode $readinessMode
                if (-not [bool] $summary.acceptance.opennova_host.discovery_ready -or
                        [int] $summary.acceptance.opennova_host.port_ownership.pid -ne
                            [int] $summary.acceptance.opennova_host.pid -or
                        [int] $summary.acceptance.opennova_host.port_ownership.port -ne
                            [int] $case.port) {
                    throw "OR did not prove exact OpenNova host ownership: case=$caseId"
                }
            }
        }
        $expectsMotionGate = [bool] $case.exercise_input -and
            $topology -in @("RO", "OO")
        $motionGateCandidate = Join-Path $expectedRunRoot "joiner\motion-gate.json"
        if ($expectsMotionGate) {
            if (-not $summary.motion_gate_witness) {
                throw "Missing steady-window motion gate: case=$caseId topology=$topology"
            }
            $joinerAcceptance = $summary.acceptance.opennova_joiner
            $expectedJoinerPid = [int] $joinerAcceptance.process_id
            $motionGate = Get-ValidatedMotionGateWitness `
                -Witness $summary.motion_gate_witness `
                -ExpectedRunRoot $expectedRunRoot `
                -ExpectedRunId $runId `
                -ExpectedTopology $topology `
                -ExpectedJoinerPid $expectedJoinerPid `
                -SteadyStarted $steadyStarted `
                -SteadyCompleted $steadyCompleted
            if (-not [bool] $joinerAcceptance.exercise_motion -or
                    -not [bool] $joinerAcceptance.motion_gate_required -or
                    -not [bool] $joinerAcceptance.motion_gate_observed -or
                    [string] $joinerAcceptance.motion_gate_run_id -ne $runId -or
                    [string] $joinerAcceptance.motion_gate_created_utc -ne
                        [string] $motionGate.created_utc -or
                    [string] $joinerAcceptance.motion_gate_steady_started_utc -ne
                        [string] $motionGate.steady_started_utc -or
                    -not [bool] $joinerAcceptance.motion_complete -or
                    [int64] $joinerAcceptance.motion_started_ticks_msec -le 0 -or
                    [int64] $joinerAcceptance.motion_completed_ticks_msec -lt
                        [int64] $joinerAcceptance.motion_started_ticks_msec -or
                    [int64] $joinerAcceptance.ticks_msec -lt
                        [int64] $joinerAcceptance.motion_completed_ticks_msec -or
                    [int] $joinerAcceptance.motion_pre_roll_ms -ne 250 -or
                    [int] $joinerAcceptance.motion_inter_phase_gap_ms -ne 250 -or
                    [int] $joinerAcceptance.look_samples_per_phase -ne 20) {
                throw "OpenNova motion did not follow its steady-window gate: case=$caseId topology=$topology"
            }
            $run | Add-Member -NotePropertyName motion_gate_witness `
                -NotePropertyValue $motionGate -Force
        }
        else {
            if ($summary.motion_gate_witness -or
                    (Test-Path -LiteralPath $motionGateCandidate)) {
                throw "Unexpected motion gate for non-gated run: case=$caseId topology=$topology"
            }
            if ($run.PSObject.Properties.Name -contains "motion_gate_witness") {
                throw "Template predeclares a motion gate for non-gated run: case=$caseId topology=$topology"
            }
            if ($topology -in @("RO", "OO")) {
                $joinerAcceptance = $summary.acceptance.opennova_joiner
                if ([bool] $joinerAcceptance.exercise_motion -or
                        [bool] $joinerAcceptance.motion_gate_required -or
                        [bool] $joinerAcceptance.motion_gate_observed -or
                        -not [string]::IsNullOrEmpty(
                            [string] $joinerAcceptance.motion_gate_run_id) -or
                        -not [string]::IsNullOrEmpty(
                            [string] $joinerAcceptance.motion_gate_created_utc) -or
                        [int64] $joinerAcceptance.motion_started_ticks_msec -ne 0 -or
                        [int64] $joinerAcceptance.motion_completed_ticks_msec -ne 0) {
                    throw "Non-exercised OpenNova joiner was not gate-free: case=$caseId topology=$topology"
                }
            }
        }
        $retailInputCandidate = Join-Path $expectedRunRoot "retail-input.json"
        if ([bool] $case.exercise_input -and $topology -in @("RR", "OR")) {
            if (-not $summary.retail_input_witness) {
                throw "Missing completed retail input witness: case=$caseId topology=$topology"
            }
            $expectedInputPid = if ($topology -eq "RR") {
                [int] $summary.acceptance.joiner_pid
            } else {
                [int] $summary.acceptance.retail_pid
            }
            $retailInputWitness = Get-ValidatedRetailInputWitness `
                -Witness $summary.retail_input_witness `
                -ExpectedRunRoot $expectedRunRoot `
                -ExpectedRunId $runId `
                -ExpectedTopology $topology `
                -ExpectedPid $expectedInputPid `
                -SteadyStarted $steadyStarted `
                -SteadyCompleted $steadyCompleted
            $run | Add-Member -NotePropertyName retail_input_witness `
                -NotePropertyValue $retailInputWitness -Force
        }
        else {
            if ($summary.retail_input_witness -or
                    (Test-Path -LiteralPath $retailInputCandidate)) {
                throw "Unexpected retail input witness for topology ${topology}: case=$caseId"
            }
            if ($run.PSObject.Properties.Name -contains "retail_input_witness") {
                throw "Template predeclares a retail input witness for a non-retail-input run: case=$caseId topology=$topology"
            }
        }
        foreach ($generated in ([ordered]@{
                capture=(Get-RepoRelative $capture)
                sha256=(Get-Sha256 $capture)
                corpus=$fingerprint
                mission_path=(Get-RepoRelative $missionPath)
                mission_sha256=$missionHash
                game_type=[int64]$case.game_type
            }).GetEnumerator()) {
            $run | Add-Member -NotePropertyName $generated.Key `
                -NotePropertyValue $generated.Value -Force
        }
        $run | Add-Member -NotePropertyName external_capture `
            -NotePropertyValue (Get-RepoRelative $wireCapture) -Force
        $run | Add-Member -NotePropertyName external_capture_sha256 `
            -NotePropertyValue (Get-Sha256 $wireCapture) -Force
        $run | Add-Member -NotePropertyName external_capture_proof `
            -NotePropertyValue ([pscustomobject]@{
                artifact_sha256 = Get-Sha256 $wireCapture
                valid = [bool] $wireCaptureProof.validation.valid
                exact_eof = [bool] $wireCaptureProof.validation.exact_eof
                block_count = [int] $wireCaptureProof.validation.block_count
                packet_block_count = [int] $wireCaptureProof.validation.packet_block_count
                tail_marker_required = [bool] $wireCaptureProof.validation.tail_marker_required
                tail_marker_found = [bool] $wireCaptureProof.validation.tail_marker_found
            }) -Force
        if ($topology -eq "OO") {
            # The tracked external dumpcap wrapper is decoded end to end by the
            # verifier. It has no hook-side async counters.
            $externalCapture = (Resolve-Path -LiteralPath ([string] $summary.external_capture)).Path
            $ooJoiner = $summary.acceptance.opennova_joiner
			$ooJoinerReady = [string] $joinerReadinessWitness.final_class -in
				@('in_match', 'post_death_redeploy', 'deploy_hold')
            if ($externalCapture -ne $capture -or
                    -not [bool] $summary.acceptance.opennova_host.discovery_ready -or
                    [int] $summary.acceptance.opennova_host.port_ownership.pid -ne
                        [int] $summary.acceptance.opennova_host.pid -or
                    [int] $summary.acceptance.opennova_host.port_ownership.port -ne
                        [int] $case.port -or
                    [string] $ooJoiner.readiness_mode -ne $readinessMode -or
                    -not $ooJoinerReady -or -not [bool] $ooJoiner.motion_complete) {
                throw "OO lacks a joined endpoint or canonical external capture: case=$caseId"
            }
            $proof = $summary.external_capture_proof
            $proofArtifactPath = (Resolve-Path -LiteralPath ([string] $proof.artifact_path)).Path
            $declaredMarkerHex = [string] $proof.tail_marker.hex
            $markerHex = $declaredMarkerHex.ToLowerInvariant()
            $markerText = Get-ValidatedPcapTailMarkerText `
                -Hex $declaredMarkerHex -ExpectedRunId $runId
            $markerSha256 = Get-HexSha256 $markerHex
            $markerSent = ConvertFrom-StrictUtcIsoTimestamp ([string] $proof.tail_marker.sent_utc)
            $joinerShutdownCompleted = ConvertFrom-StrictUtcIsoTimestamp `
                ([string] $run.opennova_joiner_shutdown_witness.completed_utc)
            $hostShutdownCompleted = ConvertFrom-StrictUtcIsoTimestamp `
                ([string] $run.opennova_host_shutdown_witness.completed_utc)
            if ($proofArtifactPath -ne $capture -or
                    [string] $proof.artifact_sha256 -ne (Get-Sha256 $capture) -or
                    -not [bool] $proof.validation.valid -or
                    -not [bool] $proof.validation.exact_eof -or
                    [int] $proof.validation.packet_block_count -le 0 -or
                    -not [bool] $proof.validation.tail_marker_required -or
                    -not [bool] $proof.validation.tail_marker_found -or
                    [string] $proof.tail_marker.sha256 -ne $markerSha256 -or
                    [string] $proof.validation.tail_marker_sha256 -ne $markerSha256 -or
                    $markerSent -lt $steadyCompleted -or
                    $markerSent -lt $joinerShutdownCompleted -or
                    $markerSent -lt $hostShutdownCompleted) {
                throw "OO pcapng completion/tail proof is invalid: case=$caseId"
            }
            $run | Add-Member -NotePropertyName external_capture_proof `
                -NotePropertyValue ([pscustomobject]@{
                    artifact_sha256 = Get-Sha256 $capture
                    valid = [bool] $proof.validation.valid
                    exact_eof = [bool] $proof.validation.exact_eof
                    block_count = [int] $proof.validation.block_count
                    packet_block_count = [int] $proof.validation.packet_block_count
                    tail_marker_required = [bool] $proof.validation.tail_marker_required
                    tail_marker_hex = $markerHex
                    tail_marker_text = $markerText
                    tail_marker_sha256 = $markerSha256
                    tail_marker_sent_utc = [string] $proof.tail_marker.sent_utc
                    tail_marker_found = [bool] $proof.validation.tail_marker_found
                }) -Force
            $run | Add-Member -NotePropertyName capture_complete `
                -NotePropertyValue $true -Force
            $run | Add-Member -NotePropertyName capture_stats_available `
                -NotePropertyValue $false -Force
        }
        else {
            if ([string]::IsNullOrWhiteSpace([string] $summary.evidence_instance_id)) {
                throw "Hook evidence has no owning instance: case=$caseId topology=$topology"
            }
            $completed = @($summary.stop_evidence | Where-Object {
                -not [bool] $_.is_error -and $_.detail -and
                [bool] $_.detail.capture_complete -and $_.detail.final_capture -and
                [string] $_.detail.instance_id -eq [string] $summary.evidence_instance_id
            })
            $completed = @($completed | Where-Object {
                -not $_.detail.capture_path -or
                (Resolve-Path -LiteralPath ([string] $_.detail.capture_path)).Path -eq $capture
            })
            if ($completed.Count -ne 1) {
                throw "Expected one clean hook stop for case=$caseId topology=$topology; found $($completed.Count)"
            }
            $final = $completed[0].detail.final_capture
            if (-not [bool] $final.clean_stop) {
                throw "Hook capture did not report a clean stop: case=$caseId topology=$topology"
            }
            $counters = $final.capture_counters
            foreach ($counterBinding in ([ordered]@{
                    capture_complete=[bool]$completed[0].detail.capture_complete
                    capture_stats_available=$true
                    dropped=[int64]$counters.dropped
                    truncated=[int64]$counters.truncated
                    write_errors=[int64]$counters.write_errors
                    unsupported_async=[int64]$counters.unsupported_async
                }).GetEnumerator()) {
                $run | Add-Member -NotePropertyName $counterBinding.Key `
                    -NotePropertyValue $counterBinding.Value -Force
            }
            if ($run.dropped -ne 0 -or $run.truncated -ne 0 -or
                $run.write_errors -ne 0 -or $run.unsupported_async -ne 0) {
                throw "Nonzero hook capture counter: case=$caseId topology=$topology"
            }
        }
    }
}

$declaredRunIds = @($suiteProvenance.run_ids | ForEach-Object { [string] $_ } | Sort-Object)
$actualRunIds = @($observedRunIds.ToArray() | Sort-Object)
if ($declaredRunIds.Count -ne $actualRunIds.Count -or
        ($declaredRunIds -join "`n") -ne ($actualRunIds -join "`n")) {
    throw "Suite provenance run set does not exactly match the $($templateCases.Count * 4)-cell matrix."
}

$outputParent = Split-Path -Parent $outputPath
New-Item -ItemType Directory -Path $outputParent -Force | Out-Null
$manifest | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $outputPath -Encoding UTF8
Write-Host "PARITY_MANIFEST=$outputPath"
Write-Host "PARITY_CORPUS_FINGERPRINT=$fingerprint"
Write-Host "PARITY_SOURCE_STATE=$($repositorySourceState.sha256)"
