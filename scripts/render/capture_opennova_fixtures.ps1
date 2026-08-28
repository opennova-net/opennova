<#
.SYNOPSIS
Capture OpenNova render fixtures through the render_fixture_capture probe.

.DESCRIPTION
Launches one windowed OpenNova instance with its opennova-game MCP endpoint
(docs/mcp.md), then runs `game_probe render_fixture_capture` once per fixture
id and judges each run by the probe's verdict (its 11-file output contract:
5 variant PNGs + 5 state sidecars + 1 manifest is the probe's own check).
-FreshProcess restarts the game per id instead of reusing one process.

The probe refuses, before its publication transaction, when the window is
missing or the wrong size: run from an interactive desktop session.

Machine paths default to the documented roots (OPENNOVA_MISSION_CORPUS for the
loose .bms corpus, OPENNOVA_JO_DIR for the retail install, GODOT_BIN for the
binary) -- never tracked defaults.

.EXAMPLE
# Full sweep at the frozen commit
.\scripts\render\capture_opennova_fixtures.ps1 -SourceCommit $frozen

.EXAMPLE
# Retake two fixtures from the same source commit
.\scripts\render\capture_opennova_fixtures.ps1 -SourceCommit $frozen `
  -Ids 00tra-tire-marks-retail,cp01-water-wide-retail
#>
[CmdletBinding()]
param(
    # Fixture ids to capture; omitted = every fixture in the catalog.
    [string[]]$Ids = @(),
    [string]$Catalog = "docs/render/render-fixtures-retail-v5.json",
    # The frozen source commit the manifests bind. Defaults to HEAD, which is
    # only valid for evidence when the tree is clean and unrebuilt.
    [string]$SourceCommit = "",
    [string]$OutputRoot = "",
    [string]$MissionResourceDir = "",
    [string]$RuntimeResourceDir = "",
    [string]$Expansion = "revx02",
    [string]$CaptureMode = "hud_hidden",
    [string]$GodotBin = "",
    [ValidatePattern("^\d+x\d+$")][string]$Resolution = "2000x1200",
    [ValidateRange(1, 65535)][int]$McpPort = 8975,
    [switch]$FreshProcess
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "..\net\lib.ps1")
$repo = Get-RepoRoot
Set-Location $repo

if (-not $SourceCommit) { $SourceCommit = (& git rev-parse HEAD).Trim() }
if ($SourceCommit -notmatch "^[0-9a-f]{40}$") {
    throw "SourceCommit must be a full lowercase 40-hex sha: $SourceCommit"
}
if (-not $MissionResourceDir) { $MissionResourceDir = Get-OpenNovaMissionCorpus }
if (-not $MissionResourceDir -or -not (Test-Path $MissionResourceDir)) {
    throw "MissionResourceDir (or OPENNOVA_MISSION_CORPUS) must name the loose .bms corpus"
}
if (-not $RuntimeResourceDir) { $RuntimeResourceDir = Get-OpenNovaRetailInstall }
if (-not $RuntimeResourceDir -or -not (Test-Path $RuntimeResourceDir)) {
    throw "RuntimeResourceDir (or OPENNOVA_JO_DIR) must name the retail install"
}
if ($GodotBin) { $env:GODOT_BIN = $GodotBin }
if (-not (Find-GodotBinary)) { throw "Godot 4.6.1 was not found; set GODOT_BIN or pass -GodotBin" }
if (-not $OutputRoot) { $OutputRoot = Join-Path $repo ".scratch\golden\render\fixtures" }
$MissionResourceDir = (Resolve-Path -LiteralPath $MissionResourceDir).Path
$RuntimeResourceDir = (Resolve-Path -LiteralPath $RuntimeResourceDir).Path

$catalogPath = Join-Path $repo $Catalog
$catalogDoc = Get-Content $catalogPath -Raw | ConvertFrom-Json
$known = @($catalogDoc.fixtures | ForEach-Object { $_.id })
if ($Ids.Count -eq 0) { $Ids = $known }
foreach ($id in $Ids) {
    if ($known -notcontains $id) { throw "unknown fixture id: $id" }
}

$dll = Join-Path $repo "godot\bin\libopennova.windows.template_debug.x86_64.dll"
if (-not (Test-Path $dll)) { throw "GDExtension not built: $dll (run scripts/build_godot.sh)" }
$catalogRes = "res://../" + ($Catalog -replace "\\", "/")

$script:Game = $null
function Start-CaptureGame {
    $script:Game = Start-OpenNovaProcess `
        -GodotArguments @("--windowed", "--resolution", $Resolution) `
        -GameArguments @("--resource-dir", $RuntimeResourceDir, "/exp", $Expansion) `
        -McpPort $McpPort
    Write-Output "GAME pid=$($script:Game.Id) mcp=$(Get-GameMcpUrl -Port $McpPort)"
}
function Stop-CaptureGame {
    if (-not $script:Game) { return }
    $null = Stop-GameViaMcp -Port $McpPort -Process $script:Game
    Stop-OpenNovaProcess -Process $script:Game
    $script:Game = $null
}

$failed = @()
try {
    Start-CaptureGame
    foreach ($id in $Ids) {
        if ($FreshProcess -and -not $script:Game) { Start-CaptureGame }
        $outDir = Join-Path $OutputRoot $id
        if (Test-Path $outDir) { Remove-Item -Recurse -Force $outDir }
        $txn = "$outDir.publication-transaction"
        if (Test-Path $txn) { Remove-Item -Recurse -Force $txn }

        $status = Invoke-GameProbe -Port $McpPort -Name "render_fixture_capture" -TimeoutSeconds 1800 -Arguments @{
            id = $id
            catalog = $catalogRes
            mode = $CaptureMode
            output_dir = $outDir
            mission_resource_dir = $MissionResourceDir
            source_commit = $SourceCommit
            gdextension_binary = $dll
        } -OnLine { param($line) Write-Verbose ("[{0}] {1}" -f $id, $line.text) }

        $count = 0
        if (Test-Path $outDir) { $count = (Get-ChildItem $outDir -File).Count }
        $ok = ([string] $status.state -eq "passed") -and $status.verdict -and [bool] $status.verdict.ok
        if ($ok -and $count -eq 11) {
            Write-Output "PASS  $id"
        } else {
            $failed += $id
            $why = if ($status.verdict) { [string] $status.verdict.summary } else { [string] $status.error }
            Write-Output "FAIL  $id (state=$($status.state) files=$count) $why"
        }
        if ($FreshProcess) { Stop-CaptureGame }
    }
}
finally {
    Stop-CaptureGame
}

Write-Output "=== SUMMARY ==="
if ($failed.Count -eq 0) {
    Write-Output "all $($Ids.Count) captures PASS (commit $($SourceCommit.Substring(0,9)))"
} else {
    Write-Output ("FAILED (" + $failed.Count + "): " + ($failed -join ", "))
    exit 1
}
