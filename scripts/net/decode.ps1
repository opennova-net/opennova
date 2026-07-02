# Decode a capture (or .sph server log) with the repo's own nw_pp, and optionally
# cross-check role partition with nw_replay. Writes a sidecar <capture>.txt.
#
# Locates nw_pp / nw_replay under build\ (Release preferred, Debug fallback) so it
# works whichever config was last built.
#
# Usage:
#   pwsh -File scripts\net\decode.ps1 -Capture .scratch\retail-ref-<stamp>.pcapng `
#        [-Items <items.def>] [-Tags '0x0c','0x0d'] [-Roles] [-Stream]
#
# Prints DECODE_FILE=<path> and the first lines of the decode.

param(
    [Parameter(Mandatory = $true)] [string] $Capture,
    [string] $Items = "",
    [string[]] $Tags = @(),
    [switch] $Roles,
    [switch] $Stream
)

$ErrorActionPreference = "Stop"
. "$PSScriptRoot\lib.ps1"

function Find-Tool {
    param([string] $Name)
    $build = Join-Path (Get-RepoRoot) "build"
    foreach ($cfg in @("Release", "Debug")) {
        $p = Join-Path $build "apps\$Name\$cfg\$Name.exe"
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

$nwpp = Find-Tool -Name "nw_pp"
if (-not $nwpp) {
    Write-Error "nw_pp.exe not found under build\; run scripts/build.sh first."
    exit 2
}

$ppArgs = @($Capture)
if ($Stream) { $ppArgs += "--stream" }
if ($Items)  { $ppArgs += @("--items", $Items) }
if ($Tags)   { $ppArgs += $Tags }

$decodeFile = "$Capture.txt"
& $nwpp @ppArgs | Tee-Object -FilePath $decodeFile | Select-Object -First 60
Write-Host "DECODE_FILE=$decodeFile"

if ($Roles) {
    $nwreplay = Find-Tool -Name "nw_replay"
    if ($nwreplay) {
        Write-Host "--- nw_replay --print-roles ---"
        & $nwreplay $Capture --print-roles
    } else {
        Write-Host "(nw_replay not built; skipping --print-roles)"
    }
}
exit 0
