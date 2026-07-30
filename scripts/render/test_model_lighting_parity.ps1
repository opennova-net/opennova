[CmdletBinding()]
param(
    [string]$GodotPath = $env:GODOT_BIN,
    [string]$MissionResourceDir = $env:NOVA_MISSION_RESOURCE_DIR,
    [string]$RuntimeResourceDir = $env:NOVA_RUNTIME_RESOURCE_DIR,
    [string]$Expansion = "revx02",
    [string]$Mission = "00TRa.bms",
    [string]$RetailImage,
    [string]$OutputDir,
    [string]$CurrentImage,
    [int]$ExpectedInteriorItemId = 101216,
    [switch]$SkipCapture
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
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
        $MissionResourceDir = $env:NOVA_RESOURCE_DIR
    }
    if ([string]::IsNullOrWhiteSpace($RuntimeResourceDir)) {
        $RuntimeResourceDir = $env:OPENNOVA_JO_DIR
    }
    if ([string]::IsNullOrWhiteSpace($GodotPath)) {
        throw "Set GODOT_BIN or pass -GodotPath to run the capture."
    }
    if ([string]::IsNullOrWhiteSpace($MissionResourceDir)) {
        throw "Set NOVA_MISSION_RESOURCE_DIR or pass -MissionResourceDir " +
            "to locate the loose authoring mission."
    }
    if ([string]::IsNullOrWhiteSpace($RuntimeResourceDir)) {
        throw "Set NOVA_RUNTIME_RESOURCE_DIR or pass -RuntimeResourceDir " +
            "to locate the packed retail install."
    }
    if ([string]::IsNullOrWhiteSpace($Expansion)) {
        throw "Pass an explicit -Expansion for the comparison capture."
    }
    Assert-FileExists $GodotPath "Godot executable"
    if (-not (Test-Path -LiteralPath $MissionResourceDir -PathType Container)) {
        throw "Loose mission authoring directory not found: $MissionResourceDir"
    }
    if (-not (Test-Path -LiteralPath $RuntimeResourceDir -PathType Container)) {
        throw "Packed runtime directory not found: $RuntimeResourceDir"
    }

    [void](New-Item -ItemType Directory -Path $OutputDir -Force)
    $captureStarted = [DateTime]::UtcNow
    $savedEnvironment = @{
        NOVA_MISSION_RESOURCE_DIR = $env:NOVA_MISSION_RESOURCE_DIR
        NOVA_RUNTIME_RESOURCE_DIR = $env:NOVA_RUNTIME_RESOURCE_DIR
        NOVA_EXPANSION = $env:NOVA_EXPANSION
        NOVA_MISSION_BMS = $env:NOVA_MISSION_BMS
        NOVA_SPAWN_CAPTURE_DIR = $env:NOVA_SPAWN_CAPTURE_DIR
        NOVA_MODEL_LIGHTING_TRACE = $env:NOVA_MODEL_LIGHTING_TRACE
    }
    try {
        $env:NOVA_MISSION_RESOURCE_DIR = $MissionResourceDir
        $env:NOVA_RUNTIME_RESOURCE_DIR = $RuntimeResourceDir
        $env:NOVA_EXPANSION = $Expansion
        $env:NOVA_MISSION_BMS = $Mission
        $env:NOVA_SPAWN_CAPTURE_DIR = $OutputDir
        $env:NOVA_MODEL_LIGHTING_TRACE = "1"

        # Godot reports known RID leaks on stderr while this probe shuts down.
        # Capture the complete native stream and let the probe PASS/fresh-image
        # contract below decide success.
        $savedErrorPreference = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        try {
            $probeOutput = @(
                & $GodotPath --path (Join-Path $repoRoot "godot") `
                    "res://tests/foliage_spawn_capture_probe.tscn" 2>&1 |
                    ForEach-Object {
                        $line = "$_"
                        if ($line -match "^\[spawn-capture\]" -or
                            $line -match "^(SCRIPT )?ERROR:" -or
                            $line -match "^RENDERER PARITY") {
                            Write-Host $line
                        }
                        $line
                    }
            )
            $probeExitCode = $LASTEXITCODE
        }
        finally {
            $ErrorActionPreference = $savedErrorPreference
        }
    }
    finally {
        foreach ($name in $savedEnvironment.Keys) {
            $value = $savedEnvironment[$name]
            if ($null -eq $value) {
                Remove-Item "Env:$name" -ErrorAction SilentlyContinue
            }
            else {
                Set-Item "Env:$name" $value
            }
        }
    }

    # The current capture scene reports known Godot RID leaks at shutdown, so its
    # native exit code is not the oracle. A fresh image plus the probe's own PASS
    # line is required; any launch/load/capture failure still remains fatal.
    if (-not ($probeOutput -match
        "\[spawn-capture\] PASS: exact frozen player-spawn state in both images")) {
        throw "Capture probe did not report PASS (native exit $probeExitCode)."
    }
    $expansionPattern = (
        "\[spawn-capture\] runtime expansion: requested={0} " +
        "actual={0} mount=packed"
    ) -f [regex]::Escape($Expansion)
    if (-not ($probeOutput -match $expansionPattern)) {
        throw "Capture did not prove the requested packed expansion '$Expansion'."
    }
    $sourceLines = @($probeOutput | Where-Object {
        "$_" -match "^\[spawn-capture\] runtime source:"
    })
    if ($sourceLines.Count -lt 3) {
        throw "Capture did not report all packed winning source entries."
    }
    foreach ($line in $sourceLines) {
        if ("$line" -notmatch "\ssource_type=pff\sarchive_path=.+") {
            throw "Capture reported a non-packed or missing winning source: $line"
        }
    }
    $missionPattern = (
        "^\[spawn-capture\] runtime source: logical_name={0} " +
        "source_type=pff archive_path=.+"
    ) -f [regex]::Escape((Split-Path -Leaf $Mission))
    if (-not ($sourceLines -match $missionPattern)) {
        throw "Capture did not report the packed winning source for $Mission."
    }
    if ($ExpectedInteriorItemId -gt 0) {
        $interiorPattern = '"local_player_interior_item_id":\s*{0}\b' -f
            $ExpectedInteriorItemId
        if (-not ($probeOutput -match $interiorPattern)) {
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
