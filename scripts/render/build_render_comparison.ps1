[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$FixtureId,
    [Parameter(Mandatory = $true)]
    [string]$RetailImage,
    [Parameter(Mandatory = $true)]
    [string]$BeforeImage,
    [Parameter(Mandatory = $true)]
    [string]$AfterImage,
    [Parameter(Mandatory = $true)]
    [string]$OutputImage,
    [Parameter(Mandatory = $true)]
    [string]$HeatmapImage,
    [Parameter(Mandatory = $true)]
    [string]$MetricsJson,
    [string]$RetailLabel = "RETAIL",
    [string]$BeforeLabel = "OPENNOVA - PRE-FIX",
    [string]$AfterLabel = "OPENNOVA - FINAL",
    [ValidateSet("retail-parity", "subsystem-ab")]
    [string]$ComparisonMode = "retail-parity",
    [string]$HeatmapLabel = ""
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

function Resolve-RequiredImage([string]$Path, [string]$Label) {
    $absolute = ConvertTo-RepoAbsolutePath $Path
    if (-not (Test-Path -LiteralPath $absolute -PathType Leaf)) {
        throw "$Label image not found: $absolute"
    }
    return (Resolve-Path -LiteralPath $absolute).Path
}

function Initialize-OutputPath([string]$Path) {
    $absolute = ConvertTo-RepoAbsolutePath $Path
    $directory = [System.IO.Path]::GetDirectoryName($absolute)
    [void](New-Item -ItemType Directory -Path $directory -Force)
    return $absolute
}

Add-Type -AssemblyName System.Drawing

if (-not ("OpenNovaRenderDelta" -as [type])) {
    Add-Type -ReferencedAssemblies "System.Drawing.dll" -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public sealed class OpenNovaDeltaMetric
{
    public double MeanAbsChannelDelta { get; set; }
    public int MaxChannelDelta { get; set; }
    public long ChangedPixels { get; set; }
}

public static class OpenNovaRenderDelta
{
    public static OpenNovaDeltaMetric Compute(
        Bitmap reference, Bitmap candidate, string heatmapPath)
    {
        if (reference.Width != candidate.Width || reference.Height != candidate.Height)
            throw new ArgumentException("comparison bitmaps must have identical dimensions");

        int width = reference.Width;
        int height = reference.Height;
        Rectangle rect = new Rectangle(0, 0, width, height);
        BitmapData refData = null;
        BitmapData candidateData = null;
        Bitmap heatmap = null;
        BitmapData heatmapData = null;
        try
        {
            refData = reference.LockBits(rect, ImageLockMode.ReadOnly,
                PixelFormat.Format32bppArgb);
            candidateData = candidate.LockBits(rect, ImageLockMode.ReadOnly,
                PixelFormat.Format32bppArgb);
            int refStride = Math.Abs(refData.Stride);
            int candidateStride = Math.Abs(candidateData.Stride);
            byte[] refBytes = new byte[refStride * height];
            byte[] candidateBytes = new byte[candidateStride * height];
            Marshal.Copy(refData.Scan0, refBytes, 0, refBytes.Length);
            Marshal.Copy(candidateData.Scan0, candidateBytes, 0, candidateBytes.Length);

            byte[] heatmapBytes = null;
            int heatmapStride = 0;
            if (!String.IsNullOrEmpty(heatmapPath))
            {
                heatmap = new Bitmap(width, height, PixelFormat.Format32bppArgb);
                heatmapData = heatmap.LockBits(rect, ImageLockMode.WriteOnly,
                    PixelFormat.Format32bppArgb);
                heatmapStride = Math.Abs(heatmapData.Stride);
                heatmapBytes = new byte[heatmapStride * height];
            }

            long sum = 0;
            long changedPixels = 0;
            int maxDelta = 0;
            for (int y = 0; y < height; ++y)
            {
                int refRow = y * refStride;
                int candidateRow = y * candidateStride;
                int heatmapRow = y * heatmapStride;
                for (int x = 0; x < width; ++x)
                {
                    int refOffset = refRow + x * 4;
                    int candidateOffset = candidateRow + x * 4;
                    int db = Math.Abs(refBytes[refOffset] - candidateBytes[candidateOffset]);
                    int dg = Math.Abs(refBytes[refOffset + 1] - candidateBytes[candidateOffset + 1]);
                    int dr = Math.Abs(refBytes[refOffset + 2] - candidateBytes[candidateOffset + 2]);
                    int pixelMax = Math.Max(dr, Math.Max(dg, db));
                    sum += dr + dg + db;
                    if (pixelMax != 0)
                        ++changedPixels;
                    if (pixelMax > maxDelta)
                        maxDelta = pixelMax;
                    if (heatmapBytes != null)
                    {
                        int heatmapOffset = heatmapRow + x * 4;
                        int intensity = Math.Min(255, pixelMax * 4);
                        heatmapBytes[heatmapOffset] = 0;
                        heatmapBytes[heatmapOffset + 1] = (byte)Math.Max(0, (intensity - 128) * 2);
                        heatmapBytes[heatmapOffset + 2] = (byte)intensity;
                        heatmapBytes[heatmapOffset + 3] = 255;
                    }
                }
            }

            if (heatmapBytes != null)
            {
                Marshal.Copy(heatmapBytes, 0, heatmapData.Scan0, heatmapBytes.Length);
                heatmap.UnlockBits(heatmapData);
                heatmapData = null;
                heatmap.Save(heatmapPath, ImageFormat.Png);
            }

            return new OpenNovaDeltaMetric {
                MeanAbsChannelDelta = (double)sum / (width * height * 3L),
                MaxChannelDelta = maxDelta,
                ChangedPixels = changedPixels,
            };
        }
        finally
        {
            if (heatmapData != null && heatmap != null)
                heatmap.UnlockBits(heatmapData);
            if (candidateData != null)
                candidate.UnlockBits(candidateData);
            if (refData != null)
                reference.UnlockBits(refData);
            if (heatmap != null)
                heatmap.Dispose();
        }
    }
}
'@
}

function New-NormalizedBitmap(
    [System.Drawing.Bitmap]$Source,
    [int]$Width,
    [int]$Height
) {
    $normalized = [System.Drawing.Bitmap]::new(
        $Width, $Height, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($normalized)
    try {
        $graphics.CompositingMode =
            [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
        $graphics.CompositingQuality =
            [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
        $graphics.InterpolationMode =
            [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $graphics.PixelOffsetMode =
            [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $graphics.DrawImage($Source, 0, 0, $Width, $Height)
    }
    finally {
        $graphics.Dispose()
    }
    return $normalized
}

function Draw-Panel(
    [System.Drawing.Graphics]$Graphics,
    [System.Drawing.Bitmap]$Bitmap,
    [string]$Label,
    [int]$Column,
    [int]$Row,
    [System.Drawing.Font]$Font,
    [System.Drawing.Brush]$HeaderBrush,
    [System.Drawing.Brush]$LabelBrush,
    [System.Drawing.Pen]$BorderPen
) {
    $panelWidth = 800
    $imageHeight = 450
    $headerHeight = 42
    $rowHeight = $headerHeight + $imageHeight
    $x = $Column * $panelWidth
    $y = $Row * $rowHeight
    $Graphics.FillRectangle($HeaderBrush, $x, $y, $panelWidth, $headerHeight)
    $Graphics.DrawString($Label, $Font, $LabelBrush,
        [System.Drawing.PointF]::new([float]($x + 12), [float]($y + 11)))
    $Graphics.DrawImage($Bitmap,
        [System.Drawing.Rectangle]::new($x, $y + $headerHeight, $panelWidth, $imageHeight),
        [System.Drawing.Rectangle]::new(0, 0, $Bitmap.Width, $Bitmap.Height),
        [System.Drawing.GraphicsUnit]::Pixel)
    $Graphics.DrawRectangle($BorderPen, $x, $y, $panelWidth - 1, $rowHeight - 1)
}

$retailPath = Resolve-RequiredImage $RetailImage "Retail"
$beforePath = Resolve-RequiredImage $BeforeImage "Pre-fix OpenNova"
$afterPath = Resolve-RequiredImage $AfterImage "Final OpenNova"
$outputPath = Initialize-OutputPath $OutputImage
$heatmapPath = Initialize-OutputPath $HeatmapImage
$metricsPath = Initialize-OutputPath $MetricsJson

$retailSource = [System.Drawing.Bitmap]::new($retailPath)
$beforeSource = [System.Drawing.Bitmap]::new($beforePath)
$afterSource = [System.Drawing.Bitmap]::new($afterPath)
$retail = $null
$before = $null
$after = $null
$heatmap = $null
$sheet = $null
$graphics = $null
$font = $null
$headerBrush = $null
$labelBrush = $null
$borderPen = $null
try {
    foreach ($entry in @(
            @{Label = "Retail"; Image = $retailSource},
            @{Label = "Pre-fix"; Image = $beforeSource},
            @{Label = "Final"; Image = $afterSource})) {
        if ($entry.Image.Width * 9 -ne $entry.Image.Height * 16) {
            throw "$($entry.Label) capture must be 16:9, got " +
                "$($entry.Image.Width)x$($entry.Image.Height)."
        }
    }
	if ($beforeSource.Width -ne $retailSource.Width -or
			$beforeSource.Height -ne $retailSource.Height -or
			$afterSource.Width -ne $retailSource.Width -or
			$afterSource.Height -ne $retailSource.Height) {
		throw ("Retail, pre-fix, and final captures must have identical dimensions; " +
			"got retail={0}x{1}, pre-fix={2}x{3}, final={4}x{5}." -f
			$retailSource.Width, $retailSource.Height,
			$beforeSource.Width, $beforeSource.Height,
			$afterSource.Width, $afterSource.Height)
	}

	$comparisonWidth = $retailSource.Width
	$comparisonHeight = $retailSource.Height

	$beforeMetric = [OpenNovaRenderDelta]::Compute($retailSource, $beforeSource, "")
	$afterMetric = [OpenNovaRenderDelta]::Compute(
		$retailSource, $afterSource, $heatmapPath)
    $resolvedHeatmapLabel = $HeatmapLabel
    if ([string]::IsNullOrWhiteSpace($resolvedHeatmapLabel)) {
        $resolvedHeatmapLabel = "$AfterLabel vs $RetailLabel heatmap"
    }
    $heatmap = [System.Drawing.Bitmap]::new($heatmapPath)

    $sheet = [System.Drawing.Bitmap]::new(
        1600, 984, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $graphics = [System.Drawing.Graphics]::FromImage($sheet)
    $graphics.Clear([System.Drawing.Color]::FromArgb(10, 10, 10))
    $graphics.CompositingQuality =
        [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
    $graphics.InterpolationMode =
        [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $graphics.PixelOffsetMode =
        [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $font = [System.Drawing.Font]::new(
        "Segoe UI", 11.0, [System.Drawing.FontStyle]::Bold,
        [System.Drawing.GraphicsUnit]::Point)
    $headerBrush = [System.Drawing.SolidBrush]::new(
        [System.Drawing.Color]::FromArgb(20, 20, 20))
    $labelBrush = [System.Drawing.SolidBrush]::new(
        [System.Drawing.Color]::FromArgb(235, 235, 235))
    $borderPen = [System.Drawing.Pen]::new(
        [System.Drawing.Color]::FromArgb(80, 80, 80), 1.0)

	Draw-Panel $graphics $retailSource $RetailLabel 0 0 $font $headerBrush $labelBrush $borderPen
	Draw-Panel $graphics $beforeSource $BeforeLabel 1 0 $font $headerBrush $labelBrush $borderPen
    Draw-Panel $graphics $afterSource $AfterLabel 0 1 $font $headerBrush $labelBrush $borderPen
    Draw-Panel $graphics $heatmap (
        "{0} - mean {1:N3}, max {2}" -f
        $resolvedHeatmapLabel,
        $afterMetric.MeanAbsChannelDelta, $afterMetric.MaxChannelDelta
    ) 1 1 $font $headerBrush $labelBrush $borderPen
    $sheet.Save($outputPath, [System.Drawing.Imaging.ImageFormat]::Png)

    $report = [ordered]@{
        schema = "opennova.render-comparison.v1"
        fixture_id = $FixtureId
        comparison_mode = $ComparisonMode
        labels = [ordered]@{
            reference = $RetailLabel
            before = $BeforeLabel
            after = $AfterLabel
            heatmap = $resolvedHeatmapLabel
        }
        comparison_size = [ordered]@{
            width = $comparisonWidth
            height = $comparisonHeight
        }
		resampled = $false
		source_sizes = [ordered]@{
			retail = [ordered]@{
				width = $retailSource.Width
				height = $retailSource.Height
			}
			before = [ordered]@{
				width = $beforeSource.Width
				height = $beforeSource.Height
			}
			after = [ordered]@{
				width = $afterSource.Width
				height = $afterSource.Height
			}
		}
        retail_vs_before = [ordered]@{
            mean_abs_channel_delta = $beforeMetric.MeanAbsChannelDelta
            max_channel_delta = $beforeMetric.MaxChannelDelta
            changed_pixels = $beforeMetric.ChangedPixels
        }
        retail_vs_after = [ordered]@{
            mean_abs_channel_delta = $afterMetric.MeanAbsChannelDelta
            max_channel_delta = $afterMetric.MaxChannelDelta
            changed_pixels = $afterMetric.ChangedPixels
        }
        sha256 = [ordered]@{
            retail = (Get-FileHash -LiteralPath $retailPath -Algorithm SHA256).Hash.ToLowerInvariant()
            before = (Get-FileHash -LiteralPath $beforePath -Algorithm SHA256).Hash.ToLowerInvariant()
            after = (Get-FileHash -LiteralPath $afterPath -Algorithm SHA256).Hash.ToLowerInvariant()
            sheet = (Get-FileHash -LiteralPath $outputPath -Algorithm SHA256).Hash.ToLowerInvariant()
            heatmap = (Get-FileHash -LiteralPath $heatmapPath -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $metricsPath -Encoding UTF8
}
finally {
    if ($borderPen -ne $null) { $borderPen.Dispose() }
    if ($labelBrush -ne $null) { $labelBrush.Dispose() }
    if ($headerBrush -ne $null) { $headerBrush.Dispose() }
    if ($font -ne $null) { $font.Dispose() }
    if ($graphics -ne $null) { $graphics.Dispose() }
    if ($sheet -ne $null) { $sheet.Dispose() }
    if ($heatmap -ne $null) { $heatmap.Dispose() }
    if ($after -ne $null) { $after.Dispose() }
    if ($before -ne $null) { $before.Dispose() }
    if ($retail -ne $null) { $retail.Dispose() }
    $afterSource.Dispose()
    $beforeSource.Dispose()
    $retailSource.Dispose()
}

Write-Host "Wrote comparison sheet: $outputPath"
Write-Host "Wrote heatmap: $heatmapPath"
Write-Host "Wrote metrics: $metricsPath"
