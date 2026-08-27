[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("RR", "RO", "OR", "OO")]
    [string] $Topology,

    [Parameter(Mandatory = $true)]
    [string] $Mission,

    [Parameter(Mandatory = $true)]
    [ValidateRange(32786, 32789)]
    [int] $Port,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^(?:auto|[0-9]+)$')]
    [string] $GameType,

    [Parameter(Mandatory = $true)]
    [string] $HostSessionName,

    [Parameter(Mandatory = $true)]
    [ValidateSet("in_match", "deploy_hold")]
    [string] $ReadinessMode,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9](?:[A-Za-z0-9._-]{0,54}[A-Za-z0-9])?$')]
    [string] $RunId,

    [ValidateRange(5, 120)]
    [int] $SteadySeconds = 15,

    [switch] $WireOnly,

    [switch] $AutoDeploy,

    [switch] $ExerciseInput,

    [ValidatePattern('^[1-9][0-9]{2,4}x[1-9][0-9]{2,4}$')]
    [string] $Resolution = "1920x1080",

    [string] $HostCallsign = "ParityHost",
    [string] $JoinerCallsign = "ParityJoin",

    [string] $SuiteId = "",
    [string] $CorpusFingerprint = "",
    [string] $MissionArtifactPath = "",
    [string] $MissionSha256 = "",

    [Parameter(Mandatory = $true)]
    [string] $RetailServerRoot,

    [Parameter(Mandatory = $true)]
    [string] $RetailClientRoot,

    [Parameter(Mandatory = $true)]
    [string] $OnHookMcpPath,

    [Parameter(Mandatory = $true)]
    [string] $GodotLauncher,

    [ValidateRange(1, 65535)]
    [int] $HostMcpPort = 8975,

    [ValidateRange(1, 65535)]
    [int] $JoinerMcpPort = 8976,

    # Diagnostic only: accept OpenNova launches through the plain runtime
    # executable (no console-wrapper job). A verdict-bearing run needs the
    # wrapper's ownership proof; the summary records the relaxation.
    [switch] $AllowDirectRuntime
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
if ([string]::IsNullOrWhiteSpace($HostSessionName)) {
    throw "HostSessionName must contain at least one non-whitespace character."
}
if ($HostSessionName.Length -gt 31 -or $HostSessionName -notmatch '^[\x20-\x7e]+$') {
    throw "HostSessionName must be at most 31 printable ASCII characters."
}
$ServerName = $HostSessionName
if (-not [string]::Equals(
        $ServerName, "Untitled ", [System.StringComparison]::Ordinal)) {
    throw "Verdict-bearing retail parity runs require exact server_name 'Untitled ' (including trailing space)."
}
if ($ReadinessMode -eq "deploy_hold" -and ($AutoDeploy -or $ExerciseInput)) {
    throw "deploy_hold readiness cannot auto-deploy or exercise input."
}
if ($WireOnly -and $Topology -ne "OR") {
    throw "WireOnly is an OR-only diagnostic mode."
}
if ($HostMcpPort -eq $JoinerMcpPort) {
    throw "HostMcpPort and JoinerMcpPort must differ."
}
if (-not $WireOnly) {
    [uint32] $wireGameType = 0
    if ($GameType -eq "auto" -or
            -not [uint32]::TryParse($GameType, [ref] $wireGameType)) {
        throw "Verdict-bearing parity runs require an explicit UInt32 GameType; zero is valid."
    }
}
if ($CorpusFingerprint -and $CorpusFingerprint -notmatch '^[0-9a-f]{64}$') {
    throw "CorpusFingerprint must be a lowercase SHA-256 when supplied."
}
if (($SuiteId -and -not $CorpusFingerprint) -or
        ($CorpusFingerprint -and -not $SuiteId)) {
    throw "SuiteId and CorpusFingerprint must be supplied together."
}
if ($MissionSha256 -and $MissionSha256 -notmatch '^[0-9a-f]{64}$') {
    throw "MissionSha256 must be a lowercase SHA-256 when supplied."
}
if (($MissionArtifactPath -and -not $MissionSha256) -or
        ($MissionSha256 -and -not $MissionArtifactPath)) {
    throw "MissionArtifactPath and MissionSha256 must be supplied together."
}
if ($SuiteId -and -not $MissionArtifactPath) {
    throw "A suite-bound run must bind its exact mission artifact."
}
$ResolvedMissionArtifact = ""
if ($MissionArtifactPath) {
    $ResolvedMissionArtifact = (Resolve-Path -LiteralPath $MissionArtifactPath).Path
    if ([System.IO.Path]::GetFileName($ResolvedMissionArtifact) -cne $Mission) {
        throw "Mission artifact leaf does not match Mission: $ResolvedMissionArtifact"
    }
    $actualMissionHash = (Get-FileHash -LiteralPath $ResolvedMissionArtifact `
        -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualMissionHash -ne $MissionSha256) {
        throw "Mission artifact SHA-256 drift: $ResolvedMissionArtifact"
    }
}

$Repo = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$ServerDir = (Resolve-Path -LiteralPath $RetailServerRoot).Path
$ClientDir = (Resolve-Path -LiteralPath $RetailClientRoot).Path
$McpExe = (Resolve-Path -LiteralPath $OnHookMcpPath).Path
$GodotLauncher = (Resolve-Path -LiteralPath $GodotLauncher).Path
if (-not (Test-Path -LiteralPath $ServerDir -PathType Container) -or
        -not (Test-Path -LiteralPath $ClientDir -PathType Container)) {
    throw "RetailServerRoot and RetailClientRoot must both be directories."
}
if (-not (Test-Path -LiteralPath $McpExe -PathType Leaf) -or
        -not (Test-Path -LiteralPath $GodotLauncher -PathType Leaf)) {
    throw "OnHookMcpPath and GodotLauncher must both be files."
}
if ([string]::Equals(
        $ServerDir, $ClientDir, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "RetailServerRoot and RetailClientRoot must be distinct installations."
}
$env:GODOT_BIN = $GodotLauncher
$Expansion = "revx02"
$RunRoot = Join-Path $Repo ".scratch\runs\$RunId"
$CaptureScript = Join-Path $Repo "scripts\net\capture.ps1"
$HostScript = Join-Path $Repo "scripts\net\host_opennova.ps1"
$JoinScript = Join-Path $Repo "scripts\net\join_opennova.ps1"
$NetLib = Join-Path $Repo "scripts\net\lib.ps1"
$ProbeExe = Join-Path $Repo "build\apps\nw_lan_probe\Release\nw-lan-probe.exe"
$InputExerciseScript = Join-Path $Repo "scripts\net\exercise_retail_input.ps1"
$WireReadyScript = Join-Path $Repo "scripts\net\wait_parity_wire_ready.ps1"

foreach ($required in @(
    $ServerDir, $ClientDir, $McpExe, $GodotLauncher, $CaptureScript, $HostScript,
    $JoinScript, $NetLib, $ProbeExe, $InputExerciseScript,
    $WireReadyScript
)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Required parity input is missing: $required"
    }
}
. $NetLib
if (Test-Path -LiteralPath $RunRoot) {
    throw "Run output already exists; choose a create-new RunId: $RunRoot"
}
New-Item -ItemType Directory -Path $RunRoot | Out-Null

$script:Mcp = $null
$script:McpRequestId = 0
$script:CaptureStarted = $false
$script:CaptureTag = "$RunId-external"
$script:OpenNovaHost = $null
$script:OpenNovaJoiner = $null
$script:OpenNovaLaunchProofs = [ordered]@{}
$script:RetailProcess = $null
$script:RetailHostProcess = $null
$script:RetailJoinerProcess = $null
$script:RetailInstanceId = ""
$script:RetailInstanceIds = [System.Collections.Generic.List[string]]::new()
$script:OwnedRetailRuns = [System.Collections.Generic.List[string]]::new()
$script:ExternalCapture = ""
$script:EvidenceCapture = ""
$script:EvidencePerspective = ""
$script:EvidenceRole = ""
$script:EvidenceInstanceId = ""
$script:Screenshot = ""
$script:Acceptance = $null
$script:StopEvidence = @()
$script:CleanupFailure = ""
$script:InputWitness = $null
$script:EffectiveRetailConfigs = [System.Collections.Generic.List[object]]::new()
$script:SteadyStartedUtc = ""
$script:SteadyCompletedUtc = ""
$script:OpenNovaHostStartOwnership = $null
$script:CaptureTailMarker = $null
$script:ExternalCaptureProof = $null
$script:MotionGateWitness = $null
$script:WireReadyWitness = $null
$script:JoinerReadyRunId = ""
$script:JoinerMotionRunId = ""
$script:OpenNovaJoinerShutdownWitness = $null
$script:OpenNovaJoinerCooperativeStopAttempted = $false
$script:OpenNovaJoinerExactFallbackRequired = $false
$script:OpenNovaHostShutdownWitness = $null
$script:OpenNovaHostExactFallbackRequired = $false

function Start-OnHookMcp {
    $start = [System.Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $McpExe
    $start.WorkingDirectory = Split-Path -Parent $McpExe
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardInput = $true
    $start.RedirectStandardOutput = $true
    $script:Mcp = [System.Diagnostics.Process]::Start($start)
    if (-not $script:Mcp) { throw "Could not launch onhook-mcp.exe" }

    $null = Invoke-OnHookRpc -Method "initialize" -Params @{
        protocolVersion = "2025-03-26"
        capabilities = @{}
        clientInfo = @{ name = "opennova-parity-harness"; version = "1" }
    }
}

function Invoke-OnHookRpc {
    param(
        [Parameter(Mandatory = $true)] [string] $Method,
        [Parameter(Mandatory = $true)] [hashtable] $Params
    )
    if (-not $script:Mcp -or $script:Mcp.HasExited) {
        throw "onhook MCP process is not running"
    }
    $script:McpRequestId++
    $request = @{
        jsonrpc = "2.0"
        id = $script:McpRequestId
        method = $Method
        params = $Params
    } | ConvertTo-Json -Depth 20 -Compress
    $script:Mcp.StandardInput.WriteLine($request)
    $script:Mcp.StandardInput.Flush()
    $line = $script:Mcp.StandardOutput.ReadLine()
    if ([string]::IsNullOrWhiteSpace($line)) {
        throw "onhook MCP returned no JSON-RPC response"
    }
    $response = $line | ConvertFrom-Json
    if ($response.PSObject.Properties.Name -contains "error") {
        throw "onhook MCP JSON-RPC error: $($response.error | ConvertTo-Json -Compress)"
    }
    return $response.result
}

function Invoke-OnHookTool {
    param(
        [Parameter(Mandatory = $true)] [string] $Name,
        [hashtable] $Arguments = @{},
        [switch] $AllowError
    )
    $result = Invoke-OnHookRpc -Method "tools/call" -Params @{
        name = $Name
        arguments = $Arguments
    }
    if ($result.isError -and -not $AllowError) {
        $message = @($result.content | Where-Object { $_.type -eq "text" } |
            ForEach-Object { $_.text }) -join "`n"
        throw "$Name failed: $message"
    }
    return $result
}

