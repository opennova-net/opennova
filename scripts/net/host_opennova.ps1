# Launch one visible OpenNova instance as a LAN listen host.
#
# The Joint Operations resource directory is persistent OpenNova user state and
# must already have been selected by running OpenNova normally once.
#
# Usage:
#   pwsh -File scripts\net\host_opennova.ps1 -HookConfig .scratch\SERVER\onhook.cfg
#   pwsh -File scripts\net\host_opennova.ps1 -Mission 01TR.bms -GameType 65568 -Resolution 1920x1080 [-Name Host] [-Port 32768] [-LanMode 1] [-MaxPlayers 4] [-Wait]

[CmdletBinding()]
param(
    [string] $Mission = "",
    [ValidateNotNullOrEmpty()] [string] $Name = "Host",
    [ValidateRange(1, 65535)] [int] $Port = 32768,
    [ValidateRange(1, 4)] [int] $LanMode = 1,
    [ValidateRange(1, 64)] [int] $MaxPlayers = 4,
    [ValidateRange(1, 300000)] [int] $ReadyTimeoutMs = 120000,
    [ValidateRange(10, 60000)] [int] $ReadyProbeIntervalMs = 1000,
    [string] $GameType = "auto",
    [string] $HookConfig = "",
    [string] $ResourceDir = "",
    [string] $Expansion = "",
    [string] $IntegrityProfile = "",
    [string] $LogFile = "",
    [string] $Resolution = "",
    [switch] $Windowed,
    [switch] $SkipReadyCheck,
    [switch] $Wait,
    [switch] $PassThru
)

$ErrorActionPreference = "Stop"
. "$PSScriptRoot\lib.ps1"

$explicitMission = $PSBoundParameters.ContainsKey("Mission")
$explicitName = $PSBoundParameters.ContainsKey("Name")
$explicitPort = $PSBoundParameters.ContainsKey("Port")
$explicitLanMode = $PSBoundParameters.ContainsKey("LanMode")
$explicitMaxPlayers = $PSBoundParameters.ContainsKey("MaxPlayers")
$explicitReadyTimeout = $PSBoundParameters.ContainsKey("ReadyTimeoutMs")
$explicitReadyProbeInterval = $PSBoundParameters.ContainsKey("ReadyProbeIntervalMs")
$explicitGameType = $PSBoundParameters.ContainsKey("GameType")
$explicitResourceDir = $PSBoundParameters.ContainsKey("ResourceDir")
$explicitExpansion = $PSBoundParameters.ContainsKey("Expansion")
$explicitIntegrityProfile = $PSBoundParameters.ContainsKey("IntegrityProfile")
$explicitResolution = $PSBoundParameters.ContainsKey("Resolution")
$explicitWindowed = $PSBoundParameters.ContainsKey("Windowed")

