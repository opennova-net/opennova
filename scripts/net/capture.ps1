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
#   pwsh -File scripts\net\capture.ps1 -Action start -Tag retail-ref [-Iface 0] [-Filter 'udp'] [-DurationSeconds 90]
#   pwsh -File scripts\net\capture.ps1 -Action stop  -Tag retail-ref [-TailMarkerHex <hex>]
#
# start prints: CAPTURE_FILE=<path>  CAPTURE_PID=<pid>
# stop  prints: CAPTURE_FILE=<path>  CAPTURE_BYTES=<n>

param(
    [Parameter(Mandatory = $true)][ValidateSet("start", "stop")] [string] $Action,
    [Parameter(Mandatory = $true)] [string] $Tag,
    [string[]] $Iface = @(),
    [switch] $All,
    [string] $Filter = "udp",
    [ValidateRange(0, 86400)] [int] $DurationSeconds = 0,
    [string] $TailMarkerHex = ""
)

$ErrorActionPreference = "Stop"
. "$PSScriptRoot\lib.ps1"

$scratch = Get-ScratchDir
# Per-tag control files so start/stop pair up without passing the path around.
$pidFile  = Join-Path $scratch "$Tag.capture.pid"
$metaFile = Join-Path $scratch "$Tag.capture.path"

function Read-StreamExactly {
    param(
        [Parameter(Mandatory = $true)] [System.IO.Stream] $Stream,
        [Parameter(Mandatory = $true)] [byte[]] $Buffer,
        [Parameter(Mandatory = $true)] [int] $Offset,
        [Parameter(Mandatory = $true)] [int] $Count,
        [Parameter(Mandatory = $true)] [string] $Context
    )
    $readTotal = 0
    while ($readTotal -lt $Count) {
        $readNow = $Stream.Read($Buffer, $Offset + $readTotal, $Count - $readTotal)
        if ($readNow -le 0) {
            throw "Truncated pcapng while reading $Context"
        }
        $readTotal += $readNow
    }
}

function Read-PcapngUInt32 {
    param(
        [Parameter(Mandatory = $true)] [byte[]] $Bytes,
        [Parameter(Mandatory = $true)] [int] $Offset,
        [Parameter(Mandatory = $true)] [bool] $LittleEndian
    )
    if ($Offset -lt 0 -or $Offset + 4 -gt $Bytes.Length) {
        throw "pcapng uint32 lies outside its block"
    }
    [byte[]] $word = @(
        $Bytes[$Offset], $Bytes[$Offset + 1],
        $Bytes[$Offset + 2], $Bytes[$Offset + 3]
    )
    if ([BitConverter]::IsLittleEndian -ne $LittleEndian) {
        [Array]::Reverse($word)
    }
    return [BitConverter]::ToUInt32($word, 0)
}

function Find-ByteSequence {
    param(
        [Parameter(Mandatory = $true)] [byte[]] $Haystack,
        [Parameter(Mandatory = $true)] [int] $Offset,
        [Parameter(Mandatory = $true)] [int] $Count,
        [Parameter(Mandatory = $true)] [byte[]] $Needle
    )
    if ($Needle.Length -eq 0 -or $Needle.Length -gt $Count) { return -1 }
    $last = $Offset + $Count - $Needle.Length
    for ($candidate = $Offset; $candidate -le $last; $candidate++) {
        $matched = $true
        for ($i = 0; $i -lt $Needle.Length; $i++) {
            if ($Haystack[$candidate + $i] -ne $Needle[$i]) {
                $matched = $false
                break
            }
        }
        if ($matched) { return $candidate }
    }
    return -1
}

function ConvertFrom-HexBytes {
    param([Parameter(Mandatory = $true)] [string] $Hex)
    if ($Hex -notmatch '^(?:[0-9A-Fa-f]{2}){8,}$') {
        throw "TailMarkerHex must contain at least 8 bytes of even-length hexadecimal"
    }
    [byte[]] $result = New-Object byte[] ($Hex.Length / 2)
    for ($i = 0; $i -lt $result.Length; $i++) {
        $result[$i] = [Convert]::ToByte($Hex.Substring($i * 2, 2), 16)
    }
    return $result
}

