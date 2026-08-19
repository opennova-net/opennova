# Normalize one declared OpenNova retail-parity source. This script has one
# deliberately narrow transform: the complete 2000x1200 source rectangle is
# drawn into a 1920x1200 destination with HighQualityBicubic interpolation.
# It never crops, translates, or changes the vertical dimension.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$InputImage,
    [Parameter(Mandatory = $true)]
    [string]$OutputImage
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$inputPath = [System.IO.Path]::GetFullPath($InputImage)
$outputPath = [System.IO.Path]::GetFullPath($OutputImage)
if (-not (Test-Path -LiteralPath $inputPath -PathType Leaf)) {
    throw "OpenNova raw image not found: $inputPath"
}
if (Test-Path -LiteralPath $outputPath) {
    throw "refusing to overwrite existing output: $outputPath"
}

$outputDirectory = [System.IO.Path]::GetDirectoryName($outputPath)
[void](New-Item -ItemType Directory -Path $outputDirectory -Force)
Add-Type -AssemblyName System.Drawing

$source = [System.Drawing.Bitmap]::new($inputPath)
$normalized = $null
$graphics = $null
$attributes = $null
try {
    if ($source.Width -ne 2000 -or $source.Height -ne 1200) {
        throw "OpenNova raw image must be exactly 2000x1200; got $($source.Width)x$($source.Height)"
    }
    $normalized = [System.Drawing.Bitmap]::new(
        1920, 1200, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $graphics = [System.Drawing.Graphics]::FromImage($normalized)
    $graphics.CompositingMode =
        [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
    $graphics.CompositingQuality =
        [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
    $graphics.InterpolationMode =
        [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $graphics.PixelOffsetMode =
        [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    # Tile-flipped edge sampling prevents the bicubic kernel from blending
    # against transparent black beyond the complete source rectangle.
    $attributes = [System.Drawing.Imaging.ImageAttributes]::new()
    $attributes.SetWrapMode(
        [System.Drawing.Drawing2D.WrapMode]::TileFlipXY)
    $graphics.DrawImage(
        $source,
        [System.Drawing.Rectangle]::new(0, 0, 1920, 1200),
        0, 0, 2000, 1200,
        [System.Drawing.GraphicsUnit]::Pixel,
        $attributes)
    $normalized.Save($outputPath, [System.Drawing.Imaging.ImageFormat]::Png)
}
finally {
    if ($attributes -ne $null) { $attributes.Dispose() }
    if ($graphics -ne $null) { $graphics.Dispose() }
    if ($normalized -ne $null) { $normalized.Dispose() }
    $source.Dispose()
}
