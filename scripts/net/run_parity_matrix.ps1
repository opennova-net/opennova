[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]{0,30}$')]
    [string] $SuitePrefix,
    [switch] $PreflightOnly,

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
$runner = Join-Path $PSScriptRoot "run_parity_topology.ps1"
$serverCfgTemplate = Join-Path $repo "scripts\net\configs\retail-lan-server.onhook.cfg"
$clientCfgTemplate = Join-Path $repo "scripts\net\configs\retail-lan-client.onhook.cfg"
$manifestTemplatePath = Join-Path $repo "scripts\net\parity-matrix.example.json"
$manifestTemplate = Get-Content -LiteralPath $manifestTemplatePath -Raw | ConvertFrom-Json
$templateCases = @($manifestTemplate.cases)
$requiredCaseIds = @($manifestTemplate.required_case_ids | ForEach-Object { [string]$_ })
$matrixBindings = Get-ParityMatrixBindings -Cases $templateCases
$caseBindings = $matrixBindings.CaseBindings
$missionBindings = @($matrixBindings.MissionBindings)
if ($templateCases.Count -eq 0 -or
        $requiredCaseIds.Count -ne $templateCases.Count -or
        ($requiredCaseIds -join "`n") -cne
            (@($templateCases | ForEach-Object { [string]$_.id }) -join "`n")) {
    throw "Manifest template required_case_ids must exactly order every runnable case."
}
$suiteRoot = Join-Path $repo ".scratch\parity-final\suites\$SuitePrefix"
$manifestPath = Join-Path $repo ".scratch\parity-final\$SuitePrefix.manifest.json"
$runIds = @($templateCases | ForEach-Object {
    $slug = [string] $caseBindings[[string] $_.id].Slug
    @("RR", "RO", "OR", "OO") | ForEach-Object {
        "$SuitePrefix-$slug-$($_.ToLowerInvariant())"
    }
})
if (@($runIds | Sort-Object -Unique).Count -ne $runIds.Count) {
    throw "Resolved parity run ids must be unique across every case/topology cell."
}
$existing = @($suiteRoot, $manifestPath) + @($runIds | ForEach-Object {
    Join-Path $repo ".scratch\runs\$_"
})
$collisions = @($existing | Where-Object { Test-Path -LiteralPath $_ })
if ($collisions.Count -ne 0) {
    throw "SuitePrefix is not create-new; existing path(s): $($collisions -join ', ')"
}

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
$env:GODOT_BIN = $godotLauncher
$godotRuntime = Resolve-GodotRuntimeBinary -Path $godotLauncher
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
$corpusPairs = @(
    [pscustomobject]@{ Role = "retail_exe"; Relative = "Jointops.exe" },
    [pscustomobject]@{ Role = "hook"; Relative = "binkw32.dll" },
    [pscustomobject]@{ Role = "resource"; Relative = "resource.pff" },
    [pscustomobject]@{ Role = "localres"; Relative = "localres.pff" },
    [pscustomobject]@{ Role = "language"; Relative = "language.pff" },
    [pscustomobject]@{ Role = "med"; Relative = "med.pff" },
    [pscustomobject]@{ Role = "expansion_pff"; Relative = "expansion\revx02\RevX02.pff" },
    [pscustomobject]@{ Role = "expansion_language_pff"; Relative = "expansion\revx02\RevX02L.pff" },
    [pscustomobject]@{ Role = "expansion_bin"; Relative = "expansion\revx02\revx02.bin" }
)
$artifactHashes = @{}
foreach ($pair in $corpusPairs) {
    $serverPath = Join-Path $serverDir $pair.Relative
    $clientPath = Join-Path $clientDir $pair.Relative
    $serverHash = (Get-FileHash -LiteralPath $serverPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $clientHash = (Get-FileHash -LiteralPath $clientPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($serverHash -ne $clientHash) {
        throw "Retail role corpus mismatch: server=$serverPath client=$clientPath"
    }
    $artifactHashes[$pair.Role] = $serverHash
    Write-Host "MATRIX_CORPUS_MATCH role=$($pair.Role) path=$($pair.Relative) sha256=$serverHash"
}
foreach ($artifact in @(
    [pscustomobject]@{ Role = "items"; Path = (Join-Path $corpusRoot "items.def") },
    [pscustomobject]@{ Role = "ammo"; Path = (Join-Path $corpusRoot "ammo.def") },
    [pscustomobject]@{ Role = "opennova_runtime"; Path = (Join-Path $repo "godot\bin\libopennova.windows.template_debug.x86_64.dll") },
    [pscustomobject]@{ Role = "godot_launcher"; Path = $godotLauncher },
    [pscustomobject]@{ Role = "godot_runtime"; Path = $godotRuntime },
    [pscustomobject]@{ Role = "nw_pp"; Path = (Join-Path $repo "build\apps\nw_pp\Release\nw_pp.exe") },
    [pscustomobject]@{ Role = "lan_probe"; Path = (Join-Path $repo "build\apps\nw_lan_probe\Release\nw-lan-probe.exe") },
    [pscustomobject]@{ Role = "onhook_mcp"; Path = $onHookMcp },
    [pscustomobject]@{ Role = "parity_runner"; Path = $runner },
    [pscustomobject]@{ Role = "matrix_runner"; Path = $PSCommandPath },
    [pscustomobject]@{ Role = "manifest_generator"; Path = (Join-Path $PSScriptRoot "generate_parity_manifest.ps1") },
    [pscustomobject]@{ Role = "auto_joiner"; Path = (Join-Path $repo "godot\tests\net\parity_joiner_driver.gd") },
    [pscustomobject]@{ Role = "capture_script"; Path = (Join-Path $repo "scripts\net\capture.ps1") },
    [pscustomobject]@{ Role = "host_script"; Path = (Join-Path $repo "scripts\net\host_opennova.ps1") },
    [pscustomobject]@{ Role = "join_script"; Path = (Join-Path $repo "scripts\net\join_opennova.ps1") },
    [pscustomobject]@{ Role = "net_lib"; Path = (Join-Path $repo "scripts\net\lib.ps1") },
    [pscustomobject]@{ Role = "retail_launcher"; Path = (Join-Path $repo "scripts\net\launch_retail_cfg.ps1") },
    [pscustomobject]@{ Role = "verifier"; Path = (Join-Path $repo "scripts\net\verify_parity_matrix.ps1") },
    [pscustomobject]@{ Role = "manifest_template"; Path = $manifestTemplatePath },
    [pscustomobject]@{ Role = "corpus_exporter"; Path = $corpusExporter },
    [pscustomobject]@{ Role = "retail_input_script"; Path = (Join-Path $repo "scripts\net\exercise_retail_input.ps1") },
    [pscustomobject]@{ Role = "wire_ready_script"; Path = (Join-Path $repo "scripts\net\wait_parity_wire_ready.ps1") },
    [pscustomobject]@{ Role = "server_cfg_template"; Path = $serverCfgTemplate },
    [pscustomobject]@{ Role = "client_cfg_template"; Path = $clientCfgTemplate }
)) {
    $artifactHashes[$artifact.Role] =
        (Get-FileHash -LiteralPath $artifact.Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
foreach ($binding in $missionBindings) {
    $missionPath = Join-Path $corpusRoot "missions\$($binding.Mission)"
    $artifactHashes[[string]$binding.MissionRole] =
        (Get-FileHash -LiteralPath $missionPath -Algorithm SHA256).Hash.ToLowerInvariant()
}
$godotRuntimeArtifacts = Get-GodotRuntimeArtifacts -RepoRoot $repo
foreach ($role in $godotRuntimeArtifacts.Keys) {
    if ($artifactHashes.ContainsKey($role)) {
        throw "Duplicate artifact role while binding Godot project: $role"
    }
    $artifactHashes[$role] = (Get-FileHash -LiteralPath $godotRuntimeArtifacts[$role] `
        -Algorithm SHA256).Hash.ToLowerInvariant()
}
$repositorySourceState = Get-NetHarnessRepositorySourceState -RepoRoot $repo
$inventoryLines = (@($artifactHashes.Keys | ForEach-Object {
    "$_=$($artifactHashes[$_])"
}) + @("repo_source_state=$($repositorySourceState.sha256)")) | Sort-Object
$inventory = ($inventoryLines -join "`n") + "`n"
$inventoryBytes = [System.Text.Encoding]::UTF8.GetBytes($inventory)
$sha = [System.Security.Cryptography.SHA256]::Create()
try {
    $corpusFingerprint = ([BitConverter]::ToString($sha.ComputeHash($inventoryBytes)) -replace '-', '').ToLowerInvariant()
}
finally {
    $sha.Dispose()
}
$suiteId = "$SuitePrefix-$($corpusFingerprint.Substring(0, 12))"

$templateCaseIds = @($manifestTemplate.cases | ForEach-Object { [string] $_.id })
if ($templateCaseIds.Count -ne $caseBindings.Count -or
        @($templateCaseIds | Where-Object { -not $caseBindings.ContainsKey($_) }).Count -ne 0) {
    throw "Manifest template cases do not match the runnable canonical-plus-holdout matrix."
}
$cases = @($manifestTemplate.cases | ForEach-Object {
    $templateCase = $_
    $binding = $caseBindings[[string] $templateCase.id]
    if (@($templateCase.exceptions).Count -ne 0) {
        throw "Manifest template case=$($templateCase.id) declares unsupported exceptions."
    }
    if ([string] $templateCase.mission -cne [string] $binding.Mission) {
        throw "Manifest template case=$($templateCase.id) changed its bound mission."
    }
    $steadyValues = @(@("RR", "RO", "OR", "OO") | ForEach-Object {
        [int] $templateCase.runs.$_.steady_seconds
    } | Sort-Object -Unique)
    if ($steadyValues.Count -ne 1 -or $steadyValues[0] -lt 5 -or $steadyValues[0] -gt 120) {
        throw "Manifest template case=$($templateCase.id) must use one supported steady duration."
    }
    [uint32] $caseGameType = 0
    if (-not [uint32]::TryParse(
            [string] $templateCase.game_type, [ref] $caseGameType)) {
        throw "Manifest template case=$($templateCase.id) has an invalid UInt32 game type."
    }
    [pscustomobject]@{
        Slug = [string] $binding.Slug
        Mission = [string] $templateCase.mission
        MissionRole = [string] $binding.MissionRole
        GameType = [string] $caseGameType
        Port = [int] $templateCase.port
        Resolution = [string] $templateCase.resolution
        HostSessionName = [string] $templateCase.host_session_name
        Host = [string] $templateCase.host_callsign
        Joiner = [string] $templateCase.joiner_callsign
        Readiness = [string] $templateCase.readiness_mode
        Steady = [int] $steadyValues[0]
        AutoDeploy = [string] $templateCase.readiness_mode -eq "in_match"
        ExerciseInput = [bool] $templateCase.exercise_input
    }
})
foreach ($case in $cases) {
    if ([string]::IsNullOrWhiteSpace([string] $case.HostSessionName) -or
            -not [string]::Equals(
                [string] $case.HostSessionName,
                "Untitled ",
                [System.StringComparison]::Ordinal) -or
            [string]::Equals(
                [string] $case.HostSessionName,
                [string] $case.Host,
                [System.StringComparison]::Ordinal)) {
        throw "Final matrix case=$($case.Slug) must bind the exact retail session name separately from its host callsign."
    }
    if ([string] $case.Readiness -notin @("in_match", "deploy_hold") -or
            ([string] $case.Readiness -eq "deploy_hold" -and
                ([bool] $case.AutoDeploy -or [bool] $case.ExerciseInput))) {
        throw "Final matrix case=$($case.Slug) has an invalid readiness/input contract."
    }
    if ([string] $case.Resolution -notmatch '^[1-9][0-9]{2,4}x[1-9][0-9]{2,4}$') {
        throw "Final matrix case=$($case.Slug) has an invalid resolution."
    }
}
if (-not (Test-ParityMatrixAutomationPorts -Cases $cases)) {
    throw "Final matrix must cover all four retail automation ports 32786-32789; additional sequential cases may reuse them."
}

if ($PreflightOnly) {
    $preflightClientCfg = Join-Path $repo ".scratch\parity-final\$SuitePrefix-preflight-$PID.onhook.cfg"
    if (Test-Path -LiteralPath $preflightClientCfg) {
        throw "Create-new OR config preflight path already exists: $preflightClientCfg"
    }
    try {
        foreach ($case in $cases) {
            Copy-Item -LiteralPath $clientCfgTemplate -Destination $preflightClientCfg
            Set-OnHookOwnedConfigValues -Path $preflightClientCfg -Values @{
                LanJoinAddress = "127.0.0.1"
                LanJoinPort = [string] $case.Port
                LanJoinCallsign = [string] $case.Joiner
            }
            $templateBase = Get-OnHookConfigBase -Path $clientCfgTemplate
            $effectiveBase = Get-OnHookConfigBase -Path $preflightClientCfg
            if ([string] $effectiveBase.Sha256 -ne [string] $templateBase.Sha256 -or
                    [string] $effectiveBase.Settings.LanJoinAddress -ne "127.0.0.1" -or
                    [string] $effectiveBase.Settings.LanJoinPort -ne [string] $case.Port -or
                    [string] $effectiveBase.Settings.LanJoinCallsign -ne [string] $case.Joiner) {
                throw "OR client cfg preflight failed for case=$($case.Slug) " +
                    "base=$($effectiveBase.Sha256)/$($templateBase.Sha256) " +
                    "address=$($effectiveBase.Settings.LanJoinAddress) " +
                    "port=$($effectiveBase.Settings.LanJoinPort) " +
                    "callsign=$($effectiveBase.Settings.LanJoinCallsign)"
            }
        }
    }
    finally {
        if (Test-Path -LiteralPath $preflightClientCfg) {
            Remove-Item -LiteralPath $preflightClientCfg -Force
        }
    }
    Write-Host "MATRIX_PREFLIGHT=PASS"
    Write-Host "MATRIX_SUITE_ID=$suiteId"
    Write-Host "MATRIX_CORPUS_FINGERPRINT=$corpusFingerprint"
    Write-Host "MATRIX_ARTIFACTS=$($artifactHashes.Count)"
    Write-Host "MATRIX_SOURCE_STATE=$($repositorySourceState.sha256)"
    Write-Host "MATRIX_SOURCE_ENTRIES=$($repositorySourceState.entry_count)"
    return
}
New-Item -ItemType Directory -Path $suiteRoot -Force | Out-Null
[pscustomobject]@{
    suite_id = $suiteId
    suite_prefix = $SuitePrefix
    corpus_fingerprint = $corpusFingerprint
    artifact_hashes = $artifactHashes
    source_state = $repositorySourceState
    runtime_corpus = $runtimeCorpus
    mission_bindings = @($missionBindings | ForEach-Object {
        [pscustomobject]@{
            mission = $_.Mission
            role = $_.MissionRole
            sha256 = $artifactHashes[$_.MissionRole]
        }
    })
    run_ids = $runIds
} | ConvertTo-Json -Depth 10 | Set-Content `
    -LiteralPath (Join-Path $suiteRoot "suite-provenance.json") -Encoding UTF8

foreach ($case in $cases) {
    foreach ($topology in @("RR", "RO", "OR", "OO")) {
        # Start every topology from the exact tracked role templates. onhook-mcp
        # then rewrites only its owned per-case role keys; each run snapshots
        # and later proves the untouched base against these hash-bound bytes.
        foreach ($binding in @(
            [pscustomobject]@{ Template = $serverCfgTemplate; Target = (Join-Path $serverDir "onhook.cfg") },
            [pscustomobject]@{ Template = $clientCfgTemplate; Target = (Join-Path $clientDir "onhook.cfg") }
        )) {
            Copy-Item -LiteralPath $binding.Template -Destination $binding.Target -Force
            if ((Get-FileHash -LiteralPath $binding.Template -Algorithm SHA256).Hash -ne
                    (Get-FileHash -LiteralPath $binding.Target -Algorithm SHA256).Hash) {
                throw "Deployed retail cfg differs from tracked template: $($binding.Target)"
            }
        }
        $runId = "$SuitePrefix-$($case.Slug)-$($topology.ToLowerInvariant())"
        $missionPath = Join-Path $corpusRoot "missions\$($case.Mission)"
        $arguments = @(
            "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $runner,
            "-Topology", $topology,
            "-Mission", $case.Mission,
            "-Port", [string] $case.Port,
            "-GameType", $case.GameType,
            "-RunId", $runId,
            "-SteadySeconds", [string] $case.Steady,
            "-Resolution", $case.Resolution,
            "-HostSessionName", $case.HostSessionName,
            "-ReadinessMode", $case.Readiness,
            "-HostCallsign", $case.Host,
            "-JoinerCallsign", $case.Joiner,
            "-SuiteId", $suiteId,
            "-CorpusFingerprint", $corpusFingerprint,
            "-MissionArtifactPath", $missionPath,
            "-MissionSha256", $artifactHashes[$case.MissionRole],
            "-RetailServerRoot", $serverDir,
            "-RetailClientRoot", $clientDir,
            "-OnHookMcpPath", $onHookMcp,
            "-GodotLauncher", $godotLauncher
        )
        if ($case.AutoDeploy -and $topology -in @("RO", "OO")) {
            $arguments += "-AutoDeploy"
        }
        if ($case.ExerciseInput) {
            $arguments += "-ExerciseInput"
        }
        Write-Host "MATRIX_CASE_BEGIN case=$($case.Slug) topology=$topology run=$runId"
        & powershell.exe @arguments
        if ($LASTEXITCODE -ne 0) {
            throw "Parity topology failed: case=$($case.Slug) topology=$topology exit=$LASTEXITCODE"
        }
        Write-Host "MATRIX_CASE_COMPLETE case=$($case.Slug) topology=$topology run=$runId"
    }
}

Write-Host "MATRIX_COMPLETE suite=$suiteId corpus=$corpusFingerprint source=$($repositorySourceState.sha256) cases=$($cases.Count) topologies=4"