function Stop-ExactProcess {
    param([System.Diagnostics.Process] $Process)
    if (-not $Process) { return }
    if ($Process.PSObject.Properties['OpenNovaLaunchProof']) {
        # The official console wrapper owns a KILL_ON_JOB_CLOSE job. Preserve
        # that lifecycle contract even when its proven runtime has exited.
        Stop-OpenNovaProcess -Process $Process
        return
    }
    try {
        $Process.Refresh()
        if ($Process.HasExited) { return }
        $null = $Process.CloseMainWindow()
        if (-not $Process.WaitForExit(8000)) {
            Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
            $null = $Process.WaitForExit(5000)
        }
    }
    catch {
        Write-Warning "Could not fully close exact PID $($Process.Id): $($_.Exception.Message)"
    }
}

function Add-CleanupFailure {
    param([Parameter(Mandatory = $true)] [string] $Message)
    if ($script:CleanupFailure) {
        $script:CleanupFailure += " | $Message"
    }
    else {
        $script:CleanupFailure = $Message
    }
    Write-Warning $Message
}

function Register-OpenNovaLaunchProof {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet("host", "joiner")]
        [string] $Role,
        [Parameter(Mandatory = $true)]
        [System.Diagnostics.Process] $Process
    )
    if ($script:OpenNovaLaunchProofs.Contains($Role)) {
        throw "Duplicate OpenNova launch proof for role '$Role'."
    }
    $proofProperty = $Process.PSObject.Properties['OpenNovaLaunchProof']
    $launcherProperty = $Process.PSObject.Properties['OpenNovaLauncherProcess']
    if (-not $proofProperty -or -not $proofProperty.Value -or
            -not $launcherProperty -or -not $launcherProperty.Value) {
        throw "OpenNova $Role process lacks centralized launcher ownership proof."
    }
    $proof = $proofProperty.Value
    if ([string] $proof.schema -cne 'opennova.godot-process-ownership.v1' -or
            (-not [bool] $proof.wrapper_used -and -not $AllowDirectRuntime) -or
            [int] $proof.runtime_pid -ne $Process.Id -or
            [int] $proof.launcher_pid -ne $launcherProperty.Value.Id -or
            ([bool] $proof.wrapper_used -and
                [int] $proof.runtime_parent_pid -ne [int] $proof.launcher_pid) -or
            -not [bool] $proof.parent_verified -or
            -not [bool] $proof.argv_verified -or
            -not [bool] $proof.active_execution_verified) {
        throw "OpenNova $Role process returned an invalid or direct-runtime launch proof."
    }
    # Break references to live Process objects. Only the serializable ownership
    # witness is retained for the create-new run summary and manifest.
    $snapshot = $proof | ConvertTo-Json -Depth 10 -Compress | ConvertFrom-Json
    $script:OpenNovaLaunchProofs.Add($Role, $snapshot)
}

function Assert-UdpPortUnowned {
    # Filtering the full table keeps an unbound port as an empty result. The
    # cmdlet's -LocalPort query reports that ordinary state as a terminating
    # CIM "not found" error under -ErrorAction Stop.
    $existing = @(Get-NetUDPEndpoint -ErrorAction Stop | Where-Object {
        [int] $_.LocalPort -eq $Port
    })
    if ($existing.Count -gt 0) {
        $owners = @($existing | ForEach-Object {
            "pid=$($_.OwningProcess) address=$($_.LocalAddress):$($_.LocalPort)"
        }) -join ", "
        throw "Parity port $Port was already bound before endpoint launch: $owners"
    }
}

function Assert-OpenNovaHostOwnsPort {
    if (-not $script:OpenNovaHost) {
        throw "Cannot prove UDP port ownership without an OpenNova host process"
    }
    $script:OpenNovaHost.Refresh()
    if ($script:OpenNovaHost.HasExited) {
        throw "OpenNova host exited before UDP port ownership could be proven"
    }
    $endpoints = @(Get-NetUDPEndpoint -ErrorAction Stop | Where-Object {
        [int] $_.LocalPort -eq $Port
    })
    $owned = @($endpoints | Where-Object {
        [int] $_.OwningProcess -eq $script:OpenNovaHost.Id
    })
    $unexpected = @($endpoints | Where-Object {
        [int] $_.OwningProcess -ne $script:OpenNovaHost.Id
    })
    if ($owned.Count -eq 0) {
        $observed = @($endpoints | ForEach-Object { [int] $_.OwningProcess }) -join ","
        throw "OpenNova host PID $($script:OpenNovaHost.Id) does not own UDP port $Port (observed PIDs: $observed)"
    }
    if ($unexpected.Count -gt 0) {
        $observed = @($unexpected | ForEach-Object { [int] $_.OwningProcess }) -join ","
        throw "Unexpected PID(s) also own OpenNova host UDP port ${Port}: $observed"
    }
    return [pscustomobject]@{
        pid = $script:OpenNovaHost.Id
        port = $Port
        endpoints = @($owned | ForEach-Object {
            [pscustomobject]@{
                local_address = [string] $_.LocalAddress
                local_port = [int] $_.LocalPort
            }
        })
    }
}

function Send-ExternalCaptureTailMarker {
    if ($Topology -ne "OO") {
        throw "External capture tail markers are reserved for OO evidence"
    }
    if ($script:CaptureTailMarker) {
        throw "External capture tail marker was already sent"
    }
    $markerText = "OPENNOVA-PCAPNG-TAIL|$RunId|$([Guid]::NewGuid().ToString('N'))"
    [byte[]] $markerBytes = [System.Text.Encoding]::UTF8.GetBytes($markerText)
    $udp = [System.Net.Sockets.UdpClient]::new(
        [System.Net.Sockets.AddressFamily]::InterNetwork)
    try {
        $sent = $udp.Send($markerBytes, $markerBytes.Length, "127.0.0.1", $Port)
    }
    finally {
        $udp.Dispose()
    }
    if ($sent -ne $markerBytes.Length) {
        throw "UDP tail marker send was short ($sent of $($markerBytes.Length) bytes)"
    }
    $markerHex = ([BitConverter]::ToString($markerBytes)).Replace('-', '').ToLowerInvariant()
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try { $markerDigest = $sha.ComputeHash($markerBytes) }
    finally { $sha.Dispose() }
    $script:CaptureTailMarker = [pscustomobject]@{
        destination = "127.0.0.1:$Port"
        bytes = $markerBytes.Length
        hex = $markerHex
        sha256 = ([BitConverter]::ToString($markerDigest)).Replace('-', '').ToLowerInvariant()
        sent_utc = [DateTime]::UtcNow.ToString("o")
        flush_wait_ms = 1000
    }
    # No endpoint remains alive after this datagram. Once its complete packet
    # block reaches disk, the capture contains every preceding endpoint packet.
    Start-Sleep -Milliseconds $script:CaptureTailMarker.flush_wait_ms
}

function Invoke-RetailInputExercise {
    param([Parameter(Mandatory = $true)] [int] $ProcessId)
    if (-not $ExerciseInput -or $Topology -notin @("RR", "OR")) {
        throw "Retail input witnesses are valid only for exercised RR/OR runs"
    }
    $path = Join-Path $RunRoot "retail-input.json"
    & powershell.exe -NoProfile -ExecutionPolicy Bypass `
        -File $InputExerciseScript -ProcessId $ProcessId `
        -Topology $Topology -RunId $RunId -Output $path
    if ($LASTEXITCODE -ne 0) {
        throw "Retail input exercise failed for exact PID ${ProcessId}: $LASTEXITCODE"
    }
    $detail = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
    $actualProperties = @($detail.PSObject.Properties.Name | Sort-Object)
    $expectedProperties = @(
        "schema", "complete", "run_id", "topology", "pid",
        "process_start_utc", "window_handle", "started_utc", "completed_utc",
        "forward_ms", "strafe_ms", "return_ms", "turn_pixels",
        "injected_events", "focus_attempts", "foreground_checks"
    ) | Sort-Object
    if (($actualProperties -join ',') -cne ($expectedProperties -join ',') -or
            [string] $detail.schema -cne "opennova.retail-input.v1" -or
            $detail.complete -isnot [bool] -or -not [bool] $detail.complete -or
            [string] $detail.run_id -cne $RunId -or
            [string] $detail.topology -cne $Topology -or
            [int] $detail.pid -ne $ProcessId -or
            [int64] $detail.window_handle -le 0 -or
            [int] $detail.forward_ms -ne 1800 -or
            [int] $detail.strafe_ms -ne 1200 -or
            [int] $detail.return_ms -ne 900 -or
            [int] $detail.turn_pixels -ne 320 -or
            [int] $detail.injected_events -ne 66 -or
            [int] $detail.focus_attempts -lt 3 -or
            [int] $detail.focus_attempts -gt 30 -or
            [int] $detail.foreground_checks -lt 195 -or
            [int] $detail.foreground_checks -gt 222) {
        throw "Retail input exercise emitted an invalid witness: $path"
    }
    $script:InputWitness = [pscustomobject]@{
        path = $path
        sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        pid = $ProcessId
        detail = $detail
    }
}

function Bind-EffectiveRetailConfig {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet("server", "client")]
        [string] $Role,
        [Parameter(Mandatory = $true)]
        [string] $Source
    )
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        throw "Effective retail $Role cfg is missing: $Source"
    }
    if (@($script:EffectiveRetailConfigs | Where-Object { $_.role -eq $Role }).Count -ne 0) {
        throw "Effective retail cfg role is already bound: $Role"
    }
    $owned = Join-Path $RunRoot "effective-$Role.onhook.cfg"
    if (Test-Path -LiteralPath $owned) {
        throw "Effective retail cfg snapshot must be create-new: $owned"
    }
    Copy-Item -LiteralPath $Source -Destination $owned
    $script:EffectiveRetailConfigs.Add([pscustomobject]@{
        role = $Role
        path = $owned
        sha256 = (Get-FileHash -LiteralPath $owned -Algorithm SHA256).Hash.ToLowerInvariant()
    })
}

function Begin-SteadyWindow {
    if ($script:SteadyStartedUtc) { throw "Steady window was already started" }
    $script:SteadyStartedUtc = [DateTime]::UtcNow.ToString("o")
}

