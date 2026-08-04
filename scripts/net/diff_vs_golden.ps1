# Diff a freshly captured session against a golden retail capture, by message
# coverage. Decodes both with the repo's own nw_pp --histogram (the stable
# machine-readable contract) and reports, per (direction, wire tag):
#
#   * GAP      -- retail emits this steady-state tag but OUR capture never does
#   * ABSENT   -- an event-conditioned tag was not exercised by this scenario
#   * SPURIOUS -- we emit an unapproved S2C tag the golden never does
#   * OK       -- both emit it (counts shown; they legitimately differ, since the
#                golden is retail<->retail and ours is retail<->opennova)
#
# It also re-decodes OUR capture in full and flags any message that failed to
# decode cleanly ("DECODE INCOMPLETE" / "decode failed") -- those are our bugs
# regardless of the golden. GAPs are the prioritized worklist for aligning the
# server; event-conditioned absence is informational, while steady-state GAPs,
# unapproved S2C SPURIOUS tags, and decode failures are regressions.
#
# This is a COVERAGE diff, not a byte diff: per-frame contents (positions, ticks,
# SCRK, seq) legitimately differ between two different sessions, so we compare
# which messages flow and in what proportion, plus that ours decode cleanly.
# Field/byte parity is enforced separately by the existing focused groundtruth
# tests. This policy mirrors npruntime_golden_diff's coverage classification.
#
# Usage:
#   pwsh -File scripts\net\diff_vs_golden.ps1 `
#        -Ours    .scratch\retail-join-<stamp>.pcapng `
#        -Golden  .scratch\golden\retail-gameplay-session.pcapng `
#        [-Items ~\Desktop\JOX\ITEMS.DEF] [-MinGolden 1]
#
# Writes <Ours>.vs-golden.txt and prints the table + a PASS/FAIL summary line.
# Exit 0 = no gaps/spurious/decode-failures; non-zero otherwise (CI-friendly).

param(
    [Parameter(Mandatory = $true)] [string] $Ours,
    [Parameter(Mandatory = $true)] [string] $Golden,
    [string] $Items = "",
    # A golden tag with fewer than this many messages is treated as noise and
    # not reported as a GAP (rare one-off control tags shouldn't dominate the
    # worklist). Raise to focus on the high-volume replication core.
    [int] $MinGolden = 1
)

$ErrorActionPreference = "Stop"
. "$PSScriptRoot\lib.ps1"

# A passive join capture cannot be required to manufacture a death, expiry,
# deploy, target, sound, team-change, or placeable event. Keep these separate
# from implementation deferrals: each path still needs its own induced witness
# or deterministic regression. Keys mirror nw_golden_diff_test.cpp.
$stateConditioned = @{
    "C 0x0f" = "entity-self-repair"
    "S 0x12" = "entity-expiry"
    "S 0x13" = "entity-death"
    "S 0x18" = "entity-self-repair"
    "S 0x1e" = "game-event"
    "S 0x26" = "entity-kill"
    "S 0x34" = "sound-trigger"
    "S 0x49" = "weapon-reload"
    "S 0x4c" = "target-assignment"
    "S 0x4e" = "player-kill"
    "S 0x50" = "team-change"
    "S 0x52" = "player-death-stats"
    "S 0x59" = "deployed-item"
    "S 0x6e" = "dead-or-deploying"
    "S 0x81" = "score-delta"
}
$allowedSpurious = @{
    "S 0x18" = "D-NET-133"
    "S 0x49" = "weapon-reload"
}

function Find-Tool {
    param([string] $Name)
    $build = Join-Path (Get-RepoRoot) "build"
    foreach ($cfg in @("Release", "Debug")) {
        $p = Join-Path $build "apps\$Name\$cfg\$Name.exe"
        if (Test-Path $p) { return $p }
    }
    $hit = Get-ChildItem $build -Recurse -Filter "$Name.exe" -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($hit) { return $hit.FullName }
    return $null
}

foreach ($f in @($Ours, $Golden)) {
    if (-not (Test-Path $f)) { Write-Error "Capture not found: $f"; exit 1 }
}
$Ours   = (Resolve-Path $Ours).Path
$Golden = (Resolve-Path $Golden).Path

$nwpp = Find-Tool -Name "nw_pp"
if (-not $nwpp) {
    Write-Error "nw_pp.exe not found under build\; run scripts/build.sh first."
    exit 2
}

# Run nw_pp and return its stdout lines. nw_pp writes progress to stderr; under
# $ErrorActionPreference='Stop' a native command's stderr is wrapped as a
# terminating NativeCommandError (a PS 5.1 quirk), so relax EAP around the call
# and drop stderr. stdout (the HIST / decode lines) is what we parse.
function Invoke-NwPp {
    param([string[]] $PpArgs)
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { $out = & $nwpp @PpArgs 2>$null } finally { $ErrorActionPreference = $old }
    return $out
}

