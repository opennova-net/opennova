[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$CurrentImage,
    [string]$RetailImage,
    [string]$OutputImage
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
if ([string]::IsNullOrWhiteSpace($RetailImage)) {
    $RetailImage = Join-Path $repoRoot `
        "screenshots\parity\foliage-terrain-regrill\courtyard-retail-vs-ours.png"
}
if ([string]::IsNullOrWhiteSpace($OutputImage)) {
    $OutputImage = Join-Path $repoRoot `
        "screenshots\parity\model-lighting\courtyard-retail-vs-opennova.png"
}

function Resolve-InputFile([string]$Path, [string]$Label) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label not found: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

$retailPath = Resolve-InputFile $RetailImage "Retail comparison"
$currentPath = Resolve-InputFile $CurrentImage "OpenNova capture"
$outputPath = [System.IO.Path]::GetFullPath($OutputImage)
$outputDir = [System.IO.Path]::GetDirectoryName($outputPath)
[void](New-Item -ItemType Directory -Path $outputDir -Force)

Add-Type -AssemblyName System.Drawing

$panelWidth = 1280
$panelHeight = 720
$headerHeight = 36
$retailSource = [System.Drawing.Rectangle]::new(1, 36, $panelWidth, $panelHeight)
$leftTarget = [System.Drawing.Rectangle]::new(0, $headerHeight, $panelWidth, $panelHeight)
$rightTarget = [System.Drawing.Rectangle]::new(
    $panelWidth, $headerHeight, $panelWidth, $panelHeight)

$retail = [System.Drawing.Bitmap]::new($retailPath)
$current = [System.Drawing.Bitmap]::new($currentPath)
$canvas = [System.Drawing.Bitmap]::new(
    2 * $panelWidth, $headerHeight + $panelHeight,
    [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
$graphics = [System.Drawing.Graphics]::FromImage($canvas)
$font = [System.Drawing.Font]::new(
    "Segoe UI", 10.0, [System.Drawing.FontStyle]::Bold,
    [System.Drawing.GraphicsUnit]::Point)
$labelBrush = [System.Drawing.SolidBrush]::new(
    [System.Drawing.Color]::FromArgb(220, 220, 220))
$headerBrush = [System.Drawing.SolidBrush]::new(
    [System.Drawing.Color]::FromArgb(18, 18, 18))
$dividerPen = [System.Drawing.Pen]::new(
    [System.Drawing.Color]::FromArgb(80, 80, 80), 1.0)
$pngStream = [System.IO.MemoryStream]::new()

try {
    if ($retail.Width -lt ($retailSource.X + $retailSource.Width) -or
        $retail.Height -lt ($retailSource.Y + $retailSource.Height)) {
        throw "Retail source no longer contains the expected 1280x720 left panel."
    }
    if ($current.Width * 9 -ne $current.Height * 16) {
        throw "OpenNova capture must be 16:9, got $($current.Width)x$($current.Height)."
    }

    $graphics.CompositingMode =
        [System.Drawing.Drawing2D.CompositingMode]::SourceOver
    $graphics.CompositingQuality =
        [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
    $graphics.InterpolationMode =
        [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $graphics.PixelOffsetMode =
        [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality

    $graphics.FillRectangle(
        $headerBrush, 0, 0, $canvas.Width, $headerHeight)
    $graphics.DrawImage(
        $retail, $leftTarget, $retailSource, [System.Drawing.GraphicsUnit]::Pixel)
    $graphics.DrawImage(
        $current, $rightTarget,
        [System.Drawing.Rectangle]::new(0, 0, $current.Width, $current.Height),
        [System.Drawing.GraphicsUnit]::Pixel)
    $graphics.DrawLine(
        $dividerPen, $panelWidth, 0, $panelWidth, $canvas.Height)
    $graphics.DrawString(
        "RETAIL Jointops.exe (JO:TR 1.7.5.7) - 00TRa training courtyard",
        $font, $labelBrush,
        [System.Drawing.PointF]::new([float]8.0, [float]9.0))
    $graphics.DrawString(
        "OPENNOVA - light_transfer, retail LGHT gating, and isolated shadow paths",
        $font, $labelBrush,
        [System.Drawing.PointF]::new([float]($panelWidth + 8), [float]9.0))

    $canvas.Save($pngStream, [System.Drawing.Imaging.ImageFormat]::Png)
    [System.IO.File]::WriteAllBytes($outputPath, $pngStream.ToArray())
}
finally {
    $pngStream.Dispose()
    $dividerPen.Dispose()
    $headerBrush.Dispose()
    $labelBrush.Dispose()
    $font.Dispose()
    $graphics.Dispose()
    $canvas.Dispose()
    $current.Dispose()
    $retail.Dispose()
}

Write-Host "Wrote $outputPath"
