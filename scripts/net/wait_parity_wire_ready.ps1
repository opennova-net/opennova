# Wait for one growing parity capture to prove that a joiner is exchanging
# decoded gameplay state with the expected host. The source capture is never
# decoded in place: every poll copies an immutable prefix snapshot first.

[CmdletBinding()]
param(
    [Alias('NwPpPath')]
    [string] $NwPp = '',
    [string] $Capture = '',
    [Alias('ItemsPath')]
    [string] $Items = '',
    [string] $Output = '',
    [Alias('ExpectedHostCallsign')]
    [string] $HostCallsign = '',
    [Alias('ExpectedJoinerCallsign')]
    [string] $JoinerCallsign = '',
    [Alias('ExpectedHostSessionName')]
    [string] $HostSessionName = '',
    [Alias('ExpectedMission')]
    [string] $Mission = '',
    [string] $MissionArtifact = '',
    [Alias('ExpectedMap')]
    [string] $Map = '',
    [Alias('ExpectedGameType')]
    [uint32] $GameType = 0,
    [Alias('ExpectedExpansion')]
    [string] $Expansion = '',
    [ValidateRange(1, 3600)]
    [int] $TimeoutSeconds = 240,
    [ValidateRange(100, 10000)]
    [int] $PollMilliseconds = 1000,
    [switch] $SelfTest
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. "$PSScriptRoot\lib.ps1"

$script:MinimumUplinks = 40
$script:MinimumFrames = 40
$script:MinimumStateSpanNs = [uint64]10000000000
$script:MinimumRttMatches = 20

function ConvertFrom-HexBytes {
    param([Parameter(Mandatory = $true)] [AllowEmptyString()] [string] $Hex)
    if ($Hex -eq '-') { return ,([byte[]]@()) }
    if (($Hex.Length % 2) -ne 0 -or $Hex -notmatch '^[0-9a-fA-F]*$') {
        throw 'Malformed parity-event body hex.'
    }
    $bytes = [byte[]]::new($Hex.Length / 2)
    for ($i = 0; $i -lt $bytes.Length; $i++) {
        $bytes[$i] = [Convert]::ToByte($Hex.Substring($i * 2, 2), 16)
    }
    return ,$bytes
}

function ConvertTo-HexBytes {
    param([byte[]] $Bytes)
    if ($null -eq $Bytes -or $Bytes.Length -eq 0) { return '' }
    return ([BitConverter]::ToString($Bytes)).Replace('-', '').ToLowerInvariant()
}

function Read-U16Le {
    param([byte[]] $Bytes, [int] $Offset)
    if ($Offset -lt 0 -or $Offset + 2 -gt $Bytes.Length) {
        throw 'u16 read exceeds parity-event body.'
    }
    return [uint16]([uint16]$Bytes[$Offset] + ([uint16]$Bytes[$Offset + 1] * 0x100))
}

function Read-U32Le {
    param([byte[]] $Bytes, [int] $Offset)
    if ($Offset -lt 0 -or $Offset + 4 -gt $Bytes.Length) {
        throw 'u32 read exceeds parity-event body.'
    }
    $value = [uint64]$Bytes[$Offset]
    $value += [uint64]$Bytes[$Offset + 1] * 0x100
    $value += [uint64]$Bytes[$Offset + 2] * 0x10000
    $value += [uint64]$Bytes[$Offset + 3] * 0x1000000
    return [uint32]$value
}

function Read-CStringAscii {
    param([byte[]] $Bytes, [ref] $Offset, [string] $Context)
    $start = [int]$Offset.Value
    $end = $start
    while ($end -lt $Bytes.Length -and $Bytes[$end] -ne 0) {
        if ($Bytes[$end] -gt 0x7f) {
            throw "$Context contains a non-ASCII string byte."
        }
        $end++
    }
    if ($end -ge $Bytes.Length) {
        throw "$Context contains an unterminated string."
    }
    $value = [Text.Encoding]::ASCII.GetString($Bytes, $start, $end - $start)
    $Offset.Value = $end + 1
    return $value
}

function ConvertTo-TagIdentity {
    param([int] $FullTag)
    return ('{0:x2}:{1}' -f ($FullTag -band 0xff),
        $(if (($FullTag -band 0x100) -ne 0) { 1 } else { 0 }))
}

function ConvertFrom-ParityLines {
    param([Parameter(Mandatory = $true)] [string[]] $Lines)
    $packets = [System.Collections.Generic.List[object]]::new()
    $events = [System.Collections.Generic.List[object]]::new()
    $states = [System.Collections.Generic.List[object]]::new()
    $entities = [System.Collections.Generic.List[object]]::new()
    $handshakes = [System.Collections.Generic.List[object]]::new()
    $datagrams = [System.Collections.Generic.List[object]]::new()

    foreach ($rawLine in $Lines) {
        $line = "$rawLine".TrimEnd([char]13, [char]10)
        if (-not $line.StartsWith('PARITY_', [StringComparison]::Ordinal)) { continue }

        if ($line -match '^PARITY_DATAGRAM frame=(\d+) ts_ns=(\d+) src=(\d+) dst=(\d+) len=(\d+) outer=([01]) opcode=(0x[0-9a-fA-F]{2}|--) class=(invalid|unknown|client_hello|client_auth|server_hello|server_auth|client_protocol|server_protocol) decode=([01])$') {
            $opcodeText = $Matches[7].ToLowerInvariant()
            $outerDecoded = $Matches[6] -eq '1'
            $datagramClass = $Matches[8]
            $decoded = $Matches[9] -eq '1'
            $opcode = if ($opcodeText -eq '--') {
                $null
            } else {
                [Convert]::ToByte($opcodeText.Substring(2), 16)
            }
            $expectedOpcode = @{
                client_hello = 0x41; client_auth = 0x42
                server_hello = 0x81; server_auth = 0x82
                client_protocol = 0x43; server_protocol = 0x83
            }
            if ((-not $outerDecoded -and
                    ($opcodeText -cne '--' -or $datagramClass -cne 'invalid' -or $decoded)) -or
                    ($outerDecoded -and $opcodeText -ceq '--') -or
                    ($datagramClass -in @('invalid', 'unknown') -and $decoded) -or
                    ($datagramClass -eq 'invalid' -and $outerDecoded) -or
                    ($datagramClass -ne 'invalid' -and -not $outerDecoded) -or
                    ($expectedOpcode.ContainsKey($datagramClass) -and
                        [byte]$opcode -ne [byte]$expectedOpcode[$datagramClass]) -or
                    ($datagramClass -eq 'unknown' -and
                        $expectedOpcode.Values -contains [byte]$opcode)) {
                throw 'PARITY_DATAGRAM classification disagrees with its outer-decode/opcode/decode fields.'
            }
            $datagrams.Add([pscustomobject]@{
                frame = [int]$Matches[1]; ts_ns = [uint64]$Matches[2]
                src_port = [int]$Matches[3]; dst_port = [int]$Matches[4]
                length = [uint64]$Matches[5]; outer_decoded = $outerDecoded
                opcode = $opcode; class = $datagramClass; decode = $decoded
            })
            continue
        }

        if ($line -match '^PARITY_HANDSHAKE frame=(\d+) ts_ns=(\d+) dir=([SC]) session=(\d+) opcode=0x(41|42|81|82) decode=([01]) len=(\d+) body=([0-9a-fA-F]*)$') {
            $opcode = [Convert]::ToByte($Matches[5], 16)
            $direction = $Matches[3]
            if (($opcode -in @(0x41, 0x42) -and $direction -cne 'C') -or
                    ($opcode -in @(0x81, 0x82) -and $direction -cne 'S')) {
                throw 'PARITY_HANDSHAKE direction disagrees with its opcode.'
            }
            [byte[]]$body = ConvertFrom-HexBytes $Matches[8]
            if ($body.Length -ne [int]$Matches[7]) {
                throw 'PARITY_HANDSHAKE body length does not match its declared length.'
            }
            $handshakes.Add([pscustomobject]@{
                frame = [int]$Matches[1]; ts_ns = [uint64]$Matches[2]
                dir = $direction; session = [int]$Matches[4]; opcode = $opcode
                decode = $Matches[6] -eq '1'; length = [int]$Matches[7]; body = $body
            })
            continue
        }

        if ($line -match '^PARITY_PACKET frame=(\d+) ts_ns=(\d+) dir=([SC]) session=(\d+) sid=0x([0-9a-fA-F]{8}) seq=(\d+) ack=(\d+) flags=0x([0-9a-fA-F]{2}) records=(\d+) tags=(\S+) wire=(\S+)$') {
            $packetFrame = [int]$Matches[1]
            $packetTs = [uint64]$Matches[2]
            $packetDirection = $Matches[3]
            $packetSession = [int]$Matches[4]
            $packetSid = [Convert]::ToUInt32($Matches[5], 16)
            $packetSequence = [uint32]$Matches[6]
            $packetAck = [uint32]$Matches[7]
            $packetFlags = [Convert]::ToUInt32($Matches[8], 16)
            $declaredRecords = [int]$Matches[9]
            $tagText = $Matches[10]
            $wireText = $Matches[11]
            $tagIdentities = [System.Collections.Generic.List[string]]::new()
            if ($tagText -ne '-') {
                foreach ($tagPart in $tagText.Split(',')) {
                    if ($tagPart -notmatch '^0x[0-9a-fA-F]{3}$') {
                        throw "Malformed PARITY_PACKET tag: $tagPart"
                    }
                    $fullTag = [Convert]::ToInt32($tagPart.Substring(2), 16)
                    $tagIdentities.Add((ConvertTo-TagIdentity $fullTag))
                }
            }
            if ($declaredRecords -ne $tagIdentities.Count) {
                throw "PARITY_PACKET frame $($Matches[1]) record count does not match its tag list."
            }
            $wireIdentities = [System.Collections.Generic.List[string]]::new()
            if ($wireText -ne '-') {
                foreach ($wirePart in $wireText.Split(',')) {
                    if ($wirePart -notmatch '^0x([0-9a-fA-F]{3}):0x([0-9a-fA-F]{2}):(\d+):([0-9a-fA-F]+|-)$') {
                        throw "Malformed PARITY_PACKET physical record: $wirePart"
                    }
                    $wireFullTag = [Convert]::ToUInt16($Matches[1], 16)
                    $wireFlags = [Convert]::ToByte($Matches[2], 16)
                    $wireLength = [uint32]$Matches[3]
                    [byte[]]$skipBytes = @()
                    if ($Matches[4] -ne '-') {
                        $skipBytes = ConvertFrom-HexBytes $Matches[4]
                    }
                    $expectedSkipLength = if (($wireFlags -band 0x08) -ne 0) {
                        1
                    } elseif (($wireFlags -band 0x10) -ne 0) {
                        2
                    } else { 0 }
                    if ($skipBytes.Length -ne $expectedSkipLength) {
                        throw 'PARITY_PACKET physical record SKIP bytes disagree with raw flags.'
                    }
                    if (($wireFlags -band 0x20) -ne 0) {
                        if ($wireLength -gt 0xff) {
                            throw 'PARITY_PACKET LEN8 record exceeds its encoded range.'
                        }
                    } elseif (($wireFlags -band 0x40) -ne 0) {
                        if ($wireLength -gt 0xffff) {
                            throw 'PARITY_PACKET LEN16 record exceeds its encoded range.'
                        }
                    } elseif ($wireLength -ne 0) {
                        throw 'PARITY_PACKET no-length record declares a nonzero encoded length.'
                    }
                    if ((($wireFlags -band 0x80) -ne 0) -ne ($wireFullTag -ge 0x100)) {
                        throw 'PARITY_PACKET physical record tag identity disagrees with raw settings bit.'
                    }
                    $wireIdentities.Add((ConvertTo-TagIdentity $wireFullTag))
                }
            }
            if ($declaredRecords -ne $wireIdentities.Count) {
                throw 'PARITY_PACKET record count does not match its physical record list.'
            }
            for ($recordIndex = 0; $recordIndex -lt $declaredRecords; $recordIndex++) {
                if ($wireIdentities[$recordIndex] -cne $tagIdentities[$recordIndex]) {
                    throw 'PARITY_PACKET tag list disagrees with its ordered physical record list.'
                }
            }
            $packets.Add([pscustomobject]@{
                frame = $packetFrame; ts_ns = $packetTs
                dir = $packetDirection; session = $packetSession
                sid = $packetSid; sequence = $packetSequence; ack = $packetAck
                flags = $packetFlags
                records = $declaredRecords; tag_identities = @($tagIdentities.ToArray())
            })
            continue
        }

        if ($line -match '^PARITY_EVENT frame=(\d+) ts_ns=(\d+) dir=([SC]) session=(\d+) participant=(?<participant>\d+) tag=0x([0-9a-fA-F]{2}) settings=([01]) len=(\d+) body=([0-9a-fA-F]*|-)$') {
            $direction = $Matches[3]
            $tag = $Matches[5].ToLowerInvariant()
            $settings = [int]$Matches[6]
            $declaredLength = [int]$Matches[7]
            $bodyText = $Matches[8]
            $ordinaryGameplay = $settings -eq 0 -and
                (($direction -eq 'C' -and $tag -eq '0c') -or
                 ($direction -eq 'S' -and $tag -eq '0a'))
            if ($ordinaryGameplay) {
                if ($bodyText -ne '-') {
                    throw "Gameplay PARITY_EVENT $direction 0x$tag unexpectedly materialized its body."
                }
                [byte[]]$body = @()
            } else {
                if ($bodyText -eq '-') {
                    throw "Material PARITY_EVENT $direction 0x$tag omitted its body."
                }
                [byte[]]$body = ConvertFrom-HexBytes $bodyText
                if ($body.Length -ne $declaredLength) {
                    throw "PARITY_EVENT $direction 0x$tag body length does not match its declaration."
                }
            }
            $events.Add([pscustomobject]@{
                frame = [int]$Matches[1]; ts_ns = [uint64]$Matches[2]
                dir = $direction; session = [int]$Matches[4]; tag = $tag
                participant = [int]$Matches['participant']
                settings = $settings; identity = ('{0}:{1}' -f $tag, $settings)
                length = $declaredLength; body = $body
            })
            continue
        }

        if ($line -match '^PARITY_STATE frame=(\d+) ts_ns=(\d+) dir=C session=(\d+) tag=0x0c kind=uplink decode=([01]) len=(\d+) handle=(\d+) type=(\d+) sub=(\d+) carrier=(\d+) pos=(-?\d+),(-?\d+),(-?\d+) orient=(-?\d+),(-?\d+) move=(\d+) state=(\d+) analog=(\d+),(\d+),(\d+) adm=(\d+) priority_nonzero=(\d+) priority=(\d+):(\d+),(\d+):(\d+),(\d+):(\d+),(\d+):(\d+)$') {
            $priorityOccupancy = 0
            foreach ($pair in @(@(22, 23), @(24, 25), @(26, 27), @(28, 29))) {
                if ([uint32]$Matches[$pair[0]] -ne 0 -or
                        [uint32]$Matches[$pair[1]] -ne 0) {
                    $priorityOccupancy++
                }
            }
            if ($priorityOccupancy -ne [uint32]$Matches[21]) {
                throw 'PARITY_STATE priority_nonzero does not match its four fixed slots.'
            }
            $states.Add([pscustomobject]@{
                frame = [int]$Matches[1]; ts_ns = [uint64]$Matches[2]
                dir = 'C'; session = [int]$Matches[3]; tag = '0c'
                decode = $Matches[4] -eq '1'; length = [int]$Matches[5]
                handle = [uint32]$Matches[6]; type_id = [uint32]$Matches[7]
                sub_op = [uint32]$Matches[8]
            })
            continue
        }

        if ($line -match '^PARITY_STATE frame=(\d+) ts_ns=(\d+) dir=S session=(\d+) tag=0x0a kind=frame decode=([01]) len=(\d+) sub=(\d+) flags=(\d+),(\d+) anchor=(-?\d+),(-?\d+),(-?\d+) local=([01]) local_state=(\d+),(-?\d+),(-?\d+) records=(\d+) players=(\d+) vehicles=(\d+) infantry=(\d+) none=(\d+) rounds=(\d+) carried=(\d+) weapon=([01]),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(-?\d+) timer=([01]),(\d+),(\d+),(\d+),(\d+),(-?\d+) env=([01]),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+) objective=([01]),(-?\d+),(-?\d+),(-?\d+),(-?\d+) mount=([01]),(\d+),([01]),(\d+),(\d+)$') {
            $passengerBound = [uint32]$Matches[53] -ne 0xffff
            if (($Matches[54] -eq '1') -ne $passengerBound) {
                throw 'PARITY_STATE phase-8 has_mount disagrees with its mount handle.'
            }
            $states.Add([pscustomobject]@{
                frame = [int]$Matches[1]; ts_ns = [uint64]$Matches[2]
                dir = 'S'; session = [int]$Matches[3]; tag = '0a'
                decode = $Matches[4] -eq '1'; length = [int]$Matches[5]
                local = $Matches[12] -eq '1'; records = [int]$Matches[16]
                players = [int]$Matches[17]
            })
            continue
        }

        if ($line -match '^PARITY_ENTITY frame=(\d+) ts_ns=(\d+) session=(\d+) class=([PVIN]) handle=(\d+) type=(\d+) pos=(\d+),(\d+),(\d+) orient=(-?\d+),(-?\d+) state=(\d+),(\d+) vital=(\d+)$') {
            $entities.Add([pscustomobject]@{
                frame = [int]$Matches[1]; ts_ns = [uint64]$Matches[2]
                session = [int]$Matches[3]; class = $Matches[4]
                handle = [uint32]$Matches[5]; type_id = [uint32]$Matches[6]
            })
            continue
        }
        throw "Malformed or unsupported nw_pp parity line: $line"
    }

    $packetIndex = @{}
    foreach ($packet in $packets) {
        $key = '{0}|{1}|{2}' -f $packet.frame, $packet.dir, $packet.session
        if ($packetIndex.ContainsKey($key)) { throw "More than one PARITY_PACKET owns $key." }
        $packetIndex[$key] = $packet
    }
    $eventCountByOwnerTag = @{}
    foreach ($event in $events) {
        $ownerKey = '{0}|{1}|{2}' -f $event.frame, $event.dir, $event.session
        if (-not $packetIndex.ContainsKey($ownerKey)) {
            throw "PARITY_EVENT has no owning packet: $ownerKey."
        }
        $owner = $packetIndex[$ownerKey]
        if ($owner.ts_ns -ne $event.ts_ns -or $owner.tag_identities -notcontains $event.identity) {
            throw "PARITY_EVENT does not match its owning packet: $ownerKey/$($event.identity)."
        }
        $countKey = '{0}|{1}' -f $ownerKey, $event.identity
        if (-not $eventCountByOwnerTag.ContainsKey($countKey)) { $eventCountByOwnerTag[$countKey] = 0 }
        $eventCountByOwnerTag[$countKey] = [int]$eventCountByOwnerTag[$countKey] + 1
    }
    foreach ($packet in $packets) {
        $ownerKey = '{0}|{1}|{2}' -f $packet.frame, $packet.dir, $packet.session
        foreach ($tagGroup in @($packet.tag_identities | Group-Object)) {
            $countKey = '{0}|{1}' -f $ownerKey, $tagGroup.Name
            $actual = if ($eventCountByOwnerTag.ContainsKey($countKey)) {
                [int]$eventCountByOwnerTag[$countKey]
            } else { 0 }
            # --parity-events deliberately omits records that nw_pp has no
            # material decoder for.  It must never invent more decoded events
            # than the packet declares, but an undecoded packet tag is valid.
            if ($actual -gt $tagGroup.Count) {
                throw "Owning packet $ownerKey has more PARITY_EVENT records than declared $($tagGroup.Name) tags."
            }
        }
    }

    $stateCountByOwner = @{}
    foreach ($state in $states) {
        $ownerKey = '{0}|{1}|{2}' -f $state.frame, $state.dir, $state.session
        if (-not $packetIndex.ContainsKey($ownerKey)) { throw "PARITY_STATE has no owning packet: $ownerKey." }
        $owner = $packetIndex[$ownerKey]
        $identity = '{0}:0' -f $state.tag
        if ($owner.ts_ns -ne $state.ts_ns -or $owner.tag_identities -notcontains $identity) {
            throw "PARITY_STATE does not match its owning packet: $ownerKey/$identity."
        }
        $stateKey = '{0}|{1}' -f $ownerKey, $identity
        if (-not $stateCountByOwner.ContainsKey($stateKey)) { $stateCountByOwner[$stateKey] = 0 }
        $stateCountByOwner[$stateKey] = [int]$stateCountByOwner[$stateKey] + 1
    }
    $gameplayStateKeys = [System.Collections.Generic.HashSet[string]]::new()
    foreach ($event in @($events | Where-Object {
        $_.settings -eq 0 -and
        (($_.dir -eq 'C' -and $_.tag -eq '0c') -or
         ($_.dir -eq 'S' -and $_.tag -eq '0a'))
    })) {
        $ownerKey = '{0}|{1}|{2}' -f $event.frame, $event.dir, $event.session
        $stateKey = '{0}|{1}:0' -f $ownerKey, $event.tag
        $null = $gameplayStateKeys.Add($stateKey)
    }
    foreach ($stateKey in $stateCountByOwner.Keys) {
        $null = $gameplayStateKeys.Add([string]$stateKey)
    }
    foreach ($stateKey in $gameplayStateKeys) {
        $expectedCount = if ($eventCountByOwnerTag.ContainsKey($stateKey)) {
            [int]$eventCountByOwnerTag[$stateKey]
        } else { 0 }
        $actualCount = if ($stateCountByOwner.ContainsKey($stateKey)) {
            [int]$stateCountByOwner[$stateKey]
        } else { 0 }
        if ($actualCount -ne $expectedCount) {
            throw "Gameplay owner $stateKey has $actualCount PARITY_STATE record(s) for $expectedCount ordinary PARITY_EVENT record(s)."
        }
    }
    foreach ($entity in $entities) {
        $ownerKey = '{0}|S|{1}' -f $entity.frame, $entity.session
        if (-not $packetIndex.ContainsKey($ownerKey)) { throw "PARITY_ENTITY has no owning packet: $ownerKey." }
        $owner = $packetIndex[$ownerKey]
        if ($owner.ts_ns -ne $entity.ts_ns -or $owner.tag_identities -notcontains '0a:0') {
            throw 'PARITY_ENTITY does not bind to an ordinary S2C 0x0A packet.'
        }
    }
    return [pscustomobject]@{
        packets = @($packets.ToArray()); events = @($events.ToArray())
        states = @($states.ToArray()); entities = @($entities.ToArray())
        handshakes = @($handshakes.ToArray())
        datagrams = @($datagrams.ToArray())
    }
}

function ConvertFrom-FullPlayerInfo {
    param($Event)
    [byte[]]$body = $Event.body
    $offset = 0
    $name = Read-CStringAscii $body ([ref]$offset) 'S2C 0x7B'
    $playerId = Read-CStringAscii $body ([ref]$offset) 'S2C 0x7B'
    $sessionName = Read-CStringAscii $body ([ref]$offset) 'S2C 0x7B'
    $missionName = Read-CStringAscii $body ([ref]$offset) 'S2C 0x7B'
    $mapName = Read-CStringAscii $body ([ref]$offset) 'S2C 0x7B'
    $wireGameType = Read-U32Le $body $offset
    $offset += 4
    $motd = Read-CStringAscii $body ([ref]$offset) 'S2C 0x7B'
    $wireExpansion = Read-CStringAscii $body ([ref]$offset) 'S2C 0x7B'
    if ($offset -ne $body.Length) { throw 'S2C 0x7B has trailing bytes.' }
    return [pscustomobject]@{
        event = $Event; name = $name; player_id = $playerId
        session_name = $sessionName; mission = $missionName; map = $mapName
        game_type = $wireGameType; motd = $motd; expansion = $wireExpansion
    }
}

function Get-PlayerSyncName {
    param($Event)
    [byte[]]$body = $Event.body
    if ($body.Length -lt 3) { throw 'S2C 0x46 is shorter than its slot/field-flags prefix.' }
    $fieldFlags = Read-U16Le $body 1
    if (($fieldFlags -band 0x0001) -eq 0) { return $null }
    if ($body.Length -lt 4) { throw 'Name-bearing S2C 0x46 omits its entity slot.' }
    $offset = 4
    return Read-CStringAscii $body ([ref]$offset) 'S2C 0x46 name field'
}

function Assert-OrdinalEqual {
    param([string] $Actual, [string] $Expected, [string] $Field)
    if (-not $Actual.Equals($Expected, [StringComparison]::Ordinal)) {
        throw "$Field mismatch: expected '$Expected', observed '$Actual'."
    }
}

function Get-TimestampSpanNs {
    param([object[]] $Values)
    if ($Values.Count -lt 2) { return [uint64]0 }
    $ordered = @($Values | Sort-Object { [uint64]$_.ts_ns })
    $first = [uint64]$ordered[0].ts_ns
    $last = [uint64]$ordered[$ordered.Count - 1].ts_ns
    if ($last -lt $first) { throw 'Parity timestamps moved backwards.' }
    return [uint64]($last - $first)
}

function Get-RttMatches {
    param([object[]] $Events, [uint64] $NotBefore)
    $ordered = @($Events | Where-Object {
        $_.settings -eq 0 -and $_.ts_ns -ge $NotBefore -and
        (($_.dir -eq 'C' -and $_.tag -eq '2c') -or
         ($_.dir -eq 'S' -and $_.tag -eq '57'))
    } | Sort-Object ts_ns, frame)
    $pending = @{}
    $matches = [System.Collections.Generic.List[object]]::new()
    foreach ($event in $ordered) {
        [byte[]]$body = $event.body
        if ($body.Length -ne 5) { throw "$($event.dir) 0x$($event.tag) RTT event is not exactly five bytes." }
        $token = ConvertTo-HexBytes ([byte[]]$body[0..3])
        if ($body[4] -gt 1) {
            throw "$($event.dir) 0x$($event.tag) RTT event has an invalid direction marker."
        }
        if ($event.dir -eq 'C') {
            # Retail also originates the opposite ping direction as S2C 0x57
            # marker=1, answered by C2S 0x2C marker=0.  This readiness gate
            # counts the client-originated direction and ignores its inverse.
            if ($body[4] -eq 0) { continue }
            if (-not $pending.ContainsKey($token)) {
                $pending[$token] = [System.Collections.Generic.Queue[object]]::new()
            }
            $pending[$token].Enqueue($event)
        } else {
            if ($body[4] -eq 1) { continue }
            if ($pending.ContainsKey($token) -and $pending[$token].Count -gt 0) {
                $request = $pending[$token].Dequeue()
                $matches.Add([pscustomobject]@{
                    token = $token; request_frame = $request.frame
                    request_ts_ns = $request.ts_ns; response_frame = $event.frame
                    response_ts_ns = $event.ts_ns
                })
            }
        }
    }
    return @($matches.ToArray())
}

function New-NotReady {
    param([string] $Reason)
    return [pscustomobject]@{ ready = $false; reason = $Reason; evidence = $null }
}

function Test-SessionWireReadiness {
    param($Facts, [int] $Session, $Expected)
    $events = @($Facts.events | Where-Object { $_.session -eq $Session })
    $packets = @($Facts.packets | Where-Object { $_.session -eq $Session })
    $states = @($Facts.states | Where-Object { $_.session -eq $Session })
    $entities = @($Facts.entities | Where-Object { $_.session -eq $Session })
    $clientSids = @($packets | Where-Object { $_.dir -eq 'C' } |
        ForEach-Object { [uint32]$_.sid } | Select-Object -Unique)
    $serverSids = @($packets | Where-Object { $_.dir -eq 'S' } |
        ForEach-Object { [uint32]$_.sid } | Select-Object -Unique)
    if ($clientSids.Count -gt 1 -or $serverSids.Count -gt 1) {
        throw "Session $Session contains more than one directional SID epoch."
    }
    if ($clientSids.Count -ne 1 -or $serverSids.Count -ne 1) {
        return New-NotReady "session $Session has not established both wire SIDs"
    }

    $playerInfos = [System.Collections.Generic.List[object]]::new()
    $advertisedMissions = [System.Collections.Generic.List[string]]::new()
    foreach ($event in @($events | Where-Object {
        $_.dir -eq 'S' -and $_.tag -eq '7b' -and $_.settings -eq 0
    })) {
        $playerInfos.Add((ConvertFrom-FullPlayerInfo $event))
    }
    if ($playerInfos.Count -eq 0) { return New-NotReady "session $Session has no recipient-scoped S2C 0x7B" }
    foreach ($info in $playerInfos) {
        Assert-OrdinalEqual $info.session_name $Expected.host_session_name 'S2C 0x7B session'
        if (-not (Test-ParityAdvertisedMissionName $info.mission `
                    $Expected.mission $Expected.mission_display_name)) {
            throw ("S2C 0x7B mission mismatch: expected exact map filename " +
                "'$($Expected.mission)' or hash-bound BMS title " +
                "'$($Expected.mission_display_name)', observed '$($info.mission)'.")
        }
        $advertisedMissions.Add([string] $info.mission)
        Assert-OrdinalEqual $info.map $Expected.map 'S2C 0x7B map'
        Assert-OrdinalEqual $info.expansion $Expected.expansion 'S2C 0x7B expansion'
        if ([uint32]$info.game_type -ne [uint32]$Expected.game_type) {
            throw "S2C 0x7B game type mismatch: expected $($Expected.game_type), observed $($info.game_type)."
        }
    }
    $distinctAdvertisedMissions = @($advertisedMissions.ToArray() | Select-Object -Unique)
    if ($distinctAdvertisedMissions.Count -ne 1) {
        throw "S2C 0x7B changed advertised mission identity inside one wire session."
    }
    $advertisedMission = [string] $distinctAdvertisedMissions[0]
    $joinerInfos = @($playerInfos | Where-Object {
        $_.name.Equals($Expected.joiner_callsign, [StringComparison]::Ordinal)
    })
    if ($joinerInfos.Count -eq 0) {
        return New-NotReady "session $Session S2C 0x7B has not identified exact joiner '$($Expected.joiner_callsign)'"
    }

    $sessionConfigs = @($events | Where-Object {
        $_.dir -eq 'S' -and $_.tag -eq '08' -and $_.settings -eq 0
    })
    if ($sessionConfigs.Count -eq 0) { return New-NotReady "session $Session has no S2C 0x08 session config" }
    foreach ($config in $sessionConfigs) {
        if ($config.body.Length -ne 51) { throw 'S2C 0x08 session config is not exactly 51 bytes.' }
        $actualGameType = Read-U32Le $config.body 12
        if ($actualGameType -ne [uint32]$Expected.game_type) {
            throw "S2C 0x08 game type mismatch: expected $($Expected.game_type), observed $actualGameType."
        }
    }

    $missionHeaders = @($events | Where-Object {
        $_.dir -eq 'S' -and $_.tag -eq '0b' -and $_.settings -eq 0
    } | Sort-Object ts_ns, frame)
    if ($missionHeaders.Count -eq 0) {
        return New-NotReady "session $Session has no S2C 0x0B BMS header"
    }
    if ($missionHeaders.Count -ne 1) {
        throw "Session $Session has $($missionHeaders.Count) S2C 0x0B BMS headers; expected exactly one."
    }
    [byte[]] $wireMissionHeader = $missionHeaders[0].body
    if ($wireMissionHeader.Length -ne 616 -or
            (Get-NetHarnessBytesSha256 $wireMissionHeader) -cne
                [string] $Expected.mission_header_sha256) {
        throw "S2C 0x0B is not the exact header of the hash-bound mission BMS."
    }

    $syncNames = [System.Collections.Generic.List[object]]::new()
    foreach ($event in @($events | Where-Object {
        $_.dir -eq 'S' -and $_.tag -eq '46' -and $_.settings -eq 0
    })) {
        $name = Get-PlayerSyncName $event
        if ($null -ne $name) { $syncNames.Add([pscustomobject]@{ name = $name; event = $event }) }
    }
    $hostSync = @($syncNames | Where-Object {
        $_.name.Equals($Expected.host_callsign, [StringComparison]::Ordinal)
    } | Sort-Object { $_.event.ts_ns } | Select-Object -First 1)
    $joinerSync = @($syncNames | Where-Object {
        $_.name.Equals($Expected.joiner_callsign, [StringComparison]::Ordinal)
    } | Sort-Object { $_.event.ts_ns } | Select-Object -First 1)
    if ($hostSync.Count -eq 0) {
        return New-NotReady "session $Session S2C 0x46 has not identified exact host '$($Expected.host_callsign)'"
    }
    if ($joinerSync.Count -eq 0) {
        return New-NotReady "session $Session S2C 0x46 has not identified exact joiner '$($Expected.joiner_callsign)'"
    }

    $missionStatus = @($events | Where-Object {
        $_.dir -eq 'C' -and $_.tag -eq '0b' -and $_.settings -eq 0
    } | Sort-Object ts_ns | Select-Object -First 1)
    if ($missionStatus.Count -eq 0) { return New-NotReady "session $Session has no C2S 0x0B mission-file status" }
    $worldLoad = @($events | Where-Object {
        $_.dir -eq 'S' -and $_.tag -eq '0f' -and $_.settings -eq 0
    } | Sort-Object ts_ns | Select-Object -First 1)
    if ($worldLoad.Count -eq 0) { return New-NotReady "session $Session has no S2C 0x0F world-state load" }

    [uint64]$notBefore = 0
    foreach ($milestone in @(
        [uint64]$hostSync[0].event.ts_ns
        [uint64]$joinerSync[0].event.ts_ns
        [uint64]$missionHeaders[0].ts_ns
        [uint64]$missionStatus[0].ts_ns
        [uint64]$worldLoad[0].ts_ns
    )) {
        if ([uint64]$milestone -gt $notBefore) { $notBefore = [uint64]$milestone }
    }

    $decodedUplinks = @($states | Where-Object {
        $_.dir -eq 'C' -and $_.tag -eq '0c' -and $_.decode -and
        $_.handle -ne 0 -and $_.ts_ns -ge $notBefore
    })
    if ($decodedUplinks.Count -eq 0) {
        return New-NotReady "session $Session has no nonzero decoded C2S 0x0C uplink after load"
    }
    $handleCandidates = [System.Collections.Generic.List[object]]::new()
    foreach ($group in @($decodedUplinks | Group-Object handle)) {
        $handle = [uint32]$group.Name
        $matchingEntities = @($entities | Where-Object {
            $_.class -eq 'P' -and $_.handle -eq $handle -and $_.ts_ns -ge $notBefore
        })
        if ($matchingEntities.Count -eq 0) { continue }
        $uplinkValues = @($group.Group | Sort-Object ts_ns, frame)
        $handleCandidates.Add([pscustomobject]@{
            handle = $handle; type_id = [uint32]$uplinkValues[0].type_id
            uplinks = $uplinkValues; entities = $matchingEntities
            count = $uplinkValues.Count; span_ns = Get-TimestampSpanNs $uplinkValues
        })
    }
    if ($handleCandidates.Count -eq 0) {
        return New-NotReady "session $Session has no decoded C2S 0x0C handle matching S2C PARITY_ENTITY class=P"
    }
    $player = @($handleCandidates | Where-Object {
        $_.count -ge $script:MinimumUplinks -and $_.span_ns -ge $script:MinimumStateSpanNs
    } | Sort-Object @{ Expression = 'count'; Descending = $true }, handle | Select-Object -First 1)
    if ($player.Count -eq 0) {
        $best = @($handleCandidates | Sort-Object @{ Expression = 'count'; Descending = $true } |
            Select-Object -First 1)[0]
        return New-NotReady ("session $Session player handle $($best.handle) has " +
            "$($best.count)/$($script:MinimumUplinks) decoded C2S 0x0C uplinks over " +
            "$([Math]::Round([double]$best.span_ns / 1000000000.0, 3))/$([double]$script:MinimumStateSpanNs / 1000000000.0)s")
    }
    $player = $player[0]

    $localFrames = @($states | Where-Object {
        $_.dir -eq 'S' -and $_.tag -eq '0a' -and $_.decode -and $_.local -and
        $_.ts_ns -ge $notBefore
    } | Sort-Object ts_ns, frame)
    $frameSpanNs = Get-TimestampSpanNs $localFrames
    if ($localFrames.Count -lt $script:MinimumFrames -or $frameSpanNs -lt $script:MinimumStateSpanNs) {
        return New-NotReady ("session $Session has $($localFrames.Count)/$($script:MinimumFrames) " +
            "decoded local S2C 0x0A frames over " +
            "$([Math]::Round([double]$frameSpanNs / 1000000000.0, 3))/$([double]$script:MinimumStateSpanNs / 1000000000.0)s")
    }
    $rttMatches = @(Get-RttMatches $events $notBefore)
    if ($rttMatches.Count -lt $script:MinimumRttMatches) {
        return New-NotReady "session $Session has $($rttMatches.Count)/$($script:MinimumRttMatches) matched C2S 0x2C/S2C 0x57 RTT tokens"
    }

    return [pscustomobject]@{
        ready = $true; reason = 'all decoded wire-readiness predicates passed'
        evidence = [ordered]@{
            session = $Session
            client_sid = ('0x{0:x8}' -f [uint32]$clientSids[0])
            server_sid = ('0x{0:x8}' -f [uint32]$serverSids[0])
            joiner_player_info_frame = [int]$joinerInfos[0].event.frame
            advertised_mission = $advertisedMission
            session_config_frame = [int]$sessionConfigs[0].frame
            mission_header_frame = [int]$missionHeaders[0].frame
            host_player_sync_frame = [int]$hostSync[0].event.frame
            joiner_player_sync_frame = [int]$joinerSync[0].event.frame
            mission_status_frame = [int]$missionStatus[0].frame
            world_load_frame = [int]$worldLoad[0].frame
            gameplay_not_before_ts_ns = $notBefore
            player_handle = [uint32]$player.handle
            player_type = [uint32]$player.type_id
            player_entity_first_frame = [int](@($player.entities | Sort-Object ts_ns, frame)[0].frame)
            c2s_0c_count = [int]$player.count
            c2s_0c_first_ts_ns = [uint64]$player.uplinks[0].ts_ns
            c2s_0c_last_ts_ns = [uint64]$player.uplinks[$player.uplinks.Count - 1].ts_ns
            c2s_0c_span_ns = [uint64]$player.span_ns
            s2c_0a_count = [int]$localFrames.Count
            s2c_0a_first_ts_ns = [uint64]$localFrames[0].ts_ns
            s2c_0a_last_ts_ns = [uint64]$localFrames[$localFrames.Count - 1].ts_ns
            s2c_0a_span_ns = [uint64]$frameSpanNs
            matched_rtt_count = [int]$rttMatches.Count
            first_rtt_token = $rttMatches[0].token
            last_rtt_token = $rttMatches[$rttMatches.Count - 1].token
        }
    }
}

function Test-WireReadiness {
    param($Facts, $Expected)
    $candidateSessions = [System.Collections.Generic.List[int]]::new()
    foreach ($event in @($Facts.events | Where-Object {
        $_.dir -eq 'S' -and $_.tag -eq '7b' -and $_.settings -eq 0
    })) {
        $info = ConvertFrom-FullPlayerInfo $event
        if ($info.name.Equals($Expected.joiner_callsign, [StringComparison]::Ordinal) -and
                -not $candidateSessions.Contains([int]$event.session)) {
            $candidateSessions.Add([int]$event.session)
        }
    }
    if ($candidateSessions.Count -eq 0) {
        return New-NotReady "no S2C 0x7B identifies exact joiner '$($Expected.joiner_callsign)'"
    }
    $ready = [System.Collections.Generic.List[object]]::new()
    $lastWait = $null
    foreach ($session in $candidateSessions) {
        $result = Test-SessionWireReadiness $Facts $session $Expected
        if ($result.ready) { $ready.Add($result) } else { $lastWait = $result }
    }
    if ($ready.Count -gt 1) { throw 'More than one wire session independently satisfies readiness.' }
    if ($ready.Count -eq 1) { return $ready[0] }
    return $lastWait
}

function ConvertTo-WindowsArgument {
    param([Parameter(Mandatory = $true)] [string] $Value)
    if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') { return $Value }
    $builder = [Text.StringBuilder]::new()
    $null = $builder.Append('"')
    $slashes = 0
    foreach ($character in $Value.ToCharArray()) {
        if ($character -eq '\') { $slashes++; continue }
        if ($character -eq '"') {
            $null = $builder.Append(('\' * (($slashes * 2) + 1)))
            $null = $builder.Append('"'); $slashes = 0; continue
        }
        if ($slashes -gt 0) { $null = $builder.Append(('\' * $slashes)); $slashes = 0 }
        $null = $builder.Append($character)
    }
    if ($slashes -gt 0) { $null = $builder.Append(('\' * ($slashes * 2))) }
    $null = $builder.Append('"')
    return $builder.ToString()
}

function Invoke-NwPpSnapshot {
    param([string] $Executable, [string] $Snapshot, [string] $ItemsPath)
    $arguments = @($Snapshot, '--items', $ItemsPath, '--parity-events') |
        ForEach-Object { ConvertTo-WindowsArgument "$_" }
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $Executable
    $start.Arguments = $arguments -join ' '
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    try {
        if (-not $process.Start()) { throw 'Could not start nw_pp.' }
        $stdoutRead = $process.StandardOutput.ReadToEndAsync()
        $stderrRead = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(60000)) {
            $process.Kill(); $null = $process.WaitForExit(5000)
            throw 'nw_pp exceeded the 60-second snapshot decode limit.'
        }
        $stdout = $stdoutRead.Result
        $stderr = $stderrRead.Result
        if ($process.ExitCode -ne 0) { throw "nw_pp exited $($process.ExitCode): $($stderr.Trim())" }
        $itemMatch = [regex]::Match($stderr, '(?m)^loaded (\d+) item names from .+$')
        if (-not $itemMatch.Success -or [int]$itemMatch.Groups[1].Value -le 0) {
            throw "nw_pp did not load a nonempty --items classifier: $($stderr.Trim())"
        }
        return [pscustomobject]@{
            lines = @([regex]::Split($stdout, '\r?\n') | Where-Object { $_.Length -gt 0 })
            item_count = [int]$itemMatch.Groups[1].Value
        }
    } finally {
        $process.Dispose()
    }
}

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
    return $prefixLength
}

function Write-CreateNewJson {
    param([string] $Path, $Value)
    $json = $Value | ConvertTo-Json -Depth 12
    $encoding = [Text.UTF8Encoding]::new($false)
    $bytes = $encoding.GetBytes($json + [Environment]::NewLine)
    $stream = [IO.File]::Open($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try {
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
    } finally {
        $stream.Dispose()
    }
}

function Add-SyntheticEvent {
    param($Builder, [char] $Direction, [string] $Tag, [uint64] $Timestamp,
        [string] $Body, [int] $Length)
    $Builder.frame = [int]$Builder.frame + 1
    $frame = [int]$Builder.frame
    $sid = if ($Direction -eq 'C') { '11111111' } else { '22222222' }
    $wireFlags = if ($Length -le 0xff) { '20' } else { '40' }
    $Builder.lines.Add(
        ('PARITY_PACKET frame={0} ts_ns={1} dir={2} session=32776 sid=0x{3} seq={0} ack=0 flags=0x00 records=1 tags=0x0{4} wire=0x0{4}:0x{5}:{6}:-' -f
            $frame, $Timestamp, $Direction, $sid, $Tag, $wireFlags, $Length))
    $Builder.lines.Add(
        ('PARITY_EVENT frame={0} ts_ns={1} dir={2} session=32776 participant=1 tag=0x{3} settings=0 len={4} body={5}' -f
            $frame, $Timestamp, $Direction, $Tag, $Length, $Body))
    return $frame
}

function New-SyntheticFullPlayerInfoBody {
    param($Expected, [string] $AdvertisedMission = '')
    if ([string]::IsNullOrEmpty($AdvertisedMission)) {
        $AdvertisedMission = [string] $Expected.mission
    }
    $body = [System.Collections.Generic.List[byte]]::new()
    foreach ($value in @($Expected.joiner_callsign, '', $Expected.host_session_name,
        $AdvertisedMission, $Expected.map)) {
        $body.AddRange([Text.Encoding]::ASCII.GetBytes($value)); $body.Add(0)
    }
    $body.AddRange([BitConverter]::GetBytes([uint32]$Expected.game_type))
    foreach ($value in @('', $Expected.expansion)) {
        $body.AddRange([Text.Encoding]::ASCII.GetBytes($value)); $body.Add(0)
    }
    return $body.ToArray()
}

function New-SyntheticPlayerSyncBody {
    param([string] $Name, [byte] $Slot)
    $body = [System.Collections.Generic.List[byte]]::new()
    $body.Add($Slot); $body.Add(1); $body.Add(0); $body.Add($Slot)
    $body.AddRange([Text.Encoding]::ASCII.GetBytes($Name)); $body.Add(0)
    return $body.ToArray()
}

function New-SyntheticParityLines {
    param($Expected, [int] $UplinkCount = 40, [int] $FrameCount = 40,
        [int] $RttCount = 20, [uint32] $EntityHandle = 136,
        [string] $AdvertisedMission = '', [switch] $CorruptMissionHeader)
    $builder = [pscustomobject]@{
        frame = 0; lines = [System.Collections.Generic.List[string]]::new()
    }
    [byte[]]$info = New-SyntheticFullPlayerInfoBody $Expected $AdvertisedMission
    $null = Add-SyntheticEvent $builder 'S' '7b' 1000000000 (ConvertTo-HexBytes $info) $info.Length
    $config = [byte[]]::new(51)
    [BitConverter]::GetBytes([uint32]$Expected.game_type).CopyTo($config, 12)
    $null = Add-SyntheticEvent $builder 'S' '08' 1100000000 (ConvertTo-HexBytes $config) $config.Length
    [byte[]]$missionHeader = [byte[]]$Expected.mission_header.Clone()
    if ($CorruptMissionHeader) {
        $missionHeader[578] = $missionHeader[578] -bxor 0x01
    }
    $null = Add-SyntheticEvent $builder 'S' '0b' 1150000000 `
        (ConvertTo-HexBytes $missionHeader) $missionHeader.Length
    [byte[]]$hostSync = New-SyntheticPlayerSyncBody $Expected.host_callsign 135
    $null = Add-SyntheticEvent $builder 'S' '46' 1200000000 (ConvertTo-HexBytes $hostSync) $hostSync.Length
    [byte[]]$joinerSync = New-SyntheticPlayerSyncBody $Expected.joiner_callsign 136
    $null = Add-SyntheticEvent $builder 'S' '46' 1300000000 (ConvertTo-HexBytes $joinerSync) $joinerSync.Length
    $null = Add-SyntheticEvent $builder 'C' '0b' 1400000000 '00' 1
    $null = Add-SyntheticEvent $builder 'S' '0f' 1500000000 '00' 1
    # Retail can run the RTT exchange in both directions.  The readiness
    # counter intentionally measures C2S-request/S2C-response pairs while
    # accepting and ignoring the inverse pair.
    $null = Add-SyntheticEvent $builder 'S' '57' 1600000000 'efbeadde01' 5
    $null = Add-SyntheticEvent $builder 'C' '2c' 1610000000 'efbeadde00' 5
    # nw_pp reports every outer tag in PARITY_PACKET but only materializes
    # PARITY_EVENT lines for tags with supported event decoders.
    $builder.frame = [int]$builder.frame + 1
    $builder.lines.Add(
        ('PARITY_PACKET frame={0} ts_ns=1700000000 dir=S session=32776 sid=0x22222222 seq={0} ack=0 flags=0x00 records=1 tags=0x00f wire=0x00f:0x00:0:-' -f
            [int]$builder.frame))
    $iterations = [Math]::Max($UplinkCount, $FrameCount)
    for ($i = 0; $i -lt $iterations; $i++) {
        [uint64]$timestamp = [uint64]3000000000 + ([uint64]$i * [uint64]300000000)
        if ($i -lt $UplinkCount) {
            $frame = Add-SyntheticEvent $builder 'C' '0c' $timestamp '-' 48
            $builder.lines.Add(
                ('PARITY_STATE frame={0} ts_ns={1} dir=C session=32776 tag=0x0c kind=uplink decode=1 len=48 handle=136 type=5305 sub=10 carrier=65535 pos=1,2,3 orient=4,5 move=0 state=1 analog=0,0,0 adm=16 priority_nonzero=4 priority=60000:9,65535:1,22:7,60001:1' -f
                    $frame, $timestamp))
        }
        if ($i -lt $RttCount) {
            $tokenBytes = [BitConverter]::GetBytes([uint32]($i + 1))
            $requestBody = ConvertTo-HexBytes ([byte[]]($tokenBytes + [byte]1))
            $null = Add-SyntheticEvent $builder 'C' '2c' ($timestamp + 1000000) $requestBody 5
        }
        if ($i -lt $FrameCount) {
            $frame = Add-SyntheticEvent $builder 'S' '0a' ($timestamp + 2000000) '-' 600
            $builder.lines.Add(
                ('PARITY_STATE frame={0} ts_ns={1} dir=S session=32776 tag=0x0a kind=frame decode=1 len=600 sub=0 flags=2,0 anchor=1,2,3 local=1 local_state=0,100,0 records=1 players=1 vehicles=0 infantry=0 none=0 rounds=0 carried=65535 weapon=0,0,0,0,0,0,0,0,0 timer=0,0,0,0,0,0 env=0,0,0,0,0,0,0,0,0 objective=0,0,0,0,0 mount=0,65535,0,0,0' -f
                    $frame, ($timestamp + 2000000)))
            $builder.lines.Add(
                ('PARITY_ENTITY frame={0} ts_ns={1} session=32776 class=P handle={2} type=5305 pos=1,2,3 orient=4,5 state=0,0 vital=100' -f
                    $frame, ($timestamp + 2000000), $EntityHandle))
        }
        if ($i -lt $RttCount) {
            $tokenBytes = [BitConverter]::GetBytes([uint32]($i + 1))
            $responseBody = ConvertTo-HexBytes ([byte[]]($tokenBytes + [byte]0))
            $null = Add-SyntheticEvent $builder 'S' '57' ($timestamp + 3000000) $responseBody 5
        }
    }
    return @($builder.lines.ToArray())
}

function Invoke-WireReadinessSelfTest {
    # Match nw_pp's current event schema, including the participant column.
    $participantFacts = ConvertFrom-ParityLines @(
        'PARITY_PACKET frame=5 ts_ns=1789006567605281900 dir=S session=32776 sid=0x22222222 seq=5 ack=0 flags=0x00 records=1 tags=0x100 wire=0x100:0xa0:9:-'
        'PARITY_EVENT frame=5 ts_ns=1789006567605281900 dir=S session=32776 participant=1 tag=0x00 settings=1 len=9 body=000020000014050000'
    )
    if ($participantFacts.events.Count -ne 1 -or
            $participantFacts.events[0].participant -ne 1 -or
            $participantFacts.events[0].body.Length -ne 9 -or
            $participantFacts.events[0].settings -ne 1) {
        throw 'Current participant event schema did not preserve its fields.'
    }
    foreach ($malformedEvent in @(
        'PARITY_EVENT frame=5 ts_ns=1 dir=S session=32776 participant=bad tag=0x00 settings=1 len=1 body=00',
        'PARITY_EVENT frame=5 ts_ns=1 dir=S session=32776 participant=1 tag=0x00 settings=1 len=2 body=00'
    )) {
        $rejected = $false
        try { $null = ConvertFrom-ParityLines @($malformedEvent) }
        catch {
            $rejected = $_.Exception.Message -match
                'Malformed or unsupported nw_pp parity line|body length does not match'
        }
        if (-not $rejected) { throw 'Malformed participant event was accepted.' }
    }
    $missionHeader = [byte[]]::new(616)
    $missionHeader[0] = [byte][char]'B'; $missionHeader[1] = [byte][char]'M'
    $missionHeader[2] = [byte][char]'S'; $missionHeader[3] = 19
    [Text.Encoding]::ASCII.GetBytes('Operation: White Noise').CopyTo($missionHeader, 4)
    $expected = [pscustomobject]@{
        host_callsign = 'ExpectedHost'; joiner_callsign = 'ExpectedJoin'
        host_session_name = 'Expected Session '; mission = 'CP04.bms'
        mission_display_name = 'Operation: White Noise'
        mission_header = $missionHeader
        mission_header_sha256 = Get-NetHarnessBytesSha256 $missionHeader
        map = 'CP04.bms'; game_type = [uint32]196640; expansion = 'revx02'
    }
    [byte[]]$longHandshakeBody = [byte[]]::new(343)
    for ($i = 0; $i -lt $longHandshakeBody.Length; $i++) {
        $longHandshakeBody[$i] = [byte]($i % 251)
    }
    $longHandshakeHex = ConvertTo-HexBytes $longHandshakeBody
    $longHandshakeLine =
        "PARITY_HANDSHAKE frame=1 ts_ns=1785792889994295500 dir=C session=32776 opcode=0x41 decode=1 len=343 body=$longHandshakeHex"
    $handshakeFacts = ConvertFrom-ParityLines @($longHandshakeLine)
    if ($handshakeFacts.handshakes.Count -ne 1 -or
            $handshakeFacts.handshakes[0].body.Length -ne 343 -or
            $handshakeFacts.handshakes[0].opcode -ne 0x41) {
        throw 'Long outer-handshake parser self-test did not preserve the captured record.'
    }
    foreach ($malformedHandshake in @(
        $longHandshakeLine.Replace('dir=C', 'dir=S'),
        $longHandshakeLine.Replace('len=343', 'len=342')
    )) {
        $malformedHandshakeRejected = $false
        try { $null = ConvertFrom-ParityLines @($malformedHandshake) }
        catch { $malformedHandshakeRejected = $true }
        if (-not $malformedHandshakeRejected) {
            throw 'Malformed outer-handshake parser self-test did not fail closed.'
        }
    }
    $datagramFacts = ConvertFrom-ParityLines @(
        'PARITY_DATAGRAM frame=1 ts_ns=1785792889994295500 src=32776 dst=32786 len=350 outer=1 opcode=0x41 class=client_hello decode=1'
    )
    if ($datagramFacts.datagrams.Count -ne 1 -or
            $datagramFacts.datagrams[0].class -cne 'client_hello') {
        throw 'Outer-datagram parser self-test did not preserve the captured record.'
    }
    $badDatagramRejected = $false
    try {
        $null = ConvertFrom-ParityLines @(
            'PARITY_DATAGRAM frame=1 ts_ns=1785792889994295500 src=32776 dst=32786 len=350 outer=1 opcode=0x41 class=server_hello decode=1'
        )
    } catch { $badDatagramRejected = $true }
    if (-not $badDatagramRejected) {
        throw 'Contradictory outer-datagram parser self-test did not fail closed.'
    }
    foreach ($malformedCurrentRecord in @(
        'PARITY_PACKET frame=1 ts_ns=1 dir=S session=32776 sid=0x22222222 seq=1 ack=0 flags=0x00 records=1 tags=0x00a wire=0x00b:0x20:3:-',
        'PARITY_STATE frame=1 ts_ns=1 dir=C session=32776 tag=0x0c kind=uplink decode=1 len=48 handle=136 type=5305 sub=10 carrier=65535 pos=1,2,3 orient=4,5 move=0 state=1 analog=0,0,0 adm=16 priority_nonzero=1 priority=0:0,0:0,0:0,0:0',
        'PARITY_STATE frame=1 ts_ns=1 dir=S session=32776 tag=0x0a kind=frame decode=1 len=600 sub=0 flags=2,0 anchor=1,2,3 local=1 local_state=0,100,0 records=1 players=1 vehicles=0 infantry=0 none=0 rounds=0 carried=65535 weapon=0,0,0,0,0,0,0,0,0 timer=0,0,0,0,0,0 env=0,0,0,0,0,0,0,0,0 objective=0,0,0,0,0 mount=1,65535,1,0,0'
    )) {
        $malformedCurrentRecordRejected = $false
        try { $null = ConvertFrom-ParityLines @($malformedCurrentRecord) }
        catch { $malformedCurrentRecordRejected = $true }
        if (-not $malformedCurrentRecordRejected) {
            throw 'Current-schema invariant self-test did not fail closed.'
        }
    }
    $positiveFacts = ConvertFrom-ParityLines (New-SyntheticParityLines $expected)
    $exactSpan = Get-TimestampSpanNs @(
        [pscustomobject]@{ ts_ns = [uint64]1785735565163187000 }
        [pscustomobject]@{ ts_ns = [uint64]1785735575163187000 }
    )
    if ($exactSpan -ne [uint64]10000000000) {
        throw 'UInt64 nanosecond span self-test lost timestamp precision.'
    }
    $positive = Test-WireReadiness $positiveFacts $expected
    if (-not $positive.ready -or $positive.evidence.player_handle -ne 136 -or
            $positive.evidence.matched_rtt_count -ne 20 -or
            $positive.evidence.advertised_mission -cne $expected.mission) {
        throw 'Positive wire-readiness self-test did not produce the expected witness.'
    }
    $titlePositive = Test-WireReadiness (
        ConvertFrom-ParityLines (New-SyntheticParityLines $expected `
            -AdvertisedMission $expected.mission_display_name)) $expected
    if (-not $titlePositive.ready -or
            $titlePositive.evidence.advertised_mission -cne
                $expected.mission_display_name) {
        throw 'Hash-bound BMS-title wire-readiness self-test did not pass.'
    }
    $arbitraryMissionRejected = $false
    try {
        $null = Test-WireReadiness (
            ConvertFrom-ParityLines (New-SyntheticParityLines $expected `
                -AdvertisedMission 'Unbound Mission Title')) $expected
    } catch {
        $arbitraryMissionRejected = $_.Exception.Message -match
            'S2C 0x7B mission mismatch'
    }
    if (-not $arbitraryMissionRejected) {
        throw 'Wire-readiness self-test accepted an unbound advertised mission.'
    }
    $wrongMissionHeaderRejected = $false
    try {
        $null = Test-WireReadiness (
            ConvertFrom-ParityLines (New-SyntheticParityLines $expected `
                -CorruptMissionHeader)) $expected
    } catch {
        $wrongMissionHeaderRejected = $_.Exception.Message -match
            'exact header of the hash-bound mission BMS'
    }
    if (-not $wrongMissionHeaderRejected) {
        throw 'Wire-readiness self-test accepted a mutated S2C 0x0B BMS header.'
    }
    $shortUplink = Test-WireReadiness (
        ConvertFrom-ParityLines (New-SyntheticParityLines $expected -UplinkCount 39)) $expected
    if ($shortUplink.ready -or $shortUplink.reason -notmatch '39/40') {
        throw 'Short-uplink fail-closed self-test did not reject 39 C2S 0x0C states.'
    }
    $shortRtt = Test-WireReadiness (
        ConvertFrom-ParityLines (New-SyntheticParityLines $expected -RttCount 19)) $expected
    if ($shortRtt.ready -or $shortRtt.reason -notmatch '19/20') {
        throw 'RTT fail-closed self-test did not reject 19 matched tokens.'
    }
    $wrongEntity = Test-WireReadiness (
        ConvertFrom-ParityLines (New-SyntheticParityLines $expected -EntityHandle 137)) $expected
    if ($wrongEntity.ready -or $wrongEntity.reason -notmatch 'matching S2C PARITY_ENTITY') {
        throw 'Entity-correlation fail-closed self-test accepted the wrong player handle.'
    }
    $wrongExpected = [pscustomobject]@{
        host_callsign = $expected.host_callsign; joiner_callsign = $expected.joiner_callsign
        host_session_name = 'Wrong Session'; mission = $expected.mission; map = $expected.map
        mission_display_name = $expected.mission_display_name
        game_type = $expected.game_type; expansion = $expected.expansion
    }
    $rejectedIdentity = $false
    try { $null = Test-WireReadiness $positiveFacts $wrongExpected }
    catch { $rejectedIdentity = $_.Exception.Message -match 'S2C 0x7B session mismatch' }
    if (-not $rejectedIdentity) {
        throw 'Exact-identity fail-closed self-test accepted a wrong session name.'
    }
    $duplicateOrdinary = @(
        'PARITY_PACKET frame=900 ts_ns=17000000000 dir=S session=32776 sid=0x22222222 seq=900 ack=0 flags=0x00 records=2 tags=0x00a,0x00a wire=0x00a:0x40:600:-,0x00a:0x40:600:-'
        'PARITY_EVENT frame=900 ts_ns=17000000000 dir=S session=32776 participant=1 tag=0x0a settings=0 len=600 body=-'
        'PARITY_EVENT frame=900 ts_ns=17000000000 dir=S session=32776 participant=1 tag=0x0a settings=0 len=600 body=-'
        'PARITY_STATE frame=900 ts_ns=17000000000 dir=S session=32776 tag=0x0a kind=frame decode=1 len=600 sub=0 flags=2,0 anchor=1,2,3 local=1 local_state=0,100,0 records=1 players=1 vehicles=0 infantry=0 none=0 rounds=0 carried=65535 weapon=0,0,0,0,0,0,0,0,0 timer=0,0,0,0,0,0 env=0,0,0,0,0,0,0,0,0 objective=0,0,0,0,0 mount=0,65535,0,0,0'
        'PARITY_STATE frame=900 ts_ns=17000000000 dir=S session=32776 tag=0x0a kind=frame decode=1 len=600 sub=0 flags=2,0 anchor=1,2,3 local=1 local_state=0,100,0 records=1 players=1 vehicles=0 infantry=0 none=0 rounds=0 carried=65535 weapon=0,0,0,0,0,0,0,0,0 timer=0,0,0,0,0,0 env=0,0,0,0,0,0,0,0,0 objective=0,0,0,0,0 mount=0,65535,0,0,0'
    )
    $null = ConvertFrom-ParityLines $duplicateOrdinary
    $missingDuplicateStateRejected = $false
    try {
        $null = ConvertFrom-ParityLines @($duplicateOrdinary | Select-Object -SkipLast 1)
    } catch {
        $missingDuplicateStateRejected =
            $_.Exception.Message -match 'has 1 PARITY_STATE record\(s\) for 2 ordinary PARITY_EVENT record\(s\)'
    }
    if (-not $missingDuplicateStateRejected) {
        throw 'Duplicate-record fail-closed self-test accepted two gameplay events with one state.'
    }
    Write-Output 'PARITY_WIRE_READY_SELF_TEST=PASS'
}

function Assert-ExpectedAscii {
    param([string] $Value, [string] $Name)
    if ([string]::IsNullOrEmpty($Value)) { throw "-$Name must be nonempty." }
    foreach ($character in $Value.ToCharArray()) {
        $code = [int][char]$character
        if ($code -lt 0x20 -or $code -gt 0x7e) {
            throw "-$Name must contain printable ASCII only."
        }
    }
}

if ($SelfTest) {
    Invoke-WireReadinessSelfTest
    exit 0
}

foreach ($requiredParameter in @(
    'Capture', 'Items', 'Output', 'HostCallsign', 'JoinerCallsign',
    'HostSessionName', 'Mission', 'MissionArtifact', 'Map', 'Expansion', 'GameType'
)) {
    if (-not $PSBoundParameters.ContainsKey($requiredParameter)) {
        throw "-$requiredParameter is required unless -SelfTest is used."
    }
}
foreach ($entry in @(
    @{ Value = $HostCallsign; Name = 'HostCallsign' }
    @{ Value = $JoinerCallsign; Name = 'JoinerCallsign' }
    @{ Value = $HostSessionName; Name = 'HostSessionName' }
    @{ Value = $Mission; Name = 'Mission' }
    @{ Value = $Map; Name = 'Map' }
    @{ Value = $Expansion; Name = 'Expansion' }
)) {
    Assert-ExpectedAscii $entry.Value $entry.Name
}
if ([string]::IsNullOrWhiteSpace($NwPp)) {
    $NwPp = Join-Path $PSScriptRoot '..\..\build\apps\nw_pp\Release\nw_pp.exe'
}
$NwPp = (Resolve-Path -LiteralPath $NwPp -ErrorAction Stop).Path
$Items = (Resolve-Path -LiteralPath $Items -ErrorAction Stop).Path
if ((Get-Item -LiteralPath $NwPp).PSIsContainer -or (Get-Item -LiteralPath $Items).PSIsContainer) {
    throw '-NwPp and -Items must identify files.'
}
$MissionArtifact = (Resolve-Path -LiteralPath $MissionArtifact -ErrorAction Stop).Path
if ((Get-Item -LiteralPath $MissionArtifact).PSIsContainer -or
        [IO.Path]::GetFileName($MissionArtifact) -cne $Mission) {
    throw '-MissionArtifact must identify the exact case mission BMS.'
}
$missionIdentity = Get-BmsHeaderIdentity -Path $MissionArtifact
$MissionDisplayName = [string] $missionIdentity.MissionDisplayName
$Capture = [IO.Path]::GetFullPath($Capture)
$extension = [IO.Path]::GetExtension($Capture).ToLowerInvariant()
if ($extension -notin @('.pcap', '.pcapng')) { throw '-Capture must have a .pcap or .pcapng extension.' }
$Output = [IO.Path]::GetFullPath($Output)
if (Test-Path -LiteralPath $Output) {
    throw "Wire-readiness witness already exists; output is create-new: $Output"
}
$outputParent = Split-Path -Parent $Output
if (-not (Test-Path -LiteralPath $outputParent -PathType Container)) {
    New-Item -ItemType Directory -Path $outputParent | Out-Null
}
$expected = [pscustomobject]@{
    host_callsign = $HostCallsign; joiner_callsign = $JoinerCallsign
    host_session_name = $HostSessionName; mission = $Mission
    mission_display_name = $MissionDisplayName; map = $Map
    mission_header = [byte[]] $missionIdentity.Bytes
    mission_header_sha256 = [string] $missionIdentity.Sha256
    game_type = [uint32]$GameType; expansion = $Expansion
}

$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
$tempRoot = Join-Path $tempBase ('opennova-wire-ready-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tempRoot | Out-Null
$deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
$iteration = 0
$lastReason = ''
try {
    while ([DateTime]::UtcNow -lt $deadline) {
        $iteration++
        if (-not (Test-Path -LiteralPath $Capture -PathType Leaf)) {
            $reason = "capture does not exist yet: $Capture"
            if ($reason -ne $lastReason) {
                Write-Host "PARITY_WIRE_READY_WAIT=$reason"; $lastReason = $reason
            }
            Start-Sleep -Milliseconds $PollMilliseconds
            continue
        }
        $snapshot = Join-Path $tempRoot ('snapshot-{0:d5}{1}' -f $iteration, $extension)
        try { $prefixBytes = Copy-CapturePrefixSnapshot $Capture $snapshot }
        catch {
            $reason = "capture snapshot unavailable: $($_.Exception.Message)"
            if ($reason -ne $lastReason) {
                Write-Host "PARITY_WIRE_READY_WAIT=$reason"; $lastReason = $reason
            }
            Start-Sleep -Milliseconds $PollMilliseconds
            continue
        }
        try { $decode = Invoke-NwPpSnapshot $NwPp $snapshot $Items }
        catch {
            if ($_.Exception.Message -match '^nw_pp exited ') {
                $reason = "snapshot decode incomplete: $($_.Exception.Message)"
                if ($reason -ne $lastReason) {
                    Write-Host "PARITY_WIRE_READY_WAIT=$reason"; $lastReason = $reason
                }
                Start-Sleep -Milliseconds $PollMilliseconds
                continue
            }
            throw
        }
        $facts = ConvertFrom-ParityLines $decode.lines
        $readiness = Test-WireReadiness $facts $expected
        if (-not $readiness.ready) {
            if ($readiness.reason -ne $lastReason) {
                Write-Host "PARITY_WIRE_READY_WAIT=$($readiness.reason)"
                $lastReason = $readiness.reason
            }
            Start-Sleep -Milliseconds $PollMilliseconds
            continue
        }
        $snapshotHash = (Get-FileHash -LiteralPath $snapshot -Algorithm SHA256).Hash.ToLowerInvariant()
        $captureItem = Get-Item -LiteralPath $Capture
        $witness = [ordered]@{
            schema = 'opennova.parity-wire-readiness.v2'
            ready = $true
            created_utc = [DateTime]::UtcNow.ToString('o')
            witness_path = $Output
            expected = [ordered]@{
                host_callsign = $HostCallsign; joiner_callsign = $JoinerCallsign
                host_session_name = $HostSessionName; mission = $Mission
                mission_display_name = $MissionDisplayName; map = $Map
                mission_header_sha256 = [string] $missionIdentity.Sha256
                game_type = [uint32]$GameType; expansion = $Expansion
            }
            thresholds = [ordered]@{
                decoded_c2s_0c = $script:MinimumUplinks
                decoded_local_s2c_0a = $script:MinimumFrames
                state_span_ns = $script:MinimumStateSpanNs
                matched_c2s_0x2c_s2c_0x57 = $script:MinimumRttMatches
            }
            source = [ordered]@{
                capture = $Capture; capture_prefix_bytes = [int64]$prefixBytes
                capture_prefix_sha256 = $snapshotHash
                capture_last_write_utc = $captureItem.LastWriteTimeUtc.ToString('o')
                nw_pp = $NwPp
                nw_pp_sha256 = (Get-FileHash -LiteralPath $NwPp -Algorithm SHA256).Hash.ToLowerInvariant()
                items = $Items
                items_sha256 = (Get-FileHash -LiteralPath $Items -Algorithm SHA256).Hash.ToLowerInvariant()
                loaded_item_count = [int]$decode.item_count
                parity_packet_count = [int]$facts.packets.Count
                parity_event_count = [int]$facts.events.Count
            }
            evidence = $readiness.evidence
        }
        Write-CreateNewJson $Output $witness
        Write-Output ('PARITY_WIRE_READY_JSON=' + ($witness | ConvertTo-Json -Depth 12 -Compress))
        exit 0
    }
    throw "Timed out after $TimeoutSeconds seconds waiting for decoded parity wire readiness. Last observation: $lastReason"
} finally {
    $resolvedTempRoot = [IO.Path]::GetFullPath($tempRoot)
    if (-not $resolvedTempRoot.StartsWith($tempBase + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean unexpected snapshot directory: $resolvedTempRoot"
    }
    if (Test-Path -LiteralPath $resolvedTempRoot) {
        Remove-Item -LiteralPath $resolvedTempRoot -Recurse -Force
    }
}