function Get-PcapngCompletionProof {
    param(
        [Parameter(Mandatory = $true)] [string] $Path,
        [byte[]] $TailMarker = @()
    )
    $stream = [System.IO.File]::Open(
        $Path,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::Read
    )
    try {
        $fileLength = $stream.Length
        if ($fileLength -le 0) { throw "Capture file is empty; no packets were recorded." }

        $blockCount = 0
        $packetBlockCount = 0
        $sectionCount = 0
        $littleEndian = $null
        $markerBlockIndex = -1
        $markerBlockType = ""
        $markerCaptureOffset = -1L

        while ($stream.Position -lt $fileLength) {
            $blockOffset = $stream.Position
            $remaining = $fileLength - $blockOffset
            if ($remaining -lt 12) {
                throw "Truncated pcapng tail: $remaining byte(s) remain after block $blockCount"
            }

            [byte[]] $header = New-Object byte[] 12
            Read-StreamExactly -Stream $stream -Buffer $header -Offset 0 -Count 12 `
                -Context "block $blockCount header"
            $isSectionHeader = (
                $header[0] -eq 0x0a -and $header[1] -eq 0x0d -and
                $header[2] -eq 0x0d -and $header[3] -eq 0x0a
            )
            if ($blockCount -eq 0 -and -not $isSectionHeader) {
                throw "pcapng does not begin with a Section Header Block"
            }
            if ($isSectionHeader) {
                if ($header[8] -eq 0x4d -and $header[9] -eq 0x3c -and
                        $header[10] -eq 0x2b -and $header[11] -eq 0x1a) {
                    $littleEndian = $true
                }
                elseif ($header[8] -eq 0x1a -and $header[9] -eq 0x2b -and
                        $header[10] -eq 0x3c -and $header[11] -eq 0x4d) {
                    $littleEndian = $false
                }
                else {
                    throw "Section Header Block $blockCount has an invalid byte-order magic"
                }
                $sectionCount++
            }
            elseif ($null -eq $littleEndian) {
                throw "pcapng block $blockCount precedes any Section Header Block"
            }

            $blockLength = [long](Read-PcapngUInt32 -Bytes $header -Offset 4 `
                -LittleEndian ([bool] $littleEndian))
            if ($blockLength -lt 12 -or ($blockLength % 4) -ne 0) {
                throw "pcapng block $blockCount has invalid total length $blockLength"
            }
            if ($isSectionHeader -and $blockLength -lt 28) {
                throw "Section Header Block $blockCount is shorter than 28 bytes"
            }
            if ($blockLength -gt $remaining) {
                throw "pcapng block $blockCount declares $blockLength bytes with only $remaining remaining"
            }
            if ($blockLength -gt [int]::MaxValue) {
                throw "pcapng block $blockCount is too large to validate safely: $blockLength"
            }

            [byte[]] $block = New-Object byte[] ([int] $blockLength)
            [Array]::Copy($header, 0, $block, 0, 12)
            if ($blockLength -gt 12) {
                Read-StreamExactly -Stream $stream -Buffer $block -Offset 12 `
                    -Count ([int] $blockLength - 12) -Context "block $blockCount body"
            }
            $trailingLength = [long](Read-PcapngUInt32 -Bytes $block `
                -Offset ([int] $blockLength - 4) -LittleEndian ([bool] $littleEndian))
            if ($trailingLength -ne $blockLength) {
                throw "pcapng block $blockCount header/trailer length mismatch ($blockLength != $trailingLength)"
            }

            $blockType = Read-PcapngUInt32 -Bytes $block -Offset 0 `
                -LittleEndian ([bool] $littleEndian)
            $packetDataOffset = -1
            $packetDataLength = 0
            if ($blockType -eq 6 -or $blockType -eq 2) {
                # Enhanced Packet Blocks and obsolete Packet Blocks share the
                # captured-length and packet-data offsets used below.
                if ($blockLength -lt 32) {
                    throw "pcapng packet block $blockCount is shorter than 32 bytes"
                }
                $capturedLength = [long](Read-PcapngUInt32 -Bytes $block -Offset 20 `
                    -LittleEndian ([bool] $littleEndian))
                if ($capturedLength -gt $blockLength - 32) {
                    throw "pcapng packet block $blockCount captured length exceeds its body"
                }
                $packetDataOffset = 28
                $packetDataLength = [int] $capturedLength
                $packetBlockCount++
            }
            elseif ($blockType -eq 3) {
                # A Simple Packet Block has no captured-length field. The bytes
                # between its original-length field and trailer are packet data
                # (plus at most three bytes of alignment padding).
                if ($blockLength -lt 16) {
                    throw "pcapng simple packet block $blockCount is shorter than 16 bytes"
                }
                $originalLength = [long](Read-PcapngUInt32 -Bytes $block -Offset 8 `
                    -LittleEndian ([bool] $littleEndian))
                $availableLength = [int] $blockLength - 16
                $packetDataOffset = 12
                $packetDataLength = [int][Math]::Min($originalLength, $availableLength)
                $packetBlockCount++
            }

            if ($TailMarker.Length -gt 0 -and $markerBlockIndex -lt 0 -and
                    $packetDataOffset -ge 0) {
                $found = Find-ByteSequence -Haystack $block -Offset $packetDataOffset `
                    -Count $packetDataLength -Needle $TailMarker
                if ($found -ge 0) {
                    $markerBlockIndex = $blockCount
                    $markerBlockType = ('0x{0:x8}' -f $blockType)
                    $markerCaptureOffset = $blockOffset + $found
                }
            }
            $blockCount++
        }

        if ($stream.Position -ne $fileLength) {
            throw "pcapng parser did not finish at exact EOF"
        }
        if ($sectionCount -le 0) { throw "pcapng contains no Section Header Block" }
        if ($packetBlockCount -le 0) { throw "pcapng contains no packet blocks" }
        if ($TailMarker.Length -gt 0 -and $markerBlockIndex -lt 0) {
            throw "Tail marker was not persisted inside captured packet data"
        }

        $markerSha256 = ""
        if ($TailMarker.Length -gt 0) {
            $sha = [System.Security.Cryptography.SHA256]::Create()
            try { $digest = $sha.ComputeHash($TailMarker) }
            finally { $sha.Dispose() }
            $markerSha256 = ([BitConverter]::ToString($digest)).Replace('-', '').ToLowerInvariant()
        }
        return [pscustomobject]@{
            valid = $true
            exact_eof = $true
            byte_length = $fileLength
            section_count = $sectionCount
            block_count = $blockCount
            packet_block_count = $packetBlockCount
            tail_marker_required = ($TailMarker.Length -gt 0)
            tail_marker_found = ($markerBlockIndex -ge 0)
            tail_marker_bytes = $TailMarker.Length
            tail_marker_sha256 = $markerSha256
            tail_marker_block_index = $markerBlockIndex
            tail_marker_block_type = $markerBlockType
            tail_marker_capture_offset = $markerCaptureOffset
        }
    }
    finally {
        $stream.Dispose()
    }
}

