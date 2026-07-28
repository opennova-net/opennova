# Launch one visible OpenNova instance as a LAN listen host.
#
# The Joint Operations resource directory is persistent OpenNova user state and
# must already have been selected by running OpenNova normally once.
#
# Usage:
#   pwsh -File scripts\net\host_opennova.ps1 -Mission ASH_I5A.BMS [-Name Host] [-Port 32768] [-Wait]

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [ValidateNotNullOrEmpty()] [string] $Mission,
    [ValidateNotNullOrEmpty()] [string] $Name = "Host",
    [ValidateRange(1, 65535)] [int] $Port = 32768,
    [switch] $Wait,
    [switch] $PassThru
)

$ErrorActionPreference = "Stop"
. "$PSScriptRoot\lib.ps1"

if ([string]::IsNullOrWhiteSpace($Mission)) { throw "-Mission cannot be blank." }
if ([string]::IsNullOrWhiteSpace($Name)) { throw "-Name cannot be blank." }

$process = Start-OpenNovaLanProcess -LanEnvironment @{
    NW_LAN_HOST = $Mission.Trim()
    NW_LAN_PORT = [string] $Port
    NW_LAN_NAME = $Name.Trim()
}

Write-Host "OPENNOVA_HOST_PID=$($process.Id)"
Write-Host "OPENNOVA_HOST_ENDPOINT=0.0.0.0:$Port"
Write-Host "OpenNova must already have a Joint Operations resource directory selected."

if ($Wait) {
    $process.WaitForExit()
    Write-Host "OPENNOVA_HOST_EXIT_CODE=$($process.ExitCode)"
}
if ($PassThru) { return $process }
