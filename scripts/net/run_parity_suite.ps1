# The retail-LAN parity suite (ADR 0050 R4): for every case in the tracked case
# table (parity-cases.json), run the five cells through run_parity_topology.ps1
# with identical case inputs -- RR, RR again (the A/A run), RO, OR, OO -- then
# judge the case with opennova-parity and print the suite summary.
#
#   powershell -File scripts\net\run_parity_suite.ps1 `
#       -CorpusRoot C:\retail\corpus `
#       -RetailServerRoot C:\retail\jox01\SERVER -RetailClientRoot C:\retail\jox01\CLIENT `
#       -OnHookMcpPath C:\opennova-int\onhook\onhook-mcp.exe `
#       -GodotLauncher C:\Godot\Godot_v4.6.1-stable_win64_console.exe `
#       [-Prefix parity-20261011] [-Cases 01tr-coop,ash-i5a-aas-deploy] `
#       [-Topologies RR,RR-aa,RO,OR,OO] `
#       [-Resolution 2560x1440] [-Evidence summary|external] `
#       [-ReferencePrefix parity-20261019-base [-ReferenceRoot <dir>]] `
#       [-JoinerDial browse|direct] [-AllowDirtyRepo] [-Plan]
#
# Run it from a real console: the cells' Godot console wrapper stalls without
# one. -Resolution must match the retail copies' own video setting (the runner
# checks the retail viewport against it); empty takes the runner's default.
#
# CorpusRoot holds the extracted items.def and missions\<MISSION>, the layout
# run_parity_topology.ps1's wire-readiness gate reads (items.def one directory
# above missions\). Each cell's RunId is <Prefix>-<case>-<rr|rr-aa|ro|or|oo>,
# its run root .scratch\runs\<RunId>.
#
# Resumable: a cell whose run root already holds run-summary.json (the runner
# writes it only on success) is reused; a run root without one is a failed
# attempt, moved aside to <RunId>.failed-<stamp> before the cell runs again.
# -Cases and -Topologies choose which cells run; the verdict always judges
# every successful cell the case has, run now or earlier. -Plan prints the
# cells and runs nothing.
#
# -ReferencePrefix judges the subject cells against an earlier suite's RR and
# RR-aa cells (<ReferencePrefix>-<case>-rr, -rr-aa under -ReferenceRoot, by
# default .scratch\runs; the evidence copy retail-parity\runs-<prefix> once a
# suite's runs moved there), which this suite then does not run. A retail cell
# is evidence for its retail binary, hook build, mission, case inputs and
# rendered hook configs, so a reference is taken only while its summaries
# match the current Jointops.exe, onhook-mcp.exe and onHook proxy hashes, the
# case's mission hash and inputs, and its RR and RR-aa configs match each
# other; a subject cell's retail config must match the reference's of the same
# role, or the case is not judged.
#
# The checkout must be clean (each cell refuses uncommitted tracked changes
# unless -AllowDirtyRepo, a diagnostic); the OpenNova joiner browses unless
# -JoinerDial direct, a diagnostic (run_parity_topology.ps1).
#
# Prints PARITY_SUITE_PREFIX and PARITY_SUITE_DLL (the GDExtension hashes),
# PARITY_SUITE_CELL / PARITY_SUITE_CASE lines, then
# PARITY_SUITE_STATUS=PASS|FAIL and PARITY_SUITE_REPORT=<suite-summary.json>.
# PASS needs every selected cell to succeed and every selected case judged PASS
# with all three subject topologies present. Exit 0 PASS, 1 FAIL.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $CorpusRoot,

    [Parameter(Mandatory = $true)]
    [string] $RetailServerRoot,

    [Parameter(Mandatory = $true)]
    [string] $RetailClientRoot,

    [Parameter(Mandatory = $true)]
    [string] $OnHookMcpPath,

    [Parameter(Mandatory = $true)]
    [string] $GodotLauncher,

    # Reuse a prefix to resume a suite.
    [ValidatePattern('^[A-Za-z0-9](?:[A-Za-z0-9._-]{0,30}[A-Za-z0-9])?$')]
    [string] $Prefix = ("parity-" + (Get-Date -Format "yyyyMMdd")),

    # Defaults to parity-cases.json beside this script.
    [string] $CasesPath = "",

    # Case ids; a comma-separated list is accepted (powershell -File passes one string).
    [string[]] $Cases = @(),

    # Any of RR, RR-aa, RO, OR, OO; comma-separated accepted.
    [string[]] $Topologies = @("RR", "RR-aa", "RO", "OR", "OO"),

    [string] $Resolution = "",

    [ValidateSet("summary", "external")]
    [string] $Evidence = "summary",

    [ValidateRange(1, 65535)]
    [int] $HostMcpPort = 8975,

    [ValidateRange(1, 65535)]
    [int] $JoinerMcpPort = 8976,

    [ValidatePattern('^(?:|[A-Za-z0-9](?:[A-Za-z0-9._-]{0,30}[A-Za-z0-9])?)$')]
    [string] $ReferencePrefix = "",

    [string] $ReferenceRoot = "",

    [ValidateSet("browse", "direct")]
    [string] $JoinerDial = "browse",

    [switch] $AllowDirtyRepo,

    [switch] $Plan
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$Repo = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$Runner = Join-Path $PSScriptRoot "run_parity_topology.ps1"
if (-not $CasesPath) { $CasesPath = Join-Path $PSScriptRoot "parity-cases.json" }

