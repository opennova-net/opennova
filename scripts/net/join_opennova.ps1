# Launch one visible OpenNova instance as a LAN joiner.
#
# The Joint Operations resource directory is persistent OpenNova user state and
# must already have been selected by running OpenNova normally once. Host accepts
# an IPv4 address or hostname without a port; use -Port separately. A mission is
# learned from the LAN session, not supplied by the normal join launch contract.
#
# Usage:
#   pwsh -File scripts\net\join_opennova.ps1 -HookConfig .scratch\CLIENT\onhook.cfg
#   pwsh -File scripts\net\join_opennova.ps1 [-Host 127.0.0.1] [-Name Joiner] [-Port 32768] [-Resolution 1920x1080] [-Wait]

[CmdletBinding()]
param(
    [Alias("Host")] [ValidateNotNullOrEmpty()] [string] $ServerHost = "127.0.0.1",
    [ValidateNotNullOrEmpty()] [string] $Name = "Joiner",
    [ValidateRange(1, 65535)] [int] $Port = 32768,
    [string] $MissionOverride = "",
    [string] $HookConfig = "",
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

$explicitHost = $PSBoundParameters.ContainsKey("ServerHost")
$explicitName = $PSBoundParameters.ContainsKey("Name")
$explicitPort = $PSBoundParameters.ContainsKey("Port")
$explicitIntegrityProfile = $PSBoundParameters.ContainsKey("IntegrityProfile")
$explicitResourceDir = $PSBoundParameters.ContainsKey("ResourceDir")
$explicitExpansion = $PSBoundParameters.ContainsKey("Expansion")
$explicitResolution = $PSBoundParameters.ContainsKey("Resolution")
$explicitWindowed = $PSBoundParameters.ContainsKey("Windowed")

if (-not [string]::IsNullOrWhiteSpace($HookConfig)) {
    $hook = Read-OnHookConfig -Path $HookConfig
    $cfg = $hook.Values
    if (-not $explicitHost -and $cfg.ContainsKey("LanJoinAddress")) {
        $ServerHost = $cfg["LanJoinAddress"]
    }
    if (-not $explicitName -and $cfg.ContainsKey("LanJoinCallsign")) {
        $Name = $cfg["LanJoinCallsign"]
    }
    if (-not $explicitPort -and $cfg.ContainsKey("LanJoinPort")) {
        $parsedPort = 0
        if (-not [int]::TryParse($cfg["LanJoinPort"], [ref] $parsedPort) -or
                $parsedPort -lt 1 -or $parsedPort -gt 65535) {
            throw "LanJoinPort in '$($hook.Path)' must be an integer from 1 through 65535."
        }
        $Port = $parsedPort
    }
    if (-not $explicitIntegrityProfile -and
            $cfg.ContainsKey("OpenNovaIntegrityProfile")) {
        $IntegrityProfile = $cfg["OpenNovaIntegrityProfile"]
    }
    if (-not $explicitResourceDir -and $cfg.ContainsKey("OpenNovaResourceDir")) {
        $ResourceDir = Resolve-OnHookConfigPath `
            -ConfigDirectory $hook.Directory `
            -Value $cfg["OpenNovaResourceDir"]
    }
    if (-not $explicitExpansion -and $cfg.ContainsKey("OpenNovaExpansion")) {
        $Expansion = $cfg["OpenNovaExpansion"]
    }
    if (-not $explicitResolution -and $cfg.ContainsKey("OpenNovaResolution")) {
        $Resolution = $cfg["OpenNovaResolution"]
    }
    if (-not $explicitWindowed -and $cfg.ContainsKey("OpenNovaWindowed")) {
        $Windowed = ConvertFrom-OnHookBoolean `
            -Value $cfg["OpenNovaWindowed"] `
            -Key "OpenNovaWindowed"
    }
}

if ([string]::IsNullOrWhiteSpace($ServerHost)) { throw "-Host cannot be blank." }
if ([string]::IsNullOrWhiteSpace($Name)) { throw "-Name cannot be blank." }
if ($PSBoundParameters.ContainsKey("MissionOverride") -and
        [string]::IsNullOrWhiteSpace($MissionOverride)) {
    throw "-MissionOverride cannot be blank when provided."
}
if ($ServerHost.Contains(":")) {
    throw "-Host accepts an IPv4 address or hostname without a port; pass the port with -Port."
}
if ($Port -lt 1 -or $Port -gt 65535) { throw "-Port must be from 1 through 65535." }
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
$lanEnvironment = @{
    NW_LAN_JOIN = $target
    NW_LAN_NAME = $Name.Trim()
}
if (-not [string]::IsNullOrWhiteSpace($MissionOverride)) {
    # Compatibility only for older direct-load runtime builds. This is deliberately
    # opt-in: a normal LAN join learns the mission from the host/session flow.
    $lanEnvironment["NW_LAN_MISSION"] = $MissionOverride.Trim()
}
if (-not [string]::IsNullOrWhiteSpace($IntegrityProfile)) {
    $lanEnvironment["NW_LAN_INTEGRITY_PROFILE"] = $IntegrityProfile.Trim()
}
$godotArguments = @()
if ($Windowed) { $godotArguments += "--windowed" }
if ($normalizedResolution) {
    $godotArguments += @("--resolution", $normalizedResolution)
}
if (-not [string]::IsNullOrWhiteSpace($LogFile)) {
    $godotArguments += @("--log-file", $LogFile)
}
$gameArguments = @()
if (-not [string]::IsNullOrWhiteSpace($ResourceDir)) {
    $gameArguments += @("--resource-dir", $ResourceDir)
}
if (-not [string]::IsNullOrWhiteSpace($Expansion)) {
    $gameArguments += @("/exp", $Expansion.Trim())
}
$process = Start-OpenNovaLanProcess `
    -LanEnvironment $lanEnvironment `
    -GodotArguments $godotArguments `
    -GameArguments $gameArguments

Write-Host "OPENNOVA_JOIN_PID=$($process.Id)"
Write-Host "OPENNOVA_JOIN_LAUNCHER_PID=$($process.OpenNovaLauncherProcess.Id)"
Write-Host "OPENNOVA_JOIN_TARGET=$target"
Write-Host "OPENNOVA_JOIN_RESOLUTION=$(if ($normalizedResolution) { $normalizedResolution } else { 'project-default' })"
if (-not [string]::IsNullOrWhiteSpace($MissionOverride)) {
    Write-Host "OPENNOVA_JOIN_MISSION_OVERRIDE=$($MissionOverride.Trim())"
}
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
            Stop-OpenNovaLanProcess -Process $process `
                -GracefulTimeoutMs 0 -ForceTimeoutMs 5000
        }
    }
    Write-Host "OPENNOVA_JOIN_EXIT_CODE=$($process.ExitCode)"
}
if ($PassThru) { return $process }
