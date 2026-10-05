# Launch one visible OpenNova instance as a LAN listen host.
#
# The host is configured entirely by launch flags (`--lan-host <mission>
# --lan-port <port> --callsign <name> --lan-mode <1..4> --lan-max-players
# <1..64> [--lan-gametype <n>] [--integrity-profile <id>] [--resource-dir
# <dir>] [/exp <name>]`, docs/dev-env-vars.md) and drives through its
# opennova-game MCP endpoint (`--mcp-port`, docs/mcp.md). Readiness is the
# JO LAN discovery answer (opennova-lan-probe) AND the endpoint's game_state
# reporting the host role on the requested port.
#
# Usage:
#   pwsh -File scripts\net\host_opennova.ps1 -Mission 01TR.bms -GameType 65568 -Resolution 1920x1080 [-Name Host] [-Port 32768] [-McpPort 8975] [-LanMode 1] [-MaxPlayers 4] [-Front] [-Wait]

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [ValidateNotNullOrEmpty()] [string] $Mission,
    [ValidateNotNullOrEmpty()] [string] $Name = "Host",
    [ValidateRange(1, 65535)] [int] $Port = 32768,
    [ValidateRange(1, 65535)] [int] $McpPort = 8975,
    [ValidateRange(1, 4)] [int] $LanMode = 1,
    [ValidateRange(1, 64)] [int] $MaxPlayers = 4,
    [ValidateRange(1, 300000)] [int] $ReadyTimeoutMs = 120000,
    [ValidateRange(10, 60000)] [int] $ReadyProbeIntervalMs = 1000,
    [ValidateRange(1, 600)] [int] $McpReadyTimeoutSeconds = 240,
    [string] $GameType = "auto",
    [string] $ResourceDir = "",
    [string] $Expansion = "",
    [string] $IntegrityProfile = "",
    [string] $LogFile = "",
    [string] $Resolution = "",
    [switch] $Windowed,
    [switch] $Front,
    [switch] $SkipReadyCheck,
    [switch] $Wait,
    [switch] $PassThru
)

$ErrorActionPreference = "Stop"
. "$PSScriptRoot\lib.ps1"

if ([string]::IsNullOrWhiteSpace($Mission)) { throw "-Mission cannot be blank." }
if ([string]::IsNullOrWhiteSpace($Name)) { throw "-Name cannot be blank." }
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

$godotArguments = @()
if ($Windowed) { $godotArguments += "--windowed" }
if ($normalizedResolution) {
    $godotArguments += @("--resolution", $normalizedResolution)
}
if (-not [string]::IsNullOrWhiteSpace($LogFile)) {
    $godotArguments += @("--log-file", $LogFile)
}
$gameArguments = @(
    "--lan-host", $Mission.Trim(),
    "--lan-port", [string] $Port,
    "--callsign", $Name.Trim(),
    "--lan-mode", [string] $LanMode,
    "--lan-max-players", [string] $MaxPlayers
)
if (-not $normalizedGameType.Equals("auto", [System.StringComparison]::OrdinalIgnoreCase)) {
    $gameArguments += @("--lan-gametype", [string] $parsedGameType)
}
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
    -McpReadyTimeoutSeconds $McpReadyTimeoutSeconds `
    -Front:$Front

Write-Host "OPENNOVA_HOST_PID=$($process.Id)"
Write-Host "OPENNOVA_HOST_LAUNCHER_PID=$($process.OpenNovaLauncherProcess.Id)"
Write-Host "OPENNOVA_HOST_ENDPOINT=0.0.0.0:$Port"
Write-Host "OPENNOVA_HOST_MCP=$(Get-GameMcpUrl -Port $McpPort)"
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
        $probe = Find-LanProbe
        if (-not $probe) {
            throw "opennova-lan-probe was not found. Build it with: cmake --build build --config Release --target opennova_lan_probe"
        }
        Write-Host "OPENNOVA_HOST_READY_WAIT_MS=$ReadyTimeoutMs"
        # Captured, not piped: with -PassThru the pipeline carries only the Process.
        $probeOutput = @(& $probe `
            --host 127.0.0.1 `
            --port $Port `
            --timeout-ms $ReadyTimeoutMs `
            --interval-ms $ReadyProbeIntervalMs)
        $probeOutput | ForEach-Object { Write-Host $_ }
        if ($LASTEXITCODE -ne 0) {
            Write-Host "OPENNOVA_HOST_READY=False"
            throw "OpenNova process $($process.Id) bound or launched but did not answer a valid JO LAN discovery probe."
        }
        # The endpoint's own account: the world is loaded and the runtime is
        # hosting on this port (discovery answers before the load completes).
        $deadline = [DateTime]::UtcNow.AddMilliseconds($ReadyTimeoutMs)
        while ($true) {
            $state = Get-StructuredResult (Invoke-GameTool -Port $McpPort -Name "game_state")
            $network = $null
            if ($state.runtime.PSObject.Properties['network']) { $network = $state.runtime.network }
            # Boot complete = the host's own player spawned (the listen server
            # auto-spawns it at the end of the mission boot).
            if ([bool] $state.shell.world_loaded -and [bool] $state.player.present -and $network -and
                    [string] $network.role -eq "host" -and [int] $network.port -eq $Port) {
                break
            }
            if ([DateTime]::UtcNow -ge $deadline) {
                Write-Host "OPENNOVA_HOST_READY=False"
                $observed = if ($network) { "role=$($network.role) port=$($network.port) world_loaded=$($state.shell.world_loaded)" } else { "no network state" }
                throw "OpenNova process $($process.Id) answered discovery but game_state reports $observed, not a loaded host on $Port."
            }
            Start-Sleep -Milliseconds $ReadyProbeIntervalMs
        }
        Write-Host "OPENNOVA_HOST_PEERS=$($network.peers)"
        Write-Host "OPENNOVA_HOST_READY=True"
    }
    catch {
        $readinessFailure = $_
        try {
            $null = Stop-GameViaMcp -Port $McpPort -Process $process -TimeoutSeconds 10
            Stop-OpenNovaProcess -Process $process
        }
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
            Stop-OpenNovaProcess -Process $process `
                -GracefulTimeoutMs 0 -ForceTimeoutMs 5000
        }
    }
    Write-Host "OPENNOVA_HOST_EXIT_CODE=$($process.ExitCode)"
}
if ($PassThru) { return $process }