# Run nw_pp --histogram on a capture; return a hashtable keyed "<dir> 0x<tag>"
# -> [pscustomobject]{ Dir; Tag; Name; Count; Bytes }.
function Get-Histogram {
    param([string] $Capture)
    $ppArgs = @($Capture, "--histogram")
    if ($Items) { $ppArgs += @("--items", $Items) }
    $out = Invoke-NwPp -PpArgs $ppArgs
    $h = @{}
    foreach ($line in $out) {
        if ($line -match '^HIST\s+([SC])\s+0x([0-9a-fA-F]{2})\s+count=(\d+)\s+bytes=(\d+)\s+name=(.*)$') {
            $key = "{0} 0x{1}" -f $Matches[1], $Matches[2].ToLower()
            $h[$key] = [pscustomobject]@{
                Dir   = $Matches[1]
                Tag   = "0x" + $Matches[2].ToLower()
                Name  = $Matches[5].Trim()
                Count = [int]$Matches[3]
                Bytes = [long]$Matches[4]
            }
        }
    }
    return $h
}

Write-Host "Decoding OURS   : $Ours"
$hOurs = Get-Histogram -Capture $Ours
Write-Host "Decoding GOLDEN : $Golden"
$hGold = Get-Histogram -Capture $Golden

if ($hOurs.Count -eq 0) {
    Write-Error "OURS decoded to zero messages -- capture empty or handshake missing (SCRK unrecoverable)."
    exit 3
}

# Union of keys, sorted by direction then tag. (Build the combined list
# explicitly -- PS 5.1 won't concatenate two hashtable KeyCollections with '+'.)
$allKeys = New-Object System.Collections.Generic.List[string]
foreach ($k in $hOurs.Keys) { $allKeys.Add($k) }
foreach ($k in $hGold.Keys) { $allKeys.Add($k) }
$keys = $allKeys | Select-Object -Unique | Sort-Object

$rows = @()
$gaps = 0; $spurious = 0; $stateAbsent = 0
foreach ($k in $keys) {
    $o = $hOurs[$k]; $g = $hGold[$k]
    $oc = if ($o) { $o.Count } else { 0 }
    $gc = if ($g) { $g.Count } else { 0 }
    $name = if ($o) { $o.Name } elseif ($g) { $g.Name } else { "?" }
    $status = "OK"
    if ($oc -eq 0 -and $gc -ge $MinGolden) {
        if ($stateConditioned.ContainsKey($k)) {
            $status = "absent(state:$($stateConditioned[$k]))"
            $stateAbsent++
        } else {
            $status = "GAP"
            $gaps++
        }
    }
    elseif ($gc -eq 0 -and $oc -gt 0) {
        $dir = $k.Split(' ')[0]
        if ($dir -ne "S") {
            $status = "extra(client)"
        } elseif ($allowedSpurious.ContainsKey($k)) {
            $status = "extra(allowed:$($allowedSpurious[$k]))"
        } else {
            $status = "SPURIOUS"
            $spurious++
        }
    }
    elseif ($oc -eq 0 -and $gc -gt 0)      { $status = "gap(noise)" }
    $parts = $k.Split(' ')
    $rows += [pscustomobject]@{
        Dir = $parts[0]; Tag = $parts[1]; Name = $name
        Ours = $oc; Golden = $gc; Status = $status
    }
}

# Re-decode OURS in full and count messages that didn't decode cleanly.
$decodeArgs = @($Ours)
if ($Items) { $decodeArgs += @("--items", $Items) }
$fullDecode = Invoke-NwPp -PpArgs $decodeArgs
$incomplete = @($fullDecode | Select-String -Pattern 'DECODE INCOMPLETE|decode failed' -SimpleMatch:$false)

$report = "$Ours.vs-golden.txt"
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("# coverage diff: ours vs golden")
[void]$sb.AppendLine("# ours   = $Ours")
[void]$sb.AppendLine("# golden = $Golden")
[void]$sb.AppendLine("")
[void]$sb.AppendLine(($rows | Format-Table Dir, Tag, Name, Ours, Golden, Status -AutoSize | Out-String).TrimEnd())
[void]$sb.AppendLine("")
[void]$sb.AppendLine("GAPS (retail emits, we don't): $gaps")
[void]$sb.AppendLine("STATE-CONDITIONED ABSENCES: $stateAbsent")
[void]$sb.AppendLine("SPURIOUS S2C (we emit, retail doesn't): $spurious")
[void]$sb.AppendLine("DECODE FAILURES in ours: $($incomplete.Count)")
if ($incomplete.Count -gt 0) {
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine("-- first decode failures --")
    foreach ($l in ($incomplete | Select-Object -First 20)) {
        [void]$sb.AppendLine($l.Line)
    }
}
Set-Content -Path $report -Value $sb.ToString() -Encoding utf8

Write-Host (($rows | Format-Table Dir, Tag, Name, Ours, Golden, Status -AutoSize | Out-String).TrimEnd())
Write-Host ""
Write-Host ("GAPS={0}  STATE_ABSENT={1}  SPURIOUS={2}  DECODE_FAILURES={3}" -f `
    $gaps, $stateAbsent, $spurious, $incomplete.Count)
Write-Host "REPORT=$report"

if ($gaps -gt 0 -or $spurious -gt 0 -or $incomplete.Count -gt 0) { exit 1 }
exit 0