# The five cells in run order: the runner's -Topology and the RunId suffix.
$Cells = @(
    [pscustomobject]@{ Name = "RR"; Topology = "RR"; Suffix = "rr"; Flag = "--rr" },
    [pscustomobject]@{ Name = "RR-aa"; Topology = "RR"; Suffix = "rr-aa"; Flag = "--rr-aa" },
    [pscustomobject]@{ Name = "RO"; Topology = "RO"; Suffix = "ro"; Flag = "--ro" },
    [pscustomobject]@{ Name = "OR"; Topology = "OR"; Suffix = "or"; Flag = "--or" },
    [pscustomobject]@{ Name = "OO"; Topology = "OO"; Suffix = "oo"; Flag = "--oo" }
)

function Split-ListArgument {
    param([string[]] $Values)
    return @($Values | ForEach-Object { "$_".Split(",") } | ForEach-Object { $_.Trim() } |
        Where-Object { $_ })
}

# A case's optional field, or $Default when the case omits it (StrictMode
# refuses a missing property). An array comes back unrolled: wrap the call in
# @() to keep a one-entry or empty list a list.
function Get-CaseField {
    param($Case, [string] $Name, $Default)
    if ($Case.PSObject.Properties.Name -contains $Name) { return $Case.$Name }
    return $Default
}

# The extension cases' optional inputs (scripts\net\parity-extension-cases.json):
# rotation (E2: the host's missions after `mission`), time_limit_minutes (E2:
# the cell's round limit) and topologies (the cells the case has; default all
# five, RR and RR-aa always among them).
function Get-CaseRotation { param($Case) return @(Get-CaseField $Case "rotation" @()) }
# late_joiner (E3): a second joiner of the first joiner's kind joins in the
# steady window (retail only so far: RR, RR-aa, OR).
function Get-CaseLateJoiner { param($Case) return [bool] (Get-CaseField $Case "late_joiner" $false) }
function Get-CaseTimeLimit { param($Case) return [int] (Get-CaseField $Case "time_limit_minutes" 0) }
# permanent_death (E5: the SERVER game.cfg's mp_permanent_death = 1 for the
# cell) and host_player_class (E5: the retail host's class, 5..9, written into
# the SERVER copy's weapon.sav record; 0 keeps the copy's).
function Get-CasePermanentDeath { param($Case) return [bool] (Get-CaseField $Case "permanent_death" $false) }
function Get-CaseHostPlayerClass { param($Case) return [int] (Get-CaseField $Case "host_player_class" 0) }
function Get-CaseTopologies {
    param($Case)
    return @(Get-CaseField $Case "topologies" @("RR", "RR-aa", "RO", "OR", "OO"))
}

# An app builds from apps\<dir> as opennova-<dir, kebab-cased>.exe.
function Find-App {
    param([string] $Dir)
    $build = Join-Path $Repo "build"
    $name = "opennova-" + ($Dir -replace '_', '-')
    foreach ($cfg in @("Release", "Debug")) {
        $path = Join-Path $build "apps\$Dir\$cfg\$name.exe"
        if (Test-Path -LiteralPath $path) { return $path }
    }
    $path = Join-Path $build "apps\$Dir\$name.exe"
    if (Test-Path -LiteralPath $path) { return $path }
    return $null
}

