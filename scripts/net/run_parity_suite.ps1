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
#       [-Resolution 2560x1440] [-Evidence summary|external] [-Plan]
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
# Prints PARITY_SUITE_CELL / PARITY_SUITE_CASE lines, then
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
    $ids = @{}
    foreach ($case in @($table.cases)) {
        foreach ($field in @("id", "mission", "game_type", "port", "readiness", "steady_seconds",
                "exercise_input", "auto_deploy")) {
            if (-not ($case.PSObject.Properties.Name -contains $field)) {
                throw "Parity case '$($case.id)' has no $field"
            }
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
        # The runner checks the port against the SERVER copy's game.cfg LAN
        # host range (stock JO:CA 32768-32787).
        if ([int] $case.port -lt 1 -or [int] $case.port -gt 65535) {
            throw "Parity case '$($case.id)': port must be 1-65535"
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
        "-JoinerMcpPort", [string] $JoinerMcpPort
    )
    if ($Resolution) { $arguments += @("-Resolution", $Resolution) }
    if ([bool] $Case.auto_deploy) { $arguments += "-AutoDeploy" }
    if ([bool] $Case.exercise_input) { $arguments += "-ExerciseInput" }
    return $arguments
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

$itemsPath = Join-Path $CorpusRoot "items.def"
$parityExe = Find-App -Dir "parity"
if (-not $Plan) {
    if (-not (Test-Path -LiteralPath $itemsPath -PathType Leaf)) {
        throw "The corpus has no items.def: $itemsPath"
    }
    if (-not $parityExe) {
        throw "opennova-parity.exe is not built; run: cmake --build build --config Release --target opennova_parity"
    }
}

$suiteDir = Join-Path $Repo ".scratch\parity-suites\$Prefix"
New-Item -ItemType Directory -Force -Path $suiteDir | Out-Null
Write-Host "PARITY_SUITE_PREFIX=$Prefix"

$caseResults = [System.Collections.Generic.List[object]]::new()
$cellFailures = 0
foreach ($case in $selectedCases) {
    $artifact = Join-Path $CorpusRoot ("missions\" + [string] $case.mission)
    $sha256 = ""
    if (-not $Plan) {
        if (-not (Test-Path -LiteralPath $artifact -PathType Leaf)) {
            throw "Parity case '$($case.id)': the corpus has no mission $artifact"
        }
        $sha256 = (Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($case.PSObject.Properties.Name -contains "mission_sha256" -and
                [string] $case.mission_sha256 -and [string] $case.mission_sha256 -cne $sha256) {
            throw "Parity case '$($case.id)': $artifact does not match the pinned mission_sha256"
        }
    }

    $cellResults = [ordered]@{}
    foreach ($cell in $Cells) {
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
            $shown = Get-CellArguments $table $case $cell $runId $artifact "<sha256>"
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

    $usable = @($Cells | Where-Object { $cellResults[$_.Name].status -in @("ran", "reused") })
    $subjects = @($usable | Where-Object { $_.Name -in @("RO", "OR", "OO") })
    $missing = @($Cells | Where-Object { $_.Name -in @("RO", "OR", "OO") -and $_ -notin $subjects } |
        ForEach-Object { $_.Name })
    $caseResult = [ordered]@{
        id = [string] $case.id
        status = "INCOMPLETE"
        failures = $null
        report = ""
        missing = @($missing)
        cells = $cellResults
    }
    $haveReference = @($usable | Where-Object { $_.Name -in @("RR", "RR-aa") }).Count -eq 2
    if ($Plan) {
        $caseResult.status = "PLANNED"
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
