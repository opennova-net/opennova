# Launch a localhost (or same-LAN) OpenNova host and joiner pair.
#
# Both children use the same locally installed mission assets. The host starts
# first; JoinDelayMs is deliberately bounded so this remains a quick bring-up aid.
#
# Usage:
#   pwsh -File scripts\net\run_lan_pair.ps1 -Mission 01TR.bms -GameType 65568 -Resolution 1920x1080

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [ValidateNotNullOrEmpty()] [string] $Mission,
    [Alias("Host")] [ValidateNotNullOrEmpty()] [string] $ServerHost = "127.0.0.1",
    [ValidateNotNullOrEmpty()] [string] $HostName = "Host",
    [ValidateNotNullOrEmpty()] [string] $JoinName = "Joiner",
    [ValidateRange(1, 65535)] [int] $Port = 32768,
    [ValidateRange(0, 10000)] [int] $JoinDelayMs = 1500,
    [string] $JoinMissionOverride = "",
    [string] $HostHookConfig = "",
    [string] $JoinHookConfig = "",
    [string] $IntegrityProfile = "",
    [string] $HostResourceDir = "",
    [string] $JoinResourceDir = "",
    [string] $Expansion = "",
    [string] $GameType = "auto",
    [string] $Resolution = "",
    [switch] $Windowed,
    [switch] $Wait
)

$ErrorActionPreference = "Stop"
. "$PSScriptRoot\lib.ps1"

$normalizedResolution = ""
if (-not [string]::IsNullOrWhiteSpace($Resolution)) {
    $normalizedResolution = ConvertTo-OpenNovaResolution -Value $Resolution
}

$hostScript = Join-Path $PSScriptRoot "host_opennova.ps1"
$joinScript = Join-Path $PSScriptRoot "join_opennova.ps1"

$hostArgs = @{
    Mission = $Mission
    Name = $HostName
    Port = $Port
    PassThru = $true
}
if (-not [string]::IsNullOrWhiteSpace($HostHookConfig)) {
    $hostArgs["HookConfig"] = $HostHookConfig
}
if (-not [string]::IsNullOrWhiteSpace($HostResourceDir)) {
    $hostArgs["ResourceDir"] = $HostResourceDir
}
if (-not [string]::IsNullOrWhiteSpace($Expansion)) {
    $hostArgs["Expansion"] = $Expansion
}
if (-not [string]::IsNullOrWhiteSpace($IntegrityProfile)) {
    $hostArgs["IntegrityProfile"] = $IntegrityProfile
}
if ($PSBoundParameters.ContainsKey("GameType")) {
    $hostArgs["GameType"] = $GameType
}
if ($normalizedResolution) {
    $hostArgs["Resolution"] = $normalizedResolution
}
if ($Windowed) { $hostArgs["Windowed"] = $true }
$hostProcess = & $hostScript @hostArgs

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
if (-not [string]::IsNullOrWhiteSpace($JoinHookConfig)) {
    $joinArgs["HookConfig"] = $JoinHookConfig
}
if (-not [string]::IsNullOrWhiteSpace($IntegrityProfile)) {
    $joinArgs["IntegrityProfile"] = $IntegrityProfile
}
if (-not [string]::IsNullOrWhiteSpace($JoinResourceDir)) {
    $joinArgs["ResourceDir"] = $JoinResourceDir
}
if (-not [string]::IsNullOrWhiteSpace($Expansion)) {
    $joinArgs["Expansion"] = $Expansion
}
if ($normalizedResolution) {
    $joinArgs["Resolution"] = $normalizedResolution
}
if ($Windowed) { $joinArgs["Windowed"] = $true }
$joinProcess = & $joinScript @joinArgs

Write-Host "OPENNOVA_PAIR_HOST_PID=$($hostProcess.Id)"
Write-Host "OPENNOVA_PAIR_JOIN_PID=$($joinProcess.Id)"
Write-Host "OPENNOVA_PAIR_RESOLUTION=$(if ($normalizedResolution) { $normalizedResolution } else { 'per-role/project-default' })"

if ($Wait) {
    foreach ($process in @($hostProcess, $joinProcess)) {
        $process.Refresh()
        if (-not $process.HasExited) { $process.WaitForExit() }
    }
    Write-Host "OPENNOVA_PAIR_EXIT_CODES=host:$($hostProcess.ExitCode),join:$($joinProcess.ExitCode)"
}
