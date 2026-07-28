# Launch one visible OpenNova instance as a LAN joiner.
#
# The Joint Operations resource directory is persistent OpenNova user state and
# must already have been selected by running OpenNova normally once. Host accepts
# an IPv4 address or hostname without a port; use -Port separately. A mission is
# learned from the LAN session, not supplied by the normal join launch contract.
#
# Usage:
#   pwsh -File scripts\net\join_opennova.ps1 [-Host 127.0.0.1] [-Name Joiner] [-Port 32768] [-Wait]

[CmdletBinding()]
param(
    [Alias("Host")] [ValidateNotNullOrEmpty()] [string] $ServerHost = "127.0.0.1",
    [ValidateNotNullOrEmpty()] [string] $Name = "Joiner",
    [ValidateRange(1, 65535)] [int] $Port = 32768,
    [string] $MissionOverride = "",
    [switch] $Wait,
    [switch] $PassThru
)

$ErrorActionPreference = "Stop"
. "$PSScriptRoot\lib.ps1"

if ([string]::IsNullOrWhiteSpace($ServerHost)) { throw "-Host cannot be blank." }
if ([string]::IsNullOrWhiteSpace($Name)) { throw "-Name cannot be blank." }
if ($PSBoundParameters.ContainsKey("MissionOverride") -and
        [string]::IsNullOrWhiteSpace($MissionOverride)) {
    throw "-MissionOverride cannot be blank when provided."
}
if ($ServerHost.Contains(":")) {
    throw "-Host accepts an IPv4 address or hostname without a port; pass the port with -Port."
}

$target = "{0}:{1}" -f $ServerHost.Trim(), $Port
$lanEnvironment = @{
    NW_LAN_JOIN = $target
    NW_LAN_NAME = $Name.Trim()
}
if (-not [string]::IsNullOrWhiteSpace($MissionOverride)) {
    # Compatibility only for older direct-load runtime builds. This is deliberately
    # opt-in: a normal LAN join learns the mission from the host/session flow.
    $lanEnvironment["NW_LAN_MISSION"] = $MissionOverride.Trim()
}
$process = Start-OpenNovaLanProcess -LanEnvironment $lanEnvironment

Write-Host "OPENNOVA_JOIN_PID=$($process.Id)"
Write-Host "OPENNOVA_JOIN_TARGET=$target"
if (-not [string]::IsNullOrWhiteSpace($MissionOverride)) {
    Write-Host "OPENNOVA_JOIN_MISSION_OVERRIDE=$($MissionOverride.Trim())"
}
Write-Host "OpenNova must already have a Joint Operations resource directory selected."

if ($Wait) {
    $process.WaitForExit()
    Write-Host "OPENNOVA_JOIN_EXIT_CODE=$($process.ExitCode)"
}
if ($PassThru) { return $process }
