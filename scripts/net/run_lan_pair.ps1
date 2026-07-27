# Launch a localhost (or same-LAN) OpenNova host and joiner pair.
#
# Both children use the same locally installed mission assets. The host starts
# first; JoinDelayMs is deliberately bounded so this remains a quick bring-up aid.
#
# Usage:
#   pwsh -File scripts\net\run_lan_pair.ps1 -Mission ASH_I5A.BMS

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [ValidateNotNullOrEmpty()] [string] $Mission,
    [Alias("Host")] [ValidateNotNullOrEmpty()] [string] $ServerHost = "127.0.0.1",
    [ValidateNotNullOrEmpty()] [string] $HostName = "Host",
    [ValidateNotNullOrEmpty()] [string] $JoinName = "Joiner",
    [ValidateRange(1, 65535)] [int] $Port = 32768,
    [ValidateRange(0, 10000)] [int] $JoinDelayMs = 1500,
    [string] $JoinMissionOverride = "",
    [switch] $Wait
)

$ErrorActionPreference = "Stop"

$hostScript = Join-Path $PSScriptRoot "host_opennova.ps1"
$joinScript = Join-Path $PSScriptRoot "join_opennova.ps1"

$hostProcess = & $hostScript `
    -Mission $Mission `
    -Name $HostName `
    -Port $Port `
    -PassThru

if ($JoinDelayMs -gt 0) {
    Write-Host "Waiting ${JoinDelayMs}ms before starting the joiner..."
    Start-Sleep -Milliseconds $JoinDelayMs
}

$hostProcess.Refresh()
if ($hostProcess.HasExited) {
    throw "The OpenNova host exited before the joiner started (exit code $($hostProcess.ExitCode))."
}

$joinArgs = @{
    ServerHost = $ServerHost
    Name = $JoinName
    Port = $Port
    PassThru = $true
}
if ($PSBoundParameters.ContainsKey("JoinMissionOverride")) {
    $joinArgs["MissionOverride"] = $JoinMissionOverride
}
$joinProcess = & $joinScript @joinArgs

Write-Host "OPENNOVA_PAIR_HOST_PID=$($hostProcess.Id)"
Write-Host "OPENNOVA_PAIR_JOIN_PID=$($joinProcess.Id)"

if ($Wait) {
    foreach ($process in @($hostProcess, $joinProcess)) {
        $process.Refresh()
        if (-not $process.HasExited) { $process.WaitForExit() }
    }
    Write-Host "OPENNOVA_PAIR_EXIT_CODES=host:$($hostProcess.ExitCode),join:$($joinProcess.ExitCode)"
}