function Publish-MotionGate {
    if (-not $ExerciseInput -or $Topology -notin @("RO", "OO")) {
        throw "Motion gates are valid only for exercised RO/OO runs"
    }
    if (-not $script:SteadyStartedUtc -or $script:SteadyCompletedUtc) {
        throw "Motion gate publication must immediately follow steady-window start"
    }
    if ($script:JoinerMotionRunId) {
        throw "Motion gate must be create-new for run $RunId"
    }
    $script:OpenNovaJoiner.Refresh()
    if ($script:OpenNovaJoiner.HasExited) {
        throw "OpenNova joiner exited before motion-gate publication"
    }
    $started = [DateTimeOffset]::Parse(
        $script:SteadyStartedUtc,
        [Globalization.CultureInfo]::InvariantCulture,
        [Globalization.DateTimeStyles]::RoundtripKind
    ).ToUniversalTime()
    $requestedUtc = [DateTimeOffset]::UtcNow
    if ($requestedUtc -lt $started -or ($requestedUtc - $started).TotalMilliseconds -gt 2000.0) {
        throw "Motion gate was not requested immediately after steady-window start"
    }
    # The exercise runs inside the joiner (walk/strafe/turn through its real
    # input path) and blocks until complete; its witness is the verdict data.
    $status = Invoke-GameProbe -Port $JoinerMcpPort -Name "parity_joiner_motion" -Process $script:OpenNovaJoiner `
        -TimeoutSeconds 60 -PollMs 500 -Arguments @{
            run_id = $RunId
            topology = $Topology
            steady_started_utc = $script:SteadyStartedUtc
            readiness_mode = $ReadinessMode
            auto_deploy = [bool] $AutoDeploy
        }
    $script:JoinerMotionRunId = [string] $status.run_id
    if ([string] $status.state -ne "passed" -or -not $status.verdict -or
            -not [bool] $status.verdict.ok) {
        $why = if ($status.verdict) { [string] $status.verdict.summary } else { [string] $status.error }
        throw "OpenNova joiner motion exercise did not complete: state=$($status.state) $why"
    }
    $witness = $status.verdict.data
    if (-not [bool] $witness.motion_complete -or -not [bool] $witness.motion_gate_observed -or
            [string] $witness.motion_gate_run_id -cne $RunId -or
            [string] $witness.motion_gate_steady_started_utc -cne $script:SteadyStartedUtc -or
            [int] $witness.process_id -ne $script:OpenNovaJoiner.Id) {
        throw "OpenNova joiner motion witness does not bind this run"
    }
    $created = ConvertFrom-RunnerStrictUtcTimestamp ([string] $witness.motion_gate_created_utc)
    if ($created -lt $started -or ($created - $started).TotalMilliseconds -gt 2000.0) {
        throw "Motion gate was not created immediately after steady-window start"
    }
    $script:MotionGateWitness = [pscustomobject]@{
        schema = "opennova.parity-motion-gate.v2"
        run_id = $RunId
        topology = $Topology
        joiner_pid = $script:OpenNovaJoiner.Id
        joiner_mcp_port = $JoinerMcpPort
        probe_run_id = $script:JoinerMotionRunId
        steady_started_utc = $script:SteadyStartedUtc
        created_utc = [string] $witness.motion_gate_created_utc
        pre_roll_ms = [int] $witness.motion_pre_roll_ms
        inter_phase_gap_ms = [int] $witness.motion_inter_phase_gap_ms
        look_samples_per_phase = [int] $witness.look_samples_per_phase
        started_ticks_msec = [int64] $witness.motion_started_ticks_msec
        completed_ticks_msec = [int64] $witness.motion_completed_ticks_msec
        forward_ms = 1800
        forward_look = 320
        strafe_ms = 1200
        strafe_look = -320
        return_ms = 900
        return_look = 160
        detail = ConvertTo-OpenNovaJoinerReadinessSnapshot $witness
    }
}

function Wait-UntilSteadyDeadline {
    if (-not $script:SteadyStartedUtc -or $script:SteadyCompletedUtc) {
        throw "Steady deadline wait is out of order"
    }
    $started = [DateTime]::Parse(
        $script:SteadyStartedUtc,
        [Globalization.CultureInfo]::InvariantCulture,
        [Globalization.DateTimeStyles]::RoundtripKind
    ).ToUniversalTime()
    $deadline = $started.AddSeconds($SteadySeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $remainingMs = [int][Math]::Ceiling(($deadline - [DateTime]::UtcNow).TotalMilliseconds)
        Start-Sleep -Milliseconds ([Math]::Min(250, [Math]::Max(1, $remainingMs)))
    }
}

function Complete-SteadyWindow {
    if (-not $script:SteadyStartedUtc -or $script:SteadyCompletedUtc) {
        throw "Steady window completion is out of order"
    }
    $script:SteadyCompletedUtc = [DateTime]::UtcNow.ToString("o")
}

function Stop-OwnedRetailRun {
    param([Parameter(Mandatory = $true)] [string] $OwnedRunId)
    $result = Invoke-OnHookTool -Name "onhook_stop_run" -Arguments @{
        run_id = $OwnedRunId
        wait_timeout_ms = 15000
    } -AllowError
    $structured = if ($result.PSObject.Properties.Name -contains "structuredContent") {
        $result.structuredContent
    } else { $null }
    $script:StopEvidence += [pscustomobject]@{
        run_id = $OwnedRunId
        is_error = [bool] $result.isError
        detail = $structured
    }
    if ($result.isError -or -not $structured -or
            -not [bool] $structured.capture_complete) {
        $message = @($result.content | Where-Object { $_.type -eq "text" } |
            ForEach-Object { $_.text }) -join "`n"
        throw "Retail run $OwnedRunId did not produce complete capture evidence: $message"
    }
    $null = $script:OwnedRetailRuns.Remove($OwnedRunId)
}

function Wait-RetailRunForPeer {
    param(
        [Parameter(Mandatory = $true)] [string] $OwnedRunId,
        [int] $TimeoutSeconds = 90
    )
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    do {
        $status = Get-StructuredResult (Invoke-OnHookTool -Name "onhook_run_status" -Arguments @{
            run_id = $OwnedRunId
        })
        $state = $status.instance.run_state
        if ($state.failure -ne "none") {
            throw "Retail run $OwnedRunId failed in phase $($state.phase): $($state.failure)"
        }
        if ($state.phase -eq "in_match" -and $state.peer -and
                $state.local_player -and
                $state.capture_counters.packets -ge 40) {
            return $status
        }
        Start-Sleep -Milliseconds 1000
    } while ((Get-Date) -lt $deadline)
    throw "Timed out waiting for retail run $OwnedRunId to observe a peer"
}

function Assert-RetailEndpointState {
    param(
        [Parameter(Mandatory = $true)] $Instance,
        [Parameter(Mandatory = $true)] [ValidateSet("host", "joiner")] [string] $Role,
        [switch] $RequireInMatch
    )
    $state = $Instance.run_state
    if ([string] $state.failure -ne "none") {
        throw "Retail $Role PID $($Instance.pid) failed in phase $($state.phase): $($state.failure)"
    }
    foreach ($counter in @("dropped", "truncated", "write_errors", "unsupported_async")) {
        if ([int64] $state.capture_counters.$counter -ne 0) {
            throw "Retail $Role PID $($Instance.pid) has nonzero capture counter ${counter}=$($state.capture_counters.$counter)"
        }
    }
    if (-not [bool] $state.active -or -not [bool] $state.peer -or
            [string] $state.capture_state -ne "recording" -or
            [int] $state.network_type -ne 2 -or
            [int] $state.requested_port -ne $Port -or
            [int64] $state.capture_counters.packets -lt 40) {
        return $false
    }
    if ($Role -eq "host") {
        if (-not [bool] $state.authority -or -not [bool] $state.responder_ready -or
                [int] $state.socket_mode -ne 3 -or [int] $state.connection_mode -ne 3 -or
                [int] $state.local_port -ne $Port) {
            return $false
        }
    }
    else {
        if ([bool] $state.authority -or [int] $state.socket_mode -ne 2 -or
                [int] $state.connection_mode -ne 2 -or [int] $state.local_port -le 0) {
            return $false
        }
    }
    if ($RequireInMatch -and
            ([string] $state.phase -ne "in_match" -or -not [bool] $state.local_player)) {
        return $false
    }
    if (-not $RequireInMatch -and $ReadinessMode -eq "deploy_hold" -and
            $Role -eq "joiner" -and
            ([string] $state.phase -ne "loading" -or [bool] $state.local_player)) {
        return $false
    }
    return $true
}

