# Decode a capture (or .sph server log) with the repo's own opennova-wire. Writes a
# sidecar <capture>.txt.
#
# Locates opennova-wire under build\ (Release preferred, Debug fallback) so it works
# whichever config was last built.
#
# Usage:
#   pwsh -File scripts\net\decode.ps1 -Capture .scratch\retail-ref-<stamp>.pcapng `
#        [-Items <items.def>] [-Tags '0x0c','0x0d'] [-Stream]
#
# -Items defaults to ITEMS.DEF under OPENNOVA_JO_ASSETS when that tree is set.
# Prints DECODE_FILE=<path> and the first lines of the decode.

param(
    [Parameter(Mandatory = $true)] [string] $Capture,
    [string] $Items = "",
    [string[]] $Tags = @(),
    [switch] $Stream
)

$ErrorActionPreference = "Stop"
. "$PSScriptRoot\lib.ps1"

# An app builds from apps\<dir> as opennova-<dir, kebab-cased>.exe.
function Find-App {
    param([string] $Dir)
    $build = Join-Path (Get-RepoRoot) "build"
    $Name = "opennova-" + ($Dir -replace '_', '-')
    foreach ($cfg in @("Release", "Debug")) {
        $p = Join-Path $build "apps\$Dir\$cfg\$Name.exe"
        if (Test-Path $p) { return $p }
    }
    $hit = Get-ChildItem $build -Recurse -Filter "$Name.exe" -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($hit) { return $hit.FullName }
    return $null
}

if (-not (Test-Path $Capture)) {
    Write-Error "Capture not found: $Capture"
    exit 1
}
$Capture = (Resolve-Path $Capture).Path
if (-not $Items) { $Items = Get-OpenNovaRetailAssetFile -Name "ITEMS.DEF" }

$wire = Find-App -Dir "wire"
if (-not $wire) {
    Write-Error "opennova-wire.exe not found under build\; run scripts/build.sh first."
    exit 2
}

$ppArgs = @($Capture)
if ($Stream) { $ppArgs += "--stream" }
if ($Items)  { $ppArgs += @("--items", $Items) }
if ($Tags)   { $ppArgs += $Tags }

$decodeFile = "$Capture.txt"
& $wire @ppArgs | Tee-Object -FilePath $decodeFile | Select-Object -First 60
Write-Host "DECODE_FILE=$decodeFile"
exit 0
