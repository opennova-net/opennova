# Launch one stock retail role whose LAN action is configured by the
# onhook.cfg beside Jointops.exe. This is the mixed-client/host harness entry:
# the cfg chooses host vs joiner, while child-only ONHOOK_* metadata owns logs
# and packet capture without leaking into later processes.
#
# Usage:
#   pwsh -File scripts\net\launch_retail_cfg.ps1 `
#       -GameDir .scratch\CLIENT -RunId pr403-retail-client `
#       -OutputDir .scratch\runs\pr403-retail-client\joiner

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $GameDir,
    [string] $HookConfig = "",
    [string] $RunId = "",
    [string] $OutputDir = "",
    [switch] $NoCapture,
    [switch] $PlanOnly,
    [switch] $Wait,
    [switch] $PassThru
)

$ErrorActionPreference = "Stop"
. "$PSScriptRoot\lib.ps1"

$GameDir = (Resolve-Path -LiteralPath $GameDir -ErrorAction Stop).Path
$exe = Join-Path $GameDir "Jointops.exe"
$proxy = Join-Path $GameDir "binkw32.dll"
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "Jointops.exe was not found in: $GameDir"
}
if (-not (Test-Path -LiteralPath $proxy -PathType Leaf)) {
    throw "The current hook proxy binkw32.dll was not found in: $GameDir"
}

