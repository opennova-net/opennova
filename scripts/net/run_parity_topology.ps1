[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("RR", "RO", "OR", "OO")]
    [string] $Topology,

    [Parameter(Mandatory = $true)]
    [string] $Mission,

    [Parameter(Mandatory = $true)]
    [ValidateRange(1, 65535)]
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

    # A network-parity scenario (scripts/net/scenarios/*.json,
    # opennova.scenario.v1; ADR 0050 R5). At the steady-window start its
    # actor (the joiner or the host) plays its prelude, takes the setup pose
    # and plays the scripted steps -- a retail actor through onHook's
    # virtual keyboard, an OpenNova actor through the scenario_play probe --
    # then the run settles for the scenario's settle_seconds. The summary
    # records every step's applied logic tick.
    [string] $Scenario = "",

    # Round end and rotation (extension E2): the missions after -Mission in
    # the host's rotation, .BMS basenames in play order (a comma-separated
    # list is accepted). A retail host takes them through onhook_host_lan's
    # rotation (the stock host screen's selected-missions list); the OpenNova
    # listen host has no rotation launch flag, so OR and OO refuse them.
    [string[]] $Rotation = @(),

    # E2: the round time limit in minutes for this cell. Both hosts read it
    # from the SERVER copy's game.cfg time_limit (retail:
    # Game_ApplySessionSettingsToGlobals @0x551CCC..0x551CD2 copies it to the
    # misnamed g_RespawnTime, and Game_StartMission @0x525242..0x525251 starts
    # a round that is not co-op with g_RoundTimeRemaining = it * 0xE88 ticks,
    # which Server_CheckWinConditions @0x51AF6E ends at zero; koth_limit is
    # the king-of-the-hill hold, @0x51AF0F..0x51AF18), so the runner writes it
    # there for the cell and restores the file's exact bytes once every
    # process has stopped. 0 keeps the copy's own value and leaves the file
    # alone.
    [ValidateRange(0, 600)]
    [int] $TimeLimitMinutes = 0,

    # The pinned SHA-256 of the SERVER copy's game.cfg (the case table's
    # server_game_cfg_sha256): the cell refuses a copy whose game.cfg differs
    # at its start. Empty skips the check (a diagnostic run).
    [ValidatePattern('^(?:|[0-9a-f]{64})$')]
    [string] $ServerGameCfgSha256 = "",

    # Late join (extension E3): a second joiner of the first joiner's kind
    # joins during the steady window, after the first joiner's scripted input,
    # so the host serves a join with a remote player already in the match and
    # the first joiner sees a player arrive. RR and OR: a retail late joiner
    # (onhook_join_lan) from -RetailLateClientRoot (default the CLIENT copy,
    # launched after the first joiner read its onhook.cfg). An OpenNova late
    # joiner (RO, OO) is not wired yet, so those refuse it.
    [switch] $LateJoiner,
    [string] $RetailLateClientRoot = "",
    [string] $LateJoinerCallsign = "ParityLate",

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

    # The retail expansion both roles load. The pinned reference install
    # (ADR 0050 d2) is JO:CA's jox01.
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9_-]{0,30}$')]
    [string] $Expansion = "jox01",

    # Diagnostic only: accept OpenNova launches through the plain runtime
    # executable (no console-wrapper job). A verdict-bearing run needs the
    # wrapper's ownership proof; the summary records the relaxation.
    [switch] $AllowDirectRuntime,

    # Diagnostic only: launch retail with onHook's behavior-changing hooks as
    # its onhook.cfg defaults them. A verdict-bearing run loads every retail
    # role with StockObserver (retail stock, the hook only observing) and
    # requires the loaded hook to confirm it; the summary records the
    # relaxation.
    [switch] $AllowModifiedRetail,

    # How the OpenNova joiner (RO, OO) reaches the host. `browse` searches as
    # the LAN screen does and joins the row on -Port (`--lan-join
    # browse:<port>`), as onHook's retail joiner does, so its 0x41s are the
    # broadcast announces a retail joiner sends. `direct` dials
    # 127.0.0.1:<port>, whose 0x41 names the dialed host as its EIP: a
    # diagnostic only. The summary records it (joiner_dial).
    [ValidateSet("browse", "direct")]
    [string] $JoinerDial = "browse",

    # Diagnostic only: run from a checkout with uncommitted tracked changes.
    # A verdict-bearing run is evidence for its repo_commit alone.
    [switch] $AllowDirtyRepo
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
if ($ReadinessMode -eq "deploy_hold" -and ($AutoDeploy -or $ExerciseInput)) {
    throw "deploy_hold readiness cannot auto-deploy or exercise input."
}
if ($WireOnly -and $Topology -ne "OR") {
    throw "WireOnly is an OR-only diagnostic mode."
}
# ConvertFrom-Json yields Int32/Int64 for integers and Double (pwsh) or
# Decimal (Windows PowerShell) for the rest.
function Test-JsonInteger {
    param($Value)
    return $Value -is [int] -or $Value -is [long]
}

function Test-JsonNumber {
    param($Value)
    return (Test-JsonInteger $Value) -or $Value -is [double] -or $Value -is [decimal]
}

# opennova.scenario.v1: the case it is written for (mission, numeric game
# type, actor), an optional prelude stage, an optional setup pose, the steps
# both drivers consume verbatim, and the settle after the last one. The drivers validate each step;
# this binds the scenario to the run's case.
function Read-ParityScenario {
    param([Parameter(Mandatory = $true)] [string] $Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Scenario file is missing: $Path"
    }
    $spec = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    $expected = @("schema", "name", "mission", "game_type", "actor", "prelude",
        "setup", "staging", "steps", "settle_seconds", "expect")
    $extra = @($spec.PSObject.Properties.Name | Where-Object { $_ -notin $expected })
    if ($extra.Count -gt 0) {
        throw "Scenario $Path has unknown fields: $($extra -join ', ')"
    }
    if ([string] $spec.schema -cne "opennova.scenario.v1" -or
            [string]::IsNullOrWhiteSpace([string] $spec.name)) {
        throw "Scenario $Path is not a named opennova.scenario.v1 document"
    }
    $missionStem = [IO.Path]::GetFileNameWithoutExtension($Mission)
    $specStem = [IO.Path]::GetFileNameWithoutExtension([string] $spec.mission)
    if ($specStem -ine $missionStem) {
        throw "Scenario $($spec.name) is written for mission $($spec.mission), not $Mission"
    }
    if ([string] $spec.game_type -cne $GameType) {
        throw "Scenario $($spec.name) is written for game type $($spec.game_type), not $GameType"
    }
    if ([string] $spec.actor -cnotin @("joiner", "host")) {
        throw "Scenario $($spec.name): the actor is the joiner or the host (actor '$($spec.actor)')"
    }
    $steps = @($spec.steps)
    if ($steps.Count -lt 1 -or -not ($steps[-1].PSObject.Properties.Name -contains "end")) {
        throw "Scenario $($spec.name) must end with an end step"
    }
    if ($spec.PSObject.Properties.Name -contains "prelude") {
        $prelude = @($spec.prelude)
        if ($prelude.Count -lt 1 -or -not ($prelude[-1].PSObject.Properties.Name -contains "end")) {
            throw "Scenario $($spec.name): the prelude must end with an end step"
        }
    }
    $settle = $spec.settle_seconds
    if (-not (Test-JsonInteger $settle) -or $settle -lt 0 -or $settle -gt 300) {
        throw "Scenario $($spec.name): settle_seconds must be an integer from 0 to 300"
    }
    if ($spec.PSObject.Properties.Name -contains "setup" -and $null -ne $spec.setup) {
        $pose = $spec.setup.pose
        $position = @($pose.position_bms)
        if ($position.Count -ne 3 -or
                @($position | Where-Object { -not (Test-JsonNumber $_) }).Count -gt 0 -or
                -not (Test-JsonNumber $pose.yaw) -or -not (Test-JsonNumber $pose.pitch)) {
            throw "Scenario $($spec.name): setup.pose needs position_bms [x, y, z], yaw and pitch numbers"
        }
    }
    # E5's staging: poses the host applies to other players' slots before the
    # actor plays (a joiner's own pose would not stick, above).
    if ($spec.PSObject.Properties.Name -contains "staging") {
        $staging = @($spec.staging)
        if ($staging.Count -lt 1) {
            throw "Scenario $($spec.name): staging lists at least one slot pose"
        }
        foreach ($entry in $staging) {
            $position = @($entry.pose.position_bms)
            if (-not (Test-JsonInteger $entry.slot) -or $entry.slot -lt 0 -or $entry.slot -gt 254 -or
                    $position.Count -ne 3 -or
                    @($position | Where-Object { -not (Test-JsonNumber $_) }).Count -gt 0 -or
                    -not (Test-JsonNumber $entry.pose.yaw) -or -not (Test-JsonNumber $entry.pose.pitch)) {
                throw "Scenario $($spec.name): each staging entry needs a slot 0..254 and a pose with position_bms [x, y, z], yaw and pitch"
            }
        }
        if ($Topology -in @("OR", "OO")) {
            throw "Scenario $($spec.name) stages players through the host; the OpenNova host has no slot pose yet (RR and RO only)"
        }
    }
    return $spec
}