function Wait-RetailProcessForPeer {
    param(
        [Parameter(Mandatory = $true)] [int] $ProcessId,
        [Parameter(Mandatory = $true)] [ValidateSet("host", "joiner")] [string] $Role,
        [int] $TimeoutSeconds = 240,
        [switch] $RequireInMatch
    )
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    do {
        $ownedProcess = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
        if (-not $ownedProcess -or $ownedProcess.HasExited) {
            throw "Retail $Role PID $ProcessId exited before satisfying readiness_mode=$ReadinessMode"
        }
        $instances = Get-StructuredResult (Invoke-OnHookTool -Name "onhook_instances")
        $instance = @($instances.instances) |
            Where-Object { [int] $_.pid -eq $ProcessId } |
            Select-Object -First 1
        if ($instance) {
            $observedInstanceId = [string] $instance.instance_id
            if ($observedInstanceId -and
                    -not $script:RetailInstanceIds.Contains($observedInstanceId)) {
                $script:RetailInstanceIds.Add($observedInstanceId)
            }
            if (Assert-RetailEndpointState -Instance $instance -Role $Role `
                    -RequireInMatch:$RequireInMatch) {
                return $instance
            }
        }
        Start-Sleep -Milliseconds 1000
    } while ((Get-Date) -lt $deadline)
    throw "Timed out waiting for retail $Role PID $ProcessId to satisfy readiness_mode=$ReadinessMode"
}

function Assert-RetailHostOwnsPort {
    param([Parameter(Mandatory = $true)] [int] $ProcessId)
    $owners = @(Get-NetUDPEndpoint -LocalPort $Port -ErrorAction SilentlyContinue)
    if ($owners.Count -eq 0 -or
            @($owners | Where-Object { [int] $_.OwningProcess -ne $ProcessId }).Count -ne 0 -or
            @($owners | Where-Object { [int] $_.OwningProcess -eq $ProcessId }).Count -eq 0) {
        throw "Retail host PID $ProcessId is not the exclusive owner of UDP $Port"
    }
    return [pscustomobject]@{
        pid = $ProcessId
        port = $Port
        endpoints = @($owners | ForEach-Object {
            [pscustomobject]@{
                local_address = [string] $_.LocalAddress
                local_port = [int] $_.LocalPort
            }
        })
    }
}

function Stop-RetailProcessCapture {
    param(
        [Parameter(Mandatory = $true)] [string] $InstanceId,
        [Parameter(Mandatory = $true)] [string] $RunLabel
    )
    $result = Invoke-OnHookTool -Name "onhook_stop_capture" -Arguments @{
        instance_id = $InstanceId
    } -AllowError
    $structured = if ($result.PSObject.Properties.Name -contains "structuredContent") {
        $result.structuredContent
    } else { $null }
    $script:StopEvidence += [pscustomobject]@{
        run_id = $RunLabel
        is_error = [bool] $result.isError
        detail = $structured
    }
    if ($result.isError -or -not $structured -or
            -not [bool] $structured.capture_complete) {
        $message = @($result.content | Where-Object { $_.type -eq "text" } |
            ForEach-Object { $_.text }) -join "`n"
        throw "Retail capture $RunLabel did not drain cleanly: $message"
    }
    $null = $script:RetailInstanceIds.Remove($InstanceId)
}

# Retail is launched only through onhook-mcp. Its role tools must return the
# exact process (pid + instance_id) like onhook_run_lan_pair does and render
# the role's onhook.cfg from their arguments; until the upstream tools do,
# these cells stop with a named error instead of guessing at a process.
function Assert-OnHookRoleLaunch {
    param(
        [Parameter(Mandatory = $true)] [string] $Tool,
        [Parameter(Mandatory = $true)] $Result
    )
    foreach ($name in @("pid", "instance_id", "run_id", "capture_path")) {
        if (-not ($Result.PSObject.Properties.Name -contains $name) -or
                [string]::IsNullOrWhiteSpace([string] $Result.$name)) {
            throw "UPSTREAM BLOCKER (onhook-mcp): $Tool returned no '$name'; the role tools must return pid/instance_id/run_id/capture_path like onhook_run_lan_pair and render onhook.cfg from their arguments (TODO.md)."
        }
    }
    if ([int] $Result.pid -le 0) {
        throw "UPSTREAM BLOCKER (onhook-mcp): $Tool returned pid $($Result.pid)."
    }
}

function Start-RetailHostExact {
    $hostDir = Join-Path $RunRoot "host"
    New-Item -ItemType Directory -Path $hostDir -Force | Out-Null
    $result = Get-StructuredResult (Invoke-OnHookTool -Name "onhook_host_lan" -Arguments @{
        mission = $Mission
        game_dir = $ServerDir
        expansion = $Expansion
        port = $Port
        callsign = $HostCallsign
        game_type = [string] $GameType
        max_players = 4
        run_id = "$RunId-host"
        output_dir = $hostDir
        capture = $true
        windowed = $true
        allow_many = $true
        wait_timeout_ms = 240000
    })
    Assert-OnHookRoleLaunch -Tool "onhook_host_lan" -Result $result
    $script:OwnedRetailRuns.Add([string] $result.run_id)
    $script:RetailHostProcess = Get-Process -Id ([int] $result.pid) -ErrorAction Stop
    Bind-EffectiveRetailConfig -Role server -Source (Join-Path $ServerDir "onhook.cfg")
    $probeOutput = @(& $ProbeExe --host 127.0.0.1 --port $Port --timeout-ms 240000 `
        --interval-ms 1000 --expect-name $ServerName)
    if ($LASTEXITCODE -ne 0) {
        throw "Retail host PID $($script:RetailHostProcess.Id) did not answer matching LAN discovery: $($probeOutput -join ' | ')"
    }
    $probeOutput | ForEach-Object { Write-Host $_ }
    $ownership = Assert-RetailHostOwnsPort -ProcessId $script:RetailHostProcess.Id
    return [pscustomobject]@{
        process = $script:RetailHostProcess
        capture_path = [string] $result.capture_path
        port_ownership = $ownership
    }
}

function Start-RetailJoinerExact {
    $joinerDir = Join-Path $RunRoot "joiner"
    New-Item -ItemType Directory -Path $joinerDir -Force | Out-Null
    $result = Get-StructuredResult (Invoke-OnHookTool -Name "onhook_join_lan" -Arguments @{
        game_dir = $ClientDir
        expansion = $Expansion
        join_address = "127.0.0.1"
        port = $Port
        callsign = $JoinerCallsign
        run_id = "$RunId-joiner"
        output_dir = $joinerDir
        capture = $true
        windowed = $true
        allow_many = $true
        wait_timeout_ms = 240000
    })
    Assert-OnHookRoleLaunch -Tool "onhook_join_lan" -Result $result
    $script:OwnedRetailRuns.Add([string] $result.run_id)
    $script:RetailJoinerProcess = Get-Process -Id ([int] $result.pid) -ErrorAction Stop
    Bind-EffectiveRetailConfig -Role client -Source (Join-Path $ClientDir "onhook.cfg")
    $instance = Wait-RetailProcessForPeer -ProcessId $script:RetailJoinerProcess.Id `
        -Role joiner -RequireInMatch:($ReadinessMode -eq "in_match")
    $instanceId = [string] $instance.instance_id
    if (-not $script:RetailInstanceIds.Contains($instanceId)) {
        $script:RetailInstanceIds.Add($instanceId)
    }
    return [pscustomobject]@{
        process = $script:RetailJoinerProcess
        instance = $instance
        instance_id = $instanceId
        capture_path = [string] $result.capture_path
    }
}

function Wait-ParityWireReady {
    if (-not $ResolvedMissionArtifact) {
        throw "Wire readiness requires a hash-bound mission artifact and corpus."
    }
    $corpusRoot = Split-Path -Parent (Split-Path -Parent $ResolvedMissionArtifact)
    $itemsPath = Join-Path $corpusRoot "items.def"
    if (-not (Test-Path -LiteralPath $itemsPath -PathType Leaf)) {
        throw "Wire readiness items.def is missing: $itemsPath"
    }
    if (-not $script:EvidenceCapture) {
        throw "Wire readiness has no canonical perspective capture."
    }
    # onHook's in-process pcap writer holds traffic.pcap exclusively until
    # capture drain.  The simultaneously owned dumpcap stream is rolling and
    # snapshot-readable, so it is the pre-steady gate artifact.  Cleanup copies
    # that exact stream into the run root; the completed retail capture remains
    # the verdict-bearing perspective capture for RR/RO/OR comparisons.
    if (-not $script:ExternalCapture) {
        throw "Wire readiness has no rolling external capture."
    }
    $witnessPath = Join-Path $RunRoot "wire-ready.json"
    $arguments = @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $WireReadyScript,
        "-Capture", $script:ExternalCapture,
        "-Items", $itemsPath,
        "-Mission", $Mission,
        "-MissionArtifact", $ResolvedMissionArtifact,
        "-GameType", [string] $GameType,
        "-HostSessionName", $ServerName,
        "-HostCallsign", $HostCallsign,
        "-JoinerCallsign", $JoinerCallsign,
        "-Map", $Mission,
        "-Expansion", $Expansion,
        "-Output", $witnessPath,
        "-TimeoutSeconds", "240"
    )
    $output = @(& powershell.exe @arguments)
    if ($LASTEXITCODE -ne 0) {
        throw "wait_parity_wire_ready.ps1 failed: $($output -join ' | ')"
    }
    $line = @($output | Where-Object { "$_" -like "PARITY_WIRE_READY_JSON=*" } |
        Select-Object -Last 1)
    if ($line.Count -ne 1) {
        throw "wait_parity_wire_ready.ps1 returned no unique witness."
    }
    $script:WireReadyWitness = "$($line[0])".Substring(
        "PARITY_WIRE_READY_JSON=".Length) | ConvertFrom-Json
    if ([string] $script:WireReadyWitness.schema -ne
            "opennova.parity-wire-readiness.v2") {
        throw "Wire readiness witness schema drift."
    }
    return $script:WireReadyWitness
}

function Save-RetailScreenshot {
    param([Parameter(Mandatory = $true)] [string] $InstanceId)
    $path = Join-Path $RunRoot "retail-frame.png"
    $result = Get-StructuredResult (Invoke-OnHookTool -Name "onhook_screenshot" -Arguments @{
        instance_id = $InstanceId
        path = $path
        preview_max_dim = 256
    })
    $script:Screenshot = $result.path
    Add-Type -AssemblyName System.Drawing
    $image = [System.Drawing.Image]::FromFile($script:Screenshot)
    try {
        $actual = "$($image.Width)x$($image.Height)"
        if ($actual -ne $Resolution) {
            throw "Retail viewport $actual does not match pinned matrix resolution $Resolution"
        }
    }
    finally { $image.Dispose() }
}

function ConvertFrom-RunnerStrictUtcTimestamp {
    param([Parameter(Mandatory = $true)] [string] $Value)
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

function Assert-RunnerExactJsonProperties {
    param($Value, [string[]] $Expected, [string] $Context)
    if (-not $Value) { throw "$Context is missing" }
    $actual = @($Value.PSObject.Properties.Name | Sort-Object)
    $wanted = @($Expected | Sort-Object)
    if (($actual -join ',') -cne ($wanted -join ',')) {
        throw "$Context property set drift: actual=$($actual -join ',') expected=$($wanted -join ',')"
    }
}

function Stop-OpenNovaJoinerCooperatively {
    if ($Topology -notin @("RO", "OO")) {
        throw "Cooperative OpenNova joiner shutdown is valid only for RO/OO"
    }
    if ($script:OpenNovaJoinerShutdownWitness) {
        return $script:OpenNovaJoinerShutdownWitness
    }
    if (-not $script:OpenNovaJoiner) { return $null }
    if ($script:OpenNovaJoinerCooperativeStopAttempted) {
        throw "The cooperative OpenNova joiner shutdown attempt already failed"
    }
    $script:OpenNovaJoinerCooperativeStopAttempted = $true
    $process = $script:OpenNovaJoiner
    $process.Refresh()
    if ($process.HasExited) {
        throw "OpenNova joiner exited before a cooperative stop could be requested"
    }
    # Cancel any probe, ask the game to quit through its endpoint, and give it
    # 20 seconds; the process-shutdown evidence then proves the clean exit.
    $requestedUtc = [DateTime]::UtcNow.ToString("o")
    $quitAcknowledged = Stop-GameViaMcp -Port $JoinerMcpPort -Process $process -TimeoutSeconds 20
    if (-not $quitAcknowledged) {
        throw "OpenNova joiner did not exit within 20 seconds of game_control quit"
    }
    $shutdown = Stop-OpenNovaProcess -Process $process `
        -GracefulTimeoutMs 0 -PassThruShutdownEvidence -QuitRequested
    if ([bool] $shutdown.exact_fallback_required -or $null -eq $shutdown.exit_code -or
            [int] $shutdown.exit_code -ne 0) {
        throw "OpenNova joiner cooperative shutdown was not clean: exact_fallback_required=$($shutdown.exact_fallback_required) exit_code=$($shutdown.exit_code)"
    }
    $requested = ConvertFrom-RunnerStrictUtcTimestamp $requestedUtc
    $completed = ConvertFrom-RunnerStrictUtcTimestamp ([string] $shutdown.completed_utc)
    $processStarted = [DateTimeOffset]::new($process.StartTime.ToUniversalTime())
    if ($requested -lt $processStarted -or $completed -lt $requested -or
            $completed -gt $requested.AddSeconds(25)) {
        throw "OpenNova joiner shutdown timestamps drifted"
    }
    $script:OpenNovaJoinerShutdownWitness = [pscustomobject][ordered]@{
        schema = "opennova.parity-joiner-shutdown.v2"
        run_id = $RunId
        topology = $Topology
        process_id = [int] $process.Id
        mcp_port = $JoinerMcpPort
        requested_utc = $requestedUtc
        completed_utc = [string] $shutdown.completed_utc
        quit_acknowledged = $true
        exit_code = [int] $shutdown.exit_code
        clean = $true
    }
    return $script:OpenNovaJoinerShutdownWitness
}

function Stop-OpenNovaHostCleanly {
    if ($Topology -notin @("OR", "OO")) {
        throw "Clean OpenNova host shutdown proof is valid only for OR/OO"
    }
    if ($script:OpenNovaHostShutdownWitness) {
        return $script:OpenNovaHostShutdownWitness
    }
    if (-not $script:OpenNovaHost) {
        throw "OpenNova host is missing before clean shutdown proof"
    }

    $process = $script:OpenNovaHost
    $logPath = Join-Path $RunRoot "host\godot.log"
    # A cooperative quit through the host's endpoint first; the window close
    # inside Stop-OpenNovaProcess is the witnessed fallback.
    $hostQuit = Stop-GameViaMcp -Port $HostMcpPort -Process $process -TimeoutSeconds 20
    $shutdown = Stop-OpenNovaProcess -Process $process `
        -PassThruShutdownEvidence -RequireCleanExit -QuitRequested:$hostQuit
    Assert-RunnerExactJsonProperties $shutdown @(
        "schema", "process_id", "requested_utc", "completed_utc",
        "close_requested", "exact_fallback_required", "launcher_killed",
        "runtime_killed", "exit_code", "clean"
    ) "OpenNova host process shutdown evidence"
    $script:OpenNovaHostExactFallbackRequired =
        [bool] $shutdown.exact_fallback_required

    $requested = ConvertFrom-RunnerStrictUtcTimestamp ([string] $shutdown.requested_utc)
    $completed = ConvertFrom-RunnerStrictUtcTimestamp ([string] $shutdown.completed_utc)
    $steadyCompleted = ConvertFrom-RunnerStrictUtcTimestamp $script:SteadyCompletedUtc
    $processStarted = [DateTimeOffset]::new($process.StartTime.ToUniversalTime())
    if ([string] $shutdown.schema -cne "opennova.process-shutdown.v1" -or
            [int] $shutdown.process_id -ne $process.Id -or
            $shutdown.close_requested -isnot [bool] -or
            -not [bool] $shutdown.close_requested -or
            $shutdown.exact_fallback_required -isnot [bool] -or
            [bool] $shutdown.exact_fallback_required -or
            $shutdown.launcher_killed -isnot [bool] -or
            [bool] $shutdown.launcher_killed -or
            $shutdown.runtime_killed -isnot [bool] -or
            [bool] $shutdown.runtime_killed -or
            -not ($shutdown.exit_code -is [int] -or $shutdown.exit_code -is [long]) -or
            [int] $shutdown.exit_code -ne 0 -or
            $shutdown.clean -isnot [bool] -or -not [bool] $shutdown.clean -or
            $requested -lt $processStarted -or $requested -lt $steadyCompleted -or
            $completed -lt $requested -or $completed -gt $requested.AddSeconds(20)) {
        throw "OpenNova host process shutdown evidence did not prove a clean post-steady exit"
    }

    if (-not (Test-Path -LiteralPath $logPath -PathType Leaf)) {
        throw "OpenNova host shutdown log is missing: $logPath"
    }
    $resolvedLogPath = (Resolve-Path -LiteralPath $logPath -ErrorAction Stop).Path
    $expectedLogPath = [IO.Path]::GetFullPath((Join-Path $RunRoot "host\godot.log"))
    if (-not $resolvedLogPath.Equals(
            $expectedLogPath, [StringComparison]::OrdinalIgnoreCase)) {
        throw "OpenNova host shutdown log escaped its exact run path: $resolvedLogPath"
    }
    [byte[]] $logRaw = [IO.File]::ReadAllBytes($resolvedLogPath)
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
            [Array]::IndexOf($logRaw, [byte] 0) -ge 0) {
        throw "OpenNova host log is empty, BOM-prefixed, or contains NUL bytes"
    }
    try { $logText = [Text.UTF8Encoding]::new($false, $true).GetString($logRaw) }
    catch { throw "OpenNova host log is not strict BOM-free UTF-8: $($_.Exception.Message)" }
    if ([string]::IsNullOrWhiteSpace($logText)) {
        throw "OpenNova host log contains no diagnostic text"
    }
    $forbiddenDiagnostics = @(
        [pscustomobject]@{ Name = "ObjectDB leak"; Pattern = '(?im)\bObjectDB\b[^\r\n]*\bleak(?:ed|s)?\b' },
        [pscustomobject]@{ Name = "resource leak"; Pattern = '(?im)\bResources?\b[^\r\n]*(?:\bstill in use\b|\bleak(?:ed|s)?\b)' },
        [pscustomobject]@{ Name = "RenderingServer leak"; Pattern = '(?im)\bRenderingServer\b[^\r\n]*(?:\bstill in use\b|\bleak(?:ed|s)?\b|get_singleton\(\))' },
        [pscustomobject]@{ Name = "RID leak"; Pattern = '(?im)\bRIDs?\b[^\r\n]*\bleak(?:ed|s)?\b' }
    )
    $detected = @($forbiddenDiagnostics | Where-Object {
        $logText -match $_.Pattern
    } | ForEach-Object { $_.Name })
    if ($detected.Count -ne 0) {
        throw "OpenNova host emitted forbidden shutdown diagnostics: $($detected -join ', ')"
    }

    $script:OpenNovaHostShutdownWitness = [pscustomobject][ordered]@{
        schema = "opennova.parity-host-shutdown.v1"
        run_id = $RunId
        topology = $Topology
        process_id = [int] $shutdown.process_id
        requested_utc = [string] $shutdown.requested_utc
        completed_utc = [string] $shutdown.completed_utc
        close_requested = [bool] $shutdown.close_requested
        exit_code = [int] $shutdown.exit_code
        clean = [bool] $shutdown.clean
        log_path = $resolvedLogPath
        log_sha256 = (Get-FileHash -LiteralPath $resolvedLogPath `
            -Algorithm SHA256).Hash.ToLowerInvariant()
        forbidden_diagnostics_absent = $true
    }
    return $script:OpenNovaHostShutdownWitness
}

function Start-OpenNovaHostExact {
    $hostDir = Join-Path $RunRoot "host"
    New-Item -ItemType Directory -Path $hostDir -Force | Out-Null
    $script:OpenNovaHost = & $HostScript `
        -Mission $Mission -Name $HostCallsign -Port $Port -LanMode 1 `
        -MaxPlayers 4 -GameType ([string] $GameType) -ResourceDir $ServerDir `
        -Expansion $Expansion `
        -IntegrityProfile "retail-revx02-024f56f2-2d087374" `
        -LogFile (Join-Path $hostDir "godot.log") `
        -Resolution $Resolution -Windowed -McpPort $HostMcpPort `
        -SkipReadyCheck -PassThru
    if (-not $script:OpenNovaHost) { throw "OpenNova host launcher returned no process" }
    Register-OpenNovaLaunchProof -Role host -Process $script:OpenNovaHost
    & $ProbeExe --host 127.0.0.1 --port $Port --timeout-ms 240000 `
        --interval-ms 1000 --expect-name $ServerName
    if ($LASTEXITCODE -ne 0) {
        throw "OpenNova host PID $($script:OpenNovaHost.Id) did not answer LAN discovery"
    }
    $script:OpenNovaHostStartOwnership = Assert-OpenNovaHostOwnsPort
    # Discovery answers while the host is still loading its world; a joiner
    # dialing before the load completes times out on preload admission. The
    # endpoint's own account gates the joiner launch.
    $null = Wait-OpenNovaHostLoaded -TimeoutSeconds 240
}

function Get-OpenNovaHostState {
    if (-not $script:OpenNovaHost) { throw "No OpenNova host to query" }
    return Get-StructuredResult (Invoke-GameTool -Port $HostMcpPort -Name "game_state")
}

function Wait-OpenNovaHostLoaded {
    param([int] $TimeoutSeconds = 240)
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ($true) {
        $script:OpenNovaHost.Refresh()
        if ($script:OpenNovaHost.HasExited) {
            throw "OpenNova host exited before its world loaded"
        }
        $state = Get-OpenNovaHostState
        $network = $null
        if ($state.runtime.PSObject.Properties['network']) { $network = $state.runtime.network }
        # The boot is complete once the host's own player has spawned (the
        # listen server auto-spawns it at the end of the mission boot); the
        # world node exists earlier, while the load still runs.
        if ([bool] $state.shell.world_loaded -and [bool] $state.player.present -and $network -and
                [string] $network.role -eq "host" -and [int] $network.port -eq $Port) {
            return $state
        }
        if ([DateTime]::UtcNow -ge $deadline) {
            throw "OpenNova host did not report a loaded world hosting on port $Port within $TimeoutSeconds s"
        }
        Start-Sleep -Milliseconds 500
    }
}

function Start-OpenNovaJoinerExact {
    $joinerDir = Join-Path $RunRoot "joiner"
    New-Item -ItemType Directory -Path $joinerDir -Force | Out-Null
    $script:OpenNovaJoiner = Start-OpenNovaProcess `
        -GodotArguments @(
            "--windowed", "--resolution", $Resolution,
            "--log-file", (Join-Path $joinerDir "godot.log")
        ) `
        -GameArguments @(
            "--lan-join", "127.0.0.1:$Port",
            "--callsign", $JoinerCallsign,
            "--integrity-profile", "retail-revx02-024f56f2-2d087374",
            "--resource-dir", $ClientDir, "/exp", $Expansion
        ) `
        -McpPort $JoinerMcpPort
    if (-not $script:OpenNovaJoiner) { throw "OpenNova joiner launcher returned no process" }
    Register-OpenNovaLaunchProof -Role joiner -Process $script:OpenNovaJoiner
}

# The joiner's readiness witness. The first call runs parity_joiner_ready in
# the joiner (blocking until the requested readiness, sending the default
# deployment pick when -AutoDeploy); every later call takes one live snapshot
# through parity_joiner_state. Both carry the same witness the readiness
# classifiers in lib.ps1 consume unchanged.
function Wait-OpenNovaJoinerReady {
    param(
        [int] $TimeoutSeconds = 240,
        [long] $MinimumTicksMsec = -1,
        [switch] $RequireMotionComplete,
        [switch] $AllowPostDeathRedeploy
    )
    $script:OpenNovaJoiner.Refresh()
    if ($script:OpenNovaJoiner.HasExited) {
        throw "OpenNova joiner exited before reporting $ReadinessMode readiness"
    }
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ($true) {
        if (-not $script:JoinerReadyRunId) {
            $status = Invoke-GameProbe -Port $JoinerMcpPort -Name "parity_joiner_ready" -Process $script:OpenNovaJoiner `
                -TimeoutSeconds $TimeoutSeconds -PollMs 1000 -Arguments @{
                    readiness_mode = $ReadinessMode
                    auto_deploy = [bool] $AutoDeploy
                    exercise_motion = [bool] $ExerciseInput
                    run_id = $RunId
                    topology = $Topology
                }
            $script:JoinerReadyRunId = [string] $status.run_id
            if ([string] $status.state -ne "passed" -or -not $status.verdict -or
                    -not [bool] $status.verdict.ok) {
                $why = if ($status.verdict) { [string] $status.verdict.summary } else { [string] $status.error }
                throw "OpenNova joiner did not reach $ReadinessMode readiness: state=$($status.state) $why"
            }
            $ready = $status.verdict.data
        }
        else {
            $status = Invoke-GameProbe -Port $JoinerMcpPort -Name "parity_joiner_state" -Process $script:OpenNovaJoiner `
                -TimeoutSeconds 30 -PollMs 250 -Arguments @{
                    readiness_mode = $ReadinessMode
                    auto_deploy = [bool] $AutoDeploy
                    exercise_motion = [bool] $ExerciseInput
                }
            if ([string] $status.state -ne "passed" -or -not $status.verdict) {
                throw "OpenNova joiner state snapshot failed: state=$($status.state) $($status.error)"
            }
            $ready = $status.verdict.data
        }
        if ([int] $ready.process_id -ne $script:OpenNovaJoiner.Id) {
            throw "OpenNova readiness witness PID does not match the owned joiner process"
        }
        if ([string] $ready.readiness_mode -ne $ReadinessMode) {
            throw "OpenNova joiner witness readiness mode does not match the run"
        }
        $readinessClass = Get-OpenNovaJoinerReadinessClass `
            -State $ready -ReadinessMode $ReadinessMode `
            -AllowPostDeathRedeploy:$AllowPostDeathRedeploy
        if ($readinessClass -eq 'invalid') {
            throw "OpenNova joiner witness reports an invalid admission/liveness state"
        }
        $ticksMsec = [long] $ready.ticks_msec
        if ($ticksMsec -gt $MinimumTicksMsec -and
                (-not $RequireMotionComplete -or [bool] $ready.motion_complete)) {
            $ready | Add-Member -NotePropertyName observed_utc `
                -NotePropertyValue ([DateTime]::UtcNow.ToString("o")) -Force
            $ready | Add-Member -NotePropertyName probe_run_id `
                -NotePropertyValue ([string] $status.run_id) -Force
            return $ready
        }
        if ([DateTime]::UtcNow -ge $deadline) {
            throw "Timed out waiting for OpenNova joiner $ReadinessMode readiness witness"
        }
        Start-Sleep -Milliseconds 250
    }
}

try {
    Write-Host "PARITY_RUN_BEGIN topology=$Topology mission=$Mission game_type=$GameType port=$Port resolution=$Resolution run=$RunId"
    Assert-UdpPortUnowned
    Start-OnHookMcp

    # Exercise the tracked capture wrapper used by the runbook so every suite
    # also covers its scalar-interface path.
    # Worst-case discovery, endpoint readiness, and decoded wire readiness can
    # run sequentially before the 120-second maximum steady window. Keep the
    # safety cap beyond that whole
    # interval; normal completion still stops the exact capture in finally,
    # after both endpoints have shut down.
    $captureStart = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass `
        -File $CaptureScript -Action start -Tag $script:CaptureTag `
        -Filter "udp and port $Port" -DurationSeconds 1500)
    if ($LASTEXITCODE -ne 0) { throw "capture.ps1 start failed: $LASTEXITCODE" }
    $capturePathLine = $captureStart | Where-Object { "$_" -like "CAPTURE_FILE=*" } |
        Select-Object -Last 1
    if (-not $capturePathLine) { throw "capture.ps1 did not report CAPTURE_FILE" }
    $script:ExternalCapture = "$capturePathLine".Substring("CAPTURE_FILE=".Length)
    $script:CaptureStarted = $true

    switch ($Topology) {
        "RR" {
            if ($ReadinessMode -eq "deploy_hold") {
                $retailHost = Start-RetailHostExact
                $retailJoiner = Start-RetailJoinerExact
                $hostInstance = Wait-RetailProcessForPeer `
                    -ProcessId $script:RetailHostProcess.Id -Role host
                $hostInstanceId = [string] $hostInstance.instance_id
                if (-not $script:RetailInstanceIds.Contains($hostInstanceId)) {
                    $script:RetailInstanceIds.Add($hostInstanceId)
                }
                $script:EvidenceCapture = [string] $retailJoiner.capture_path
                $script:EvidencePerspective = "retail-client"
                $script:EvidenceRole = "joiner"
                $script:EvidenceInstanceId = [string] $retailJoiner.instance_id
                $script:RetailInstanceId = [string] $retailJoiner.instance_id
                $null = Wait-ParityWireReady
                $script:Acceptance = [pscustomobject]@{
                    host = $hostInstance.run_state
                    joiner = $retailJoiner.instance.run_state
                    host_pid = $script:RetailHostProcess.Id
                    joiner_pid = $script:RetailJoinerProcess.Id
                    wire_ready = $script:WireReadyWitness
                }
                Begin-SteadyWindow
                Wait-UntilSteadyDeadline
                $hostInstance = Wait-RetailProcessForPeer `
                    -ProcessId $script:RetailHostProcess.Id -Role host
                $joinerInstance = Wait-RetailProcessForPeer `
                    -ProcessId $script:RetailJoinerProcess.Id -Role joiner
                $script:Acceptance = [pscustomobject]@{
                    host = $hostInstance.run_state
                    joiner = $joinerInstance.run_state
                    host_pid = $script:RetailHostProcess.Id
                    joiner_pid = $script:RetailJoinerProcess.Id
                    wire_ready = $script:WireReadyWitness
                }
                Complete-SteadyWindow
                Stop-RetailProcessCapture -InstanceId ([string] $retailJoiner.instance_id) `
                    -RunLabel "$RunId-joiner"
                Stop-RetailProcessCapture -InstanceId $hostInstanceId `
                    -RunLabel "$RunId-host"
                Stop-ExactProcess -Process $script:RetailJoinerProcess
                $script:RetailJoinerProcess = $null
                Stop-ExactProcess -Process $script:RetailHostProcess
                $script:RetailHostProcess = $null
                break
            }
            $pairResult = Invoke-OnHookTool -Name "onhook_run_lan_pair" -Arguments @{
                mission = $Mission
                host_game_dir = $ServerDir
                joiner_game_dir = $ClientDir
                expansion = $Expansion
                join_address = "127.0.0.1"
                port = $Port
                host_callsign = $HostCallsign
                joiner_callsign = $JoinerCallsign
                game_type = [string] $GameType
                max_players = 4
                run_id = $RunId
                output_dir = $RunRoot
                capture = $true
                windowed = $true
                allow_many = $true
                wait_timeout_ms = 240000
            }
            $pair = Get-StructuredResult $pairResult
            $script:OwnedRetailRuns.Add([string] $pair.host.run_id)
            $script:OwnedRetailRuns.Add([string] $pair.joiner.run_id)
            Bind-EffectiveRetailConfig -Role server -Source (Join-Path $ServerDir "onhook.cfg")
            Bind-EffectiveRetailConfig -Role client -Source (Join-Path $ClientDir "onhook.cfg")
            $hostStatus = Wait-RetailRunForPeer -OwnedRunId ([string] $pair.host.run_id)
            $joinerStatus = Wait-RetailRunForPeer -OwnedRunId ([string] $pair.joiner.run_id)
            $script:EvidenceCapture = [string] $pair.joiner.capture_path
            $script:EvidencePerspective = "retail-client"
            $script:EvidenceRole = "joiner"
            $script:EvidenceInstanceId = [string] $pair.joiner.instance_id
            $script:Acceptance = [pscustomobject]@{
                host = $hostStatus.instance.run_state
                joiner = $joinerStatus.instance.run_state
                host_pid = [int] $pair.host.pid
                joiner_pid = [int] $pair.joiner.pid
            }
            $null = Wait-ParityWireReady
            $script:Acceptance | Add-Member -NotePropertyName wire_ready `
                -NotePropertyValue $script:WireReadyWitness -Force
            Begin-SteadyWindow
            if ($ExerciseInput) {
                Invoke-RetailInputExercise -ProcessId ([int] $pair.joiner.pid)
            }
            Wait-UntilSteadyDeadline
            $hostStatus = Wait-RetailRunForPeer -OwnedRunId ([string] $pair.host.run_id)
            $joinerStatus = Wait-RetailRunForPeer -OwnedRunId ([string] $pair.joiner.run_id)
            $script:Acceptance = [pscustomobject]@{
                host = $hostStatus.instance.run_state
                joiner = $joinerStatus.instance.run_state
                host_pid = [int] $pair.host.pid
                joiner_pid = [int] $pair.joiner.pid
                wire_ready = $script:WireReadyWitness
            }
            Complete-SteadyWindow
            Save-RetailScreenshot -InstanceId ([string] $pair.joiner.instance_id)
            Stop-OwnedRetailRun -OwnedRunId ([string] $pair.joiner.run_id)
            Stop-OwnedRetailRun -OwnedRunId ([string] $pair.host.run_id)
        }
        "RO" {
            if ($ReadinessMode -eq "deploy_hold") {
                $retailHost = Start-RetailHostExact
                $script:EvidenceCapture = [string] $retailHost.capture_path
                $script:EvidencePerspective = "retail-host"
                $script:EvidenceRole = "host"
                Start-OpenNovaJoinerExact
                $hostInstance = Wait-RetailProcessForPeer `
                    -ProcessId $script:RetailHostProcess.Id -Role host
                $hostInstanceId = [string] $hostInstance.instance_id
                $script:RetailInstanceId = $hostInstanceId
                $script:EvidenceInstanceId = $hostInstanceId
                if (-not $script:RetailInstanceIds.Contains($hostInstanceId)) {
                    $script:RetailInstanceIds.Add($hostInstanceId)
                }
                $joinerAcceptance = Wait-OpenNovaJoinerReady
                $initialJoinerAcceptance = $joinerAcceptance
                $initialJoinerTicks = [long] $joinerAcceptance.ticks_msec
                $null = Wait-ParityWireReady
                $script:Acceptance = [pscustomobject]@{
                    retail_host = [pscustomobject]@{
                        instance = $hostInstance
                        pid = $script:RetailHostProcess.Id
                        port_ownership = $retailHost.port_ownership
                    }
                    opennova_joiner = $joinerAcceptance
                    wire_ready = $script:WireReadyWitness
                }
                Begin-SteadyWindow
                Wait-UntilSteadyDeadline
                $script:OpenNovaJoiner.Refresh()
                if ($script:OpenNovaJoiner.HasExited) {
                    throw "OpenNova joiner exited during the RO deploy-hold steady window"
                }
                $hostInstance = Wait-RetailProcessForPeer `
                    -ProcessId $script:RetailHostProcess.Id -Role host
                $joinerAcceptance = Wait-OpenNovaJoinerReady `
                    -MinimumTicksMsec $initialJoinerTicks
                if (-not (Test-OpenNovaJoinerReadinessTransition `
                        -Initial $initialJoinerAcceptance -Final $joinerAcceptance `
                        -ReadinessMode $ReadinessMode)) {
                    throw "OpenNova joiner deploy-hold heartbeat transition is invalid"
                }
                $script:Acceptance = [pscustomobject]@{
                    retail_host = [pscustomobject]@{
                        instance = $hostInstance
                        pid = $script:RetailHostProcess.Id
                        port_ownership = (Assert-RetailHostOwnsPort `
                            -ProcessId $script:RetailHostProcess.Id)
                    }
                    opennova_joiner_initial = $initialJoinerAcceptance
                    opennova_joiner = $joinerAcceptance
                    wire_ready = $script:WireReadyWitness
                }
                Complete-SteadyWindow
                $null = Stop-OpenNovaJoinerCooperatively
                $script:OpenNovaJoiner = $null
                Stop-RetailProcessCapture -InstanceId $hostInstanceId `
                    -RunLabel "$RunId-host"
                Stop-ExactProcess -Process $script:RetailHostProcess
                $script:RetailHostProcess = $null
                break
            }
            $hostResult = Invoke-OnHookTool -Name "onhook_host_lan" -Arguments @{
                mission = $Mission
                game_dir = $ServerDir
                expansion = $Expansion
                port = $Port
                callsign = $HostCallsign
                game_type = [string] $GameType
                max_players = 4
                run_id = "$RunId-host"
                output_dir = $RunRoot
                capture = $true
                windowed = $true
                allow_many = $true
                wait_timeout_ms = 240000
            }
            $retailHost = Get-StructuredResult $hostResult
            $script:OwnedRetailRuns.Add([string] $retailHost.run_id)
            Bind-EffectiveRetailConfig -Role server -Source (Join-Path $ServerDir "onhook.cfg")
            $script:EvidenceCapture = [string] $retailHost.capture_path
            $script:EvidencePerspective = "retail-host"
            $script:EvidenceRole = "host"
            $script:EvidenceInstanceId = [string] $retailHost.instance_id
            Start-OpenNovaJoinerExact
            $retailAcceptance = Wait-RetailRunForPeer -OwnedRunId ([string] $retailHost.run_id)
            $joinerAcceptance = Wait-OpenNovaJoinerReady
            $initialJoinerAcceptance = $joinerAcceptance
            $initialJoinerTicks = [long] $joinerAcceptance.ticks_msec
            $script:Acceptance = [pscustomobject]@{
                retail_host = $retailAcceptance
                opennova_joiner = $joinerAcceptance
            }
            $null = Wait-ParityWireReady
            $script:Acceptance | Add-Member -NotePropertyName wire_ready `
                -NotePropertyValue $script:WireReadyWitness -Force
            Begin-SteadyWindow
            if ($ExerciseInput) {
                Publish-MotionGate
            }
            Wait-UntilSteadyDeadline
            $script:OpenNovaJoiner.Refresh()
            if ($script:OpenNovaJoiner.HasExited) {
                throw "OpenNova joiner exited during the RO steady window"
            }
            $retailAcceptance = Wait-RetailRunForPeer -OwnedRunId ([string] $retailHost.run_id)
            $joinerAcceptance = Wait-OpenNovaJoinerReady `
                -MinimumTicksMsec $initialJoinerTicks -RequireMotionComplete `
                -AllowPostDeathRedeploy
            if (-not (Test-OpenNovaJoinerReadinessTransition `
                    -Initial $initialJoinerAcceptance -Final $joinerAcceptance `
                    -ReadinessMode $ReadinessMode -AllowPostDeathRedeploy `
                    -RequireMotionComplete)) {
                throw "OpenNova joiner in-match/post-death heartbeat transition is invalid"
            }
            $script:Acceptance = [pscustomobject]@{
                retail_host = $retailAcceptance
                opennova_joiner_initial = $initialJoinerAcceptance
                opennova_joiner = $joinerAcceptance
                wire_ready = $script:WireReadyWitness
            }
            Complete-SteadyWindow
            Save-RetailScreenshot -InstanceId ([string] $retailHost.instance_id)
            $null = Stop-OpenNovaJoinerCooperatively
            $script:OpenNovaJoiner = $null
            Stop-OwnedRetailRun -OwnedRunId ([string] $retailHost.run_id)
        }
        "OR" {
            Start-OpenNovaHostExact
            $joinerDir = Join-Path $RunRoot "joiner"
            New-Item -ItemType Directory -Path $joinerDir -Force | Out-Null
            $retailJoinLaunch = Get-StructuredResult (Invoke-OnHookTool -Name "onhook_join_lan" -Arguments @{
                game_dir = $ClientDir
                expansion = $Expansion
                join_address = "127.0.0.1"
                port = $Port
                callsign = $JoinerCallsign
                run_id = "$RunId-joiner"
                output_dir = $joinerDir
                capture = $true
                windowed = $true
                allow_many = $true
                wait_timeout_ms = 240000
            })
            Assert-OnHookRoleLaunch -Tool "onhook_join_lan" -Result $retailJoinLaunch
            $script:OwnedRetailRuns.Add([string] $retailJoinLaunch.run_id)
            $script:RetailProcess = Get-Process -Id ([int] $retailJoinLaunch.pid) -ErrorAction Stop
            $script:RetailJoinerProcess = $script:RetailProcess
            $clientCfg = Join-Path $ClientDir "onhook.cfg"
            $cfgText = Get-Content -LiteralPath $clientCfg -Raw
            foreach ($expected in @(
                "LanJoinAddress 127.0.0.1",
                "LanJoinPort $Port",
                "LanJoinCallsign $JoinerCallsign",
                "OpenNovaExpansion $Expansion"
            )) {
                if ($cfgText -notmatch [regex]::Escape($expected)) {
                    throw "CLIENT cfg is not pinned to the current matrix case: missing '$expected'"
                }
            }
            Bind-EffectiveRetailConfig -Role client -Source $clientCfg
            $script:EvidenceCapture = [string] $retailJoinLaunch.capture_path
            $script:EvidencePerspective = "retail-client"
            $script:EvidenceRole = "joiner"
            if ($WireOnly) {
                Begin-SteadyWindow
                Wait-UntilSteadyDeadline
                $script:RetailProcess.Refresh()
                if ($script:RetailProcess.HasExited) {
                    throw "Retail joiner exited during the OR wire-only steady window"
                }
                $endOwnership = Assert-OpenNovaHostOwnsPort
                $instances = Get-StructuredResult (Invoke-OnHookTool -Name "onhook_instances")
                $instance = @($instances.instances) |
                    Where-Object { [int] $_.pid -eq $script:RetailProcess.Id } |
                    Select-Object -First 1
                if ($instance) {
                    $script:RetailInstanceId = [string] $instance.instance_id
                    $script:Acceptance = [pscustomobject]@{
                        retail_joiner = $instance.run_state
                        opennova_host = [pscustomobject]@{
                            pid = $script:OpenNovaHost.Id
                            port_ownership = $endOwnership
                        }
                    }
                    if ($instance.capture_ready) {
                        Save-RetailScreenshot -InstanceId $script:RetailInstanceId
                    }
                } else {
                    $script:Acceptance = [pscustomobject]@{
                        wire_only = $true
                        retail_pid = $script:RetailProcess.Id
                        sustained_seconds = $SteadySeconds
                        opennova_host = [pscustomobject]@{
                            pid = $script:OpenNovaHost.Id
                            port_ownership = $endOwnership
                        }
                    }
                }
                Complete-SteadyWindow
            } else {
                $instance = Wait-RetailProcessForPeer -ProcessId $script:RetailProcess.Id `
                    -Role joiner -RequireInMatch:($ReadinessMode -eq "in_match")
                $script:RetailInstanceId = [string] $instance.instance_id
                $script:EvidenceInstanceId = $script:RetailInstanceId
                if (-not $script:RetailInstanceIds.Contains($script:RetailInstanceId)) {
                    $script:RetailInstanceIds.Add($script:RetailInstanceId)
                }
                $null = Wait-ParityWireReady
                Begin-SteadyWindow
                if ($ExerciseInput) {
                    Invoke-RetailInputExercise -ProcessId $script:RetailProcess.Id
                }
                Wait-UntilSteadyDeadline
                $script:RetailProcess.Refresh()
                $script:OpenNovaHost.Refresh()
                if ($script:RetailProcess.HasExited -or $script:OpenNovaHost.HasExited) {
                    throw "An OR endpoint exited during the steady window"
                }
                $instance = Wait-RetailProcessForPeer -ProcessId $script:RetailProcess.Id `
                    -Role joiner -RequireInMatch:($ReadinessMode -eq "in_match")
                & $ProbeExe --host 127.0.0.1 --port $Port --timeout-ms 5000 `
                    --interval-ms 250 --expect-name $ServerName
                if ($LASTEXITCODE -ne 0) {
                    throw "OpenNova host stopped answering matching discovery during OR"
                }
                $endOwnership = Assert-OpenNovaHostOwnsPort
                $script:Acceptance = [pscustomobject]@{
                    retail_joiner = $instance.run_state
                    retail_pid = $script:RetailProcess.Id
                    wire_ready = $script:WireReadyWitness
                    opennova_host = [pscustomobject]@{
                        pid = $script:OpenNovaHost.Id
                        mcp_port = $HostMcpPort
                        discovery_ready = $true
                        port_ownership = $endOwnership
                        state = Get-OpenNovaHostState
                    }
                }
                Complete-SteadyWindow
                if ($ReadinessMode -eq "in_match") {
                    Save-RetailScreenshot -InstanceId $script:RetailInstanceId
                }
            }
            if ($script:RetailInstanceId) {
                Stop-RetailProcessCapture -InstanceId $script:RetailInstanceId `
                    -RunLabel "$RunId-joiner"
            }
            Stop-ExactProcess -Process $script:RetailProcess
            $script:RetailProcess = $null
            $script:RetailJoinerProcess = $null
            $null = Stop-OpenNovaHostCleanly
            $script:OpenNovaHost = $null
        }
        "OO" {
            Start-OpenNovaHostExact
            Start-OpenNovaJoinerExact
            $joinerAcceptance = Wait-OpenNovaJoinerReady
            $initialJoinerAcceptance = $joinerAcceptance
            $initialJoinerTicks = [long] $joinerAcceptance.ticks_msec
            $script:EvidenceCapture = $script:ExternalCapture
            $script:EvidencePerspective = "external"
            $script:EvidenceRole = "external"
            $null = Wait-ParityWireReady
            Begin-SteadyWindow
            if ($ExerciseInput) {
                Publish-MotionGate
            }
            Wait-UntilSteadyDeadline
            $script:OpenNovaHost.Refresh()
            $script:OpenNovaJoiner.Refresh()
            if ($script:OpenNovaHost.HasExited -or $script:OpenNovaJoiner.HasExited) {
                throw "An OpenNova endpoint exited before sustained capture completed"
            }
            & $ProbeExe --host 127.0.0.1 --port $Port --timeout-ms 5000 `
                --interval-ms 250 --expect-name $ServerName
            if ($LASTEXITCODE -ne 0) {
                throw "OpenNova host stopped answering matching discovery during OO"
            }
            $endOwnership = Assert-OpenNovaHostOwnsPort
            $joinerAcceptance = Wait-OpenNovaJoinerReady `
                -MinimumTicksMsec $initialJoinerTicks -RequireMotionComplete `
                -AllowPostDeathRedeploy:($ReadinessMode -eq 'in_match')
            if (-not (Test-OpenNovaJoinerReadinessTransition `
                    -Initial $initialJoinerAcceptance -Final $joinerAcceptance `
                    -ReadinessMode $ReadinessMode `
                    -AllowPostDeathRedeploy:($ReadinessMode -eq 'in_match') `
                    -RequireMotionComplete)) {
                throw "OpenNova joiner steady-window heartbeat transition is invalid"
            }
            $script:Acceptance = [pscustomobject]@{
                host_pid = $script:OpenNovaHost.Id
                joiner_pid = $script:OpenNovaJoiner.Id
                sustained_seconds = $SteadySeconds
                wire_ready = $script:WireReadyWitness
                opennova_host = [pscustomobject]@{
                    pid = $script:OpenNovaHost.Id
                    mcp_port = $HostMcpPort
                    discovery_ready = $true
                    port_ownership = $endOwnership
                    state = Get-OpenNovaHostState
                }
                opennova_joiner_initial = $initialJoinerAcceptance
                opennova_joiner = $joinerAcceptance
            }
            Complete-SteadyWindow
            $stoppedJoiner = $script:OpenNovaJoiner
            $stoppedHost = $script:OpenNovaHost
            $null = Stop-OpenNovaJoinerCooperatively
            $script:OpenNovaJoiner = $null
            $null = Stop-OpenNovaHostCleanly
            $script:OpenNovaHost = $null
            $stoppedJoiner.Refresh()
            $stoppedHost.Refresh()
            if (-not $stoppedJoiner.HasExited -or -not $stoppedHost.HasExited) {
                throw "An OO endpoint remained alive before the capture tail marker"
            }
            Assert-UdpPortUnowned
            Send-ExternalCaptureTailMarker
        }
    }
}
finally {
    if ($script:OpenNovaJoiner) {
        try {
            $null = Stop-OpenNovaJoinerCooperatively
            $script:OpenNovaJoiner = $null
        }
        catch {
            $cooperativeError = $_.Exception.Message
            $script:OpenNovaJoinerExactFallbackRequired = $true
            try { Stop-ExactProcess -Process $script:OpenNovaJoiner }
            catch { Add-CleanupFailure "OpenNova joiner exact fallback failed: $($_.Exception.Message)" }
            $script:OpenNovaJoiner = $null
            Add-CleanupFailure "OpenNova joiner cooperative cleanup failed before exact fallback: $cooperativeError"
        }
    }
    if ($script:OpenNovaHost) {
        try {
            $script:OpenNovaHost.Refresh()
            if ($Topology -in @("OR", "OO") -and
                    -not $script:OpenNovaHost.HasExited) {
                $script:OpenNovaHostExactFallbackRequired = $true
            }
            Stop-ExactProcess -Process $script:OpenNovaHost
        }
        catch { Add-CleanupFailure "OpenNova host cleanup failed: $($_.Exception.Message)" }
    }

    if ($script:Mcp -and -not $script:Mcp.HasExited) {
        $cleanupInstances = @($script:RetailInstanceIds.ToArray())
        [array]::Reverse($cleanupInstances)
        foreach ($instanceId in $cleanupInstances) {
            try {
                Stop-RetailProcessCapture -InstanceId $instanceId `
                    -RunLabel "$RunId-cleanup-$instanceId"
            }
            catch { Add-CleanupFailure "Retail capture cleanup failed for ${instanceId}: $($_.Exception.Message)" }
        }
    }
    try { Stop-ExactProcess -Process $script:RetailJoinerProcess }
    catch { Add-CleanupFailure "Retail joiner cleanup failed: $($_.Exception.Message)" }
    try { Stop-ExactProcess -Process $script:RetailHostProcess }
    catch { Add-CleanupFailure "Retail host cleanup failed: $($_.Exception.Message)" }
    try { Stop-ExactProcess -Process $script:RetailProcess }
    catch { Add-CleanupFailure "Retail process cleanup failed: $($_.Exception.Message)" }

    if ($script:Mcp -and -not $script:Mcp.HasExited) {
        $cleanupRuns = @($script:OwnedRetailRuns.ToArray())
        [array]::Reverse($cleanupRuns)
        foreach ($owned in $cleanupRuns) {
            try { Stop-OwnedRetailRun -OwnedRunId $owned }
            catch { Write-Warning "Cleanup failed for retail run ${owned}: $($_.Exception.Message)" }
        }
    }

    if ($script:CaptureStarted) {
        try {
            $captureStopArguments = @(
                "-NoProfile", "-ExecutionPolicy", "Bypass",
                "-File", $CaptureScript,
                "-Action", "stop",
                "-Tag", $script:CaptureTag
            )
            if ($script:CaptureTailMarker) {
                $captureStopArguments += @(
                    "-TailMarkerHex", [string] $script:CaptureTailMarker.hex)
            }
            $captureStop = @(& powershell.exe @captureStopArguments)
            if ($LASTEXITCODE -ne 0) { throw "capture.ps1 stop failed: $LASTEXITCODE" }
            $proofLine = $captureStop | Where-Object {
                "$_" -like "CAPTURE_PROOF_JSON=*"
            } | Select-Object -Last 1
            if (-not $proofLine) {
                throw "capture.ps1 stop did not report CAPTURE_PROOF_JSON"
            }
            $validation = "$proofLine".Substring("CAPTURE_PROOF_JSON=".Length) |
                ConvertFrom-Json
            if (-not [bool] $validation.valid -or -not [bool] $validation.exact_eof -or
                    [int] $validation.packet_block_count -le 0) {
                throw "capture.ps1 returned an invalid pcapng completion proof"
            }
            if ($Topology -eq "OO") {
                if (-not $script:CaptureTailMarker) {
                    throw "Successful OO capture has no post-endpoint tail marker"
                }
                if (-not [bool] $validation.tail_marker_required -or
                        -not [bool] $validation.tail_marker_found -or
                        [string] $validation.tail_marker_sha256 -ne
                            [string] $script:CaptureTailMarker.sha256) {
                    throw "OO capture tail marker proof does not match the sent marker"
                }
            }
            if (-not (Test-Path -LiteralPath $script:ExternalCapture) -or
                    (Get-Item -LiteralPath $script:ExternalCapture).Length -le 0) {
                throw "External capture is empty or missing: $($script:ExternalCapture)"
            }
            $captureSource = $script:ExternalCapture
            $sourceHash = (Get-FileHash -LiteralPath $captureSource `
                -Algorithm SHA256).Hash.ToLowerInvariant()
            $runCapture = Join-Path $RunRoot "external.pcapng"
            Copy-Item -LiteralPath $captureSource -Destination $runCapture
            $artifactHash = (Get-FileHash -LiteralPath $runCapture `
                -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($artifactHash -ne $sourceHash) {
                throw "External capture artifact copy does not match validated source"
            }
            $script:ExternalCapture = $runCapture
            $script:ExternalCaptureProof = [pscustomobject]@{
                validated_source = $captureSource
                artifact_path = $runCapture
                artifact_sha256 = $artifactHash
                validation = $validation
                tail_marker = $script:CaptureTailMarker
            }
            if ($Topology -eq "OO") {
                $script:EvidenceCapture = $runCapture
            }
        }
        catch {
            Add-CleanupFailure "External capture cleanup failed: $($_.Exception.Message)"
        }
        $script:CaptureStarted = $false
    }

    if ($script:Mcp -and -not $script:Mcp.HasExited) {
        try { $script:Mcp.StandardInput.Close() } catch {}
        if (-not $script:Mcp.WaitForExit(5000)) {
            Stop-Process -Id $script:Mcp.Id -Force -ErrorAction SilentlyContinue
        }
    }
}

if ($script:CleanupFailure) {
    throw $script:CleanupFailure
}

$expectsHostShutdownWitness = $Topology -in @("OR", "OO")
if ($expectsHostShutdownWitness) {
    if (-not $script:OpenNovaHostShutdownWitness -or
            $script:OpenNovaHostExactFallbackRequired) {
        throw "Verdict-bearing $Topology run lacks a clean OpenNova host shutdown witness"
    }
}
elseif ($script:OpenNovaHostShutdownWitness -or
        $script:OpenNovaHostExactFallbackRequired) {
    throw "Unexpected OpenNova host shutdown evidence for topology $Topology"
}

$expectsJoinerShutdownWitness = $Topology -in @("RO", "OO")
if ($expectsJoinerShutdownWitness) {
    if (-not $script:OpenNovaJoinerShutdownWitness -or
            $script:OpenNovaJoinerExactFallbackRequired) {
        throw "Verdict-bearing $Topology run lacks a clean cooperative OpenNova joiner shutdown witness"
    }
}
elseif ($script:OpenNovaJoinerShutdownWitness -or
        $script:OpenNovaJoinerExactFallbackRequired) {
    throw "Unexpected OpenNova joiner shutdown evidence for topology $Topology"
}

$expectedLaunchProofRoles = switch ($Topology) {
    "RR" { @() }
    "RO" { @("joiner") }
    "OR" { @("host") }
    "OO" { @("host", "joiner") }
}
$actualLaunchProofRoles = @($script:OpenNovaLaunchProofs.Keys |
    ForEach-Object { [string] $_ } | Sort-Object)
if (($actualLaunchProofRoles -join ',') -cne
        (@($expectedLaunchProofRoles | Sort-Object) -join ',')) {
    throw "OpenNova launch-proof role set drift: topology=$Topology expected=$($expectedLaunchProofRoles -join ',') actual=$($actualLaunchProofRoles -join ',')"
}

$summary = [pscustomobject]@{
    topology = $Topology
    mission = $Mission
    mission_path = $ResolvedMissionArtifact
    mission_sha256 = $MissionSha256
    game_type = $GameType
    port = $Port
    resolution = $Resolution
    expansion = $Expansion
    host_session_name = $HostSessionName
    server_name = $ServerName
    host_callsign = $HostCallsign
    joiner_callsign = $JoinerCallsign
    readiness_mode = $ReadinessMode
    steady_seconds = $SteadySeconds
    steady_started_utc = $script:SteadyStartedUtc
    steady_completed_utc = $script:SteadyCompletedUtc
    integrity_profile = "retail-revx02-024f56f2-2d087374"
    run_id = $RunId
    suite_id = $SuiteId
    corpus_fingerprint = $CorpusFingerprint
    wire_only = [bool] $WireOnly
    exercise_input = [bool] $ExerciseInput
    run_root = $RunRoot
    external_capture = $script:ExternalCapture
    external_capture_proof = $script:ExternalCaptureProof
    evidence_capture = $script:EvidenceCapture
    evidence_perspective = $script:EvidencePerspective
    evidence_role = $script:EvidenceRole
    evidence_instance_id = $script:EvidenceInstanceId
    screenshot = $script:Screenshot
    acceptance = $script:Acceptance
    opennova_launch_proofs = [pscustomobject] $script:OpenNovaLaunchProofs
    direct_runtime_launch = [bool] $AllowDirectRuntime
    opennova_host_mcp_port = $(if ($Topology -in @("OR", "OO")) { $HostMcpPort } else { $null })
    opennova_joiner_mcp_port = $(if ($Topology -in @("RO", "OO")) { $JoinerMcpPort } else { $null })
    joiner_ready_run_id = $script:JoinerReadyRunId
    joiner_motion_run_id = $script:JoinerMotionRunId
    opennova_joiner_shutdown_witness = $script:OpenNovaJoinerShutdownWitness
    opennova_joiner_exact_fallback_required = [bool] $script:OpenNovaJoinerExactFallbackRequired
    stop_evidence = $script:StopEvidence
    retail_input_witness = $script:InputWitness
    motion_gate_witness = $script:MotionGateWitness
    wire_ready_witness = $script:WireReadyWitness
    effective_retail_configs = @($script:EffectiveRetailConfigs.ToArray())
}
if ($expectsHostShutdownWitness) {
    $summary | Add-Member -NotePropertyName opennova_host_shutdown_witness `
        -NotePropertyValue $script:OpenNovaHostShutdownWitness
    $summary | Add-Member -NotePropertyName opennova_host_exact_fallback_required `
        -NotePropertyValue ([bool] $script:OpenNovaHostExactFallbackRequired)
}
$summaryJson = $summary | ConvertTo-Json -Depth 30
$summaryJson | Set-Content -LiteralPath (Join-Path $RunRoot "run-summary.json") -Encoding UTF8
Write-Host ("PARITY_SUMMARY_JSON=" + ($summary | ConvertTo-Json -Depth 30 -Compress))
Write-Host "PARITY_RUN_COMPLETE topology=$Topology mission=$Mission run=$RunId"