if (-not [string]::IsNullOrWhiteSpace($HookConfig)) {
    $hook = Read-OnHookConfig -Path $HookConfig
    $cfg = $hook.Values
    if (-not $explicitMission -and $cfg.ContainsKey("LanHostMission")) {
        $Mission = $cfg["LanHostMission"]
    }
    if (-not $explicitName -and $cfg.ContainsKey("LanHostCallsign")) {
        $Name = $cfg["LanHostCallsign"]
    }
    if (-not $explicitPort -and $cfg.ContainsKey("LanHostPort")) {
        $parsedPort = 0
        if (-not [int]::TryParse($cfg["LanHostPort"], [ref] $parsedPort) -or
                $parsedPort -lt 1 -or $parsedPort -gt 65535) {
            throw "LanHostPort in '$($hook.Path)' must be an integer from 1 through 65535."
        }
        $Port = $parsedPort
    }
    if (-not $explicitLanMode -and $cfg.ContainsKey("LanHostLanMode")) {
        $parsedLanMode = 0
        if (-not [int]::TryParse($cfg["LanHostLanMode"], [ref] $parsedLanMode) -or
                $parsedLanMode -lt 1 -or $parsedLanMode -gt 4) {
            throw "LanHostLanMode in '$($hook.Path)' must be an integer from 1 through 4."
        }
        $LanMode = $parsedLanMode
    }
    if (-not $explicitMaxPlayers -and $cfg.ContainsKey("LanHostMaxPlayers")) {
        $parsedMaxPlayers = 0
        if (-not [int]::TryParse($cfg["LanHostMaxPlayers"], [ref] $parsedMaxPlayers) -or
                $parsedMaxPlayers -lt 1 -or $parsedMaxPlayers -gt 64) {
            throw "LanHostMaxPlayers in '$($hook.Path)' must be an integer from 1 through 64."
        }
        $MaxPlayers = $parsedMaxPlayers
    }
    if (-not $explicitReadyTimeout -and $cfg.ContainsKey("LanHostReadyTimeoutMs")) {
        $parsedReadyTimeout = 0
        if (-not [int]::TryParse($cfg["LanHostReadyTimeoutMs"], [ref] $parsedReadyTimeout) -or
                $parsedReadyTimeout -lt 1 -or $parsedReadyTimeout -gt 300000) {
            throw "LanHostReadyTimeoutMs in '$($hook.Path)' must be an integer from 1 through 300000."
        }
        $ReadyTimeoutMs = $parsedReadyTimeout
    }
    if (-not $explicitReadyProbeInterval -and $cfg.ContainsKey("LanHostReadyProbeIntervalMs")) {
        $parsedReadyProbeInterval = 0
        if (-not [int]::TryParse($cfg["LanHostReadyProbeIntervalMs"], [ref] $parsedReadyProbeInterval) -or
                $parsedReadyProbeInterval -lt 10 -or $parsedReadyProbeInterval -gt 60000) {
            throw "LanHostReadyProbeIntervalMs in '$($hook.Path)' must be an integer from 10 through 60000."
        }
        $ReadyProbeIntervalMs = $parsedReadyProbeInterval
    }
    if (-not $explicitGameType -and $cfg.ContainsKey("LanHostGameType")) {
        $GameType = $cfg["LanHostGameType"]
    }
    if (-not $explicitResourceDir -and $cfg.ContainsKey("OpenNovaResourceDir")) {
        $ResourceDir = Resolve-OnHookConfigPath `
            -ConfigDirectory $hook.Directory `
            -Value $cfg["OpenNovaResourceDir"]
    }
    if (-not $explicitExpansion -and $cfg.ContainsKey("OpenNovaExpansion")) {
        $Expansion = $cfg["OpenNovaExpansion"]
    }
    if (-not $explicitIntegrityProfile -and
            $cfg.ContainsKey("OpenNovaIntegrityProfile")) {
        $IntegrityProfile = $cfg["OpenNovaIntegrityProfile"]
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

if ([string]::IsNullOrWhiteSpace($Mission)) { throw "-Mission cannot be blank." }
if ([string]::IsNullOrWhiteSpace($Name)) { throw "-Name cannot be blank." }
if ($Port -lt 1 -or $Port -gt 65535) { throw "-Port must be from 1 through 65535." }
if ($MaxPlayers -lt 1 -or $MaxPlayers -gt 64) {
    throw "-MaxPlayers must be from 1 through 64."
}
$normalizedGameType = $GameType.Trim()
$parsedGameType = [uint32]0
if (-not $normalizedGameType.Equals("auto", [System.StringComparison]::OrdinalIgnoreCase) -and
        -not [uint32]::TryParse($normalizedGameType, [ref] $parsedGameType)) {
    throw "-GameType must be 'auto' or an unsigned 32-bit integer."
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

$lanEnvironment = @{
    NW_LAN_HOST = $Mission.Trim()
    NW_LAN_PORT = [string] $Port
    NW_LAN_NAME = $Name.Trim()
    NW_LAN_MODE = [string] $LanMode
    NW_LAN_MAX_PLAYERS = [string] $MaxPlayers
}
if (-not $normalizedGameType.Equals("auto", [System.StringComparison]::OrdinalIgnoreCase)) {
    $lanEnvironment["NW_LAN_GAMETYPE"] = [string] $parsedGameType
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

Write-Host "OPENNOVA_HOST_PID=$($process.Id)"
Write-Host "OPENNOVA_HOST_LAUNCHER_PID=$($process.OpenNovaLauncherProcess.Id)"
Write-Host "OPENNOVA_HOST_ENDPOINT=0.0.0.0:$Port"
Write-Host "OPENNOVA_HOST_LAN_MODE=$LanMode"
Write-Host "OPENNOVA_HOST_MAX_PLAYERS=$MaxPlayers"
Write-Host "OPENNOVA_HOST_GAME_TYPE=$normalizedGameType"
Write-Host "OPENNOVA_HOST_RESOLUTION=$(if ($normalizedResolution) { $normalizedResolution } else { 'project-default' })"
if (-not [string]::IsNullOrWhiteSpace($ResourceDir)) {
    Write-Host "OPENNOVA_HOST_RESOURCE_DIR=$ResourceDir"
} else {
    Write-Host "OpenNova must already have a Joint Operations resource directory selected."
}
if (-not [string]::IsNullOrWhiteSpace($Expansion)) {
    Write-Host "OPENNOVA_HOST_EXPANSION=$($Expansion.Trim())"
}
if (-not [string]::IsNullOrWhiteSpace($IntegrityProfile)) {
    Write-Host "OPENNOVA_HOST_INTEGRITY_PROFILE=$($IntegrityProfile.Trim())"
}
Write-Host "OPENNOVA_HOST_WINDOWED=$([bool] $Windowed)"
if (-not [string]::IsNullOrWhiteSpace($LogFile)) {
    Write-Host "OPENNOVA_HOST_LOG=$LogFile"
}

if (-not $SkipReadyCheck) {
    try {
        $probe = Find-NwLanProbe
        if (-not $probe) {
            throw "nw-lan-probe was not found. Build it with: cmake --build build --config Release --target opennova_nw_lan_probe"
        }
        Write-Host "OPENNOVA_HOST_READY_WAIT_MS=$ReadyTimeoutMs"
        & $probe `
            --host 127.0.0.1 `
            --port $Port `
            --timeout-ms $ReadyTimeoutMs `
            --interval-ms $ReadyProbeIntervalMs
        if ($LASTEXITCODE -ne 0) {
            Write-Host "OPENNOVA_HOST_READY=False"
            throw "OpenNova process $($process.Id) bound or launched but did not answer a valid JO LAN discovery probe."
        }
        Write-Host "OPENNOVA_HOST_READY=True"
    }
    catch {
        $readinessFailure = $_
        try { Stop-OpenNovaLanProcess -Process $process }
        catch {
            throw "OpenNova host readiness failed: $($readinessFailure.Exception.Message) Cleanup also failed for runtime PID $($process.Id): $($_.Exception.Message)"
        }
        throw $readinessFailure
    }
} else {
    Write-Host "OPENNOVA_HOST_READY=Unchecked"
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
    Write-Host "OPENNOVA_HOST_EXIT_CODE=$($process.ExitCode)"
}
if ($PassThru) { return $process }
