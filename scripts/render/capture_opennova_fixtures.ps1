<#
.SYNOPSIS
Capture OpenNova render fixtures through the exact-pose probe, one id at a time.

.DESCRIPTION
Runs `godot/tests/render_fixture_capture_probe.tscn` once per fixture id and judges
each run by the probe's 11-file output contract (5 variant PNGs + 5 state
sidecars + 1 manifest) -- never by stdout, which PowerShell 5.1 garbles for
the non-console Godot binary.

Must run in the FOREGROUND of an interactive desktop session: from a detached
or background shell the probe takes its publication-transaction lock and
aborts having written only owner.json, failing every fixture with no message.

Machine paths (mission corpus, retail install, Godot binary) are read from the
environment or parameters -- never tracked defaults.

.EXAMPLE
# Full sweep at the frozen commit
.\scripts\render\capture_opennova_fixtures.ps1 -SourceCommit $frozen

.EXAMPLE
# Retake two fixtures from the same source commit
.\scripts\render\capture_opennova_fixtures.ps1 -SourceCommit $frozen `
  -Ids 00tra-tire-marks-retail,cp01-water-wide-retail
#>
param(
    # Fixture ids to capture; omitted = every fixture in the catalog.
    [string[]]$Ids = @(),
    [string]$Catalog = "docs/render/render-fixtures-retail-v4.json",
    # The frozen source commit the manifests bind. Defaults to HEAD, which is
    # only valid for evidence when the tree is clean and unrebuilt.
    [string]$SourceCommit = "",
    [string]$OutputRoot = "",
    [string]$MissionResourceDir = $env:NOVA_MISSION_RESOURCE_DIR,
    [string]$RuntimeResourceDir = $env:NOVA_RUNTIME_RESOURCE_DIR,
    [string]$Expansion = "revx02",
    [string]$CaptureMode = "hud_hidden",
    [string]$GodotBin = $env:GODOT_BIN,
    [ValidatePattern("^\d+x\d+$")][string]$Resolution = "2000x1200"
)

$ErrorActionPreference = "Stop"
$repo = (& git rev-parse --show-toplevel).Trim() -replace "/", "\"
Set-Location $repo

if (-not $SourceCommit) { $SourceCommit = (& git rev-parse HEAD).Trim() }
if ($SourceCommit -notmatch "^[0-9a-f]{40}$") {
    throw "SourceCommit must be a full lowercase 40-hex sha: $SourceCommit"
}
if (-not $MissionResourceDir -or -not (Test-Path $MissionResourceDir)) {
    throw "MissionResourceDir (or NOVA_MISSION_RESOURCE_DIR) must name the loose .bms corpus"
}
if (-not $RuntimeResourceDir -or -not (Test-Path $RuntimeResourceDir)) {
    throw "RuntimeResourceDir (or NOVA_RUNTIME_RESOURCE_DIR) must name the retail install"
}
if (-not $GodotBin) {
    # Worktrees have no .godot-bin of their own; use the main checkout's.
    $common = (& git rev-parse --path-format=absolute --git-common-dir).Trim()
    $GodotBin = Join-Path (Split-Path $common -Parent) ".godot-bin\Godot_v4.6.1-stable_win64.exe"
}
if (-not (Test-Path $GodotBin)) { throw "Godot binary not found: $GodotBin" }
if (-not $OutputRoot) { $OutputRoot = Join-Path $repo ".scratch\golden\render\fixtures" }

$catalogPath = Join-Path $repo $Catalog
$catalogDoc = Get-Content $catalogPath -Raw | ConvertFrom-Json
$known = @($catalogDoc.fixtures | ForEach-Object { $_.id })
if ($Ids.Count -eq 0) { $Ids = $known }
foreach ($id in $Ids) {
    if ($known -notcontains $id) { throw "unknown fixture id: $id" }
}

$dll = Join-Path $repo "godot\bin\libopennova.windows.template_debug.x86_64.dll"
if (-not (Test-Path $dll)) { throw "GDExtension not built: $dll (run scripts/build_godot.sh)" }

$env:GODOT_BIN = $GodotBin
$env:NOVA_EVIDENCE_SOURCE_COMMIT = $SourceCommit
$env:NOVA_GDEXTENSION_BINARY = $dll
$env:NOVA_RENDER_FIXTURE_CATALOG = "res://../" + ($Catalog -replace "\\", "/")
$env:NOVA_RENDER_CAPTURE_MODE = $CaptureMode
$env:NOVA_MISSION_RESOURCE_DIR = $MissionResourceDir
$env:NOVA_RUNTIME_RESOURCE_DIR = $RuntimeResourceDir
$env:NOVA_EXPANSION = $Expansion

$failed = @()
foreach ($id in $Ids) {
    $outDir = Join-Path $OutputRoot $id
    $env:NOVA_RENDER_FIXTURE_ID = $id
    $env:NOVA_RENDER_FIXTURE_OUTPUT = $outDir
    if (Test-Path $outDir) { Remove-Item -Recurse -Force $outDir }
    $txn = "$outDir.publication-transaction"
    if (Test-Path $txn) { Remove-Item -Recurse -Force $txn }

    & $GodotBin --path godot --resolution $Resolution `
        res://tests/render_fixture_capture_probe.tscn 2>$null | Out-Null

    $count = 0
    if (Test-Path $outDir) { $count = (Get-ChildItem $outDir -File).Count }
    if ($count -eq 11) {
        Write-Output "PASS  $id"
    } else {
        $failed += $id
        Write-Output "FAIL  $id (files=$count)"
    }
}

Write-Output "=== SUMMARY ==="
if ($failed.Count -eq 0) {
    Write-Output "all $($Ids.Count) captures PASS (commit $($SourceCommit.Substring(0,9)))"
} else {
    Write-Output ("FAILED (" + $failed.Count + "): " + ($failed -join ", "))
    exit 1
}