$expectedConfig = Join-Path $GameDir "onhook.cfg"
if ([string]::IsNullOrWhiteSpace($HookConfig)) {
    $HookConfig = $expectedConfig
}
$hook = Read-OnHookConfig -Path $HookConfig
if (-not $hook.Path.Equals(
        [System.IO.Path]::GetFullPath($expectedConfig),
        [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Retail reads only the onhook.cfg beside Jointops.exe; expected: $expectedConfig"
}
$cfg = $hook.Values

$hasHost = $cfg.ContainsKey("LanHostMission") -and
    -not [string]::IsNullOrWhiteSpace($cfg["LanHostMission"])
$hasJoiner = $cfg.ContainsKey("LanJoinAddress") -and
    -not [string]::IsNullOrWhiteSpace($cfg["LanJoinAddress"])
if ($hasHost -eq $hasJoiner) {
    throw "onhook.cfg must configure exactly one role: LanHostMission or LanJoinAddress."
}
$role = if ($hasHost) { "host" } else { "joiner" }
$portKey = if ($hasHost) { "LanHostPort" } else { "LanJoinPort" }
if (-not $cfg.ContainsKey($portKey)) {
    throw "$portKey is required for owned capture metadata."
}
$port = 0
if (-not [int]::TryParse($cfg[$portKey], [ref] $port) -or
        $port -lt 1 -or $port -gt 65535) {
    throw "$portKey must be an integer from 1 through 65535."
}

if ([string]::IsNullOrWhiteSpace($RunId)) {
    $RunId = "retail-$role-$(Get-RunStamp)"
}
if ($RunId -notmatch '^[A-Za-z0-9](?:[A-Za-z0-9._-]{0,61}[A-Za-z0-9])?$') {
    throw "RunId must be 1-63 safe characters and start/end with a letter or digit."
}
if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = Join-Path (Get-ScratchDir "runs") "$RunId\$role"
} elseif (-not [System.IO.Path]::IsPathRooted($OutputDir)) {
    $OutputDir = Join-Path (Get-RepoRoot) $OutputDir
}
$OutputDir = [System.IO.Path]::GetFullPath($OutputDir)
if (-not $PlanOnly) {
    if (-not (Test-Path -LiteralPath $OutputDir -PathType Container)) {
        New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
    }
    $OutputDir = (Resolve-Path -LiteralPath $OutputDir).Path
} elseif (Test-Path -LiteralPath $OutputDir -PathType Container) {
    $OutputDir = (Resolve-Path -LiteralPath $OutputDir).Path
}

$logPath = Join-Path $OutputDir "onhook.log"
$capturePath = Join-Path $OutputDir "traffic.pcap"
if (-not $PlanOnly) {
    if (Test-Path -LiteralPath $logPath) {
        throw "Hook log already exists; run artifacts are create-new: $logPath"
    }
    if (-not $NoCapture -and (Test-Path -LiteralPath $capturePath)) {
        throw "Hook capture already exists; run artifacts are create-new: $capturePath"
    }
}

$windowed = $true
if ($cfg.ContainsKey("OpenNovaWindowed")) {
    $windowed = ConvertFrom-OnHookBoolean `
        -Value $cfg["OpenNovaWindowed"] -Key "OpenNovaWindowed"
}
$expansion = ""
if ($cfg.ContainsKey("OpenNovaExpansion")) {
    $expansion = $cfg["OpenNovaExpansion"].Trim()
    if ($expansion -notmatch '^(?:|[A-Za-z0-9][A-Za-z0-9_-]{0,30})$') {
        throw "OpenNovaExpansion must be empty or a 1-31 character retail expansion slug."
    }
}

$arguments = @()
if ($windowed) { $arguments += "/w" }
$arguments += "/many"
if (-not [string]::IsNullOrWhiteSpace($expansion)) {
    $arguments += @("/exp", $expansion)
}

Write-Host "RETAIL_CFG_ROLE=$role"
Write-Host "RETAIL_CFG_PATH=$($hook.Path)"
Write-Host "RETAIL_CFG_PORT=$port"
Write-Host "RETAIL_CFG_WINDOWED=$windowed"
Write-Host "RETAIL_CFG_EXPANSION=$expansion"
Write-Host "RETAIL_CFG_ARGS=$($arguments -join ' ')"
Write-Host "RETAIL_RUN_ID=$RunId"
Write-Host "RETAIL_LOG=$logPath"
if (-not $NoCapture) { Write-Host "RETAIL_CAPTURE=$capturePath" }
if ($PlanOnly) {
    Write-Host "RETAIL_PLAN_ONLY=True"
    exit 0
}

$isolatedNames = @(
    "ONHOOK_RUN_ID", "ONHOOK_RUN_ROLE", "ONHOOK_LOG_PATH",
    "ONHOOK_CAPTURE_PATH", "ONHOOK_CAPTURE_PORT"
)
$inheritedLanNames = @(Get-ChildItem Env: | Where-Object { $_.Name -like "NW_LAN_*" } |
    ForEach-Object { $_.Name })
$environmentNames = @($isolatedNames + $inheritedLanNames | Select-Object -Unique)
$saved = @{}
foreach ($name in $environmentNames) {
    $saved[$name] = [Environment]::GetEnvironmentVariable(
        $name, [EnvironmentVariableTarget]::Process)
}

try {
    foreach ($name in $environmentNames) {
        [Environment]::SetEnvironmentVariable(
            $name, $null, [EnvironmentVariableTarget]::Process)
    }
    $env:ONHOOK_RUN_ID = $RunId
    $env:ONHOOK_RUN_ROLE = $role
    $env:ONHOOK_LOG_PATH = $logPath
    if (-not $NoCapture) {
        $env:ONHOOK_CAPTURE_PATH = $capturePath
        $env:ONHOOK_CAPTURE_PORT = [string] $port
    }
    $process = Start-Process -FilePath $exe `
        -WorkingDirectory $GameDir `
        -ArgumentList $arguments `
        -PassThru
}
finally {
    foreach ($name in $environmentNames) {
        [Environment]::SetEnvironmentVariable(
            $name, $null, [EnvironmentVariableTarget]::Process)
    }
    foreach ($name in $saved.Keys) {
        [Environment]::SetEnvironmentVariable(
            $name, $saved[$name], [EnvironmentVariableTarget]::Process)
    }
}

Write-Host "RETAIL_PID=$($process.Id)"
if ($Wait) {
    $process.WaitForExit()
    Write-Host "RETAIL_EXIT_CODE=$($process.ExitCode)"
}
if ($PassThru) { return $process }
