[CmdletBinding()]
param(
    [string]$MaxBatch = $env:OPENNOVA_3DSMAXBATCH,
    [string]$OutputRoot = "",
    [string[]]$Fixture = @(),
    [int]$TimeoutSeconds = 240
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Resolve-Path (Join-Path $scriptDir "..")
$runner = Join-Path $scriptDir "max_dcc_roundtrip_runner.py"

function Find-3dsMaxBatch {
    $candidates = @()
    $autodeskRoot = "C:\Program Files\Autodesk"
    if (Test-Path -LiteralPath $autodeskRoot) {
        $candidates = @(Get-ChildItem -LiteralPath $autodeskRoot -Directory -Filter "3ds Max *" |
            ForEach-Object { Join-Path $_.FullName "3dsmaxbatch.exe" } |
            Where-Object { Test-Path -LiteralPath $_ } |
            Sort-Object -Descending)
    }
    if ($candidates.Count -gt 0) {
        return $candidates[0]
    }
    return $null
}

if (-not $MaxBatch) {
    $MaxBatch = Find-3dsMaxBatch
}

if (-not $MaxBatch -or -not (Test-Path -LiteralPath $MaxBatch)) {
    throw "3dsmaxbatch.exe was not found. Set OPENNOVA_3DSMAXBATCH or pass -MaxBatch."
}

Push-Location $root
$oldOutputRoot = $env:OPENNOVA_MAX_DCC_OUTPUT_ROOT
$oldFixtures = $env:OPENNOVA_MAX_DCC_FIXTURES
$oldTimeout = $env:OPENNOVA_MAX_DCC_TIMEOUT_S
try {
    $effectiveOutputRoot = $OutputRoot
    if (-not $effectiveOutputRoot -and $env:OPENNOVA_MAX_DCC_OUTPUT_ROOT) {
        $effectiveOutputRoot = $env:OPENNOVA_MAX_DCC_OUTPUT_ROOT
    }
    if (-not $effectiveOutputRoot) {
        $effectiveOutputRoot = Join-Path $root "build-max-dcc"
    }

    if ($OutputRoot) {
        $env:OPENNOVA_MAX_DCC_OUTPUT_ROOT = $OutputRoot
    }
    if ($Fixture.Count -gt 0) {
        $env:OPENNOVA_MAX_DCC_FIXTURES = ($Fixture -join ",")
    }
    $env:OPENNOVA_MAX_DCC_TIMEOUT_S = [string]$TimeoutSeconds

    & $MaxBatch $runner
    $exitCode = $LASTEXITCODE
    $summaryPath = Join-Path $effectiveOutputRoot "summary.json"
    if ((Test-Path -LiteralPath $summaryPath)) {
        $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
        if ($summary.failed -eq $false) {
            if ($exitCode -ne 0) {
                Write-Warning "3dsmaxbatch returned $exitCode, but $summaryPath reports a successful OpenNova Max DCC run."
            }
            exit 0
        }
    }
    exit $exitCode
}
finally {
    if ($null -eq $oldOutputRoot) {
        Remove-Item Env:\OPENNOVA_MAX_DCC_OUTPUT_ROOT -ErrorAction SilentlyContinue
    } else {
        $env:OPENNOVA_MAX_DCC_OUTPUT_ROOT = $oldOutputRoot
    }
    if ($null -eq $oldFixtures) {
        Remove-Item Env:\OPENNOVA_MAX_DCC_FIXTURES -ErrorAction SilentlyContinue
    } else {
        $env:OPENNOVA_MAX_DCC_FIXTURES = $oldFixtures
    }
    if ($null -eq $oldTimeout) {
        Remove-Item Env:\OPENNOVA_MAX_DCC_TIMEOUT_S -ErrorAction SilentlyContinue
    } else {
        $env:OPENNOVA_MAX_DCC_TIMEOUT_S = $oldTimeout
    }
    Pop-Location
}
