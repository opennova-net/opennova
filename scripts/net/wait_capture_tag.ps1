# Wait for the first record of one tag in one direction in a growing parity
# capture, at or after a given capture time, and print it. As in
# wait_parity_wire_ready.ps1, the source capture is never decoded in place:
# every poll copies an immutable prefix snapshot first and decodes it with
# opennova-wire --parity-events. Prints one line
#   PARITY_CAPTURE_TAG frame=<n> ts_ns=<capture ns> dir=<S|C> tag=0x<tt>
# and exits 0, or PARITY_CAPTURE_TAG_TIMEOUT and exits 2.

[CmdletBinding()]
param(
    [string] $Wire = '',
    [Parameter(Mandatory = $true)] [string] $Capture,
    [Parameter(Mandatory = $true)] [string] $Items,
    [ValidateSet('S', 'C')] [string] $Direction = 'S',
    [Parameter(Mandatory = $true)] [ValidatePattern('^0x[0-9a-fA-F]{2}$')] [string] $Tag,
    # Capture time (Unix ns) the record must not precede.
    [int64] $AfterUnixNs = 0,
    [ValidateRange(1, 3600)] [int] $TimeoutSeconds = 120,
    [ValidateRange(100, 10000)] [int] $PollMilliseconds = 1000
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# The prefix copy and the decode are wait_parity_wire_ready.ps1's
# (Copy-CapturePrefixSnapshot, Invoke-WireSnapshot).
function Copy-CapturePrefixSnapshot {
    param([string] $Source, [string] $Destination)
    $sourceStream = [IO.File]::Open(
        $Source, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
    try {
        $prefixLength = [int64]$sourceStream.Length
        $destinationStream = [IO.File]::Open(
            $Destination, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
        try {
            $buffer = [byte[]]::new(1024 * 1024)
            [int64]$remaining = $prefixLength
            while ($remaining -gt 0) {
                $requested = [int][Math]::Min([int64]$buffer.Length, $remaining)
                $read = $sourceStream.Read($buffer, 0, $requested)
                if ($read -le 0) { throw "Capture stopped yielding bytes before prefix $prefixLength." }
                $destinationStream.Write($buffer, 0, $read)
                $remaining -= $read
            }
            $destinationStream.Flush($true)
        } finally {
            $destinationStream.Dispose()
        }
    } finally {
        $sourceStream.Dispose()
    }
}

function Invoke-WireSnapshot {
    param([string] $Executable, [string] $Snapshot, [string] $ItemsPath)
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $Executable
    $start.Arguments = (@($Snapshot, '--items', $ItemsPath, '--parity-events') |
        ForEach-Object { '"' + $_ + '"' }) -join ' '
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    try {
        if (-not $process.Start()) { throw 'Could not start opennova-wire.' }
        $stdoutRead = $process.StandardOutput.ReadToEndAsync()
        $null = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(60000)) {
            $process.Kill(); $null = $process.WaitForExit(5000)
            throw 'opennova-wire exceeded the 60-second snapshot decode limit.'
        }
        if ($process.ExitCode -ne 0) { return @() }
        return @([regex]::Split($stdoutRead.Result, '\r?\n') | Where-Object { $_.Length -gt 0 })
    } finally {
        $process.Dispose()
    }
}

if ([string]::IsNullOrWhiteSpace($Wire)) {
    $Wire = Join-Path $PSScriptRoot '..\..\build\apps\wire\Release\opennova-wire.exe'
}
$Wire = (Resolve-Path -LiteralPath $Wire -ErrorAction Stop).Path
$Items = (Resolve-Path -LiteralPath $Items -ErrorAction Stop).Path
$Capture = [IO.Path]::GetFullPath($Capture)
$extension = [IO.Path]::GetExtension($Capture).ToLowerInvariant()
if ($extension -notin @('.pcap', '.pcapng')) { throw '-Capture must have a .pcap or .pcapng extension.' }
$tagHex = $Tag.Substring(2).ToLowerInvariant()
$pattern = "^PARITY_EVENT frame=(\d+) ts_ns=(\d+) dir=$Direction session=\d+ participant=\d+ tag=0x$tagHex "
$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ('opennova-capture-tag-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tempRoot | Out-Null
$deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
$iteration = 0
try {
    while ([DateTime]::UtcNow -lt $deadline) {
        $iteration++
        if (Test-Path -LiteralPath $Capture -PathType Leaf) {
            $snapshot = Join-Path $tempRoot ('snapshot-{0:d5}{1}' -f $iteration, $extension)
            $copied = $true
            try { Copy-CapturePrefixSnapshot $Capture $snapshot } catch { $copied = $false }
            if ($copied) {
                foreach ($line in (Invoke-WireSnapshot $Wire $snapshot $Items)) {
                    $match = [regex]::Match($line, $pattern)
                    if ($match.Success -and [int64]$match.Groups[2].Value -ge $AfterUnixNs) {
                        Write-Output ("PARITY_CAPTURE_TAG frame={0} ts_ns={1} dir={2} tag=0x{3}" -f
                            $match.Groups[1].Value, $match.Groups[2].Value, $Direction, $tagHex)
                        exit 0
                    }
                }
                Remove-Item -LiteralPath $snapshot -Force -ErrorAction SilentlyContinue
            }
        }
        Start-Sleep -Milliseconds $PollMilliseconds
    }
    Write-Output "PARITY_CAPTURE_TAG_TIMEOUT dir=$Direction tag=0x$tagHex after_ns=$AfterUnixNs"
    exit 2
} finally {
    Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
}