function Read-ParityCases {
    param([string] $Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "The parity case table is missing: $Path"
    }
    $table = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    if ([string] $table.schema -cne "opennova.parity-cases.v1") {
        throw "$Path is not an opennova.parity-cases.v1 table"
    }
    # The corpus is pinned: items.def for the table, each case's mission.
    if (-not ($table.PSObject.Properties.Name -contains "items_sha256") -or
            [string] $table.items_sha256 -notmatch '^[0-9a-f]{64}$') {
        throw "$Path pins no items_sha256"
    }
    # The SERVER copy's game.cfg is pinned too: both hosts run in that copy and
    # read it; every cell checks it at its start and restores it at its end.
    if (-not ($table.PSObject.Properties.Name -contains "server_game_cfg_sha256") -or
            [string] $table.server_game_cfg_sha256 -notmatch '^[0-9a-f]{64}$') {
        throw "$Path pins no server_game_cfg_sha256"
    }
    $ids = @{}
    foreach ($case in @($table.cases)) {
        foreach ($field in @("id", "mission", "mission_sha256", "game_type", "port", "readiness",
                "steady_seconds", "exercise_input", "auto_deploy")) {
            if (-not ($case.PSObject.Properties.Name -contains $field)) {
                throw "Parity case '$($case.id)' has no $field"
            }
        }
        if ([string] $case.mission_sha256 -notmatch '^[0-9a-f]{64}$') {
            throw "Parity case '$($case.id)': mission_sha256 is a lowercase SHA-256"
        }
        if ([string] $case.id -notmatch '^[a-z0-9](?:[a-z0-9-]{0,22}[a-z0-9])?$') {
            throw "Parity case id '$($case.id)' must be 1-24 lowercase letters, digits or dashes"
        }
        if ($ids.ContainsKey([string] $case.id)) { throw "Parity case id '$($case.id)' repeats" }
        $ids[[string] $case.id] = $true
        if ([string] $case.readiness -cnotin @("in_match", "deploy_hold")) {
            throw "Parity case '$($case.id)': readiness is in_match or deploy_hold"
        }
        if ($case.readiness -eq "deploy_hold" -and
                ([bool] $case.exercise_input -or [bool] $case.auto_deploy)) {
            throw "Parity case '$($case.id)': deploy_hold can neither exercise input nor auto-deploy"
        }
        # A case may play a scenario (scenarios\<file>) in place of the input
        # exercise: its actor plays it at the steady window's start.
        if ($case.PSObject.Properties.Name -contains "scenario") {
            if ([bool] $case.exercise_input) {
                throw "Parity case '$($case.id)': a scenario replaces the input exercise"
            }
            if (-not (Test-Path -LiteralPath (Join-Path $PSScriptRoot ("scenarios\" + [string] $case.scenario)) -PathType Leaf)) {
                throw "Parity case '$($case.id)': no scenario scenarios\$($case.scenario)"
            }
        }
        # The runner checks the port against the SERVER copy's game.cfg LAN
        # host range (stock JO:CA 32768-32787).
        if ([int] $case.port -lt 1 -or [int] $case.port -gt 65535) {
            throw "Parity case '$($case.id)': port must be 1-65535"
        }
        foreach ($entry in @(Get-CaseRotation $case)) {
            if ("$entry" -notmatch '^[^\\/:*?"<>|,\x00-\x1f]+\.[Bb][Mm][Ss]$') {
                throw "Parity case '$($case.id)': rotation entry '$entry' is not a .BMS basename"
            }
        }
        $limit = Get-CaseTimeLimit $case
        if ($limit -lt 0 -or $limit -gt 600) {
            throw "Parity case '$($case.id)': time_limit_minutes must be 0-600"
        }
        if ($case.PSObject.Properties.Name -contains "permanent_death" -and
                $case.permanent_death -isnot [bool]) {
            throw "Parity case '$($case.id)': permanent_death is true or false"
        }
        $class = Get-CaseHostPlayerClass $case
        if ($class -notin @(0, 5, 6, 7, 8, 9)) {
            throw "Parity case '$($case.id)': host_player_class is 5-9 (0 keeps the copy's)"
        }
        $caseTopologies = @(Get-CaseTopologies $case)
        foreach ($name in $caseTopologies) {
            if ($name -cnotin @("RR", "RR-aa", "RO", "OR", "OO")) {
                throw "Parity case '$($case.id)': unknown topology '$name'"
            }
        }
        if ("RR" -cnotin $caseTopologies -or "RR-aa" -cnotin $caseTopologies -or
                @($caseTopologies | Where-Object { $_ -cin @("RO", "OR", "OO") }).Count -eq 0) {
            throw "Parity case '$($case.id)': topologies needs RR, RR-aa and at least one subject"
        }
        # The OpenNova listen host has no rotation launch flag (run_parity_topology.ps1).
        if (@(Get-CaseRotation $case).Count -gt 0 -and
                @($caseTopologies | Where-Object { $_ -cin @("OR", "OO") }).Count -gt 0) {
            throw "Parity case '$($case.id)': a rotation case runs RR, RR-aa and RO only"
        }
        # Only a retail host stages players (onhook_load_debug_snapshot
        # target_slot); the OpenNova host has no slot pose (run_parity_topology.ps1).
        if ($case.PSObject.Properties.Name -contains "scenario" -and
                ((Get-Content -LiteralPath (Join-Path $PSScriptRoot ("scenarios\" + [string] $case.scenario)) -Raw |
                    ConvertFrom-Json).PSObject.Properties.Name -contains "staging") -and
                @($caseTopologies | Where-Object { $_ -cin @("OR", "OO") }).Count -gt 0) {
            throw "Parity case '$($case.id)': a staged scenario runs RR, RR-aa and RO only"
        }
        # char and click steps are onHook's: the OpenNova scenario_play driver
        # plays neither (run_parity_topology.ps1), so their actor stays retail.
        if ($case.PSObject.Properties.Name -contains "scenario") {
            $play = Get-Content -LiteralPath (Join-Path $PSScriptRoot ("scenarios\" + [string] $case.scenario)) -Raw | ConvertFrom-Json
            $typed = @(@($play.steps) + @(if ($play.PSObject.Properties.Name -contains "prelude") { @($play.prelude) }) |
                Where-Object { $_.PSObject.Properties.Name -contains "char" -or $_.PSObject.Properties.Name -contains "click" })
            $openNovaActorCells = if ([string] $play.actor -eq "host") { @("OR", "OO") } else { @("RO", "OO") }
            if ($typed.Count -gt 0 -and @($caseTopologies | Where-Object { $_ -cin $openNovaActorCells }).Count -gt 0) {
                throw "Parity case '$($case.id)': its scenario types or clicks, so its $($play.actor) stays retail (no $($openNovaActorCells -join ' or '))"
            }
        }
        # The host's class is written for a retail host (run_parity_topology.ps1).
        if ($class -ne 0 -and @($caseTopologies | Where-Object { $_ -cin @("OR", "OO") }).Count -gt 0) {
            throw "Parity case '$($case.id)': a host_player_class case runs RR, RR-aa and RO only"
        }
        # The runner wires a retail late joiner only (run_parity_topology.ps1).
        if ((Get-CaseLateJoiner $case) -and
                @($caseTopologies | Where-Object { $_ -cin @("RO", "OO") }).Count -gt 0) {
            throw "Parity case '$($case.id)': a late-joiner case runs RR, RR-aa and OR only"
        }
    }
    return $table
}

function Get-CellArguments {
    param($Table, $Case, $Cell, [string] $RunId, [string] $Artifact, [string] $Sha256)
    $arguments = @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $Runner,
        "-Topology", $Cell.Topology,
        "-Mission", [string] $Case.mission,
        "-Port", [string] $Case.port,
        "-GameType", [string] $Case.game_type,
        "-HostSessionName", [string] $Table.host_session_name,
        "-ReadinessMode", [string] $Case.readiness,
        "-RunId", $RunId,
        "-SteadySeconds", [string] $Case.steady_seconds,
        "-MissionArtifactPath", $Artifact,
        "-MissionSha256", $Sha256,
        "-Expansion", [string] $Table.expansion,
        "-RetailServerRoot", $RetailServerRoot,
        "-RetailClientRoot", $RetailClientRoot,
        "-OnHookMcpPath", $OnHookMcpPath,
        "-GodotLauncher", $GodotLauncher,
        "-HostMcpPort", [string] $HostMcpPort,
        "-JoinerMcpPort", [string] $JoinerMcpPort,
        "-ServerGameCfgSha256", [string] $Table.server_game_cfg_sha256
    )
    if ($Resolution) { $arguments += @("-Resolution", $Resolution) }
    if ([bool] $Case.auto_deploy) { $arguments += "-AutoDeploy" }
    if ([bool] $Case.exercise_input) { $arguments += "-ExerciseInput" }
    if ($Case.PSObject.Properties.Name -contains "scenario") {
        $arguments += @("-Scenario", (Join-Path $PSScriptRoot ("scenarios\" + [string] $Case.scenario)))
    }
    if ($Cell.Topology -in @("RO", "OO")) { $arguments += @("-JoinerDial", $JoinerDial) }
    $rotation = @(Get-CaseRotation $Case)
    if ($rotation.Count -gt 0) { $arguments += @("-Rotation", ($rotation -join ",")) }
    $limit = Get-CaseTimeLimit $Case
    if ($limit -gt 0) { $arguments += @("-TimeLimitMinutes", [string] $limit) }
    if (Get-CasePermanentDeath $Case) { $arguments += "-PermanentDeath" }
    $class = Get-CaseHostPlayerClass $Case
    if ($class -ne 0) { $arguments += @("-HostPlayerClass", [string] $class) }
    if (Get-CaseLateJoiner $Case) { $arguments += "-LateJoiner" }
    if ($AllowDirtyRepo) { $arguments += "-AllowDirtyRepo" }
    return $arguments
}

function Get-Sha256OrEmpty {
    param([string] $Path)
    if (-not $Path -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) { return "" }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Read-RunSummary {
    param([string] $RunRoot)
    $path = Join-Path $RunRoot "run-summary.json"
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return $null }
    return Get-Content -LiteralPath $path -Raw -Encoding UTF8 | ConvertFrom-Json
}

# role -> sha256 of a summary's effective_retail_configs.
function Get-RetailConfigHashes {
    param($Summary)
    $hashes = @{}
    if ($Summary -and $Summary.PSObject.Properties.Name -contains "effective_retail_configs") {
        foreach ($cfg in @($Summary.effective_retail_configs)) {
            $hashes[[string] $cfg.role] = [string] $cfg.sha256
        }
    }
    return $hashes
}

# Why a reference RR / RR-aa summary is not evidence for this case under the
# current retail identity (an empty list when it is).
function Get-ReferenceMismatches {
    param($Summary, $Case, [string] $MissionSha256)
    if (-not $Summary) { return @("no run-summary.json") }
    $reasons = [System.Collections.Generic.List[string]]::new()
    $fields = $Summary.PSObject.Properties.Name
    $expected = [ordered]@{
        topology = "RR"
        retail_exe_sha256 = $script:CurrentRetail.retail_exe_sha256
        onhook_mcp_sha256 = $script:CurrentRetail.onhook_mcp_sha256
        onhook_proxy_sha256 = $script:CurrentRetail.onhook_proxy_sha256
        mission = [string] $Case.mission
        mission_sha256 = $MissionSha256
        game_type = [string] $Case.game_type
        port = [string] $Case.port
        readiness_mode = [string] $Case.readiness
        steady_seconds = [string] $Case.steady_seconds
        exercise_input = [string] [bool] $Case.exercise_input
        expansion = [string] $script:Table.expansion
        host_session_name = [string] $script:Table.host_session_name
    }
    foreach ($name in $expected.Keys) {
        $want = [string] $expected[$name]
        if (-not ($fields -contains $name)) {
            $reasons.Add("$name not recorded")
        }
        elseif ([string] $Summary.$name -cne $want) {
            $reasons.Add("$name $($Summary.$name) is not $want")
        }
    }
    # E2 inputs, checked when the case has them (a summary from before E2
    # records neither and is evidence only for a case without them).
    $rotation = (@(Get-CaseRotation $Case) -join ",")
    $recordedRotation = if ($fields -contains "rotation") {
        (@($Summary.rotation | ForEach-Object { [string] $_ }) -join ",")
    } else { "" }
    if ($recordedRotation -cne $rotation) {
        $reasons.Add("rotation '$recordedRotation' is not '$rotation'")
    }
    $limit = Get-CaseTimeLimit $Case
    $recordedLimit = if ($fields -contains "time_limit_minutes") {
        [int] $Summary.time_limit_minutes
    } else { 0 }
    if ($recordedLimit -ne $limit) {
        $reasons.Add("time_limit_minutes $recordedLimit is not $limit")
    }
    # E5 inputs: a summary from before E5 records neither (evidence only for a
    # case without them).
    $recordedPermanent = ($fields -contains "permanent_death") -and [bool] $Summary.permanent_death
    if ($recordedPermanent -ne (Get-CasePermanentDeath $Case)) {
        $reasons.Add("permanent_death $recordedPermanent is not $(Get-CasePermanentDeath $Case)")
    }
    $recordedClass = if ($fields -contains "host_player_class") { [int] $Summary.host_player_class } else { 0 }
    if ($recordedClass -ne (Get-CaseHostPlayerClass $Case)) {
        $reasons.Add("host_player_class $recordedClass is not $(Get-CaseHostPlayerClass $Case)")
    }
    $recordedLate = ($fields -contains "late_joiner") -and ($null -ne $Summary.late_joiner)
    if ($recordedLate -ne (Get-CaseLateJoiner $Case)) {
        $reasons.Add("late_joiner $recordedLate is not $(Get-CaseLateJoiner $Case)")
    }
    if ($fields -contains "retail_stock_observer" -and -not [bool] $Summary.retail_stock_observer) {
        $reasons.Add("the retail roles ran without StockObserver")
    }
    $recordedCfg = if ($fields -contains "server_copy_files") {
        [string] (@($Summary.server_copy_files) | Where-Object { $_.file -eq "game.cfg" } | Select-Object -First 1).sha256
    } else { "" }
    if ($recordedCfg -cne [string] $script:Table.server_game_cfg_sha256) {
        $reasons.Add("SERVER game.cfg '$recordedCfg' is not the pinned $($script:Table.server_game_cfg_sha256)")
    }
    $wantScenario = if ($Case.PSObject.Properties.Name -contains "scenario") {
        [string] (Get-Content -LiteralPath (Join-Path $PSScriptRoot ("scenarios\" + [string] $Case.scenario)) -Raw |
            ConvertFrom-Json).name
    } else { "" }
    $hadScenario = if ($fields -contains "scenario_witness" -and $Summary.scenario_witness) {
        [string] $Summary.scenario_witness.name
    } else { "" }
    if ($hadScenario -cne $wantScenario) {
        $reasons.Add("scenario '$hadScenario' is not '$wantScenario'")
    }
    return @($reasons.ToArray())
}

$table = Read-ParityCases -Path $CasesPath
$selectedTopologies = @(Split-ListArgument $Topologies)
foreach ($name in $selectedTopologies) {
    if ($name -cnotin @($Cells.Name)) {
        throw "Unknown topology '$name' (RR, RR-aa, RO, OR or OO)"
    }
}
$selectedCases = @($table.cases)
$requestedCases = @(Split-ListArgument $Cases)
if ($requestedCases.Count -gt 0) {
    foreach ($id in $requestedCases) {
        if ($id -cnotin @($table.cases.id)) { throw "Unknown parity case '$id'" }
    }
    $selectedCases = @($table.cases | Where-Object { $_.id -cin $requestedCases })
}
if ([string] $table.host_session_name -notmatch '^[\x20-\x7e]{1,31}$') {
    throw "host_session_name must be 1-31 printable ASCII characters"
}

$script:Table = $table
$itemsPath = Join-Path $CorpusRoot "items.def"
$parityExe = Find-App -Dir "parity"
if (-not $Plan) {
    if (-not (Test-Path -LiteralPath $itemsPath -PathType Leaf)) {
        throw "The corpus has no items.def: $itemsPath"
    }
    if ((Get-Sha256OrEmpty $itemsPath) -cne [string] $table.items_sha256) {
        throw "The corpus items.def does not match the pinned items_sha256: $itemsPath"
    }
    if (-not $parityExe) {
        throw "opennova-parity.exe is not built; run: cmake --build build --config Release --target opennova_parity"
    }
}

# The mission artifacts, each against its pin (the plan hashes what is there).
$missionHashes = @{}
foreach ($case in $selectedCases) {
    $artifact = Join-Path $CorpusRoot ("missions\" + [string] $case.mission)
    $sha256 = Get-Sha256OrEmpty $artifact
    if (-not $sha256 -and -not $Plan) {
        throw "Parity case '$($case.id)': the corpus has no mission $artifact"
    }
    if ($sha256 -and $sha256 -cne [string] $case.mission_sha256) {
        throw "Parity case '$($case.id)': $artifact does not match the pinned mission_sha256"
    }
    $missionHashes[[string] $case.id] = $sha256
}

# The reference RR / RR-aa cells, each checked before any cell runs.
$referenceRunsDir = ""
$references = @{}
if ($ReferencePrefix) {
    if ($ReferencePrefix -ceq $Prefix) {
        throw "-ReferencePrefix names this suite's own prefix; a resumed suite reuses its cells already"
    }
    $referenceRunsDir = if ($ReferenceRoot) {
        (Resolve-Path -LiteralPath $ReferenceRoot).Path
    } else { Join-Path $Repo ".scratch\runs" }
    $script:CurrentRetail = [pscustomobject]@{
        retail_exe_sha256 = Get-Sha256OrEmpty (Join-Path $RetailServerRoot "Jointops.exe")
        onhook_mcp_sha256 = Get-Sha256OrEmpty $OnHookMcpPath
        onhook_proxy_sha256 = Get-Sha256OrEmpty (Join-Path $RetailServerRoot "binkw32.dll")
    }
    foreach ($case in $selectedCases) {
        $reference = @{}
        foreach ($cell in @($Cells | Where-Object { $_.Name -in @("RR", "RR-aa") })) {
            $refRunId = "$ReferencePrefix-$($case.id)-$($cell.Suffix)"
            $refRoot = Join-Path $referenceRunsDir $refRunId
            $refSummary = Read-RunSummary $refRoot
            $reasons = @(Get-ReferenceMismatches $refSummary $case $missionHashes[[string] $case.id])
            if ($reasons.Count -gt 0) {
                throw "Parity case '$($case.id)': reference $refRunId is not evidence for this suite: $($reasons -join '; ')"
            }
            $reference[$cell.Name] = [pscustomobject]@{
                run_id = $refRunId
                run_root = $refRoot
                configs = Get-RetailConfigHashes $refSummary
            }
        }
        foreach ($role in @("server", "client")) {
            $rr = [string] $reference["RR"].configs[$role]
            if (-not $rr -or $rr -cne [string] $reference["RR-aa"].configs[$role]) {
                throw "Parity case '$($case.id)': the reference RR and RR-aa $role hook configs differ or are missing"
            }
        }
        $references[[string] $case.id] = $reference
    }
}

$suiteDir = Join-Path $Repo ".scratch\parity-suites\$Prefix"
New-Item -ItemType Directory -Force -Path $suiteDir | Out-Null
$repoCommit = [string] (& git -C $Repo rev-parse HEAD 2>$null)
$dllRuntime = Get-Sha256OrEmpty (Join-Path $Repo "godot\bin\libopennova.windows.template_debug.x86_64.dll")
$dllEditor = Get-Sha256OrEmpty (Join-Path $Repo "godot\bin\libopennova_editor.windows.template_debug.x86_64.dll")
Write-Host "PARITY_SUITE_PREFIX=$Prefix"
Write-Host ("PARITY_SUITE_DLL runtime=$(if ($dllRuntime) { $dllRuntime } else { 'absent' })" +
    " editor=$(if ($dllEditor) { $dllEditor } else { 'absent' }) commit=$repoCommit")
if ($ReferencePrefix) {
    Write-Host "PARITY_SUITE_REFERENCE prefix=$ReferencePrefix root=$referenceRunsDir"
}

$caseResults = [System.Collections.Generic.List[object]]::new()
$cellFailures = 0
foreach ($case in $selectedCases) {
    $artifact = Join-Path $CorpusRoot ("missions\" + [string] $case.mission)
    $sha256 = $missionHashes[[string] $case.id]
    $reference = if ($ReferencePrefix) { $references[[string] $case.id] } else { $null }

    $cellResults = [ordered]@{}
    $caseTopologies = @(Get-CaseTopologies $case)
    foreach ($cell in $Cells) {
        if ($cell.Name -cnotin $caseTopologies) {
            Write-Host "PARITY_SUITE_CELL case=$($case.id) topology=$($cell.Name) run=- status=not_applicable"
            $cellResults[$cell.Name] = [pscustomobject]@{
                run_id = ""
                run_root = ""
                status = "not_applicable"
                selected = $false
            }
            continue
        }
        if ($reference -and $reference.ContainsKey($cell.Name)) {
            $ref = $reference[$cell.Name]
            Write-Host "PARITY_SUITE_CELL case=$($case.id) topology=$($cell.Name) run=$($ref.run_id) status=referenced"
            $cellResults[$cell.Name] = [pscustomobject]@{
                run_id = $ref.run_id
                run_root = $ref.run_root
                status = "referenced"
                selected = $false
            }
            continue
        }
        $runId = "$Prefix-$($case.id)-$($cell.Suffix)"
        if ($runId.Length -gt 56) { throw "RunId $runId is longer than the runner's 56 characters" }
        $runRoot = Join-Path $Repo ".scratch\runs\$runId"
        $summary = Join-Path $runRoot "run-summary.json"
        $selected = $cell.Name -cin $selectedTopologies
        $status = "absent"
        if (Test-Path -LiteralPath $summary -PathType Leaf) {
            $status = "reused"
        }
        elseif ($selected -and $Plan) {
            $status = "planned"
            $shown = Get-CellArguments $table $case $cell $runId $artifact $(if ($sha256) { $sha256 } else { "<sha256>" })
            Write-Host ("PARITY_SUITE_PLAN case=$($case.id) topology=$($cell.Name) run=$runId args=" +
                ($shown[5..($shown.Count - 1)] -join " "))
        }
        elseif ($selected) {
            if (Test-Path -LiteralPath $runRoot) {
                $aside = "$runId.failed-" + (Get-Date -Format "yyyyMMddTHHmmss")
                try {
                    Rename-Item -LiteralPath $runRoot -NewName $aside
                    Write-Host "PARITY_SUITE_NOTE case=$($case.id) topology=$($cell.Name) moved the failed attempt aside to $aside"
                }
                catch {
                    Write-Host "PARITY_SUITE_NOTE case=$($case.id) topology=$($cell.Name) cannot move $runRoot aside: $($_.Exception.Message)"
                }
            }
            if (Test-Path -LiteralPath $runRoot) {
                $status = "failed"
            }
            else {
                # One retry: a cell started right after the previous one occasionally
                # fails on a port or process the last cell is still releasing, and
                # passes when run again (seen on deploy-hold RO and OO cells).
                for ($attempt = 1; $attempt -le 2; ++$attempt) {
                    if ($attempt -gt 1) {
                        $aside = "$runId.failed-" + (Get-Date -Format "yyyyMMddTHHmmss")
                        if (Test-Path -LiteralPath $runRoot) {
                            Rename-Item -LiteralPath $runRoot -NewName $aside
                        }
                        Write-Host "PARITY_SUITE_NOTE case=$($case.id) topology=$($cell.Name) retrying after a failed attempt ($aside)"
                        Start-Sleep -Seconds 20
                    }
                    Write-Host "PARITY_SUITE_RUN case=$($case.id) topology=$($cell.Name) run=$runId attempt=$attempt"
                    $arguments = Get-CellArguments $table $case $cell $runId $artifact $sha256
                    & powershell.exe @arguments
                    $exitCode = $LASTEXITCODE
                    $status = if ($exitCode -eq 0 -and (Test-Path -LiteralPath $summary -PathType Leaf)) {
                        "ran"
                    } else { "failed" }
                    if ($status -eq "ran") { break }
                }
            }
            if ($status -eq "failed") { $cellFailures++ }
        }
        Write-Host "PARITY_SUITE_CELL case=$($case.id) topology=$($cell.Name) run=$runId status=$status"
        $cellResults[$cell.Name] = [pscustomobject]@{
            run_id = $runId
            run_root = $runRoot
            status = $status
            selected = $selected
        }
    }

    $usable = @($Cells | Where-Object { $cellResults[$_.Name].status -in @("ran", "reused", "referenced") })
    $subjects = @($usable | Where-Object { $_.Name -in @("RO", "OR", "OO") })
    $missing = @($Cells | Where-Object { $_.Name -in @("RO", "OR", "OO") -and
            $_.Name -cin $caseTopologies -and $_ -notin $subjects } |
        ForEach-Object { $_.Name })
    # A referenced RR is evidence only for subjects whose retail role loaded the
    # same rendered hook config: RO's retail host (server), OR's retail joiner
    # (client).
    $drift = [System.Collections.Generic.List[string]]::new()
    if ($reference -and -not $Plan) {
        foreach ($pair in @(@("RO", "server"), @("OR", "client"))) {
            $result = $cellResults[$pair[0]]
            if ($result.status -notin @("ran", "reused")) { continue }
            $subjectConfig = [string] (Get-RetailConfigHashes (Read-RunSummary $result.run_root))[$pair[1]]
            if ($subjectConfig -cne [string] $reference["RR"].configs[$pair[1]]) {
                $drift.Add("$($pair[0]) $($pair[1]) hook config $subjectConfig is not the reference's")
            }
        }
    }
    $caseResult = [ordered]@{
        id = [string] $case.id
        status = "INCOMPLETE"
        failures = $null
        report = ""
        missing = @($missing)
        reference_drift = @($drift.ToArray())
        cells = $cellResults
    }
    $haveReference = @($usable | Where-Object { $_.Name -in @("RR", "RR-aa") }).Count -eq 2
    if ($Plan) {
        $caseResult.status = "PLANNED"
    }
    elseif ($drift.Count -gt 0) {
        $caseResult.status = "ERROR"
        Write-Host "PARITY_SUITE_NOTE case=$($case.id) not judged: $($drift -join '; ')"
    }
    elseif ($haveReference -and $subjects.Count -gt 0) {
        $report = Join-Path $suiteDir "$($case.id).verdict.json"
        $parityArgs = @("--items", $itemsPath, "--out", $report, "--evidence", $Evidence)
        foreach ($cell in $usable) {
            $parityArgs += @($cell.Flag, $cellResults[$cell.Name].run_root)
        }
        $lines = @(& $parityExe @parityArgs)
        $exitCode = $LASTEXITCODE
        $lines | ForEach-Object { Write-Host $_ }
        $statusLine = @($lines | Where-Object { "$_" -like "PARITY_STATUS=*" } | Select-Object -Last 1)
        $failureLine = @($lines | Where-Object { "$_" -like "PARITY_FAILURES=*" } | Select-Object -Last 1)
        if ($exitCode -in @(0, 1) -and $statusLine.Count -eq 1) {
            $caseResult.status = "$($statusLine[0])".Substring("PARITY_STATUS=".Length)
            if ($failureLine.Count -eq 1) {
                $caseResult.failures = [int] "$($failureLine[0])".Substring("PARITY_FAILURES=".Length)
            }
            $caseResult.report = $report
        }
        else {
            $caseResult.status = "ERROR"
            Write-Host "PARITY_SUITE_NOTE case=$($case.id) opennova-parity exited $exitCode"
        }
        if ($caseResult.status -eq "PASS" -and $missing.Count -gt 0) {
            $caseResult.status = "INCOMPLETE"
        }
        # E3: the late joiner's own session, judged as a second verdict (each
        # run's late_joiner capture bound to its client port, from its readiness).
        if (Get-CaseLateJoiner $case) {
            $lateReport = Join-Path $suiteDir "$($case.id).late.verdict.json"
            $lateArgs = @("--items", $itemsPath, "--out", $lateReport, "--evidence", "late")
            foreach ($cell in $usable) {
                $lateArgs += @($cell.Flag, $cellResults[$cell.Name].run_root)
            }
            $lateLines = @(& $parityExe @lateArgs)
            $lateExit = $LASTEXITCODE
            $lateLines | ForEach-Object { Write-Host $_ }
            $lateStatus = @($lateLines | Where-Object { "$_" -like "PARITY_STATUS=*" } | Select-Object -Last 1)
            $caseResult.late_status = if ($lateExit -in @(0, 1) -and $lateStatus.Count -eq 1) {
                "$($lateStatus[0])".Substring("PARITY_STATUS=".Length)
            } else { "ERROR" }
            $caseResult.late_report = $lateReport
            if ($caseResult.status -eq "PASS" -and $caseResult.late_status -ne "PASS") {
                $caseResult.status = $caseResult.late_status
            }
            Write-Host "PARITY_SUITE_NOTE case=$($case.id) late joiner verdict=$($caseResult.late_status) report=$lateReport"
        }
    }
    $caseResults.Add([pscustomobject] $caseResult)
    Write-Host ("PARITY_SUITE_CASE case=$($case.id) status=$($caseResult.status)" +
        " failures=$(if ($null -eq $caseResult.failures) { '-' } else { $caseResult.failures })" +
        " missing=$(if ($missing.Count) { $missing -join ',' } else { '-' })" +
        " report=$(if ($caseResult.report) { $caseResult.report } else { '-' })")
}

$pass = -not $Plan -and $cellFailures -eq 0 -and $caseResults.Count -gt 0 -and
    @($caseResults | Where-Object { $_.status -ne "PASS" }).Count -eq 0
$suiteReport = Join-Path $suiteDir "suite-summary.json"
[pscustomobject]@{
    schema = "opennova.parity-suite.v1"
    prefix = $Prefix
    status = $(if ($Plan) { "PLAN" } elseif ($pass) { "PASS" } else { "FAIL" })
    cell_failures = $cellFailures
    evidence = $Evidence
    repo_commit = $repoCommit
    gdextension_sha256 = $dllRuntime
    gdextension_editor_sha256 = $dllEditor
    joiner_dial = $JoinerDial
    reference_prefix = $ReferencePrefix
    reference_root = $referenceRunsDir
    cases = @($caseResults.ToArray())
} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $suiteReport -Encoding UTF8
if ($Plan) {
    Write-Host "PARITY_SUITE_STATUS=PLAN"
    Write-Host "PARITY_SUITE_REPORT=$suiteReport"
    exit 0
}
Write-Host "PARITY_SUITE_STATUS=$(if ($pass) { 'PASS' } else { 'FAIL' })"
Write-Host "PARITY_SUITE_REPORT=$suiteReport"
exit $(if ($pass) { 0 } else { 1 })
