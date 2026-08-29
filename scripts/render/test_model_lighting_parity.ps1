[CmdletBinding()]
param(
    [string]$GodotPath = "",
    [string]$MissionResourceDir = "",
    [string]$RuntimeResourceDir = "",
    [string]$Expansion = "revx02",
    [string]$Mission = "00TRa.bms",
    [string]$RetailImage,
    [string]$OutputDir,
    [string]$CurrentImage,
    [int]$ExpectedInteriorItemId = 101216,
    [ValidatePattern("^\d+x\d+$")][string]$Resolution = "1280x720",
    [ValidateRange(1, 65535)][int]$McpPort = 8975,
    [switch]$SkipCapture
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot "..\net\lib.ps1")

$repoRoot = Get-RepoRoot
function ConvertTo-RepoAbsolutePath([string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $repoRoot $Path))
}

if ([string]::IsNullOrWhiteSpace($RetailImage)) {
    $RetailImage = Join-Path $repoRoot `
        "screenshots\parity\model-lighting\courtyard-retail-revx02.png"
}
else {
    $RetailImage = ConvertTo-RepoAbsolutePath $RetailImage
}
if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = Join-Path $repoRoot ".scratch\model-lighting-parity"
}
else {
    $OutputDir = ConvertTo-RepoAbsolutePath $OutputDir
}
if ([string]::IsNullOrWhiteSpace($CurrentImage)) {
    $CurrentImage = Join-Path $OutputDir "00TRa_spawn_default.png"
}
else {
    $CurrentImage = ConvertTo-RepoAbsolutePath $CurrentImage
}

function Assert-FileExists([string]$Path, [string]$Label) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label not found: $Path"
    }
}

function Convert-CanonicalRoi(
    [int[]]$Canonical,
    [double]$Scale,
    [int]$OffsetX,
    [int]$OffsetY
) {
    return [int[]]@(
        ($OffsetX + [int][Math]::Round($Canonical[0] * $Scale))
        ($OffsetY + [int][Math]::Round($Canonical[1] * $Scale))
        [int][Math]::Round($Canonical[2] * $Scale)
        [int][Math]::Round($Canonical[3] * $Scale)
    )
}

function Get-MeanLuma([System.Drawing.Bitmap]$Bitmap, [int[]]$Roi) {
    $right = $Roi[0] + $Roi[2]
    $bottom = $Roi[1] + $Roi[3]
    if ($Roi[0] -lt 0 -or $Roi[1] -lt 0 -or
        $right -gt $Bitmap.Width -or $bottom -gt $Bitmap.Height) {
        throw "ROI $($Roi -join ',') exceeds $($Bitmap.Width)x$($Bitmap.Height) image"
    }

    [double]$sum = 0.0
    for ($y = $Roi[1]; $y -lt $bottom; $y++) {
        for ($x = $Roi[0]; $x -lt $right; $x++) {
            $pixel = $Bitmap.GetPixel($x, $y)
            $sum += 0.2126 * $pixel.R + 0.7152 * $pixel.G + 0.0722 * $pixel.B
        }
    }
    return $sum / ($Roi[2] * $Roi[3])
}

function Get-Median([double[]]$Values) {
    $sorted = [double[]]$Values.Clone()
    [Array]::Sort($sorted)
    $middle = [int][Math]::Floor($sorted.Length / 2)
    if (($sorted.Length % 2) -eq 1) {
        return $sorted[$middle]
    }
    return 0.5 * ($sorted[$middle - 1] + $sorted[$middle])
}

if (-not $SkipCapture) {
    if ([string]::IsNullOrWhiteSpace($MissionResourceDir)) {
        $MissionResourceDir = Get-OpenNovaRetailAssets
    }
    if ([string]::IsNullOrWhiteSpace($RuntimeResourceDir)) {
        $RuntimeResourceDir = Get-OpenNovaRetailInstall
    }
    if (-not [string]::IsNullOrWhiteSpace($GodotPath)) { $env:GODOT_BIN = $GodotPath }
    if (-not (Find-GodotBinary)) {
        throw "Set GODOT_BIN or pass -GodotPath to run the capture."
    }
    if ([string]::IsNullOrWhiteSpace($MissionResourceDir)) {
        throw "Set OPENNOVA_JO_ASSETS or pass -MissionResourceDir " +
            "to locate the loose authoring mission."
    }
    if ([string]::IsNullOrWhiteSpace($RuntimeResourceDir)) {
        throw "Set OPENNOVA_JO_DIR or pass -RuntimeResourceDir " +
            "to locate the packed retail install."
    }
    if ([string]::IsNullOrWhiteSpace($Expansion)) {
        throw "Pass an explicit -Expansion for the comparison capture."
    }
    if (-not (Test-Path -LiteralPath $MissionResourceDir -PathType Container)) {
        throw "Loose mission authoring directory not found: $MissionResourceDir"
    }
    if (-not (Test-Path -LiteralPath $RuntimeResourceDir -PathType Container)) {
        throw "Packed runtime directory not found: $RuntimeResourceDir"
    }
    $expectedMissionPath = (Resolve-Path -LiteralPath (
        Join-Path $MissionResourceDir $Mission)).Path

    [void](New-Item -ItemType Directory -Path $OutputDir -Force)
    $captureStarted = [DateTime]::UtcNow

    # One windowed game with its MCP endpoint; the capture is the
    # foliage_spawn_capture probe (docs/mcp.md), judged by its verdict and the
    # report it returns -- never by stdout.
    $game = Start-OpenNovaProcess `
        -GodotArguments @("--windowed", "--resolution", $Resolution) `
        -GameArguments @("--resource-dir", $RuntimeResourceDir, "/exp", $Expansion) `
        -McpPort $McpPort
    try {
        $status = Invoke-GameProbe -Port $McpPort -Name "foliage_spawn_capture" `
            -TimeoutSeconds 900 -Arguments @{
                mission = $Mission
                mission_path = $expectedMissionPath
                expansion = $Expansion
                output_dir = $OutputDir
                model_lighting_trace = $true
            } -OnLine { param($line) Write-Host $line.text }
    }
    finally {
        $null = Stop-GameViaMcp -Port $McpPort -Process $game
        Stop-OpenNovaProcess -Process $game
    }

    if ([string] $status.state -ne "passed" -or -not $status.verdict -or
            -not [bool] $status.verdict.ok) {
        $why = if ($status.verdict) { [string] $status.verdict.summary } else { [string] $status.error }
        throw "Capture probe did not PASS (state=$($status.state)): $why"
    }
    $report = $status.verdict.data
    if (-not $report -or -not ($report.PSObject.Properties.Name -contains "runtime_expansion")) {
        throw "Capture probe returned no runtime expansion report."
    }
    $expansionReport = $report.runtime_expansion
    if ([string] $expansionReport.requested -cne $Expansion -or
            [string] $expansionReport.actual -cne $Expansion -or
            [string] $expansionReport.mount -ne "packed") {
        throw "Capture did not prove the requested packed expansion '$Expansion'."
    }
    $sources = @($report.runtime_sources)
    if ($sources.Count -lt 3) {
        throw "Capture did not report the exact mission and packed dependency sources."
    }
    $sawExactLooseMission = $false
    foreach ($source in $sources) {
        $logicalName = [string] $source.logical_name
        $sourceType = [string] $source.source_type
        $sourcePath = [string] $source.source
        if ($sourceType -ieq 'pff') {
            if (-not $sourcePath.EndsWith('.pff',
                    [System.StringComparison]::OrdinalIgnoreCase) -or
                    -not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
                throw "Capture reported a missing packed dependency source: $logicalName <- $sourcePath"
            }
            continue
        }
        if ($sourceType -ieq 'loose' -and
                $logicalName -ieq (Split-Path -Leaf $Mission) -and
                [System.IO.Path]::GetFullPath($sourcePath) -ieq $expectedMissionPath) {
            $sawExactLooseMission = $true
            continue
        }
        throw "Capture reported an unexpected loose dependency source: $logicalName <- $sourcePath"
    }
    if (-not $sawExactLooseMission) {
        throw "Capture did not report the exact saved loose mission $expectedMissionPath."
    }
    if ($ExpectedInteriorItemId -gt 0) {
        $trace = $report.model_lighting_trace
        if (-not $trace -or [int] $trace.local_player_interior_item_id -ne $ExpectedInteriorItemId) {
            throw "Capture did not resolve expected interior items.def id " +
                "$ExpectedInteriorItemId."
        }
    }
    Assert-FileExists $CurrentImage "Fresh OpenNova capture"
    if ((Get-Item -LiteralPath $CurrentImage).LastWriteTimeUtc -lt
        $captureStarted.AddSeconds(-1)) {
        throw "OpenNova capture was not refreshed: $CurrentImage"
    }
}

Assert-FileExists $RetailImage "Committed retail revx02 capture"
Assert-FileExists $CurrentImage "OpenNova capture"
Add-Type -AssemblyName System.Drawing

$retail = [System.Drawing.Bitmap]::new($RetailImage)
$current = [System.Drawing.Bitmap]::new($CurrentImage)
try {
    if ($retail.Width * 9 -ne $retail.Height * 16) {
        throw "Retail capture must be 16:9, got $($retail.Width)x$($retail.Height)."
    }
    if ($current.Width * 9 -ne $current.Height * 16) {
        throw "OpenNova capture must be 16:9, got $($current.Width)x$($current.Height)."
    }

    # These broad, texture-stable regions avoid the HUD and viewmodel. Retail is
    # the fresh borderless revx02 client; both frames scale from 1280x720.
    $modelRegions = [ordered]@{
        ceiling = [int[]]@(280, 20, 500, 120)
        center_wall = [int[]]@(560, 190, 250, 130)
        right_pillar = [int[]]@(930, 190, 80, 140)
        floor_left = [int[]]@(220, 600, 260, 100)
        crate_left = [int[]]@(30, 300, 100, 230)
    }
    $grassRegion = [int[]]@(230, 470, 270, 100)
    $retailScale = $retail.Width / 1280.0
    $retailGrass = Get-MeanLuma $retail (
        Convert-CanonicalRoi $grassRegion $retailScale 0 0)
    $currentScale = $current.Width / 1280.0
    $currentGrass = Get-MeanLuma $current (
        Convert-CanonicalRoi $grassRegion $currentScale 0 0)

    [double[]]$brightnessFactors = @()
    Write-Host ""
    Write-Host "Model brightness (normalized to same-frame outdoor grass)"
    foreach ($entry in $modelRegions.GetEnumerator()) {
        $retailMean = Get-MeanLuma $retail (
            Convert-CanonicalRoi $entry.Value $retailScale 0 0)
        $currentMean = Get-MeanLuma $current (
            Convert-CanonicalRoi $entry.Value $currentScale 0 0)
        $factor = ($currentMean / $currentGrass) / ($retailMean / $retailGrass)
        $brightnessFactors += $factor
        Write-Host ("  {0,-13} retail={1,7:N2} current={2,7:N2} factor={3:N3}" -f
            $entry.Key, $retailMean, $currentMean, $factor)
    }
    $brightnessMedian = Get-Median $brightnessFactors

    Write-Host ""
    Write-Host ("Brightness median factor: {0:N3} (required 0.850..1.150)" -f
        $brightnessMedian)

    $failures = [System.Collections.Generic.List[string]]::new()
    if ($brightnessMedian -lt 0.85 -or $brightnessMedian -gt 1.15) {
        $failures.Add(
            "model brightness factor $($brightnessMedian.ToString('N3')) is outside retail tolerance")
    }
    if ($failures.Count -gt 0) {
        [Console]::Error.WriteLine(
            "RENDERER PARITY RED: " + ($failures -join "; "))
        exit 1
    }
    Write-Host "RENDERER PARITY PASS"
}
finally {
    $retail.Dispose()
    $current.Dispose()
}
