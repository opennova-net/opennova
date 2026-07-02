# Detect the live-capture substrate (Npcap + dumpcap) for the net harness.
#
# The repo only READS pcaps today; live capture relies on an external Npcap +
# dumpcap install. This script reports whether the machine can capture, and in
# particular whether single-box loopback capture (the chosen topology) is
# possible. It is the first step of every run_* orchestrator.
#
# Usage:
#   pwsh -File scripts\net\detect_capture.ps1
#
# Output (stdout, machine-readable key=value lines consumed by capture.ps1):
#   DUMPCAP=<path>
#   NPCAP_RUNNING=<true|false>
#   LOOPBACK_IFACE=<device name>   (only when a loopback adapter is present;
#                                   pass verbatim to capture.ps1 -Iface)
#   CAPTURE_READY=<loopback|nic-only|no>
#
# Exit codes:
#   0  loopback capture ready (DUMPCAP + Npcap loopback adapter)
#   3  dumpcap not found            (install Wireshark)
#   4  Npcap/loopback unavailable   (install Npcap with loopback support, or
#                                     fall back to real-NIC single-box capture)

$ErrorActionPreference = "Stop"
. "$PSScriptRoot\lib.ps1"

$dumpcap = Find-Dumpcap
if (-not $dumpcap) {
    Write-Host "CAPTURE_READY=no"
    Write-Error @"
dumpcap not found. Install Wireshark (which bundles dumpcap + Npcap), then re-run.
  https://www.wireshark.org/download.html
Check 'Install Npcap' and its 'Support loopback traffic' option during setup.
"@
    exit 3
}
Write-Host "DUMPCAP=$dumpcap"

$npcap = Test-NpcapRunning
Write-Host "NPCAP_RUNNING=$($npcap.ToString().ToLower())"

$ifaces = Get-CaptureInterfaces -Dumpcap $dumpcap
$loopback = $ifaces | Where-Object { $_.IsLoopback } | Select-Object -First 1

Write-Host "--- dumpcap interfaces ---"
foreach ($i in $ifaces) {
    $tag = if ($i.IsLoopback) { " [loopback]" } else { "" }
    Write-Host ("  {0}. {1} {2}{3}" -f $i.Number, $i.Name, $i.Description, $tag)
}
Write-Host "--------------------------"

if ($loopback) {
    Write-Host "LOOPBACK_IFACE=$($loopback.Name)"
    Write-Host "CAPTURE_READY=loopback"
    exit 0
}

# dumpcap present but no loopback adapter: real-NIC single-box capture is still
# viable (a process sees its own subnet broadcasts), just not loopback.
if ($ifaces.Count -gt 0) {
    Write-Host "CAPTURE_READY=nic-only"
} else {
    Write-Host "CAPTURE_READY=no"
}
Write-Error @"
No Npcap loopback adapter found (Npcap running: $npcap).
Single-box LOOPBACK capture needs Npcap installed with 'Support loopback traffic'.
Fallbacks:
  - Reinstall Npcap with the loopback option, then re-run; or
  - Capture single-box on the real NIC (subnet broadcast loops back to local
    instances) by passing a non-loopback interface to capture.ps1; or
  - Have the retail driver join 127.0.0.1 by address to avoid broadcast discovery.
"@
exit 4