if ($Action -eq "start") {
    if ($TailMarkerHex) {
        throw "TailMarkerHex is valid only with -Action stop"
    }
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

    # PowerShell scalarizes a one-item pipeline result; under strict mode the
    # resulting string has no collection Count contract. Preserve the array so
    # one-interface captures take the same validation path as multi-interface.
    $devNames = @($devNames | Select-Object -Unique)
    if ($devNames.Count -eq 0) { Write-Error "No capture interfaces resolved."; exit 4 }

    $out = Join-Path $scratch ("{0}-{1}.pcapng" -f $Tag, (Get-RunStamp))
    $args = @()
    foreach ($dn in $devNames) {
        $args += @("-i", $dn)
        # dumpcap capture filters are interface-scoped: with multiple `-i`
        # arguments, one trailing `-f` applies only to the final interface.
        # Start-Process also flattens ArgumentList on Windows, so preserve a
        # multi-word filter as one native argument with explicit quotes.
        if ($Filter) {
            $quotedFilter = '"' + $Filter.Replace('"', '\"') + '"'
            $args += @("-f", $quotedFilter)
        }
    }
    if ($DurationSeconds -gt 0) {
        # Let dumpcap close its own pcapng section/interface blocks. A forced
        # Stop-Process can leave the final Enhanced Packet Block truncated.
        $args += @("-a", "duration:$DurationSeconds")
    }
    $args += @("-w", $out, "-q")
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
    if ($DurationSeconds -gt 0) {
        Write-Host "CAPTURE_AUTO_STOP_SECONDS=$DurationSeconds"
    }
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
[byte[]] $tailMarker = @()
if ($TailMarkerHex) {
    $tailMarker = [byte[]] @(ConvertFrom-HexBytes -Hex $TailMarkerHex)
}
$proof = Get-PcapngCompletionProof -Path $out -TailMarker $tailMarker
Write-Host ("CAPTURE_PROOF_JSON=" + ($proof | ConvertTo-Json -Compress))
exit 0
