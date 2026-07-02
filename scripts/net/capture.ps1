# Start/stop a dumpcap capture into <repo>\.scratch\<tag>-<stamp>.pcapng.
#
# Wraps the external dumpcap (located + validated by detect_capture.ps1). Default
# interface is the Npcap loopback adapter (single-box host+join topology); pass
# -Iface to capture a real NIC instead. Default filter is all UDP so broadcast
# LAN discovery on unknown ports is never missed; narrow with -Filter if desired.
#
# Captures must run from process start through the behavior under study: mid-stream
# captures miss the handshake and cannot recover SCRK/session keys.
#
# Usage:
#   pwsh -File scripts\net\capture.ps1 -Action start -Tag retail-ref [-Iface 0] [-Filter 'udp']
#   pwsh -File scripts\net\capture.ps1 -Action stop  -Tag retail-ref
#
# start prints: CAPTURE_FILE=<path>  CAPTURE_PID=<pid>
# stop  prints: CAPTURE_FILE=<path>  CAPTURE_BYTES=<n>

param(
    [Parameter(Mandatory = $true)][ValidateSet("start", "stop")] [string] $Action,
    [Parameter(Mandatory = $true)] [string] $Tag,
    [string[]] $Iface = @(),
    [switch] $All,
    [string] $Filter = "udp"
)

$ErrorActionPreference = "Stop"
. "$PSScriptRoot\lib.ps1"

$scratch = Get-ScratchDir
# Per-tag control files so start/stop pair up without passing the path around.
$pidFile  = Join-Path $scratch "$Tag.capture.pid"
$metaFile = Join-Path $scratch "$Tag.capture.path"

if ($Action -eq "start") {
    $dumpcap = Find-Dumpcap
    if (-not $dumpcap) {
        Write-Error "dumpcap not found; run detect_capture.ps1 first."
        exit 3
    }
    # Resolve to a list of dumpcap device NAMEs (e.g. \Device\NPF_Loopback). dumpcap
    # rejects the `dumpcap -D` ordinal as `-i`, so we always map to the name.
    $ifaces = Get-CaptureInterfaces -Dumpcap $dumpcap
    $devNames = @()

    if ($All) {
        # Single-box LAN can broadcast over the real NIC instead of 127.0.0.1, so
        # capture the loopback adapter AND every connected physical adapter at once.
        $lo = $ifaces | Where-Object { $_.IsLoopback } | Select-Object -First 1
        if ($lo) { $devNames += $lo.Name }
        $upGuids = @(Get-NetAdapter -Physical -ErrorAction SilentlyContinue |
            Where-Object { $_.Status -eq 'Up' } | ForEach-Object { $_.InterfaceGuid })
        foreach ($i in $ifaces) {
            if ($i.IsLoopback) { continue }
            foreach ($g in $upGuids) {
                if ($g -and ($i.Name -like "*$g*")) { $devNames += $i.Name; break }
            }
        }
    }
    elseif ($Iface.Count -gt 0) {
        foreach ($spec in $Iface) {
            if ($spec -match '^\d+$') {
                $m = $ifaces | Where-Object { $_.Number -eq [int]$spec } | Select-Object -First 1
                if (-not $m) { Write-Error "No interface numbered $spec (see detect_capture.ps1)."; exit 4 }
                $devNames += $m.Name
            } else { $devNames += $spec }
        }
    }
    else {
        $lo = $ifaces | Where-Object { $_.IsLoopback } | Select-Object -First 1
        if (-not $lo) { Write-Error "No loopback interface; pass -Iface or -All (see detect_capture.ps1)."; exit 4 }
        $devNames += $lo.Name
    }

    $devNames = $devNames | Select-Object -Unique
    if ($devNames.Count -eq 0) { Write-Error "No capture interfaces resolved."; exit 4 }

    $out = Join-Path $scratch ("{0}-{1}.pcapng" -f $Tag, (Get-RunStamp))
    $args = @()
    foreach ($dn in $devNames) { $args += @("-i", $dn) }
    $args += @("-w", $out, "-q")
    if ($Filter) { $args += @("-f", $Filter) }
    Write-Host ("CAPTURE_IFACES={0}" -f ($devNames -join ', '))

    $proc = Start-Process -FilePath $dumpcap -ArgumentList $args -PassThru -WindowStyle Hidden
    Set-Content -Path $pidFile  -Value $proc.Id -Encoding ascii
    Set-Content -Path $metaFile -Value $out     -Encoding ascii

    # Give dumpcap a moment to open the adapter and create the file.
    Start-Sleep -Milliseconds 800
    if ($proc.HasExited) {
        Write-Error "dumpcap exited immediately (exit $($proc.ExitCode)); check interface/filter/privileges."
        exit 5
    }
    Write-Host "CAPTURE_FILE=$out"
    Write-Host "CAPTURE_PID=$($proc.Id)"
    exit 0
}

# stop
if (-not (Test-Path $pidFile) -or -not (Test-Path $metaFile)) {
    Write-Error "No active capture for tag '$Tag' (missing $pidFile / $metaFile)."
    exit 1
}
$capPid = [int](Get-Content $pidFile -Raw).Trim()
$out    = (Get-Content $metaFile -Raw).Trim()

$p = Get-Process -Id $capPid -ErrorAction SilentlyContinue
if ($p) {
    # dumpcap writes pcapng incrementally and flushes frequently; the on-disk file
    # is valid through the last flushed block even on a hard stop.
    Stop-Process -Id $capPid -Force
    $p.WaitForExit(5000) | Out-Null
}
Remove-Item $pidFile, $metaFile -ErrorAction SilentlyContinue

if (-not (Test-Path $out)) {
    Write-Error "Capture file not found: $out"
    exit 1
}
$bytes = (Get-Item $out).Length
Write-Host "CAPTURE_FILE=$out"
Write-Host "CAPTURE_BYTES=$bytes"
if ($bytes -le 0) {
    Write-Error "Capture file is empty; no packets were recorded."
    exit 6
}
exit 0
