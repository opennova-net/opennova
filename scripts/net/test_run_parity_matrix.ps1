[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $PSScriptRoot 'lib.ps1')
$runner = Join-Path $PSScriptRoot 'run_parity_matrix.ps1'

$trackedMatrix = Get-Content -LiteralPath (
    Join-Path $PSScriptRoot 'parity-matrix.example.json') -Raw | ConvertFrom-Json
$crossoverCases = @($trackedMatrix.cases)
$crossoverBindings = Get-ParityMatrixBindings -Cases $crossoverCases
$tdmHoldout = @($crossoverCases | Where-Object {
    [string]$_.id -ceq '01tr-tdm-holdout'
})
$waypointHoldout = @($crossoverCases | Where-Object {
    [string]$_.id -ceq 'tdh-waypoint-holdout'
})
if ($crossoverBindings.CaseBindings.Count -ne 6 -or
        @($crossoverBindings.MissionBindings).Count -ne 4 -or
        @($trackedMatrix.required_case_ids).Count -ne 6 -or
        [string]$crossoverBindings.CaseBindings['01tr-waypoint'].Slug -cne '01tr' -or
        [string]$crossoverBindings.CaseBindings['01tr-tdm-holdout'].Slug -cne
            '01tr-tdm-holdout' -or
        [string]$crossoverBindings.CaseBindings['tdh-waypoint-holdout'].Slug -cne
            'tdh-waypoint-holdout' -or
        $tdmHoldout.Count -ne 1 -or
        [string]$tdmHoldout[0].mission -cne '01TR.bms' -or
        [uint32]$tdmHoldout[0].game_type -ne [uint32]65536 -or
        [int]$tdmHoldout[0].port -ne 32788 -or
        $waypointHoldout.Count -ne 1 -or
        [string]$waypointHoldout[0].mission -cne 'TDH_I3A.bms' -or
        [uint32]$waypointHoldout[0].game_type -ne [uint32]65568 -or
        [int]$waypointHoldout[0].port -ne 32789 -or
        -not (Test-ParityMatrixAutomationPorts -Cases $crossoverCases)) {
    throw 'Crossover matrix planning did not retain six cases over four deduplicated mission artifacts/ports.'
}
$weakPorts = @($crossoverCases | ForEach-Object {
    [pscustomobject]@{id=$_.id;mission=$_.mission;Port=32786}
})
if (Test-ParityMatrixAutomationPorts -Cases $weakPorts) {
    throw 'Crossover matrix port-floor mutation self-test failed.'
}

$suitePrefix = 'test-collision-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$suiteRoot = Join-Path $repo ".scratch\parity-final\suites\$suitePrefix"

if (Test-Path -LiteralPath $suiteRoot) {
    throw "Create-new test suite path unexpectedly exists: $suiteRoot"
}

New-Item -ItemType Directory -Path $suiteRoot -Force | Out-Null
try {
    $priorPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass `
            -File $runner -SuitePrefix $suitePrefix -PreflightOnly `
            -RetailServerRoot 'C:\missing-parity-server' `
            -RetailClientRoot 'C:\missing-parity-client' `
            -OnHookMcpPath 'C:\missing-onhook-mcp.exe' `
            -GodotLauncher 'C:\missing-godot.exe' 2>&1)
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $priorPreference
    }

    $text = $output -join "`n"
    if ($exitCode -eq 0 -or $text -notmatch 'SuitePrefix is not create-new') {
        throw "Preflight did not reject the occupied suite before machine validation. exit=$exitCode output=$text"
    }
}
finally {
    if (Test-Path -LiteralPath $suiteRoot) {
        Remove-Item -LiteralPath $suiteRoot -Recurse -Force
    }
}

$holdoutPrefix = 'test-holdout-collision-' +
    [guid]::NewGuid().ToString('N').Substring(0, 8)
