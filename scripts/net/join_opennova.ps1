# Launch one visible OpenNova instance as a LAN joiner.
#
# The joiner is configured entirely by launch flags (`--lan-join host:port
# --callsign <name> [--integrity-profile <id>] [--resource-dir <dir>] [/exp
# <name>]`, docs/dev-env-vars.md) and drives through its opennova-game MCP
# endpoint (`--mcp-port`, docs/mcp.md). The mission is learned from the LAN
# session (D-NET-194); there is no local override. -Host accepts an IPv4
# address or hostname without a port; use -Port separately.
#
# Usage:
#   pwsh -File scripts\net\join_opennova.ps1 [-Host 127.0.0.1] [-Name Joiner] [-Port 32768] [-McpPort 8976] [-Resolution 1920x1080] [-Wait]

[CmdletBinding()]
param(
    [Alias("Host")] [ValidateNotNullOrEmpty()] [string] $ServerHost = "127.0.0.1",
    [ValidateNotNullOrEmpty()] [string] $Name = "Joiner",
    [ValidateRange(1, 65535)] [int] $Port = 32768,
    [ValidateRange(1, 65535)] [int] $McpPort = 8976,
    [ValidateRange(1, 600)] [int] $McpReadyTimeoutSeconds = 240,
    [string] $IntegrityProfile = "",
    [string] $ResourceDir = "",
    [string] $Expansion = "",
    [string] $LogFile = "",
    [string] $Resolution = "",
    [switch] $Windowed,
    [switch] $Wait,
    [switch] $PassThru
)

$ErrorActionPreference = "Stop"
. "$PSScriptRoot\lib.ps1"

if ([string]::IsNullOrWhiteSpace($ServerHost)) { throw "-Host cannot be blank." }
if ([string]::IsNullOrWhiteSpace($Name)) { throw "-Name cannot be blank." }
if ($ServerHost.Contains(":")) {
    throw "-Host accepts an IPv4 address or hostname without a port; pass the port with -Port."
}
$normalizedResolution = ""
if (-not [string]::IsNullOrWhiteSpace($Resolution)) {
    $normalizedResolution = ConvertTo-OpenNovaResolution -Value $Resolution
}
if (-not [string]::IsNullOrWhiteSpace($ResourceDir)) {
    $ResourceDir = (Resolve-Path -LiteralPath $ResourceDir -ErrorAction Stop).Path
}
if (-not [string]::IsNullOrWhiteSpace($LogFile)) {
    $LogFile = [System.IO.Path]::GetFullPath($LogFile)
    $logDirectory = Split-Path -Parent $LogFile
    if (-not (Test-Path -LiteralPath $logDirectory -PathType Container)) {
        throw "The -LogFile parent directory does not exist: $logDirectory"
    }
}

$target = "{0}:{1}" -f $ServerHost.Trim(), $Port
$godotArguments = @()
if ($Windowed) { $godotArguments += "--windowed" }
if ($normalizedResolution) {
    $godotArguments += @("--resolution", $normalizedResolution)
}
if (-not [string]::IsNullOrWhiteSpace($LogFile)) {
    $godotArguments += @("--log-file", $LogFile)
}
$gameArguments = @("--lan-join", $target, "--callsign", $Name.Trim())
if (-not [string]::IsNullOrWhiteSpace($IntegrityProfile)) {
    $gameArguments += @("--integrity-profile", $IntegrityProfile.Trim())
}
if (-not [string]::IsNullOrWhiteSpace($ResourceDir)) {
    $gameArguments += @("--resource-dir", $ResourceDir)
}
if (-not [string]::IsNullOrWhiteSpace($Expansion)) {
    $gameArguments += @("/exp", $Expansion.Trim())
}
$process = Start-OpenNovaProcess `
    -GodotArguments $godotArguments `
    -GameArguments $gameArguments `
    -McpPort $McpPort `
    -McpReadyTimeoutSeconds $McpReadyTimeoutSeconds

Write-Host "OPENNOVA_JOIN_PID=$($process.Id)"
Write-Host "OPENNOVA_JOIN_LAUNCHER_PID=$($process.OpenNovaLauncherProcess.Id)"
Write-Host "OPENNOVA_JOIN_TARGET=$target"
Write-Host "OPENNOVA_JOIN_MCP=$(Get-GameMcpUrl -Port $McpPort)"
Write-Host "OPENNOVA_JOIN_RESOLUTION=$(if ($normalizedResolution) { $normalizedResolution } else { 'project-default' })"
if (-not [string]::IsNullOrWhiteSpace($IntegrityProfile)) {
    Write-Host "OPENNOVA_JOIN_INTEGRITY_PROFILE=$($IntegrityProfile.Trim())"
}
if (-not [string]::IsNullOrWhiteSpace($ResourceDir)) {
    Write-Host "OPENNOVA_JOIN_RESOURCE_DIR=$ResourceDir"
} else {
    Write-Host "OpenNova must already have a Joint Operations resource directory selected."
}
if (-not [string]::IsNullOrWhiteSpace($Expansion)) {
    Write-Host "OPENNOVA_JOIN_EXPANSION=$($Expansion.Trim())"
}
Write-Host "OPENNOVA_JOIN_WINDOWED=$([bool] $Windowed)"
if (-not [string]::IsNullOrWhiteSpace($LogFile)) {
    Write-Host "OPENNOVA_JOIN_LOG=$LogFile"
}

if ($Wait) {
    $process.WaitForExit()
    $launcher = $process.OpenNovaLauncherProcess
    if ($process.OpenNovaLaunchProof.wrapper_used) {
        if (-not $launcher.WaitForExit(5000)) {
            Stop-OpenNovaProcess -Process $process `
                -GracefulTimeoutMs 0 -ForceTimeoutMs 5000
        }
    }
    Write-Host "OPENNOVA_JOIN_EXIT_CODE=$($process.ExitCode)"
}
if ($PassThru) { return $process }