$script:ScenarioSpec = $null
$script:ScenarioWitness = $null
if ($Scenario) {
    if ($ExerciseInput -or $WireOnly) {
        throw "A scenario plays with neither ExerciseInput nor WireOnly: its steps drive the joiner."
    }
    $script:ScenarioSpec = Read-ParityScenario -Path $Scenario
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
# E2's rotation tail, one entry per mission (powershell -File passes a
# comma-separated list as one string).
$RotationList = @($Rotation | ForEach-Object { "$_".Split(',') } |
    ForEach-Object { $_.Trim() } | Where-Object { $_ })
foreach ($entry in $RotationList) {
    if ($entry -notmatch '^[^\\/:*?"<>|,\x00-\x1f]+\.[Bb][Mm][Ss]$') {
        throw "Rotation entry '$entry' is not a .BMS basename."
    }
}
if ($RotationList.Count -gt 0 -and $Topology -in @("OR", "OO")) {
    throw "-Rotation needs a retail host (RR or RO): the OpenNova listen host has no rotation launch flag."
}
if ($LateJoiner -and $Topology -in @("RO", "OO")) {
    throw "-LateJoiner needs a retail joiner (RR or OR): an OpenNova late joiner is not wired yet."
}
if ($LateJoiner -and $WireOnly) {
    throw "-LateJoiner and -WireOnly do not combine."
}
if ($LateJoiner -and ([string]::IsNullOrWhiteSpace($LateJoinerCallsign) -or
        $LateJoinerCallsign -ceq $JoinerCallsign -or $LateJoinerCallsign -ceq $HostCallsign)) {
    throw "-LateJoinerCallsign must name a third player."
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
# Both role copies must be the same retail executable; the hash rides the
# summary as the run's retail identity.
$RetailServerExeSha256 = (Get-FileHash -LiteralPath (Join-Path $ServerDir "Jointops.exe") `
    -Algorithm SHA256).Hash.ToLowerInvariant()
$RetailClientExeSha256 = (Get-FileHash -LiteralPath (Join-Path $ClientDir "Jointops.exe") `
    -Algorithm SHA256).Hash.ToLowerInvariant()
if ($RetailServerExeSha256 -ne $RetailClientExeSha256) {
    throw "RetailServerRoot and RetailClientRoot hold different Jointops.exe images."
}
# The jox01 reference is one exact executable (ADR 0050 decision 2); a stock
# jox01 run on any other image is not evidence for it.
$PinnedJox01ExeSha256 = "9a1035440a53af2057ce0995ac42dced840d3b9fd53c04dc86041a962b84fe57"
if ($Expansion -eq "jox01" -and -not $AllowModifiedRetail -and
        $RetailServerExeSha256 -ne $PinnedJox01ExeSha256) {
    throw "Jointops.exe $RetailServerExeSha256 is not the pinned jox01 reference ($PinnedJox01ExeSha256)."
}
# What built the OpenNova side: this checkout's commit (and whether it was
# clean), its GDExtension and the Godot binary, recorded with every run.
function Get-FileSha256OrNull {
    param([string] $Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
$RepoCommit = (& git -C $Repo rev-parse HEAD 2>$null)
$RepoDirty = [bool] (& git -C $Repo status --porcelain --untracked-files=no 2>$null)
if ($RepoDirty -and -not $AllowDirtyRepo) {
    throw "The checkout has uncommitted tracked changes; a run is evidence for repo_commit alone (-AllowDirtyRepo for a diagnostic run)."
}
$GdextensionDll = Join-Path $Repo "godot\bin\libopennova.windows.template_debug.x86_64.dll"
# A `--path godot` source run loads the editor variant (bin/opennova.gdextension's
# plain windows.debug.x86_64 row), so both variants are recorded.
$GdextensionEditorDll = Join-Path $Repo "godot\bin\libopennova_editor.windows.template_debug.x86_64.dll"
# The onHook proxy (binkw32.dll, forwarding to the renamed original) in both
# copies is one build, recorded as the run's hook identity with the MCP exe.
$OnHookProxySha256 = Get-FileSha256OrNull (Join-Path $ServerDir "binkw32.dll")
$clientProxySha256 = Get-FileSha256OrNull (Join-Path $ClientDir "binkw32.dll")
if (-not $OnHookProxySha256 -or $OnHookProxySha256 -ne $clientProxySha256) {
    throw "The SERVER and CLIENT copies do not hold the same onHook proxy binkw32.dll ($OnHookProxySha256 vs $clientProxySha256)."
}
foreach ($copy in @($ServerDir, $ClientDir)) {
    if (-not (Test-Path -LiteralPath (Join-Path $copy "binkw32_.dll") -PathType Leaf)) {
        throw "$copy has no binkw32_.dll, the original Bink DLL the onHook proxy forwards to."
    }
}
# E3's retail late joiner runs from its own copy when given one, else from the
# CLIENT copy; either way the same retail image under the same hook build.
$LateClientDir = $ClientDir
if ($LateJoiner -and $RetailLateClientRoot) {
    $LateClientDir = (Resolve-Path -LiteralPath $RetailLateClientRoot).Path
    if ([string]::Equals($LateClientDir, $ServerDir, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "RetailLateClientRoot must not be the SERVER copy."
    }
    if ((Get-FileSha256OrNull (Join-Path $LateClientDir "Jointops.exe")) -ne $RetailServerExeSha256 -or
            (Get-FileSha256OrNull (Join-Path $LateClientDir "binkw32.dll")) -ne $OnHookProxySha256 -or
            -not (Test-Path -LiteralPath (Join-Path $LateClientDir "binkw32_.dll") -PathType Leaf)) {
        throw "RetailLateClientRoot must hold the same Jointops.exe and onHook proxy pair as the other copies."
    }
}
# A retail LAN host binds only inside its game.cfg mplanserverportmin..max
# (stock JO:CA: 32768..32787), so a port outside the SERVER copy's own range can
# never host a retail cell.
$serverPortBounds = @{}
foreach ($key in @("mplanserverportmin", "mplanserverportmax")) {
    $match = Select-String -LiteralPath (Join-Path $ServerDir "game.cfg") `
        -Pattern ('^\s*' + $key + '\s*=\s*"?(\d+)"?') | Select-Object -First 1
    if ($match) { $serverPortBounds[$key] = [int] $match.Matches[0].Groups[1].Value }
}
if ($serverPortBounds.Count -eq 2 -and ($Port -lt $serverPortBounds["mplanserverportmin"] -or
        $Port -gt $serverPortBounds["mplanserverportmax"])) {
    throw "Port $Port is outside the SERVER copy's LAN host range $($serverPortBounds["mplanserverportmin"])..$($serverPortBounds["mplanserverportmax"]) (game.cfg mplanserverportmin/max)."
}
# A retail host advertises its game.cfg game_name verbatim (S2C 0x2C/0x7B), so
# the session name every cell expects is the SERVER copy's own, exactly.
$serverGameName = Select-String -LiteralPath (Join-Path $ServerDir "game.cfg") `
    -Pattern '^\s*game_name\s*=\s*"([^"]*)"' | Select-Object -First 1
if (-not $serverGameName) {
    throw "RetailServerRoot game.cfg has no game_name."
}
$retailGameName = $serverGameName.Matches[0].Groups[1].Value
if (-not [string]::Equals($ServerName, $retailGameName, [System.StringComparison]::Ordinal)) {
    throw "HostSessionName '$ServerName' must equal the SERVER copy's game.cfg game_name '$retailGameName' exactly."
}
$StockObserver = -not $AllowModifiedRetail
$RunRoot = Join-Path $Repo ".scratch\runs\$RunId"
$CaptureScript = Join-Path $Repo "scripts\net\capture.ps1"
$HostScript = Join-Path $Repo "scripts\net\host_opennova.ps1"
$JoinScript = Join-Path $Repo "scripts\net\join_opennova.ps1"
$NetLib = Join-Path $Repo "scripts\net\lib.ps1"
$ProbeExe = Join-Path $Repo "build\apps\lan_probe\Release\opennova-lan-probe.exe"
$WireReadyScript = Join-Path $Repo "scripts\net\wait_parity_wire_ready.ps1"

foreach ($required in @(
    $ServerDir, $ClientDir, $McpExe, $GodotLauncher, $CaptureScript, $HostScript,
    $JoinScript, $NetLib, $ProbeExe,
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
# The cell's own console output, errors included, beside its evidence: a suite
# runs cells as child processes whose console it does not keep.
$script:RunnerTranscript = $false
try {
    Start-Transcript -LiteralPath (Join-Path $RunRoot "runner.log") | Out-Null
    $script:RunnerTranscript = $true
}
catch {
    Write-Warning "No runner transcript: $($_.Exception.Message)"
}

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

# The retail joiner's motion exercise on exercised RR/OR runs: forward with a
# right turn, strafe right with a left turn, strafe left with half a right turn
# (the catalog codes forward 152, strafe_right 157, strafe_left 156;
# engine/runtime/controls/controls.cpp), played tick-exact through onHook's
# focus-free scripted input (onhook_play_input, bridge 1.9) rather than
# foreground SendInput. Each leg holds its key and spreads its turn over 20
# relative mouse samples.
$script:RetailInputLegs = @(
    [pscustomobject]@{ name = "forward"; code = 152; ticks = 112; turn_pixels = 320 },
    [pscustomobject]@{ name = "strafe"; code = 157; ticks = 75; turn_pixels = -320 },
    [pscustomobject]@{ name = "return"; code = 156; ticks = 56; turn_pixels = 160 }
)

function Get-RetailInputSteps {
    $steps = [System.Collections.Generic.List[object]]::new()
    $tick = 10
    foreach ($leg in $script:RetailInputLegs) {
        $steps.Add([pscustomobject]@{ tick = $tick; down = $leg.code })
        $samples = 20
        $per = [int][Math]::Truncate($leg.turn_pixels / $samples)
        $remainder = $leg.turn_pixels - $per * $samples
        for ($i = 0; $i -lt $samples; ++$i) {
            $dx = $per + $(if ($i -eq $samples - 1) { $remainder } else { 0 })
            $at = $tick + [int][Math]::Floor($i * $leg.ticks / $samples)
            $steps.Add([pscustomobject]@{ tick = $at; look_px = @($dx, 0) })
        }
        $tick += $leg.ticks
        $steps.Add([pscustomobject]@{ tick = $tick; up = $leg.code })
        $tick += 1
    }
    $steps.Add([pscustomobject]@{ tick = $tick + 5; end = $true })
    return $steps.ToArray()
}

function Invoke-RetailInputExercise {
    param(
        [Parameter(Mandatory = $true)] [string] $InstanceId,
        [Parameter(Mandatory = $true)] [int] $ProcessId
    )
    if (-not $ExerciseInput -or $Topology -notin @("RR", "OR")) {
        throw "Retail input witnesses are valid only for exercised RR/OR runs"
    }
    $path = Join-Path $RunRoot "retail-input.json"
    if (Test-Path -LiteralPath $path) {
        throw "Retail input witness must be create-new: $path"
    }
    $steps = Get-RetailInputSteps
    $started = [DateTime]::UtcNow
    $played = Get-StructuredResult (Invoke-OnHookTool -Name "onhook_play_input" -Arguments @{
        instance_id = $InstanceId
        steps = $steps
        wait = $true
        # Generous: two retail windows on one machine tick well below 62 Hz.
        timeout_ms = 60000
    })
    $unapplied = @($played.steps | Where-Object {
        [string] $_.kind -ne "end" -and $null -eq $_.applied_logic_tick
    })
    if ([string] $played.state -ne "done" -or $unapplied.Count -gt 0) {
        throw "Retail input exercise did not play on instance ${InstanceId}: state=$($played.state) result=$($played.result) unapplied=$($unapplied.Count)"
    }
    [pscustomobject]@{
        schema = "opennova.retail-input.v2"
        run_id = $RunId
        topology = $Topology
        instance_id = $InstanceId
        pid = $ProcessId
        driver = "onhook_play_input"
        started_utc = $started.ToString("o")
        completed_utc = [DateTime]::UtcNow.ToString("o")
        legs = $script:RetailInputLegs
        steps = $steps
        record = $played
    } | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $path -Encoding UTF8
    $script:InputWitness = [pscustomobject]@{
        path = $path
        sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        pid = $ProcessId
        instance_id = $InstanceId
        state = [string] $played.state
        applied_steps = @($played.steps).Count
    }
}

function Bind-EffectiveRetailConfig {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet("server", "client", "late_client")]
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

# The desktop the game windows share. Retail centres the cursor on its own
# window every frame while it holds focus (Game_ProcessMainFrame
# @0x526401..0x526458 -> Input_PumpAndCenterCursor @0x7616D0: SetCursorPos to
# the client origin + 320,240, then the delta is the last WM_MOUSEMOVE minus
# 320,240), and confines the cursor system-wide to its window rect whenever it
# leaves a menu, with no focus check (UI_CloseMenuScreen ->
# Game_ClipCursorToGameWindow @0x4983C0, ClipCursor @0x4983D8). A clip another
# window left (a joiner still at its small launch size) clamps the focused
# window's centring, so it reads one constant delta every frame: its body's
# yaw steps every frame and its pitch pins at the clamp (the R1 rows of i1 and
# c1, reproduced by parity-20261020-r1probe2). The runner releases any clip as
# the steady window opens and records the desktop at both ends of the window;
# a cell whose focused retail window could not centre is marked unclean.
$script:DesktopWitness = [ordered]@{}
function Initialize-DesktopProbe {
    if ('ParityDesktop' -as [type]) { return }
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class ParityDesktop {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool GetClipCursor(out RECT r);
    [DllImport("user32.dll", EntryPoint = "ClipCursor")] public static extern bool ReleaseClip(IntPtr r);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
    public static List<IntPtr> VisibleWindowsOf(uint pid) {
        var found = new List<IntPtr>();
        EnumWindows((h, l) => {
            uint owner;
            GetWindowThreadProcessId(h, out owner);
            if (owner == pid && IsWindowVisible(h)) found.Add(h);
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
'@
}

function Get-DesktopState {
    Initialize-DesktopProbe
    $foreground = [ParityDesktop]::GetForegroundWindow()
    $foregroundPid = [uint32]0
    $null = [ParityDesktop]::GetWindowThreadProcessId($foreground, [ref] $foregroundPid)
    $clip = New-Object ParityDesktop+RECT
    $null = [ParityDesktop]::GetClipCursor([ref] $clip)
    $roles = @(
        @{ role = "retail_host"; process = $script:RetailHostProcess; retail = $true },
        @{ role = "retail_joiner"; process = $script:RetailJoinerProcess; retail = $true },
        @{ role = "retail_late_joiner"; process = $script:RetailLateJoinerProcess; retail = $true },
        @{ role = "opennova_host"; process = $script:OpenNovaHost; retail = $false },
        @{ role = "opennova_joiner"; process = $script:OpenNovaJoiner; retail = $false }
    )
    # A retail process the cell holds no handle for (a path that keeps only the
    # tool's pid) still centres and clips: every Jointops.exe counts.
    $known = @($roles | Where-Object { $_.process } | ForEach-Object { [int] $_.process.Id })
    foreach ($other in @(Get-Process -Name "Jointops" -ErrorAction SilentlyContinue)) {
        if ($known -notcontains [int] $other.Id) {
            $roles += @{ role = "retail"; process = $other; retail = $true }
        }
    }
    $windows = @()
    $clean = $true
    foreach ($entry in $roles) {
        if (-not $entry.process) { continue }
        foreach ($window in [ParityDesktop]::VisibleWindowsOf([uint32] $entry.process.Id)) {
            $rect = New-Object ParityDesktop+RECT
            $null = [ParityDesktop]::GetWindowRect($window, [ref] $rect)
            $origin = New-Object ParityDesktop+POINT
            $null = [ParityDesktop]::ClientToScreen($window, [ref] $origin)
            $focused = [uint32] $entry.process.Id -eq $foregroundPid
            # Retail's centring point; only the focused retail window centres.
            $warp = if ($entry.retail) { @(($origin.X + 320), ($origin.Y + 240)) } else { $null }
            $warpInClip = if ($warp) {
                $warp[0] -ge $clip.L -and $warp[0] -lt $clip.R -and $warp[1] -ge $clip.T -and $warp[1] -lt $clip.B
            } else { $null }
            if ($entry.retail -and $focused -and -not $warpInClip) { $clean = $false }
            $windows += [pscustomobject][ordered]@{
                role = $entry.role
                hwnd = [int64] $window
                retail = [bool] $entry.retail
                pid = $entry.process.Id
                rect = @($rect.L, $rect.T, $rect.R, $rect.B)
                client_origin = @($origin.X, $origin.Y)
                warp = $warp
                warp_in_clip = $warpInClip
                focused = $focused
            }
        }
    }
    return [pscustomobject][ordered]@{
        utc = [DateTime]::UtcNow.ToString("o")
        foreground_pid = $foregroundPid
        clip = @($clip.L, $clip.T, $clip.R, $clip.B)
        windows = $windows
        clean = $clean
    }
}

# As the steady window opens: what the desktop held, then any clip released
# (ClipCursor(NULL), the desktop's state, not the game's code), and every
# retail window told it has no focus (WM_KILLFOCUS, retail's focus flag
# cleared @0x762670): a window the guard could not take the foreground from,
# or one that took focus again while it loaded, centres no more.
function Reset-DesktopCursor {
    $before = Get-DesktopState
    $released = [ParityDesktop]::ReleaseClip([IntPtr]::Zero)
    $unfocused = 0
    foreach ($window in @($before.windows | Where-Object { $_.retail })) {
        if ([ParityDesktop]::PostMessage([IntPtr] $window.hwnd, 0x0008, [IntPtr]::Zero, [IntPtr]::Zero)) { $unfocused++ }
    }
    $after = Get-DesktopState
    $script:DesktopWitness.steady_start = [pscustomobject][ordered]@{
        found = $before
        released = [bool] $released
        retail_windows_unfocused = $unfocused
        after = $after
    }
    Write-Host ("PARITY_RUN_NOTE desktop at the steady start: clip {0} -> {1}, foreground pid {2}" -f
        ($before.clip -join ","), ($after.clip -join ","), $after.foreground_pid)
}

# The desktop guard: for the whole cell no game window keeps the foreground.
# Without real focus a retail window neither centres nor reads a cursor delta
# (the gate above); scripted input brings its own virtual focus and zeroes
# the real delta when the window is not really focused. So a clip any window
# leaves cannot reach a body's look. The guard hands the foreground to the
# shell's desktop window (GetShellWindow) whenever a Jointops.exe or Godot
# window holds it, and
# logs each hand-off.
$script:DesktopGuard = $null
$script:DesktopGuardLog = ""
function Start-DesktopGuard {
    $script:DesktopGuardLog = Join-Path $RunRoot "desktop-guard.log"
    $script:DesktopGuard = Start-Job -ArgumentList $script:DesktopGuardLog -ScriptBlock {
        param([string] $Log)
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class ParityDesktopGuard {
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetShellWindow();
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    // A background process may not take the foreground once a launched game
    // has claimed it: joined to the foreground thread's input, it may.
    public static bool HandOff(IntPtr foreground, IntPtr target) {
        uint owner;
        uint thread = GetWindowThreadProcessId(foreground, out owner);
        uint self = GetCurrentThreadId();
        bool attached = thread != 0 && thread != self && AttachThreadInput(self, thread, true);
        bool ok = SetForegroundWindow(target);
        if (attached) AttachThreadInput(self, thread, false);
        return ok;
    }
}
'@
        $desktop = [ParityDesktopGuard]::GetShellWindow()
        Add-Content -LiteralPath $Log -Value "$([DateTime]::UtcNow.ToString('o')) guard start desktop=$desktop"
        # Once per foreground spell: hand the foreground to the desktop; when
        # Windows refuses (a launched game keeps it), tell the window it lost
        # focus (WM_KILLFOCUS: retail clears its focus flag, @0x762670), as an
        # alt-tab away would.
        $handled = [IntPtr]::Zero
        while ($true) {
            $foreground = [ParityDesktopGuard]::GetForegroundWindow()
            if ($foreground -ne $handled) {
                $owner = [uint32]0
                $null = [ParityDesktopGuard]::GetWindowThreadProcessId($foreground, [ref] $owner)
                $name = try { (Get-Process -Id $owner -ErrorAction Stop).ProcessName } catch { "" }
                if ($name -eq "Jointops" -or $name -like "Godot*") {
                    $ok = [ParityDesktopGuard]::HandOff($foreground, $desktop)
                    Start-Sleep -Milliseconds 50
                    $still = [ParityDesktopGuard]::GetForegroundWindow() -eq $foreground
                    $killed = $false
                    if ($still) {
                        $killed = [ParityDesktopGuard]::PostMessage($foreground, 0x0008, [IntPtr]::Zero, [IntPtr]::Zero)
                    }
                    Add-Content -LiteralPath $Log -Value ("{0} foreground {1} pid={2} hwnd={3} -> desktop ok={4} kept={5} killfocus={6}" -f
                        [DateTime]::UtcNow.ToString('o'), $name, $owner, $foreground, $ok, $still, $killed)
                    $handled = $foreground
                }
            }
            Start-Sleep -Milliseconds 100
        }
    }
}

function Stop-DesktopGuard {
    if (-not $script:DesktopGuard) { return }
    Stop-Job -Job $script:DesktopGuard -ErrorAction SilentlyContinue
    Remove-Job -Job $script:DesktopGuard -Force -ErrorAction SilentlyContinue
    $script:DesktopGuard = $null
}

function Complete-DesktopWitness {
    $state = Get-DesktopState
    $script:DesktopWitness.steady_end = $state
    if (-not $state.clean) {
        Write-Host ("PARITY_RUN_NOTE desktop UNCLEAN at the steady end: the focused retail window's centre lies outside the cursor clip {0}" -f ($state.clip -join ","))
    }
}

function Begin-SteadyWindow {
    if ($script:SteadyStartedUtc) { throw "Steady window was already started" }
    try { Reset-DesktopCursor }
    catch { Write-Host "PARITY_RUN_NOTE desktop probe failed at the steady start: $($_.Exception.Message)" }
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

# The SERVER copy's mutable files. Both hosts run in that copy: retail's
# Jointops.exe in its install, ours through --working-dir. Each reads game.cfg
# and player.sav at its start and writes them, and weapon.sav (the root's and
# the expansion's), at its save points: retail saves at its exit
# (Game_SaveConfig from Game_Run). Every cell starts from the same bytes:
# game.cfg's are the case table's pin (-ServerGameCfgSha256), and the cleanup
# writes each file's start bytes back once every process has stopped (and
# deletes one the cell created).
$script:ServerCopyFiles = @()
function Get-BytesSha256([byte[]] $Bytes) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($Bytes)) -replace '-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}

function Save-ServerCopyFiles {
    $relative = @("game.cfg", "player.sav", "weapon.sav")
    if ($Expansion) { $relative += (Join-Path (Join-Path "expansion" $Expansion) "weapon.sav") }
    $script:ServerCopyFiles = @(foreach ($rel in $relative) {
        $path = Join-Path $ServerDir $rel
        $exists = Test-Path -LiteralPath $path -PathType Leaf
        $bytes = if ($exists) { [IO.File]::ReadAllBytes($path) } else { $null }
        $sha = if ($exists) { Get-BytesSha256 $bytes } else { "" }
        [pscustomobject][ordered]@{
            file = $rel
            path = $path
            bytes = $bytes
            sha256 = $sha
            effective_sha256 = $sha
            written_during_cell = $false
            restored = $false
        }
    })
    $cfg = $script:ServerCopyFiles | Where-Object { $_.file -eq "game.cfg" }
    if (-not $cfg.sha256) { throw "The SERVER copy has no game.cfg: $($cfg.path)" }
    if ($ServerGameCfgSha256 -and $cfg.sha256 -cne $ServerGameCfgSha256) {
        throw "SERVER game.cfg is $($cfg.sha256), not the pinned $ServerGameCfgSha256 (a cell left it changed, or the copy moved on: restore it, or re-pin the case table)."
    }
}

function Restore-ServerCopyFiles {
    if (-not $script:ServerCopyFiles.Count) { return }
    foreach ($process in @($script:RetailHostProcess, $script:RetailProcess, $script:OpenNovaHost)) {
        if ($process) {
            try { $null = $process.WaitForExit(15000) } catch {}
        }
    }
    $failures = @()
    foreach ($entry in $script:ServerCopyFiles) {
        $present = Test-Path -LiteralPath $entry.path -PathType Leaf
        $now = if ($present) { (Get-FileHash -LiteralPath $entry.path -Algorithm SHA256).Hash.ToLowerInvariant() } else { "" }
        $entry.written_during_cell = $now -ne $entry.effective_sha256
        if ($now -eq $entry.sha256) { $entry.restored = $true; continue }
        if ($null -eq $entry.bytes) {
            Remove-Item -LiteralPath $entry.path -Force
        } else {
            [IO.File]::WriteAllBytes($entry.path, $entry.bytes)
        }
        $after = if (Test-Path -LiteralPath $entry.path -PathType Leaf) { (Get-FileHash -LiteralPath $entry.path -Algorithm SHA256).Hash.ToLowerInvariant() } else { "" }
        if ($after -ne $entry.sha256) {
            $failures += "$($entry.file) left $after, not the cell's start $($entry.sha256)"
        } else {
            $entry.restored = $true
        }
    }
    if ($script:ServerGameCfgOverride) {
        $script:ServerGameCfgOverride.restored = [bool] ($script:ServerCopyFiles | Where-Object { $_.file -eq "game.cfg" }).restored
    }
    if ($failures.Count) { throw ("SERVER copy restore: " + ($failures -join "; ")) }
}

# E2's time limit: the SERVER copy's game.cfg time_limit for this cell (the
# retail host reads the file at its start, the OpenNova host runs in the same
# copy). The cleanup's restore takes the copy back to the cell's start bytes.
$script:ServerGameCfgOverride = $null
function Set-ServerTimeLimit {
    $cfg = $script:ServerCopyFiles | Where-Object { $_.file -eq "game.cfg" }
    $path = $cfg.path
    $latin1 = [Text.Encoding]::GetEncoding(28591)
    $original = $cfg.bytes
    $text = $latin1.GetString($original)
    $found = [regex]::Matches($text, '(?m)^[ \t]*time_limit[ \t]*=[ \t]*"?(\d+)"?')
    if ($found.Count -ne 1) {
        throw "SERVER game.cfg holds $($found.Count) time_limit lines; the time limit needs exactly one."
    }
    $digits = $found[0].Groups[1]
    $updated = $text.Substring(0, $digits.Index) + [string] $TimeLimitMinutes +
        $text.Substring($digits.Index + $digits.Length)
    $bytes = $latin1.GetBytes($updated)
    $script:ServerGameCfgOverride = [pscustomobject][ordered]@{
        path = $path
        key = "time_limit"
        value = $TimeLimitMinutes
        original_value = [int] $digits.Value
        original_sha256 = $cfg.sha256
        effective_sha256 = Get-BytesSha256 $bytes
        restored = $false
    }
    $cfg.effective_sha256 = $script:ServerGameCfgOverride.effective_sha256
    [IO.File]::WriteAllBytes($path, $bytes)
    Write-Host "PARITY_RUN_NOTE SERVER game.cfg time_limit $($digits.Value) -> $TimeLimitMinutes for this cell"
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

# One scripted stage on the joiner, waited to its end: the setup pose first
# when one is given, then the steps. A retail joiner takes the pose through
# onhook_load_debug_snapshot and the steps through onhook_play_input
# (retail's own key and mouse handlers at the Input_ProcessFrame seams); an
# OpenNova joiner takes both through the scenario_play probe (ScriptedInput
# at the ControlsModel device seam). Both report each step's applied tick.
# One opennova.debug_snapshot.v1 file for a scenario pose, create-new.
function Write-ScenarioPoseSnapshot {
    param(
        [Parameter(Mandatory = $true)] [string] $Path,
        [Parameter(Mandatory = $true)] $Pose
    )
    if (Test-Path -LiteralPath $Path) {
        throw "Scenario pose snapshot must be create-new: $Path"
    }
    $position = @($Pose.position_bms)
    [pscustomobject]@{
        schema = "opennova.debug_snapshot.v1"
        player = [pscustomobject]@{
            position_bms = [pscustomobject]@{
                x = $position[0]; y = $position[1]; z = $position[2]
            }
            orientation_mission_deg = [pscustomobject]@{
                yaw = $Pose.yaw; pitch = $Pose.pitch
            }
        }
    } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $Path -Encoding ASCII
}

# E5: the scenario's staging poses, applied by the retail host to each
# listed slot (onhook_load_debug_snapshot target_slot, opennova-int #74), in
# order, before the actor's setup and steps.
function Invoke-ScenarioStaging {
    param([string] $RetailHostInstanceId = "")
    $spec = $script:ScenarioSpec
    if (-not ($spec.PSObject.Properties.Name -contains "staging")) { return $null }
    if (-not $RetailHostInstanceId) {
        throw "Scenario staging on $Topology needs the retail host's instance id"
    }
    $applied = [System.Collections.Generic.List[object]]::new()
    $index = 0
    foreach ($entry in @($spec.staging)) {
        $path = Join-Path $RunRoot "scenario-staging-$index.debug_snapshot.json"
        Write-ScenarioPoseSnapshot -Path $path -Pose $entry.pose
        $applied.Add((Get-StructuredResult (Invoke-OnHookTool -Name "onhook_load_debug_snapshot" -Arguments @{
            instance_id = $RetailHostInstanceId
            path = $path
            target_slot = [int] $entry.slot
        })))
        ++$index
    }
    return $applied.ToArray()
}

function Invoke-ScenarioStage {
    param(
        [Parameter(Mandatory = $true)] [object[]] $Steps,
        $Setup = $null,
        [string] $RetailInstanceId = ""
    )
    $hasPose = $null -ne $Setup -and $null -ne $Setup.pose
    $actor = [string] $script:ScenarioSpec.actor
    $retailActor = ($actor -eq "joiner" -and $Topology -in @("RR", "OR")) -or
        ($actor -eq "host" -and $Topology -in @("RR", "RO"))
    # Generous: two retail windows on one machine tick well below 62 Hz.
    $timeoutSeconds = [int] ($Steps[-1].tick / 10.0 + 60)
    if ($retailActor) {
        if (-not $RetailInstanceId) {
            throw "Scenario play on $Topology needs the retail $actor's instance id"
        }
        $setupResult = $null
        if ($hasPose) {
            $snapshotPath = Join-Path $RunRoot "scenario-setup.debug_snapshot.json"
            Write-ScenarioPoseSnapshot -Path $snapshotPath -Pose $Setup.pose
            $setupResult = Get-StructuredResult (Invoke-OnHookTool -Name "onhook_load_debug_snapshot" -Arguments @{
                instance_id = $RetailInstanceId
                path = $snapshotPath
            })
        }
        $played = Get-StructuredResult (Invoke-OnHookTool -Name "onhook_play_input" -Arguments @{
            instance_id = $RetailInstanceId
            steps = $Steps
            wait = $true
            timeout_ms = $timeoutSeconds * 1000
        })
        # Retail runs no input frame while a menu that does not pass input
        # through is up; a run that waits there past its end step expires
        # rather than finishing. Every step before the end must still apply.
        $unapplied = @($played.steps | Where-Object {
            [string] $_.kind -ne "end" -and $null -eq $_.applied_logic_tick
        })
        if ([string] $played.state -notin @("done", "expired") -or $unapplied.Count -gt 0) {
            throw "Retail scenario $($script:ScenarioSpec.name) did not play: state=$($played.state) result=$($played.result) unapplied=$($unapplied.Count)"
        }
        return [pscustomobject]@{ driver = "onhook_play_input"; setup = $setupResult; record = $played }
    }
    $stage = @{ steps = $Steps }
    if ($null -ne $Setup) { $stage.setup = $Setup }
    $port = if ($actor -eq "host") { $HostMcpPort } else { $JoinerMcpPort }
    $process = if ($actor -eq "host") { $script:OpenNovaHost } else { $script:OpenNovaJoiner }
    $status = Invoke-GameProbe -Port $port -Name "scenario_play" -Process $process `
        -TimeoutSeconds $timeoutSeconds -PollMs 500 -Arguments @{
            scenario = $stage
            wait = $true
        }
    if ([string] $status.state -ne "passed" -or -not $status.verdict -or
            -not [bool] $status.verdict.ok) {
        $why = if ($status.verdict) { [string] $status.verdict.summary } else { [string] $status.error }
        throw "OpenNova scenario $($script:ScenarioSpec.name) did not play: state=$($status.state) $why"
    }
    $record = $status.verdict.data
    return [pscustomobject]@{ driver = "scenario_play"; setup = $record.setup; record = $record }
}

# Plays -Scenario on its actor (the joiner or the host) right after the
# steady window starts: its prelude stage when it has one (e.g. the deploy
# key on a deploy_hold run), then the setup pose and the steps, then the
# settle. A pose sticks only on the authority's own player: a client snaps
# its players to the host's replicated pose past 2 units
# [orig: Entity_UpdateInfantryPlayerBody @0x4B42C1..0x4B434A, skipped on the
#  authority @0x4B432A], so a posed scenario's actor is the host.
function Invoke-ScenarioIfRequested {
    param(
        [string] $RetailJoinerInstanceId = "",
        [string] $RetailHostInstanceId = ""
    )
    if (-not $script:ScenarioSpec) { return }
    if (-not $script:SteadyStartedUtc -or $script:SteadyCompletedUtc) {
        throw "Scenario play must follow the steady-window start"
    }
    if ($script:ScenarioWitness) {
        throw "Scenario was already played in run $RunId"
    }
    $spec = $script:ScenarioSpec
    $retailInstanceId = if ([string] $spec.actor -eq "host") {
        $RetailHostInstanceId
    } else {
        $RetailJoinerInstanceId
    }
    $startedUtc = [DateTime]::UtcNow.ToString("o")
    $prelude = $null
    if ($spec.PSObject.Properties.Name -contains "prelude") {
        $prelude = Invoke-ScenarioStage -Steps @($spec.prelude) `
            -RetailInstanceId $retailInstanceId
    }
    $staging = Invoke-ScenarioStaging -RetailHostInstanceId $RetailHostInstanceId
    $setup = if ($spec.PSObject.Properties.Name -contains "setup") { $spec.setup } else { $null }
    $main = Invoke-ScenarioStage -Steps @($spec.steps) -Setup $setup `
        -RetailInstanceId $retailInstanceId
    $playedUtc = [DateTime]::UtcNow.ToString("o")
    Start-Sleep -Seconds ([int] $spec.settle_seconds)
    # The run keeps the exact script it played; the comparator checks every
    # run played the same steps and reads the expectations from the source.
    $playedCopy = Join-Path $RunRoot "scenario.json"
    if (Test-Path -LiteralPath $playedCopy) {
        throw "Played scenario copy must be create-new: $playedCopy"
    }
    Copy-Item -LiteralPath $Scenario -Destination $playedCopy
    $script:ScenarioWitness = [pscustomobject]@{
        schema = "opennova.parity-scenario.v1"
        name = [string] $spec.name
        source = (Resolve-Path -LiteralPath $Scenario).Path
        path = $playedCopy
        sha256 = (Get-FileHash -LiteralPath $playedCopy -Algorithm SHA256).Hash.ToLowerInvariant()
        actor = [string] $spec.actor
        driver = $main.driver
        started_utc = $startedUtc
        played_utc = $playedUtc
        settled_utc = [DateTime]::UtcNow.ToString("o")
        settle_seconds = [int] $spec.settle_seconds
        prelude = $(if ($prelude) { $prelude.record } else { $null })
        staging = $staging
        setup = $main.setup
        record = $main.record
    }
}

function Complete-SteadyWindow {
    if (-not $script:SteadyStartedUtc -or $script:SteadyCompletedUtc) {
        throw "Steady window completion is out of order"
    }
    $script:SteadyCompletedUtc = [DateTime]::UtcNow.ToString("o")
    try { Complete-DesktopWitness }
    catch { Write-Host "PARITY_RUN_NOTE desktop probe failed at the steady end: $($_.Exception.Message)" }
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
        [switch] $RequireInMatch,
        # After a scenario played, a deploy_hold joiner may have deployed.
        [switch] $AllowScenarioDeploy
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
            ([string] $state.phase -ne "loading" -or [bool] $state.local_player) -and
            -not ($AllowScenarioDeploy -and [string] $state.phase -eq "in_match" -and
                [bool] $state.local_player)) {
        return $false
    }
    return $true
}

function Wait-RetailProcessForPeer {
    param(
        [Parameter(Mandatory = $true)] [int] $ProcessId,
        [Parameter(Mandatory = $true)] [ValidateSet("host", "joiner")] [string] $Role,
        [int] $TimeoutSeconds = 240,
        [switch] $RequireInMatch,
        [switch] $AllowScenarioDeploy
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
                    -RequireInMatch:$RequireInMatch -AllowScenarioDeploy:$AllowScenarioDeploy) {
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

# Retail is launched only through onhook-mcp (bridge protocol 1.8 or later):
# its role tools return the exact process (pid + instance_id), render the
# role's onhook.cfg from their arguments, and report whether the loaded hook
# runs retail stock. A launch missing any of that stops the cell instead of
# guessing at a process or a configuration.
function Assert-RetailStock {
    param(
        [Parameter(Mandatory = $true)] [string] $Tool,
        [Parameter(Mandatory = $true)] $Reported
    )
    if ($StockObserver -and -not ($Reported -eq $true)) {
        throw "$Tool did not confirm stock_observer; deploy onhook (bridge protocol 1.8+) to both retail copies, or pass -AllowModifiedRetail for a diagnostic run."
    }
}

function Assert-OnHookRoleLaunch {
    param(
        [Parameter(Mandatory = $true)] [string] $Tool,
        [Parameter(Mandatory = $true)] $Result
    )
    foreach ($name in @("pid", "instance_id", "run_id", "capture_path", "hook_config_path")) {
        if (-not ($Result.PSObject.Properties.Name -contains $name) -or
                [string]::IsNullOrWhiteSpace([string] $Result.$name)) {
            throw "onhook-mcp $Tool returned no '$name'; deploy onhook-mcp from bridge protocol 1.8 or later."
        }
    }
    if ([int] $Result.pid -le 0) {
        throw "onhook-mcp $Tool returned pid $($Result.pid)."
    }
    $reported = if ($Result.PSObject.Properties.Name -contains "stock_observer") {
        $Result.stock_observer
    } else { $null }
    Assert-RetailStock -Tool $Tool -Reported $reported
}

# onhook_host_lan's arguments for this cell's retail host; the rotation key
# only when the cell has a tail, so every other cell's call is unchanged.
function Get-RetailHostArguments {
    param([Parameter(Mandatory = $true)] [string] $OutputDir)
    $arguments = @{
        mission = $Mission
        game_dir = $ServerDir
        expansion = $Expansion
        port = $Port
        callsign = $HostCallsign
        game_type = [string] $GameType
        max_players = 4
        run_id = "$RunId-host"
        output_dir = $OutputDir
        capture = $true
        windowed = $true
        allow_many = $true
        stock_observer = $StockObserver
        wait_timeout_ms = 240000
    }
    if ($RotationList.Count -gt 0) { $arguments.rotation = @($RotationList) }
    return $arguments
}

# The hook echoes the rotation it built; a hook pair without it (before
# opennova-int's host rotation) cannot host an E2 cell.
function Assert-RetailHostRotation {
    param([Parameter(Mandatory = $true)] $Result)
    if ($RotationList.Count -eq 0) { return }
    $echo = if ($Result.PSObject.Properties.Name -contains "rotation") {
        @($Result.rotation | ForEach-Object { [string] $_ })
    } else { $null }
    if ($null -eq $echo -or ($echo -join ',') -cne ($RotationList -join ',')) {
        throw "onhook_host_lan did not build the requested rotation ($($RotationList -join ',')); deploy an onHook pair with host rotation."
    }
}

# E3: the retail late joiner, launched during the steady window and owned by
# run id like the pair's roles (its capture drains and its process stops
# through onhook_stop_run; the cleanup stops it with every owned run).
$script:LateJoinerWitness = $null
$script:RetailLateJoinerProcess = $null
function Start-RetailLateJoiner {
    if (-not $LateJoiner) { return }
    $lateDir = Join-Path $RunRoot "late"
    New-Item -ItemType Directory -Path $lateDir -Force | Out-Null
    $requested = [DateTime]::UtcNow.ToString("o")
    $result = Get-StructuredResult (Invoke-OnHookTool -Name "onhook_join_lan" -Arguments @{
        game_dir = $LateClientDir
        expansion = $Expansion
        join_address = "127.0.0.1"
        port = $Port
        callsign = $LateJoinerCallsign
        run_id = "$RunId-late"
        output_dir = $lateDir
        capture = $true
        windowed = $true
        allow_many = $true
        stock_observer = $StockObserver
        readiness = $(if ($ReadinessMode -eq "in_match") { "in_match" } else { "peer" })
        wait_timeout_ms = 240000
    })
    Assert-OnHookRoleLaunch -Tool "onhook_join_lan (late joiner)" -Result $result
    $script:OwnedRetailRuns.Add([string] $result.run_id)
    $script:RetailLateJoinerProcess = Get-Process -Id ([int] $result.pid) -ErrorAction Stop
    Bind-EffectiveRetailConfig -Role late_client -Source (Join-Path $LateClientDir "onhook.cfg")
    $status = if ($ReadinessMode -eq "in_match") {
        Wait-RetailRunForPeer -OwnedRunId ([string] $result.run_id)
    } else {
        Get-StructuredResult (Invoke-OnHookTool -Name "onhook_run_status" -Arguments @{
            run_id = [string] $result.run_id
        })
    }
    $state = $status.instance.run_state
    if ([string] $state.failure -ne "none" -or -not [bool] $state.peer -or
            [int] $state.local_port -le 0) {
        throw "Retail late joiner $($result.run_id) is not a connected peer: phase $($state.phase), failure $($state.failure)"
    }
    $script:LateJoinerWitness = [pscustomobject][ordered]@{
        kind = "retail"
        callsign = $LateJoinerCallsign
        run_id = [string] $result.run_id
        pid = [int] $result.pid
        instance_id = [string] $result.instance_id
        game_dir = $LateClientDir
        capture_path = [string] $result.capture_path
        # The comparator's session selector: the late joiner's client port.
        client_port = [int] $state.local_port
        launch_requested_utc = $requested
        ready_utc = [DateTime]::UtcNow.ToString("o")
        ready_state = $state
        final_state = $null
    }
}

function Complete-RetailLateJoiner {
    if (-not $script:LateJoinerWitness) { return }
    $script:RetailLateJoinerProcess.Refresh()
    if ($script:RetailLateJoinerProcess.HasExited) {
        throw "The retail late joiner exited during the steady window"
    }
    $runId = [string] $script:LateJoinerWitness.run_id
    $status = if ($ReadinessMode -eq "in_match") {
        Wait-RetailRunForPeer -OwnedRunId $runId
    } else {
        Get-StructuredResult (Invoke-OnHookTool -Name "onhook_run_status" -Arguments @{
            run_id = $runId
        })
    }
    $script:LateJoinerWitness.final_state = $status.instance.run_state
    Stop-OwnedRetailRun -OwnedRunId $runId
    $script:RetailLateJoinerProcess = $null
}

function Start-RetailHostExact {
    $hostDir = Join-Path $RunRoot "host"
    New-Item -ItemType Directory -Path $hostDir -Force | Out-Null
    $result = Get-StructuredResult (Invoke-OnHookTool -Name "onhook_host_lan" `
        -Arguments (Get-RetailHostArguments -OutputDir $hostDir))
    Assert-OnHookRoleLaunch -Tool "onhook_host_lan" -Result $result
    $script:OwnedRetailRuns.Add([string] $result.run_id)
    Assert-RetailHostRotation -Result $result
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
        stock_observer = $StockObserver
        # deploy_hold returns once the session is connected: an A&S joiner
        # holds on the deploy screen with no local Person, which the
        # in_match readiness waits for.
        readiness = $(if ($ReadinessMode -eq "in_match") { "in_match" } else { "peer" })
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
    $quitAcknowledgedUtc = [DateTime]::UtcNow.ToString("o")
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
    $acknowledged = ConvertFrom-RunnerStrictUtcTimestamp $quitAcknowledgedUtc
    $completed = ConvertFrom-RunnerStrictUtcTimestamp ([string] $shutdown.completed_utc)
    $processStarted = [DateTimeOffset]::new($process.StartTime.ToUniversalTime())
    # Each leg within its own budget: the quit leg is Stop-GameViaMcp's two
    # endpoint calls (10 s each) and its 20 s exit wait; the evidence leg is
    # Stop-OpenNovaProcess's two launcher waits (ForceTimeoutMs, 5 s each).
    if ($requested -lt $processStarted -or $acknowledged -lt $requested -or
            $acknowledged -gt $requested.AddSeconds(40) -or $completed -lt $acknowledged -or
            $completed -gt $acknowledged.AddSeconds(10)) {
        throw "OpenNova joiner shutdown timestamps drifted: requested $requestedUtc, quit acknowledged $quitAcknowledgedUtc, completed $($shutdown.completed_utc)"
    }
    $script:OpenNovaJoinerShutdownWitness = [pscustomobject][ordered]@{
        schema = "opennova.parity-joiner-shutdown.v2"
        run_id = $RunId
        topology = $Topology
        process_id = [int] $process.Id
        mcp_port = $JoinerMcpPort
        requested_utc = $requestedUtc
        quit_acknowledged_utc = $quitAcknowledgedUtc
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
        -WorkingDir $ServerDir -Expansion $Expansion `
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
    $joinTarget = if ($JoinerDial -eq "browse") { "browse:$Port" } else { "127.0.0.1:$Port" }
    $script:OpenNovaJoiner = Start-OpenNovaProcess `
        -GodotArguments @(
            "--windowed", "--resolution", $Resolution,
            "--log-file", (Join-Path $joinerDir "godot.log")
        ) `
        -GameArguments (@(
            "--lan-join", $joinTarget,
            "--callsign", $JoinerCallsign
        ) + @(
            "--resource-dir", $ClientDir, "/exp", $Expansion
        )) `
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
        [switch] $AllowPostDeathRedeploy,
        [switch] $AllowScenarioDeploy
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
            -AllowPostDeathRedeploy:$AllowPostDeathRedeploy `
            -AllowScenarioDeploy:$AllowScenarioDeploy
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
    Write-Host "PARITY_RUN_BEGIN topology=$Topology mission=$Mission game_type=$GameType port=$Port resolution=$Resolution joiner_dial=$JoinerDial run=$RunId"
    Assert-UdpPortUnowned
    Save-ServerCopyFiles
    Start-DesktopGuard
    if ($TimeLimitMinutes -gt 0) { Set-ServerTimeLimit }
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
                Invoke-ScenarioIfRequested -RetailJoinerInstanceId ([string] $retailJoiner.instance_id) `
                    -RetailHostInstanceId $hostInstanceId
                Start-RetailLateJoiner
                Wait-UntilSteadyDeadline
                $hostInstance = Wait-RetailProcessForPeer `
                    -ProcessId $script:RetailHostProcess.Id -Role host
                $joinerInstance = Wait-RetailProcessForPeer `
                    -ProcessId $script:RetailJoinerProcess.Id -Role joiner `
                    -AllowScenarioDeploy:([bool] $script:ScenarioSpec)
                $script:Acceptance = [pscustomobject]@{
                    host = $hostInstance.run_state
                    joiner = $joinerInstance.run_state
                    host_pid = $script:RetailHostProcess.Id
                    joiner_pid = $script:RetailJoinerProcess.Id
                    wire_ready = $script:WireReadyWitness
                }
                Complete-RetailLateJoiner
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
            $pairArguments = @{
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
                stock_observer = $StockObserver
                wait_timeout_ms = 240000
            }
            if ($RotationList.Count -gt 0) { $pairArguments.rotation = @($RotationList) }
            $pairResult = Invoke-OnHookTool -Name "onhook_run_lan_pair" -Arguments $pairArguments
            $pair = Get-StructuredResult $pairResult
            $script:OwnedRetailRuns.Add([string] $pair.host.run_id)
            $script:OwnedRetailRuns.Add([string] $pair.joiner.run_id)
            Assert-RetailHostRotation -Result $pair
            Assert-RetailStock -Tool "onhook_run_lan_pair host" `
                -Reported $pair.host.instance.stock_observer
            Assert-RetailStock -Tool "onhook_run_lan_pair joiner" `
                -Reported $pair.joiner.instance.stock_observer
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
            Invoke-ScenarioIfRequested -RetailJoinerInstanceId ([string] $pair.joiner.instance_id) `
                -RetailHostInstanceId ([string] $pair.host.instance_id)
            if ($ExerciseInput) {
                Invoke-RetailInputExercise -InstanceId ([string] $pair.joiner.instance_id) `
                    -ProcessId ([int] $pair.joiner.pid)
            }
            Start-RetailLateJoiner
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
            Complete-RetailLateJoiner
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
                Invoke-ScenarioIfRequested -RetailHostInstanceId $hostInstanceId
                Wait-UntilSteadyDeadline
                $script:OpenNovaJoiner.Refresh()
                if ($script:OpenNovaJoiner.HasExited) {
                    throw "OpenNova joiner exited during the RO deploy-hold steady window"
                }
                $hostInstance = Wait-RetailProcessForPeer `
                    -ProcessId $script:RetailHostProcess.Id -Role host
                $joinerAcceptance = Wait-OpenNovaJoinerReady `
                    -MinimumTicksMsec $initialJoinerTicks `
                    -AllowScenarioDeploy:([bool] $script:ScenarioSpec)
                if (-not (Test-OpenNovaJoinerReadinessTransition `
                        -Initial $initialJoinerAcceptance -Final $joinerAcceptance `
                        -ReadinessMode $ReadinessMode `
                        -AllowScenarioDeploy:([bool] $script:ScenarioSpec))) {
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
            $hostResult = Invoke-OnHookTool -Name "onhook_host_lan" `
                -Arguments (Get-RetailHostArguments -OutputDir $RunRoot)
            $retailHost = Get-StructuredResult $hostResult
            $script:OwnedRetailRuns.Add([string] $retailHost.run_id)
            Assert-OnHookRoleLaunch -Tool "onhook_host_lan" -Result $retailHost
            Assert-RetailHostRotation -Result $retailHost
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
            Invoke-ScenarioIfRequested -RetailHostInstanceId ([string] $retailHost.instance_id)
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
                stock_observer = $StockObserver
                readiness = $(if ($ReadinessMode -eq "in_match") { "in_match" } else { "peer" })
                wait_timeout_ms = 240000
            })
            Assert-OnHookRoleLaunch -Tool "onhook_join_lan" -Result $retailJoinLaunch
            $script:OwnedRetailRuns.Add([string] $retailJoinLaunch.run_id)
            $script:RetailProcess = Get-Process -Id ([int] $retailJoinLaunch.pid) -ErrorAction Stop
            $script:RetailJoinerProcess = $script:RetailProcess
            $clientCfg = Join-Path $ClientDir "onhook.cfg"
            $cfgText = Get-Content -LiteralPath $clientCfg -Raw
            # The managed keys onhook_join_lan renders for this case (the
            # expansion travels as its launch argument, not in the cfg).
            foreach ($expected in @(
                "LanJoinAddress 127.0.0.1",
                "LanJoinPort $Port",
                "LanJoinCallsign $JoinerCallsign",
                "StockObserver $(if ($StockObserver) { 1 } else { 0 })"
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
                Invoke-ScenarioIfRequested -RetailJoinerInstanceId $script:RetailInstanceId
                if ($ExerciseInput) {
                    Invoke-RetailInputExercise -InstanceId $script:RetailInstanceId `
                        -ProcessId $script:RetailProcess.Id
                }
                Start-RetailLateJoiner
                Wait-UntilSteadyDeadline
                $script:RetailProcess.Refresh()
                $script:OpenNovaHost.Refresh()
                if ($script:RetailProcess.HasExited -or $script:OpenNovaHost.HasExited) {
                    throw "An OR endpoint exited during the steady window"
                }
                $instance = Wait-RetailProcessForPeer -ProcessId $script:RetailProcess.Id `
                    -Role joiner -RequireInMatch:($ReadinessMode -eq "in_match") `
                    -AllowScenarioDeploy:([bool] $script:ScenarioSpec)
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
                Complete-RetailLateJoiner
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
            Invoke-ScenarioIfRequested
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
                -AllowPostDeathRedeploy:($ReadinessMode -eq 'in_match') `
                -AllowScenarioDeploy:([bool] $script:ScenarioSpec)
            if (-not (Test-OpenNovaJoinerReadinessTransition `
                    -Initial $initialJoinerAcceptance -Final $joinerAcceptance `
                    -ReadinessMode $ReadinessMode `
                    -AllowPostDeathRedeploy:($ReadinessMode -eq 'in_match') `
                    -AllowScenarioDeploy:([bool] $script:ScenarioSpec) `
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
    try { Stop-DesktopGuard }
    catch { Add-CleanupFailure "Desktop guard stop failed: $($_.Exception.Message)" }
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
    try { Stop-ExactProcess -Process $script:RetailLateJoinerProcess }
    catch { Add-CleanupFailure "Retail late joiner cleanup failed: $($_.Exception.Message)" }
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

    if ($script:ServerCopyFiles.Count) {
        try { Restore-ServerCopyFiles }
        catch { Add-CleanupFailure "SERVER copy restore failed: $($_.Exception.Message)" }
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
    retail_exe_sha256 = $RetailServerExeSha256
    retail_stock_observer = [bool] $StockObserver
    onhook_mcp_sha256 = (Get-FileHash -LiteralPath $McpExe -Algorithm SHA256).Hash.ToLowerInvariant()
    onhook_proxy_sha256 = $OnHookProxySha256
    # Both hosts here are listen hosts (a playing host); a dedicated host
    # (opennova-serve, a retail Serve Only host) is another kind.
    host_kind = "listen"
    # E2: the host's rotation tail (empty: one mission) and the cell's round
    # time limit (0: the copy's own game.cfg value), with the game.cfg write
    # and its restore.
    rotation = @($RotationList)
    time_limit_minutes = $TimeLimitMinutes
    server_game_cfg_override = $script:ServerGameCfgOverride
    # The SERVER copy's mutable files: each one's start hash (game.cfg's is
    # the case table's pin), whether the cell's processes wrote it, and the
    # restore of its start bytes.
    server_copy_files = @($script:ServerCopyFiles | ForEach-Object {
        [pscustomobject][ordered]@{
            file = $_.file
            sha256 = $_.sha256
            written_during_cell = $_.written_during_cell
            restored = $_.restored
        }
    })
    # The shared desktop at the steady window's start (the cursor clip found
    # and released) and end (clean: the focused retail window could centre).
    desktop_witness = $(if ($script:DesktopWitness.Count) { [pscustomobject] $script:DesktopWitness } else { $null })
    # The guard's hand-offs of the foreground from a game window to the desktop.
    desktop_guard_handoffs = $(if ($script:DesktopGuardLog -and (Test-Path -LiteralPath $script:DesktopGuardLog)) { @(Get-Content -LiteralPath $script:DesktopGuardLog | Where-Object { $_ -like '* foreground *' }).Count } else { $null })
    # The OpenNova host's working directory (OR, OO): the SERVER copy, as
    # retail's host runs in its install.
    opennova_host_working_dir = $(if ($Topology -in @("OR", "OO")) { $ServerDir } else { $null })
    # E3: the late joiner (null without -LateJoiner): its run, capture and
    # client port (the comparator's session selector), ready and final state.
    late_joiner = $script:LateJoinerWitness
    joiner_dial =$(if ($Topology -in @("RO", "OO")) { $JoinerDial } else { $null })
    repo_commit = [string] $RepoCommit
    repo_dirty = $RepoDirty
    allow_dirty_repo = [bool] $AllowDirtyRepo
    gdextension_sha256 = Get-FileSha256OrNull $GdextensionDll
    gdextension_editor_sha256 = Get-FileSha256OrNull $GdextensionEditorDll
    godot_launcher_sha256 = Get-FileSha256OrNull $GodotLauncher
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
    scenario_witness = $script:ScenarioWitness
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
if ($script:RunnerTranscript) { Stop-Transcript | Out-Null }
