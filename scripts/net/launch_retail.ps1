# Launch a stock retail Jointops.exe instance with the wire-observability oracle
# flags. PHASE 0 (no driver): launches with flags only; the human performs the
# host/join clicks. The Phase 2 retail driver will later inject + drive these same
# instances unattended.
#
# Reads the JO install dir from -GameDir, else $env:JO_GAME_DIR. This path points
# at copyrighted retail files and is machine-specific: set JO_GAME_DIR in
# .claude/settings.local.json env, never commit it.
#
# Usage:
#   pwsh -File scripts\net\launch_retail.ps1 -Tag host [-GameDir 'C:\Games\JointOps'] [-Exp jox01] [-Dev]
#
# Each instance gets /connectlog (-> _connectlog.txt in the game dir) and /profile
# (-> .scratch\retail-<tag>-<stamp>.sph). Prints RETAIL_PID and the .sph path.

param(
    [Parameter(Mandatory = $true)] [string] $Tag,
    [string] $GameDir = "",
    [string] $Exp = "",
    [switch] $Dev
)

$ErrorActionPreference = "Stop"
. "$PSScriptRoot\lib.ps1"

if (-not $GameDir) { $GameDir = $env:JO_GAME_DIR }
if (-not $GameDir) {
    Write-Error "No JO install dir. Pass -GameDir or set JO_GAME_DIR (the folder with Jointops.exe)."
    exit 1
}
$exe = Join-Path $GameDir "Jointops.exe"
if (-not (Test-Path $exe)) {
    Write-Error "Jointops.exe not found in: $GameDir"
    exit 1
}

$scratch = Get-ScratchDir
$sph = Join-Path $scratch ("retail-{0}-{1}.sph" -f $Tag, (Get-RunStamp))

# /w windowed, /many multi-instance (two clients on one box), /game jo SCR key,
# /connectlog gate dialogue, /profile per-frame session recording. /d dev override
# and /exp <slug> are optional.
$flags = @("/w", "/many", "/game", "jo", "/connectlog", "/profile", $sph)
if ($Dev) { $flags += "/d" }
if ($Exp) { $flags += @("/exp", $Exp) }

$proc = Start-Process -FilePath $exe -WorkingDirectory $GameDir -ArgumentList $flags -PassThru
Write-Host "RETAIL_PID=$($proc.Id)"
Write-Host "RETAIL_SPH=$sph"
Write-Host "RETAIL_CONNECTLOG=$(Join-Path $GameDir '_connectlog.txt')"
exit 0