$holdoutRunRoot = Join-Path $repo (
    ".scratch\runs\$holdoutPrefix-01tr-tdm-holdout-rr")
if (Test-Path -LiteralPath $holdoutRunRoot) {
    throw "Create-new holdout run path unexpectedly exists: $holdoutRunRoot"
}
New-Item -ItemType Directory -Path $holdoutRunRoot -Force | Out-Null
try {
    $priorPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass `
            -File $runner -SuitePrefix $holdoutPrefix -PreflightOnly `
            -RetailServerRoot 'C:\missing-parity-server' `
            -RetailClientRoot 'C:\missing-parity-client' `
            -OnHookMcpPath 'C:\missing-onhook-mcp.exe' `
            -GodotLauncher 'C:\missing-godot.exe' 2>&1)
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $priorPreference
    }

    $text = $output -join "`n"
    if ($exitCode -eq 0 -or $text -notmatch 'SuitePrefix is not create-new' -or
            $text -notmatch [regex]::Escape($holdoutRunRoot)) {
        throw "Preflight did not reject the occupied holdout run before machine validation. exit=$exitCode output=$text"
    }
}
finally {
    if (Test-Path -LiteralPath $holdoutRunRoot) {
        Remove-Item -LiteralPath $holdoutRunRoot -Recurse -Force
    }
}

$fixtureRoot = Join-Path ([IO.Path]::GetTempPath()) `
    ('opennova-parity-matrix-' + [guid]::NewGuid().ToString('N'))
$serverRoot = Join-Path $fixtureRoot 'server'
$clientRoot = Join-Path $fixtureRoot 'client'
$fakeBin = Join-Path $fixtureRoot 'bin'
$mcpPath = Join-Path $fixtureRoot 'onhook-mcp.exe'
$godotPath = Join-Path $fixtureRoot 'fake-godot.exe'
$expectedCorpus = Join-Path $repo '.scratch\parity\corpus'
$corpusSuitePrefix = 'test-corpus-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$priorPath = $env:PATH
$priorExpectedCorpus = $env:PARITY_TEST_EXPECTED_CORPUS

try {
    New-Item -ItemType Directory -Path $serverRoot, $clientRoot, $fakeBin -Force |
        Out-Null
    New-Item -ItemType File -Path $mcpPath, $godotPath -Force | Out-Null
    @(
        '@echo off'
        ':next'
        'if "%~1"=="" exit /b 39'
        'if /I "%~1"=="--output-dir" goto output_dir'
        'shift'
        'goto next'
        ':output_dir'
        'shift'
        'if /I "%~1"=="%PARITY_TEST_EXPECTED_CORPUS%" exit /b 37'
        'exit /b 38'
    ) | Set-Content -LiteralPath (Join-Path $fakeBin 'uv.cmd') -Encoding Ascii
    $env:PATH = "$fakeBin;$priorPath"
    $env:PARITY_TEST_EXPECTED_CORPUS = $expectedCorpus

    $priorPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass `
            -File $runner -SuitePrefix $corpusSuitePrefix -PreflightOnly `
            -RetailServerRoot $serverRoot -RetailClientRoot $clientRoot `
            -OnHookMcpPath $mcpPath -GodotLauncher $godotPath 2>&1)
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $priorPreference
    }

    $text = $output -join "`n"
    if ($exitCode -eq 0 -or
            $text -notmatch 'Runtime VFS corpus verification failed: 37') {
        throw "Preflight did not verify the documented corpus root '$expectedCorpus'. exit=$exitCode output=$text"
    }
}
finally {
    $env:PATH = $priorPath
    $env:PARITY_TEST_EXPECTED_CORPUS = $priorExpectedCorpus
    if (Test-Path -LiteralPath $fixtureRoot) {
        Remove-Item -LiteralPath $fixtureRoot -Recurse -Force
    }
}

Write-Host 'PARITY_MATRIX_SELF_TEST=PASS'
