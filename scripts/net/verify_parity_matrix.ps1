# Verify a complete retail/OpenNova LAN parity matrix from an ignored JSON
# manifest. Captures are decoded fresh with nw_pp --parity-events; coverage
# sidecars are intentionally never trusted as evidence.

[CmdletBinding()]
param(
    [string] $Manifest = "",
    [string] $Output = "",
    [switch] $Force,
    [switch] $SelfTest
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
. "$PSScriptRoot\lib.ps1"

$script:Failures = [System.Collections.Generic.List[object]]::new()
$script:CaseReports = [System.Collections.Generic.List[object]]::new()
$script:RetentionWitnesses = 0
$script:RetailAutomationPortMin = 32786
$script:RetailAutomationPortMax = 32789

function Test-RetailAutomationPort {
    param([int] $Port)
    return $Port -ge $script:RetailAutomationPortMin -and
        $Port -le $script:RetailAutomationPortMax
}

function Test-Field {
    param($Object, [Parameter(Mandatory = $true)] [string] $Name)
    return $null -ne $Object -and
        $null -ne $Object.PSObject.Properties[$Name]
}

function Test-UInt32JsonField {
    param($Object, [Parameter(Mandatory = $true)] [string] $Name)
    if (-not (Test-Field $Object $Name)) { return $false }
    $value = $Object.$Name
    $integerTyped = $value -is [byte] -or $value -is [sbyte] -or
        $value -is [int16] -or $value -is [uint16] -or
        $value -is [int32] -or $value -is [uint32] -or
        $value -is [int64] -or $value -is [uint64]
    if (-not $integerTyped) {
        return $false
    }
    try { [decimal]$number = $value }
    catch { return $false }
    return $number -ge 0 -and $number -le [decimal]4294967295 -and
        [decimal]::Truncate($number) -eq $number
}

function Get-Field {
    param($Object, [Parameter(Mandatory = $true)] [string] $Name, $Default = $null)
    if (Test-Field $Object $Name) { return $Object.$Name }
    return $Default
}

function Test-CanonicalMatrixPolicy {
    param($Document, [hashtable] $ArtifactPaths)

    $expectedTemplatePath = [IO.Path]::GetFullPath((Join-Path `
        (Get-RepoRoot) 'scripts\net\parity-matrix.example.json'))
    if (-not $ArtifactPaths.ContainsKey('manifest_template') -or
            -not ([IO.Path]::GetFullPath(
                [string]$ArtifactPaths['manifest_template']).Equals(
                    $expectedTemplatePath,
                    [StringComparison]::OrdinalIgnoreCase))) {
        Add-ParityFailure 'CANONICAL_TEMPLATE_PATH' '' '' `
            'manifest_template must be the current tracked canonical parity template.'
        return $false
    }

    try {
        $template = Get-Content -LiteralPath $expectedTemplatePath -Raw |
            ConvertFrom-Json
    } catch {
        Add-ParityFailure 'CANONICAL_MATRIX_POLICY' '' '' `
            "Canonical parity template is unreadable: $($_.Exception.Message)"
        return $false
    }

    $valid = $true
    $templateSchema = Get-Field $template 'schema' $null
    $activeSchema = Get-Field $Document 'schema' $null
    if (-not ($templateSchema -is [int] -or $templateSchema -is [long]) -or
            -not ($activeSchema -is [int] -or $activeSchema -is [long]) -or
            $activeSchema.GetType() -ne $templateSchema.GetType() -or
            [int64]$activeSchema -ne [int64]$templateSchema) {
        Add-ParityFailure 'CANONICAL_SCHEMA_POLICY' '' '' `
            'Manifest schema must retain the tracked canonical numeric type and value.'
        $valid = $false
    }
    $templateCorpus = Get-Field $template 'corpus' $null
    $activeCorpus = Get-Field $Document 'corpus' $null
    $templateExpansion = Get-Field $templateCorpus 'expansion' $null
    $activeExpansion = Get-Field $activeCorpus 'expansion' $null
    if ($templateExpansion -isnot [string] -or
            $activeExpansion -isnot [string] -or
            -not ([string]$activeExpansion).Equals(
                [string]$templateExpansion, [StringComparison]::Ordinal)) {
        Add-ParityFailure 'CANONICAL_EXPANSION_POLICY' '' '' `
            'corpus.expansion must exactly match the tracked canonical profile.'
        $valid = $false
    }
    $sourceStateFields = @(
        'schema','mode','sha256','entry_count','submodule_count') | Sort-Object
    $templateSourceState = Get-Field $templateCorpus 'source_state' $null
    $activeSourceState = Get-Field $activeCorpus 'source_state' $null
    $templateSourceFields = if ($null -ne $templateSourceState) {
        @($templateSourceState.PSObject.Properties.Name | Sort-Object)
    } else { @() }
    $activeSourceFields = if ($null -ne $activeSourceState) {
        @($activeSourceState.PSObject.Properties.Name | Sort-Object)
    } else { @() }
    if (($templateSourceFields -join ',') -cne ($sourceStateFields -join ',') -or
            ($activeSourceFields -join ',') -cne ($sourceStateFields -join ',') -or
            (Get-Field $templateSourceState 'schema' $null) -isnot [string] -or
            (Get-Field $activeSourceState 'schema' $null) -isnot [string] -or
            [string](Get-Field $templateSourceState 'schema' '') -cne
                'opennova.repository-source-state.v1' -or
            [string](Get-Field $activeSourceState 'schema' '') -cne
                [string](Get-Field $templateSourceState 'schema' '') -or
            (Get-Field $templateSourceState 'mode' $null) -isnot [string] -or
            (Get-Field $activeSourceState 'mode' $null) -isnot [string] -or
            [string](Get-Field $templateSourceState 'mode' '') -cne
                'git-nonignored-worktree-content' -or
            [string](Get-Field $activeSourceState 'mode' '') -cne
                [string](Get-Field $templateSourceState 'mode' '') -or
            [string](Get-Field $templateSourceState 'sha256' '') -cnotmatch
                '^[0-9a-f]{64}$' -or
            [string](Get-Field $activeSourceState 'sha256' '') -cnotmatch
                '^[0-9a-f]{64}$' -or
            -not ((Get-Field $templateSourceState 'entry_count' $null) -is [int] -or
                (Get-Field $templateSourceState 'entry_count' $null) -is [long]) -or
            -not ((Get-Field $activeSourceState 'entry_count' $null) -is [int] -or
                (Get-Field $activeSourceState 'entry_count' $null) -is [long]) -or
            -not ((Get-Field $templateSourceState 'submodule_count' $null) -is [int] -or
                (Get-Field $templateSourceState 'submodule_count' $null) -is [long]) -or
            -not ((Get-Field $activeSourceState 'submodule_count' $null) -is [int] -or
                (Get-Field $activeSourceState 'submodule_count' $null) -is [long]) -or
            [int64](Get-Field $templateSourceState 'entry_count' -1) -lt 0 -or
            [int64](Get-Field $activeSourceState 'entry_count' -1) -lt 0 -or
            [int64](Get-Field $templateSourceState 'submodule_count' -1) -lt 0 -or
            [int64](Get-Field $activeSourceState 'submodule_count' -1) -lt 0) {
        Add-ParityFailure 'CANONICAL_SOURCE_STATE_POLICY' '' '' `
            'corpus.source_state must retain the tracked repository-content contract.'
        $valid = $false
    }
    $templateRequiredProperty = $template.PSObject.Properties['required_case_ids']
    $activeRequiredProperty = $Document.PSObject.Properties['required_case_ids']
    $templateRequired = if ($templateRequiredProperty -and
            $templateRequiredProperty.Value -is [System.Array]) {
        @($templateRequiredProperty.Value)
    } else { @() }
    $activeRequired = if ($activeRequiredProperty -and
            $activeRequiredProperty.Value -is [System.Array]) {
        @($activeRequiredProperty.Value)
    } else { @() }
    $canonicalCoreCaseIds = @(
        '01tr-waypoint','tdh-i3a-tdm','cp04-deploy-hold','03tr-dense')
    if ($templateRequired.Count -lt $canonicalCoreCaseIds.Count -or
            @($templateRequired | Where-Object { $_ -isnot [string] }).Count -ne 0 -or
            @($activeRequired | Where-Object { $_ -isnot [string] }).Count -ne 0 -or
            @($canonicalCoreCaseIds | Where-Object {
                $templateRequired -notcontains $_
            }).Count -ne 0 -or
            ($activeRequired -join "`n") -cne ($templateRequired -join "`n")) {
        Add-ParityFailure 'CANONICAL_CASE_SET' '' '' `
            'Verdict-bearing evidence must include the four canonical cases; tracked crossover/holdout cases may extend that required set.'
        $valid = $false
    }

    $minimumFloors = [ordered]@{
        minimum_distinct_missions=4
        minimum_distinct_game_types=4
        minimum_distinct_ports=4
        minimum_exercised_input_cases=3
        minimum_in_match_cases=3
        minimum_deploy_hold_cases=1
        minimum_distinct_integrity_rows=2
    }
    foreach ($field in $minimumFloors.Keys) {
        $activeValue = Get-Field $Document $field $null
        $templateValue = Get-Field $template $field $null
        if (-not ($activeValue -is [int] -or $activeValue -is [long]) -or
                -not ($templateValue -is [int] -or $templateValue -is [long]) -or
                [int64]$templateValue -lt [int64]$minimumFloors[$field] -or
                [int64]$activeValue -ne [int64]$templateValue) {
            Add-ParityFailure 'CANONICAL_MATRIX_POLICY' '' '' `
                "Matrix field '$field' must exactly match the tracked policy and its structural floor."
            $valid = $false
        }
    }

    $templateComparison = Get-Field $template 'comparison' $null
    $activeComparison = Get-Field $Document 'comparison' $null
    $templateComparisonFields = if ($null -ne $templateComparison) {
        @($templateComparison.PSObject.Properties.Name | Sort-Object)
    } else { @() }
    $activeComparisonFields = if ($null -ne $activeComparison) {
        @($activeComparison.PSObject.Properties.Name | Sort-Object)
    } else { @() }
    if ($templateComparisonFields.Count -eq 0 -or
            ($activeComparisonFields -join "`n") -cne
                ($templateComparisonFields -join "`n")) {
        Add-ParityFailure 'CANONICAL_COMPARISON_POLICY' '' '' `
            'Comparison policy fields must exactly match the tracked canonical template.'
        $valid = $false
    }
    foreach ($field in $templateComparisonFields) {
        $templateValue = Get-Field $templateComparison $field $null
        $activeValue = Get-Field $activeComparison $field $null
        $sameType = $null -ne $templateValue -and $null -ne $activeValue -and
            $activeValue.GetType() -eq $templateValue.GetType()
        if (-not $sameType -or [decimal]$activeValue -ne [decimal]$templateValue) {
            Add-ParityFailure 'CANONICAL_COMPARISON_POLICY' '' '' `
                "Comparison field '$field' must retain its tracked numeric type and value."
            $valid = $false
        }
    }

    $templateCases = @(Get-Field $template 'cases' @())
    $activeCases = @(Get-Field $Document 'cases' @())
    $templateIds = @($templateCases | ForEach-Object { "$(Get-Field $_ 'id' '')" })
    $activeIds = @($activeCases | ForEach-Object { "$(Get-Field $_ 'id' '')" })
    if ($templateCases.Count -lt $canonicalCoreCaseIds.Count -or
            @($canonicalCoreCaseIds | Where-Object {
                $templateIds -notcontains $_
            }).Count -ne 0 -or
            ($activeIds -join "`n") -cne ($templateIds -join "`n")) {
        Add-ParityFailure 'CANONICAL_CASE_SET' '' '' `
            'Manifest cases must contain the four canonical cases and exactly match the ordered tracked matrix, including any tracked crossover/holdout cases.'
        $valid = $false
    }

    foreach ($templateCase in $templateCases) {
        $caseId = "$(Get-Field $templateCase 'id' '')"
        $activeCase = @($activeCases | Where-Object {
            "$(Get-Field $_ 'id' '')" -ceq $caseId
        } | Select-Object -First 1)
        if ($activeCase.Count -ne 1) {
            Add-ParityFailure 'CANONICAL_CASE_BINDING' $caseId '' `
                'Canonical case is missing or duplicated.'
            $valid = $false
            continue
        }
        $activeCase = $activeCase[0]
        foreach ($field in @(
            'id','mission','resolution','host_session_name','server_name',
            'host_callsign','joiner_callsign','readiness_mode','control_mode'
        )) {
            $activeValue = Get-Field $activeCase $field $null
            $templateValue = Get-Field $templateCase $field $null
            if ($activeValue -isnot [string] -or $templateValue -isnot [string] -or
                    -not ([string]$activeValue).Equals(
                        [string]$templateValue, [StringComparison]::Ordinal)) {
                Add-ParityFailure 'CANONICAL_CASE_BINDING' $caseId '' `
                    "Canonical string field '$field' differs from tracked policy."
                $valid = $false
            }
        }
        foreach ($field in @('game_type','port')) {
            if (-not (Test-UInt32JsonField $activeCase $field) -or
                    -not (Test-UInt32JsonField $templateCase $field) -or
                    [uint32]$activeCase.$field -ne [uint32]$templateCase.$field) {
                Add-ParityFailure 'CANONICAL_CASE_BINDING' $caseId '' `
                    "Canonical numeric field '$field' differs from tracked policy."
                $valid = $false
            }
        }
        foreach ($field in @('exercise_input','require_analog','maintenance')) {
            $activeValue = Get-Field $activeCase $field $null
            $templateValue = Get-Field $templateCase $field $null
            if ($activeValue -isnot [bool] -or $templateValue -isnot [bool] -or
                    [bool]$activeValue -ne [bool]$templateValue) {
                Add-ParityFailure 'CANONICAL_CASE_BINDING' $caseId '' `
                    "Canonical boolean field '$field' differs from tracked policy."
                $valid = $false
            }
        }
        foreach ($field in @('minimum_integrity_events','minimum_control_quartets')) {
            $templateHasField = Test-Field $templateCase $field
            $activeHasField = Test-Field $activeCase $field
            if ($templateHasField -ne $activeHasField -or
                    ($templateHasField -and
                        (-not ((Get-Field $templateCase $field $null) -is [int] -or
                            (Get-Field $templateCase $field $null) -is [long]) -or
                         -not ((Get-Field $activeCase $field $null) -is [int] -or
                            (Get-Field $activeCase $field $null) -is [long]) -or
                         [int64](Get-Field $activeCase $field 0) -ne
                            [int64](Get-Field $templateCase $field 0)))) {
                Add-ParityFailure 'CANONICAL_CASE_BINDING' $caseId '' `
                    "Canonical optional numeric field '$field' differs from tracked policy."
                $valid = $false
            }
        }
        $templateRuns = Get-Field $templateCase 'runs' $null
        $activeRuns = Get-Field $activeCase 'runs' $null
        $requiredTopologies = @('RR','RO','OR','OO')
        $expectedTopologyNames = @($requiredTopologies | Sort-Object)
        $templateTopologyNames = if ($null -eq $templateRuns) {
            @()
        } else {
            @($templateRuns.PSObject.Properties.Name | Sort-Object)
        }
        $activeTopologyNames = if ($null -eq $activeRuns) {
            @()
        } else {
            @($activeRuns.PSObject.Properties.Name | Sort-Object)
        }
        if (($templateTopologyNames -join ',') -cne
                ($expectedTopologyNames -join ',') -or
                ($activeTopologyNames -join ',') -cne
                ($expectedTopologyNames -join ',')) {
            Add-ParityFailure 'CANONICAL_TOPOLOGY_SET' $caseId '' `
                'runs must contain exactly RR, RO, OR, and OO with no extra topology properties.'
            $valid = $false
        }
        foreach ($topology in $requiredTopologies) {
            $templateRun = Get-Field $templateRuns $topology $null
            $activeRun = Get-Field $activeRuns $topology $null
            $templateSteady = Get-Field $templateRun 'steady_seconds' $null
            $activeSteady = Get-Field $activeRun 'steady_seconds' $null
            if (-not ($templateSteady -is [int] -or
                        $templateSteady -is [long]) -or
                    -not ($activeSteady -is [int] -or
                        $activeSteady -is [long]) -or
                    [int64]$activeSteady -ne [int64]$templateSteady) {
                Add-ParityFailure 'CANONICAL_STEADY_WINDOW' $caseId $topology `
                    'steady_seconds must exactly match the tracked canonical run window.'
                $valid = $false
            }
        }
        $exceptionsProperty = $activeCase.PSObject.Properties['exceptions']
        if (-not $exceptionsProperty -or
                $exceptionsProperty.Value -isnot [System.Array]) {
            Add-ParityFailure 'CASE_EXCEPTIONS_TYPE' $caseId '' `
                'exceptions must be an explicit JSON array.'
            $valid = $false
        } elseif (@($exceptionsProperty.Value).Count -ne 0) {
            Add-ParityFailure 'PARITY_EXCEPTIONS_FORBIDDEN' $caseId '' `
                'Verdict-bearing parity evidence may not waive wire differences.'
            $valid = $false
        }
    }

    $validGameTypes = @($activeCases | Where-Object {
        Test-UInt32JsonField $_ 'game_type'
    } | ForEach-Object { [uint32]$_.game_type })
    $hasNonTeam = @($validGameTypes | Where-Object { ($_ -band 0x10000) -eq 0 }).Count -gt 0
    $hasTeam = @($validGameTypes | Where-Object { ($_ -band 0x10000) -ne 0 }).Count -gt 0
    $hasObjective = @($validGameTypes | Where-Object { ($_ -band 0x20000) -ne 0 }).Count -gt 0
    $hasNonObjective = @($validGameTypes | Where-Object { ($_ -band 0x20000) -eq 0 }).Count -gt 0
    $hasWaypoint = @($validGameTypes | Where-Object { ($_ -band 0x20) -ne 0 }).Count -gt 0
    $hasNonWaypoint = @($validGameTypes | Where-Object { ($_ -band 0x20) -eq 0 }).Count -gt 0
    if (-not ($hasNonTeam -and $hasTeam -and $hasObjective -and
            $hasNonObjective -and $hasWaypoint -and $hasNonWaypoint)) {
        Add-ParityFailure 'GAME_TYPE_BRANCH_COVERAGE' '' '' `
            'Canonical game types must exercise non-team, team, objective, non-objective, waypoint, and non-waypoint branches.'
        $valid = $false
    }
    return $valid
}

function Add-ParityFailure {
    param(
        [Parameter(Mandatory = $true)] [string] $Code,
        [string] $CaseId = "",
        [string] $Topology = "",
        [Parameter(Mandatory = $true)] [string] $Message
    )
    $script:Failures.Add([pscustomobject]@{
        code = $Code
        case_id = $CaseId
        topology = $Topology
        message = $Message
    })
}

function Test-RepositorySourceStateBinding {
    param(
        [Parameter(Mandatory = $true)] [AllowNull()] $Declared,
        [Parameter(Mandatory = $true)] $Current
    )

    $expectedProperties = @(
        'schema','mode','sha256','entry_count','submodule_count') | Sort-Object
    $declaredProperties = if ($null -ne $Declared) {
        @($Declared.PSObject.Properties.Name | Sort-Object)
    } else { @() }
    $integerFieldsValid = (Test-Field $Declared 'entry_count') -and
        (Test-Field $Declared 'submodule_count') -and
        ((Get-Field $Declared 'entry_count' $null) -is [int] -or
            (Get-Field $Declared 'entry_count' $null) -is [long]) -and
        ((Get-Field $Declared 'submodule_count' $null) -is [int] -or
            (Get-Field $Declared 'submodule_count' $null) -is [long])
    if (($declaredProperties -join ',') -cne ($expectedProperties -join ',') -or
            (Get-Field $Declared 'schema' $null) -isnot [string] -or
            [string](Get-Field $Declared 'schema' '') -cne
                'opennova.repository-source-state.v1' -or
            (Get-Field $Declared 'mode' $null) -isnot [string] -or
            [string](Get-Field $Declared 'mode' '') -cne
                'git-nonignored-worktree-content' -or
            [string](Get-Field $Declared 'sha256' '') -cnotmatch '^[0-9a-f]{64}$' -or
            [string](Get-Field $Declared 'sha256' '') -ceq ('0' * 64) -or
            -not $integerFieldsValid -or
            [int64](Get-Field $Declared 'entry_count' 0) -le 0 -or
            [int64](Get-Field $Declared 'submodule_count' -1) -lt 0) {
        Add-ParityFailure 'REPOSITORY_SOURCE_STATE_INVALID' '' '' `
            'corpus.source_state must be one exact nonempty repository content binding.'
        return $false
    }
    foreach ($field in $expectedProperties) {
        if ([string](Get-Field $Declared $field '') -cne
                [string](Get-Field $Current $field '')) {
            Add-ParityFailure 'REPOSITORY_SOURCE_STATE_MISMATCH' '' '' `
                "Repository source state differs from capture/preflight at '$field'."
            return $false
        }
    }
    return $true
}

function Register-UniqueCaptureHash {
    param(
        [hashtable] $Seen,
        [string] $Sha256,
        [string] $CaseId,
        [string] $Topology
    )
    if ($Seen.ContainsKey($Sha256)) {
        Add-ParityFailure "CAPTURE_BYTES_REUSED" $CaseId $Topology `
            "Capture bytes duplicate $($Seen[$Sha256]); every case/topology needs independent evidence."
        return $false
    }
    $Seen[$Sha256] = "$CaseId/$Topology"
    return $true
}

function ConvertTo-NormalizedTag {
    param([Parameter(Mandatory = $true)] $Value)
    $text = "$Value".Trim()
    $number = 0
    if ($text -match '^0[xX]([0-9a-fA-F]+)$') {
        $number = [Convert]::ToInt32($Matches[1], 16)
    } elseif (-not [int]::TryParse($text, [ref] $number)) {
        throw "Invalid protocol tag: $Value"
    }
    if ($number -lt 0 -or $number -gt 0x1FF) { throw "Protocol tag is out of range: $Value" }
    return ('{0:x2}' -f ($number -band 0xFF))
}

function ConvertTo-TagIdentity {
    param([Parameter(Mandatory = $true)] $Value)
    $text = "$Value".Trim()
    $number = 0
    if ($text -match '^0[xX]([0-9a-fA-F]+)$') {
        $number = [Convert]::ToInt32($Matches[1], 16)
    } elseif (-not [int]::TryParse($text, [ref] $number)) {
        throw "Invalid protocol tag: $Value"
    }
    if ($number -lt 0 -or $number -gt 0x1FF) { throw "Protocol tag is out of range: $Value" }
    return ('{0:x2}:{1}' -f ($number -band 0xFF), $(if (($number -band 0x100) -ne 0) { 1 } else { 0 }))
}

function ConvertFrom-HexBytes {
    param([Parameter(Mandatory = $true)] [AllowEmptyString()] [string] $Hex)
    if ($Hex -eq "-") { return [byte[]]@() }
    if (($Hex.Length % 2) -ne 0 -or $Hex -notmatch '^[0-9a-fA-F]*$') {
        throw "Malformed parity-event body hex."
    }
    $bytes = [byte[]]::new($Hex.Length / 2)
    for ($i = 0; $i -lt $bytes.Length; $i++) {
        $bytes[$i] = [Convert]::ToByte($Hex.Substring($i * 2, 2), 16)
    }
    return $bytes
}

function ConvertTo-CompactHex {
    param([byte[]] $Bytes)
    if ($null -eq $Bytes -or $Bytes.Length -eq 0) { return '' }
    return ([BitConverter]::ToString($Bytes)).Replace('-', '').ToLowerInvariant()
}

function Get-StableBodyKey {
    param($Event, [string] $Direction, [string] $Tag)
    [byte[]]$bytes = Get-Field $Event 'body' ([byte[]]@())
    if ($Direction -eq 'S' -and $Tag -eq '0f:0') {
        # S2C 0x0F starts with the live session tick. The remaining bytes are
        # the same-case deployment pose, game flags, score, waypoint, and
        # location fields that must match RR.
        if ($bytes.Length -lt 4) { return "short:$(ConvertTo-CompactHex $bytes)" }
        return ConvertTo-CompactHex ([byte[]]$bytes[4..($bytes.Length - 1)])
    }
    return ConvertTo-CompactHex $bytes
}

function Read-U16Le {
    param([byte[]] $Bytes, [int] $Offset)
    if ($Offset -lt 0 -or $Offset + 2 -gt $Bytes.Length) { throw "u16 read exceeds body." }
    return [uint16]([uint16]$Bytes[$Offset] -bor ([uint16]$Bytes[$Offset + 1] -shl 8))
}

function Read-U32Le {
    param([byte[]] $Bytes, [int] $Offset)
    if ($Offset -lt 0 -or $Offset + 4 -gt $Bytes.Length) { throw "u32 read exceeds body." }
    # PowerShell's bitwise operators can coerce the high byte through signed
    # Int32, making any value >= 0x80000000 throw during the UInt32 cast. Build
    # in UInt64 arithmetic so every possible retail CRC/key remains valid.
    $value = [uint64]$Bytes[$Offset]
    $value += [uint64]$Bytes[$Offset + 1] * 0x100
    $value += [uint64]$Bytes[$Offset + 2] * 0x10000
    $value += [uint64]$Bytes[$Offset + 3] * 0x1000000
    return [uint32]$value
}

function Read-CStringAscii {
    param([byte[]] $Bytes, [ref] $Offset)
    $start = [int]$Offset.Value
    $end = $start
    while ($end -lt $Bytes.Length -and $Bytes[$end] -ne 0) { $end++ }
    if ($end -ge $Bytes.Length) { throw "Unterminated string in S2C 0x7B." }
    $value = [Text.Encoding]::ASCII.GetString($Bytes, $start, $end - $start)
    $Offset.Value = $end + 1
    return $value
}

function Test-BodyContainsCStringAscii {
    param([byte[]] $Bytes, [string] $Value)
    [byte[]] $needle = @([Text.Encoding]::ASCII.GetBytes($Value) + [byte]0)
    if ($needle.Length -gt $Bytes.Length) { return $false }
    for ($start = 0; $start -le $Bytes.Length - $needle.Length; $start++) {
        $matches = $true
        for ($i = 0; $i -lt $needle.Length; $i++) {
            if ($Bytes[$start + $i] -ne $needle[$i]) {
                $matches = $false
                break
            }
        }
        if ($matches) { return $true }
    }
    return $false
}

function Find-NwPp {
    $build = Join-Path (Get-RepoRoot) "build"
    foreach ($cfg in @("Release", "Debug")) {
        $candidate = Join-Path $build "apps\nw_pp\$cfg\nw_pp.exe"
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
    }
    $hit = Get-ChildItem -LiteralPath $build -Recurse -Filter "nw_pp.exe" `
        -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($hit) { return $hit.FullName }
    return $null
}

function ConvertFrom-HandshakeTlv {
    param(
        [Parameter(Mandatory = $true)] [byte[]] $Bytes,
        [Parameter(Mandatory = $true)] [byte] $Opcode
    )
    $u32Names = @(
        'DE','CI','PM','EIP','EPN','ET','UT','HK','SF','P1','P2','NP','MP',
        'NC','RIP','RPN','CK','SIP','SPN','MI','CR','JFC','JFP','SK'
    )
    $stringNames = @(
        'NVS','CO','AP','BDAT','PN','PV1','PV2','PV3','SN','PL','SUS1','SUS2',
        'NA','SCRK','JFS'
    )
    $fields = [System.Collections.Generic.List[object]]::new()
    $offset = 0
    $ordinal = 0
    while ($offset -lt $Bytes.Length) {
        $nameStart = $offset
        while ($offset -lt $Bytes.Length -and $Bytes[$offset] -ne 0) { $offset++ }
        if ($offset -ge $Bytes.Length -or $offset -eq $nameStart) {
            throw 'Outer handshake contains an unterminated or empty TLV name.'
        }
        [byte[]]$nameBytes = @($Bytes[$nameStart..($offset - 1)])
        if (@($nameBytes | Where-Object { $_ -lt 0x21 -or $_ -gt 0x7e }).Count -ne 0) {
            throw 'Outer handshake contains a non-ASCII TLV name.'
        }
        $name = [Text.Encoding]::ASCII.GetString($nameBytes)
        $offset++
        if ($offset + 2 -gt $Bytes.Length) {
            throw "Outer handshake TLV $name has no UInt16 length."
        }
        $length = [int](Read-U16Le $Bytes $offset)
        $offset += 2
        if ($offset + $length -gt $Bytes.Length) {
            throw "Outer handshake TLV $name exceeds its packet body."
        }
        [byte[]]$value = if ($length -eq 0) {
            [byte[]]@()
        } else {
            [byte[]]$Bytes[$offset..($offset + $length - 1)]
        }
        $offset += $length
        $kind = 'bytes'
        $cuType = $null
        $cuName = $null
        $cuValueLength = $null
        [byte[]]$cuValue = @()
        $csDirection = $null
        $csIndex = $null
        $csValue = $null
        if ($u32Names -ccontains $name) {
            if ($length -ne 4) { throw "Outer handshake UInt32 TLV $name is not four bytes." }
            $kind = 'u32'
        } elseif ($stringNames -ccontains $name) {
            if ($length -lt 1 -or $value[$length - 1] -ne 0) {
                throw "Outer handshake string TLV $name is not NUL terminated."
            }
            $kind = 'string'
        } elseif ($name -ceq 'PG') {
            if ($length -ne 16) { throw 'Outer handshake PG is not 16 bytes.' }
            $kind = 'guid'
        } elseif ($name -ceq 'CS') {
            if ($Opcode -ne 0x82 -or $length -ne 6 -or $value[0] -notin @(0,1)) {
                throw 'Outer handshake CS has an invalid opcode, direction, or length.'
            }
            $kind = 'cs'
            $csDirection = [uint32]$value[0]
            $csIndex = [uint32]$value[1]
            $csValue = [uint32](Read-U32Le $value 2)
        } elseif ($name -ceq 'CU') {
            if ($Opcode -notin @(0x42,0x82) -or $length -lt 5) {
                throw 'Outer handshake CU has an invalid opcode or length.'
            }
            $kind = 'cu'
            $cuType = [uint32]$value[0]
            if (($Opcode -eq 0x42 -and $cuType -notin @(1,2)) -or
                    ($Opcode -eq 0x82 -and $cuType -ne 3)) {
                throw 'Outer handshake CU kind is invalid for its direction.'
            }
            $inner = 1
            $innerNameStart = $inner
            while ($inner -lt $value.Length -and $value[$inner] -ne 0) { $inner++ }
            if ($inner -ge $value.Length -or $inner -eq $innerNameStart) {
                throw 'Outer handshake CU has an invalid name.'
            }
            [byte[]]$innerNameBytes = @($value[$innerNameStart..($inner - 1)])
            if (@($innerNameBytes | Where-Object { $_ -lt 0x21 -or $_ -gt 0x7e }).Count -ne 0) {
                throw 'Outer handshake CU has a non-ASCII name.'
            }
            $cuName = [Text.Encoding]::ASCII.GetString($innerNameBytes)
            $inner++
            if ($inner + 2 -gt $value.Length) {
                throw 'Outer handshake CU has no value length.'
            }
            $cuValueLength = [int](Read-U16Le $value $inner)
            $inner += 2
            if ($cuValueLength -lt 1 -or
                    $inner + $cuValueLength -ne $value.Length -or
                    $value[$value.Length - 1] -ne 0) {
                throw 'Outer handshake CU value length or terminator is invalid.'
            }
            $cuValue = [byte[]]$value[$inner..($value.Length - 1)]
        }
        $fields.Add([pscustomobject]@{
            ordinal=$ordinal;name=$name;kind=$kind;length=$length;value=$value
            cu_type=$cuType;cu_name=$cuName;cu_value_length=$cuValueLength;cu_value=$cuValue
            cs_direction=$csDirection;cs_index=$csIndex;cs_value=$csValue
        })
        $ordinal++
    }
    return @($fields.ToArray())
}

function ConvertFrom-ParityLines {
    param([Parameter(Mandatory = $true)] [string[]] $Lines)

    $events = [System.Collections.Generic.List[object]]::new()
    $packets = [System.Collections.Generic.List[object]]::new()
    $states = [System.Collections.Generic.List[object]]::new()
    $entities = [System.Collections.Generic.List[object]]::new()
    $handshakes = [System.Collections.Generic.List[object]]::new()
    $datagrams = [System.Collections.Generic.List[object]]::new()
    $histogram = @{}
    foreach ($rawLine in $Lines) {
        $line = "$rawLine"
        if ($line -match '^PARITY_DATAGRAM frame=(\d+) ts_ns=(\d+) src=(\d+) dst=(\d+) len=(\d+) outer=([01]) opcode=(0x[0-9a-fA-F]{2}|--) class=(invalid|unknown|client_hello|client_auth|server_hello|server_auth|client_protocol|server_protocol) decode=([01])$') {
            $frame = [int]$Matches[1]
            $tsNs = [uint64]$Matches[2]
            $srcPort = [int]$Matches[3]
            $dstPort = [int]$Matches[4]
            $payloadLength = [uint64]$Matches[5]
            $outerDecoded = $Matches[6] -eq '1'
            $opcodeText = $Matches[7].ToLowerInvariant()
            $datagramClass = $Matches[8]
            $decoded = $Matches[9] -eq '1'
            $opcode = if ($opcodeText -eq '--') {
                $null
            } else {
                [Convert]::ToByte($opcodeText.Substring(2), 16)
            }
            $expectedOpcode = @{
                client_hello=0x41;client_auth=0x42
                server_hello=0x81;server_auth=0x82
                client_protocol=0x43;server_protocol=0x83
            }
            if ((-not $outerDecoded -and
                    ($opcodeText -cne '--' -or $datagramClass -cne 'invalid' -or $decoded)) -or
                    ($outerDecoded -and $opcodeText -ceq '--') -or
                    ($datagramClass -in @('invalid','unknown') -and $decoded) -or
                    ($datagramClass -eq 'invalid' -and $outerDecoded) -or
                    ($datagramClass -ne 'invalid' -and -not $outerDecoded) -or
                    ($expectedOpcode.ContainsKey($datagramClass) -and
                        [byte]$opcode -ne [byte]$expectedOpcode[$datagramClass]) -or
                    ($datagramClass -eq 'unknown' -and
                        $expectedOpcode.Values -contains [byte]$opcode)) {
                throw 'PARITY_DATAGRAM classification disagrees with its outer-decode/opcode/decode fields.'
            }
            $datagrams.Add([pscustomobject]@{
                frame=$frame;ts_ns=$tsNs;src_port=$srcPort;dst_port=$dstPort
                length=$payloadLength;outer_decoded=$outerDecoded;opcode=$opcode
                class=$datagramClass;decode=$decoded
            })
            continue
        }
        if ($line -match '^PARITY_HANDSHAKE frame=(\d+) ts_ns=(\d+) dir=([SC]) session=(\d+) opcode=0x(41|42|81|82) decode=([01]) len=(\d+) body=([0-9a-fA-F]*)$') {
            $opcode = [Convert]::ToByte($Matches[5], 16)
            $direction = $Matches[3]
            if (($opcode -in @(0x41,0x42) -and $direction -cne 'C') -or
                    ($opcode -in @(0x81,0x82) -and $direction -cne 'S')) {
                throw 'PARITY_HANDSHAKE direction disagrees with its opcode.'
            }
            [byte[]]$body = @(ConvertFrom-HexBytes $Matches[8])
            if ($body.Length -ne [int]$Matches[7]) {
                throw 'PARITY_HANDSHAKE body length does not match its declared length.'
            }
            $handshakes.Add([pscustomobject]@{
                frame=[int]$Matches[1];ts_ns=[uint64]$Matches[2]
                dir=$direction;session=[int]$Matches[4];opcode=$opcode
                decode=$Matches[6] -eq '1';length=[int]$Matches[7];body=$body
                fields=@(ConvertFrom-HandshakeTlv $body $opcode)
            })
            continue
        }
        if ($line -match '^PARITY_PACKET frame=(\d+) ts_ns=(\d+) dir=([SC]) session=(\d+) sid=(0x[0-9a-fA-F]+) seq=(\d+) ack=(\d+) flags=(0x[0-9a-fA-F]+) records=(\d+) tags=(\S+) wire=(\S+)$') {
            # Save every line-level match before parsing nested record fields:
            # PowerShell's automatic $Matches table is replaced by each -match.
            $packetFrame = [int]$Matches[1]
            $packetTs = [uint64]$Matches[2]
            $packetDirection = $Matches[3]
            $packetSession = [int]$Matches[4]
            $packetSid = [Convert]::ToUInt32($Matches[5].Substring(2), 16)
            $packetSequence = [uint32]$Matches[6]
            $packetAck = [uint32]$Matches[7]
            $packetFlags = $Matches[8].ToLowerInvariant()
            $records = [int]$Matches[9]
            $tagText = $Matches[10]
            $wireText = $Matches[11]
            $tagIdentities = @()
            if ($tagText -ne '-') {
                $tagIdentities = @($tagText.Split(',') | ForEach-Object { ConvertTo-TagIdentity $_ })
            }
            $tags = @($tagIdentities | ForEach-Object { $_.Substring(0, 2) })
            if ($records -ne $tagIdentities.Count) {
                throw "PARITY_PACKET record count does not match its tag list."
            }

            $wireRecords = [System.Collections.Generic.List[object]]::new()
            if ($wireText -ne '-') {
                foreach ($wirePart in $wireText.Split(',')) {
                    if ($wirePart -notmatch '^0x([0-9a-fA-F]{3}):0x([0-9a-fA-F]{2}):(\d+):([0-9a-fA-F]+|-)$') {
                        throw "Malformed PARITY_PACKET physical record: $wirePart"
                    }
                    $wireFullTagText = "0x$($Matches[1])"
                    $wireIdentity = ConvertTo-TagIdentity $wireFullTagText
                    $wireFullTag = [Convert]::ToUInt16($Matches[1], 16)
                    $wireRawFlags = [Convert]::ToByte($Matches[2], 16)
                    $wireEncodedLength = [uint32]$Matches[3]
                    $wireSkipHex = $Matches[4].ToLowerInvariant()
                    [byte[]]$wireSkipBytes = @()
                    if ($wireSkipHex -ne '-') {
                        $wireSkipBytes = [byte[]]@(ConvertFrom-HexBytes $wireSkipHex)
                    }
                    $expectedSkipLength = if (($wireRawFlags -band 0x08) -ne 0) {
                        1
                    } elseif (($wireRawFlags -band 0x10) -ne 0) {
                        2
                    } else {
                        0
                    }
                    if (@($wireSkipBytes).Count -ne $expectedSkipLength) {
                        throw 'PARITY_PACKET physical record SKIP bytes disagree with raw flags.'
                    }
                    if (($wireRawFlags -band 0x20) -ne 0) {
                        if ($wireEncodedLength -gt 0xff) {
                            throw 'PARITY_PACKET LEN8 record exceeds its encoded range.'
                        }
                    } elseif (($wireRawFlags -band 0x40) -ne 0) {
                        if ($wireEncodedLength -gt 0xffff) {
                            throw 'PARITY_PACKET LEN16 record exceeds its encoded range.'
                        }
                    } elseif ($wireEncodedLength -ne 0) {
                        throw 'PARITY_PACKET no-length record declares a nonzero encoded length.'
                    }
                    $settingsHigh = ($wireRawFlags -band 0x80) -ne 0
                    if ($settingsHigh -ne ($wireFullTag -ge 0x100)) {
                        throw 'PARITY_PACKET physical record tag identity disagrees with raw settings bit.'
                    }
                    $wireKey = ('{0}|raw={1:x2}|len={2}|skip={3}' -f
                        $wireIdentity,$wireRawFlags,$wireEncodedLength,$wireSkipHex)
                    $wireRecords.Add([pscustomobject]@{
                        full_tag = [uint16]$wireFullTag
                        identity = $wireIdentity
                        raw_flags = [byte]$wireRawFlags
                        raw_hex = ('0x{0:x2}' -f $wireRawFlags)
                        encoded_length = [uint32]$wireEncodedLength
                        skip_bytes = $wireSkipBytes
                        skip_hex = $wireSkipHex
                        key = $wireKey
                    })
                }
            }
            if ($records -ne $wireRecords.Count) {
                throw 'PARITY_PACKET record count does not match its physical record list.'
            }
            for ($recordIndex = 0; $recordIndex -lt $records; $recordIndex++) {
                if ($wireRecords[$recordIndex].identity -cne $tagIdentities[$recordIndex]) {
                    throw 'PARITY_PACKET tag list disagrees with its ordered physical record list.'
                }
            }
            $packets.Add([pscustomobject]@{
                frame = $packetFrame
                ts_ns = $packetTs
                dir = $packetDirection
                session = $packetSession
                sid = $packetSid
                epoch = 0
                connection_key = ''
                sequence = $packetSequence
                ack = $packetAck
                flags = $packetFlags
                records = $records
                tags = $tags
                tag_identities = $tagIdentities
                wire_records = @($wireRecords.ToArray())
                wire_key = (@($wireRecords.ToArray() | ForEach-Object { $_.key }) -join ',')
            })
            continue
        }
        if ($line -match '^PARITY_EVENT frame=(\d+) ts_ns=(\d+) dir=([SC]) session=(\d+) tag=0x([0-9a-fA-F]{2}) settings=([01]) len=(\d+) body=([0-9a-fA-F]*|-)$') {
            $tag = $Matches[5].ToLowerInvariant()
            [byte[]]$body = @(ConvertFrom-HexBytes $Matches[8])
            $direction = $Matches[3]
            $declaredLength = [int]$Matches[7]
            $settingsUpdate = $Matches[6] -eq '1'
            $identity = "${tag}:$(if ($settingsUpdate) { '1' } else { '0' })"
            # nw_pp materializes every payload except the two ordinary
            # high-volume gameplay bodies whose complete structured decodes
            # follow as PARITY_STATE/PARITY_ENTITY records. Settings-table
            # 0x10A/0x10C remain material because identity includes bit 8.
            $material = -not (($direction -eq 'C' -and $identity -eq '0c:0') -or
                ($direction -eq 'S' -and $identity -eq '0a:0'))
            if ($material -and $body.Length -ne $declaredLength) {
                throw "Material parity-event body length does not match its declared length."
            }
            $event = [pscustomobject]@{
                frame = [int]$Matches[1]
                ts_ns = [uint64]$Matches[2]
                dir = $direction
                session = [int]$Matches[4]
                sid = [uint32]0
                epoch = 0
                connection_key = ''
                tag = $tag
                identity = $identity
                settings_update = $settingsUpdate
                length = $declaredLength
                body_materialized = $material
                body = $body
            }
            $events.Add($event)
            $key = "$($event.dir):$identity"
            if (-not $histogram.ContainsKey($key)) { $histogram[$key] = 0 }
            $histogram[$key] = [int]$histogram[$key] + 1
            continue
        }
        if ($line -match '^PARITY_STATE frame=(\d+) ts_ns=(\d+) dir=C session=(\d+) tag=0x0c kind=uplink decode=([01]) len=(\d+) handle=(\d+) type=(\d+) sub=(\d+) carrier=(\d+) pos=(-?\d+),(-?\d+),(-?\d+) orient=(-?\d+),(-?\d+) move=(\d+) state=(\d+) analog=(\d+),(\d+),(\d+) adm=(\d+) priority_nonzero=(\d+) priority=(\d+):(\d+),(\d+):(\d+),(\d+):(\d+),(\d+):(\d+)$') {
            $priorityOccupancy = 0
            foreach ($pair in @(@(22,23), @(24,25), @(26,27), @(28,29))) {
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
                dir = 'C'; session = [int]$Matches[3]; tag = '0c'; kind = 'uplink'
                sid = [uint32]0; epoch = 0; connection_key = ''
                decode = $Matches[4] -eq '1'; length = [int]$Matches[5]
                handle = [uint32]$Matches[6]; type_id = [uint32]$Matches[7]
                sub_op = [uint32]$Matches[8]; carrier = [uint32]$Matches[9]
                pos_x = [int64]$Matches[10]; pos_y = [int64]$Matches[11]; pos_z = [int64]$Matches[12]
                orient_a = [int]$Matches[13]; orient_b = [int]$Matches[14]
                move = [uint32]$Matches[15]; state_a = [uint32]$Matches[16]
                analog_x = [uint32]$Matches[17]; analog_y = [uint32]$Matches[18]; analog_z = [uint32]$Matches[19]
                adm = [uint32]$Matches[20]; priority_nonzero = [uint32]$Matches[21]
                priority_handle_0 = [uint32]$Matches[22]; priority_score_0 = [uint32]$Matches[23]
                priority_handle_1 = [uint32]$Matches[24]; priority_score_1 = [uint32]$Matches[25]
                priority_handle_2 = [uint32]$Matches[26]; priority_score_2 = [uint32]$Matches[27]
                priority_handle_3 = [uint32]$Matches[28]; priority_score_3 = [uint32]$Matches[29]
            })
            continue
        }
        if ($line -match '^PARITY_STATE frame=(\d+) ts_ns=(\d+) dir=S session=(\d+) tag=0x0a kind=frame decode=([01]) len=(\d+) sub=(\d+) flags=(\d+),(\d+) anchor=(-?\d+),(-?\d+),(-?\d+) local=([01]) local_state=(\d+),(-?\d+),(-?\d+) records=(\d+) players=(\d+) vehicles=(\d+) infantry=(\d+) none=(\d+) rounds=(\d+) local_mount=(\d+) weapon=([01]),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(-?\d+) timer=([01]),(\d+),(\d+),(\d+),(\d+),(-?\d+) env=([01]),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+) objective=([01]),(-?\d+),(-?\d+),(-?\d+),(-?\d+) mount=([01]),(\d+),([01]),(\d+),(\d+)$') {
            $passengerBound = [uint32]$Matches[53] -ne 0xffff
            if (($Matches[54] -eq '1') -ne $passengerBound) {
                throw 'PARITY_STATE phase-8 has_mount disagrees with its mount handle.'
            }
            $states.Add([pscustomobject]@{
                frame = [int]$Matches[1]; ts_ns = [uint64]$Matches[2]
                dir = 'S'; session = [int]$Matches[3]; tag = '0a'; kind = 'frame'
                sid = [uint32]0; epoch = 0; connection_key = ''
                decode = $Matches[4] -eq '1'; length = [int]$Matches[5]
                sub_block = [uint32]$Matches[6]; flags1 = [uint32]$Matches[7]; flags2 = [uint32]$Matches[8]
                anchor_x = [int64]$Matches[9]; anchor_y = [int64]$Matches[10]; anchor_z = [int64]$Matches[11]
                local_tail = $Matches[12] -eq '1'; state_a = [uint32]$Matches[13]
                health = [int]$Matches[14]; state_word = [int]$Matches[15]
                records = [int]$Matches[16]; players = [int]$Matches[17]; vehicles = [int]$Matches[18]
                infantry = [int]$Matches[19]; none = [int]$Matches[20]; rounds = [int]$Matches[21]
                local_mount = [uint32]$Matches[22]
                weapon_present = $Matches[23] -eq '1'
                weapon_preround_timer = [uint32]$Matches[24]
                weapon_slot_state360 = [uint32]$Matches[25]
                weapon_slot_state368 = [uint32]$Matches[26]
                weapon_slot_state364 = [uint32]$Matches[27]
                weapon_slot_state356 = [uint32]$Matches[28]
                weapon_slot_state460 = [uint32]$Matches[29]
                weapon_reload_seconds = [uint32]$Matches[30]
                weapon_uniform_team_mask = [int]$Matches[31]
                timer_present = $Matches[32] -eq '1'
                timer_state0 = [uint32]$Matches[33]
                timer_state1 = [uint32]$Matches[34]
                timer_state2 = [uint32]$Matches[35]
                timer_state3 = [uint32]$Matches[36]
                timer_seconds = [int]$Matches[37]
                env_present = $Matches[38] -eq '1'
                env_fog_dist = [uint32]$Matches[39]
                env_fog_accel = [uint32]$Matches[40]
                env_tod_fixed = [uint32]$Matches[41]
                env_quake_ticks = [uint32]$Matches[42]
                env_cloud_scroll = [uint32]$Matches[43]
                env_rain_pct = [uint32]$Matches[44]
                env_overcast = [uint32]$Matches[45]
                env_param = [uint32]$Matches[46]
                objective_present = $Matches[47] -eq '1'
                objective_state_0 = [int]$Matches[48]
                objective_state_1 = [int]$Matches[49]
                objective_state_2 = [int]$Matches[50]
                objective_state_3 = [int]$Matches[51]
                passenger_present = $Matches[52] -eq '1'
                passenger_mount_handle = [uint32]$Matches[53]
                passenger_has_mount = $Matches[54] -eq '1'
                passenger_clip = [uint32]$Matches[55]
                passenger_reserve = [uint32]$Matches[56]
            })
            continue
        }
        if ($line -match '^PARITY_ENTITY frame=(\d+) ts_ns=(\d+) session=(\d+) class=([PVIN]) handle=(\d+) type=(\d+) pos=(\d+),(\d+),(\d+) orient=(-?\d+),(-?\d+) state=(\d+),(\d+) vital=(\d+)$') {
            $entities.Add([pscustomobject]@{
                frame = [int]$Matches[1]; ts_ns = [uint64]$Matches[2]; session = [int]$Matches[3]
                dir = 'S'; tag = '0a'; sid = [uint32]0; epoch = 0; connection_key = ''
                class = $Matches[4]; handle = [uint32]$Matches[5]; type_id = [uint32]$Matches[6]
                pos_x = [uint32]$Matches[7]; pos_y = [uint32]$Matches[8]; pos_z = [uint32]$Matches[9]
                orient_a = [int]$Matches[10]; orient_b = [int]$Matches[11]
                state_a = [uint32]$Matches[12]; state_b = [uint32]$Matches[13]; vital = [uint32]$Matches[14]
            })
            continue
        }
        if ($line.StartsWith('PARITY_')) {
            throw "Malformed or unsupported parity-event contract line: $line"
        }
    }

    # A client UDP port can be reused after reconnect. Build a fail-closed
    # connection epoch from both directional wire SIDs, then bind every
    # semantic line to its unique containing packet. Cross-direction joins use
    # connection_key, which contains the session, epoch, S SID, and C SID.
    foreach ($sessionGroup in @($packets | Group-Object session)) {
        $epoch = 0
        $current = @{ S=$null; C=$null }
        foreach ($packet in @($sessionGroup.Group | Sort-Object frame)) {
            $dir = $packet.dir
            if ($null -eq $current[$dir]) {
                $current[$dir] = $packet.sid
            } elseif ([uint32]$current[$dir] -ne [uint32]$packet.sid) {
                $epoch++
                $current = @{ S=$null; C=$null }
                $current[$dir] = $packet.sid
            }
            $packet.epoch = $epoch
        }
        foreach ($epochGroup in @($sessionGroup.Group | Group-Object epoch)) {
            $sSids = @($epochGroup.Group | Where-Object { $_.dir -eq 'S' } |
                ForEach-Object { $_.sid } | Select-Object -Unique)
            $cSids = @($epochGroup.Group | Where-Object { $_.dir -eq 'C' } |
                ForEach-Object { $_.sid } | Select-Object -Unique)
            if ($sSids.Count -gt 1 -or $cSids.Count -gt 1) {
                throw "Wire SID epoch contains multiple directional SIDs."
            }
            $sSid = if ($sSids.Count -eq 1) { '{0:x8}' -f [uint32]$sSids[0] } else { '-' }
            $cSid = if ($cSids.Count -eq 1) { '{0:x8}' -f [uint32]$cSids[0] } else { '-' }
            $key = "$($sessionGroup.Name)|$($epochGroup.Name)|S$sSid|C$cSid"
            foreach ($packet in $epochGroup.Group) { $packet.connection_key = $key }
        }
    }

    $ownerIndex = @{}
    foreach ($packet in $packets) {
        $packetIdentities = @{}
        foreach ($identity in $packet.tag_identities) {
            if ($packetIdentities.ContainsKey($identity)) { continue }
            $packetIdentities[$identity] = $true
            $key = "$($packet.frame)|$($packet.dir)|$($packet.session)|$identity"
            if (-not $ownerIndex.ContainsKey($key)) {
                $ownerIndex[$key] = [System.Collections.Generic.List[object]]::new()
            }
            $ownerIndex[$key].Add($packet)
        }
    }
    foreach ($event in $events) {
        $key = "$($event.frame)|$($event.dir)|$($event.session)|$($event.identity)"
        $owners = @(if ($ownerIndex.ContainsKey($key)) { $ownerIndex[$key].ToArray() })
        if ($owners.Count -ne 1) { throw "PARITY_EVENT '$key' binds to $($owners.Count) SID-bearing packets." }
        $event.sid = $owners[0].sid
        $event.epoch = $owners[0].epoch
        $event.connection_key = $owners[0].connection_key
    }
    foreach ($state in $states) {
        $identity = "$($state.tag):0"
        $key = "$($state.frame)|$($state.dir)|$($state.session)|$identity"
        $owners = @(if ($ownerIndex.ContainsKey($key)) { $ownerIndex[$key].ToArray() })
        if ($owners.Count -ne 1) { throw "PARITY_STATE does not bind to one ordinary SID-bearing packet." }
        $state.sid = $owners[0].sid
        $state.epoch = $owners[0].epoch
        $state.connection_key = $owners[0].connection_key
    }
    foreach ($entity in $entities) {
        $key = "$($entity.frame)|S|$($entity.session)|0a:0"
        $owners = @(if ($ownerIndex.ContainsKey($key)) { $ownerIndex[$key].ToArray() })
        if ($owners.Count -ne 1) { throw "PARITY_ENTITY does not bind to one ordinary SID-bearing packet." }
        $entity.sid = $owners[0].sid
        $entity.epoch = $owners[0].epoch
        $entity.connection_key = $owners[0].connection_key
    }

    $timestamps = @($packets | Where-Object { $_.ts_ns -gt 0 } | ForEach-Object { $_.ts_ns })
    $firstTs = if ($timestamps.Count -gt 0) { [uint64]($timestamps | Measure-Object -Minimum).Minimum } else { [uint64]0 }
    $lastTs = if ($timestamps.Count -gt 0) { [uint64]($timestamps | Measure-Object -Maximum).Maximum } else { [uint64]0 }
    $durationMs = if ($lastTs -ge $firstTs -and $firstTs -ne 0) {
        [double]($lastTs - $firstTs) / 1000000.0
    } else { 0.0 }

    return [pscustomobject]@{
        events = @($events.ToArray())
        packets = @($packets.ToArray())
        states = @($states.ToArray())
        entities = @($entities.ToArray())
        handshakes = @($handshakes.ToArray())
        datagrams = @($datagrams.ToArray())
        histogram = $histogram
        first_ts_ns = $firstTs
        last_ts_ns = $lastTs
        duration_ms = $durationMs
    }
}

function Invoke-CaptureDecode {
    param(
        [Parameter(Mandatory = $true)] [string] $NwPp,
        [Parameter(Mandatory = $true)] [string] $Capture,
        [Parameter(Mandatory = $true)] [string] $CaseId,
        [Parameter(Mandatory = $true)] [string] $Topology,
        [string] $ItemsPath = ""
    )
    $oldEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $decodeArgs = @($Capture, '--parity-events')
        if (-not [string]::IsNullOrWhiteSpace($ItemsPath)) {
            $decodeArgs += @('--items', $ItemsPath)
        }
        $lines = @(& $NwPp @decodeArgs 2>$null)
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $oldEap
    }
    if ($exitCode -ne 0) {
        Add-ParityFailure "DECODE_FAILED" $CaseId $Topology "nw_pp exited $exitCode for $Capture"
        return $null
    }
    $facts = ConvertFrom-ParityLines -Lines @($lines | ForEach-Object { "$_" })
    if ($facts.events.Count -eq 0 -or $facts.packets.Count -eq 0 -or
            $facts.datagrams.Count -eq 0) {
        Add-ParityFailure "EMPTY_DECODE" $CaseId $Topology `
            "Capture produced no session events, packets, or raw datagram accounting records."
        return $null
    }
    return $facts
}

function Resolve-ManifestPath {
    param([string] $Base, [string] $Value)
    if ([string]::IsNullOrWhiteSpace($Value)) { return "" }
    if ([IO.Path]::IsPathRooted($Value)) { return [IO.Path]::GetFullPath($Value) }
    if ($Value.StartsWith('repo:', [StringComparison]::OrdinalIgnoreCase)) {
        $relative = $Value.Substring(5).TrimStart('/', '\')
        $root = [IO.Path]::GetFullPath((Get-RepoRoot)).TrimEnd('\')
        $resolved = [IO.Path]::GetFullPath((Join-Path $root $relative))
        if (-not $resolved.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw "repo: manifest path escapes the repository: $Value"
        }
        return $resolved
    }
    if ($Value -match '^\.scratch[\\/]') {
        return [IO.Path]::GetFullPath((Join-Path (Get-RepoRoot) $Value))
    }
    return [IO.Path]::GetFullPath((Join-Path $Base $Value))
}

function ConvertFrom-StrictUtcIsoTimestamp {
    param([Parameter(Mandatory = $true)] [string] $Value)

    # The launch runner emits DateTime.UtcNow.ToString("o"). Accept the same
    # ISO-8601 UTC shape (with up to seven fractional digits), but reject
    # offsets, local/unspecified times, whitespace, and other permissive
    # DateTime parser inputs. Capture timestamps are Unix-epoch nanoseconds.
    if ($Value -notmatch '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d{1,7})?Z$') {
        throw "Timestamp is not strict ISO-8601 UTC: $Value"
    }
    [string[]]$formats = @("yyyy-MM-dd'T'HH:mm:ss'Z'")
    foreach ($fractionDigits in 1..7) {
        $formats += "yyyy-MM-dd'T'HH:mm:ss.$('f' * $fractionDigits)'Z'"
    }
    $styles = [Globalization.DateTimeStyles]::AssumeUniversal -bor
        [Globalization.DateTimeStyles]::AdjustToUniversal
    $parsed = [DateTimeOffset]::ParseExact(
        $Value, $formats, [Globalization.CultureInfo]::InvariantCulture, $styles)
    $unixEpochTicks = [DateTime]::new(
        1970, 1, 1, 0, 0, 0, [DateTimeKind]::Utc).Ticks
    $epochTicks = $parsed.UtcDateTime.Ticks - $unixEpochTicks
    if ($epochTicks -lt 0) { throw "Timestamp predates the Unix epoch: $Value" }
    return [uint64]$epochTicks * [uint64]100
}

function Get-FactCollection {
    param(
        [Parameter(Mandatory = $true)] $Facts,
        [Parameter(Mandatory = $true)] [string] $Preferred,
        [Parameter(Mandatory = $true)] [string] $Fallback
    )
    if (Test-Field $Facts $Preferred) { return @((Get-Field $Facts $Preferred @())) }
    return @((Get-Field $Facts $Fallback @()))
}

function Get-ComparisonHistogram {
    param([Parameter(Mandatory = $true)] $Facts)
    if (Test-Field $Facts 'comparison_histogram') {
        return Get-Field $Facts 'comparison_histogram' @{}
    }
    return Get-Field $Facts 'histogram' @{}
}

function Test-PacketGameplayTag {
    param($Packet, [Parameter(Mandatory = $true)] [string] $Direction)
    $identity = if ($Direction -eq 'C') { '0c:0' } else { '0a:0' }
    return @((Get-Field $Packet 'tag_identities' @()) | ForEach-Object {
        "$($_)".ToLowerInvariant()
    }) -contains $identity
}

function Get-InitialProtocolPrefixPackets {
    param([Parameter(Mandatory = $true)] $Facts)
    $prefix = [System.Collections.Generic.List[object]]::new()
    foreach ($direction in @('C','S')) {
        foreach ($packet in @((Get-Field $Facts 'packets' @()) |
                Where-Object { $_.dir -eq $direction } | Sort-Object frame)) {
            $prefix.Add($packet)
            if (Test-PacketGameplayTag $packet $direction) { break }
        }
    }
    return @($prefix.ToArray() | Sort-Object frame)
}

function Set-SteadyWindowFacts {
    param(
        [Parameter(Mandatory = $true)] $Facts,
        [Parameter(Mandatory = $true)] $Run,
        [Parameter(Mandatory = $true)] [string] $CaseId,
        [Parameter(Mandatory = $true)] [string] $Topology
    )

    $startText = "$(Get-Field $Run 'steady_started_utc' '')"
    $completedText = "$(Get-Field $Run 'steady_completed_utc' '')"
    try {
        [uint64]$startNs = ConvertFrom-StrictUtcIsoTimestamp $startText
        [uint64]$completedNs = ConvertFrom-StrictUtcIsoTimestamp $completedText
    } catch {
        Add-ParityFailure 'STEADY_TIMESTAMP_INVALID' $CaseId $Topology $_.Exception.Message
        return $false
    }
    if ($completedNs -le $startNs) {
        Add-ParityFailure 'STEADY_WINDOW_ORDER' $CaseId $Topology `
            'steady_completed_utc must be later than steady_started_utc.'
        return $false
    }

    $declaredSeconds = [double](Get-Field $Run 'steady_seconds' 0)
    $survivalMs = [double]($completedNs - $startNs) / 1000000.0
    $declaredMs = $declaredSeconds * 1000.0
    if ($declaredSeconds -le 0) {
        Add-ParityFailure 'STEADY_WINDOW_TOO_SHORT' $CaseId $Topology `
            'steady_seconds must be greater than zero.'
        return $false
    }
    [uint64]$declaredNs = [uint64][Math]::Round(
        $declaredSeconds * 1000000000.0, [MidpointRounding]::AwayFromZero)
    if ($declaredNs -eq 0 -or $startNs -gt [uint64]::MaxValue - $declaredNs) {
        Add-ParityFailure 'STEADY_WINDOW_TOO_SHORT' $CaseId $Topology `
            'steady_seconds cannot be represented as a capture timestamp interval.'
        return $false
    }
    [uint64]$sliceEndNs = $startNs + $declaredNs
    if ($completedNs -lt $sliceEndNs) {
        Add-ParityFailure 'STEADY_WINDOW_TOO_SHORT' $CaseId $Topology `
            "Endpoint survival interval $([Math]::Round($survivalMs, 3))ms is shorter than steady_seconds=${declaredSeconds}."
        return $false
    }
    $maximumOverrunMs = 15000.0
    if ($survivalMs -gt $declaredMs + $maximumOverrunMs) {
        Add-ParityFailure 'STEADY_WINDOW_TOO_LONG' $CaseId $Topology `
            "Endpoint survival interval $([Math]::Round($survivalMs, 3))ms exceeds steady_seconds by more than $maximumOverrunMs ms."
        return $false
    }

    $timestampCollections = @('packets','events','states','entities')
    if (Test-Field $Facts 'datagrams') { $timestampCollections += 'datagrams' }
    foreach ($collectionName in $timestampCollections) {
        foreach ($record in @((Get-Field $Facts $collectionName @()))) {
            if (-not (Test-Field $record 'ts_ns')) {
                Add-ParityFailure 'TIMESTAMP_MISSING' $CaseId $Topology `
                    "Decoded $collectionName record does not expose ts_ns."
                return $false
            }
            try { [uint64]$recordTimestamp = Get-Field $record 'ts_ns' 0 } catch {
                Add-ParityFailure 'TIMESTAMP_MISSING' $CaseId $Topology `
                    "Decoded $collectionName record has an invalid ts_ns."
                return $false
            }
            if ($recordTimestamp -eq 0) {
                Add-ParityFailure 'TIMESTAMP_MISSING' $CaseId $Topology `
                    "Decoded $collectionName record does not expose a nonzero timestamp."
                return $false
            }
        }
    }

    $packets = @((Get-Field $Facts 'packets' @()))
    $states = @((Get-Field $Facts 'states' @()))
    $boundaryToleranceNs = [uint64]500000000
    $maximumGameplayGapNs = [uint64]1000000000
    foreach ($direction in @('C','S')) {
        $expectedKind = if ($direction -eq 'C') { 'uplink' } else { 'frame' }
        [uint64[]]$gameplayTimestamps = @($states | Where-Object {
            "$(Get-Field $_ 'dir' '')" -eq $direction -and
                "$(Get-Field $_ 'kind' '')" -eq $expectedKind -and
                [bool](Get-Field $_ 'decode' $false) -and
                [uint64](Get-Field $_ 'ts_ns' 0) -ge $startNs -and
                [uint64](Get-Field $_ 'ts_ns' 0) -le $sliceEndNs
        } | ForEach-Object { [uint64]$_.ts_ns } | Sort-Object)
        if ($gameplayTimestamps.Count -eq 0 -or
                $gameplayTimestamps[0] -gt $startNs + $boundaryToleranceNs) {
            Add-ParityFailure 'STEADY_BOUNDARY_GAMEPLAY_MISSING' $CaseId $Topology `
                "$direction decoded gameplay state has no sample inside the first 500 ms of the declared steady interval."
        }
        if ($gameplayTimestamps.Count -eq 0 -or
                $gameplayTimestamps[$gameplayTimestamps.Count - 1] -lt
                    $sliceEndNs - $boundaryToleranceNs) {
            Add-ParityFailure 'STEADY_BOUNDARY_GAMEPLAY_MISSING' $CaseId $Topology `
                "$direction decoded gameplay state has no sample inside the last 500 ms of the declared steady interval."
        }
        if ($gameplayTimestamps.Count -gt 0) {
            [uint64[]]$coverage = @($startNs) + $gameplayTimestamps + @($sliceEndNs)
            for ($i = 1; $i -lt $coverage.Count; $i++) {
                if ($coverage[$i] - $coverage[$i - 1] -gt $maximumGameplayGapNs) {
                    $gapMs = [double]($coverage[$i] - $coverage[$i - 1]) / 1000000.0
                    Add-ParityFailure 'STEADY_GAMEPLAY_GAP' $CaseId $Topology `
                        "$direction decoded gameplay state has a $([Math]::Round($gapMs, 3)) ms gap inside the declared steady interval."
                    break
                }
            }
        }
    }
    if (@($script:Failures | Where-Object {
            $_.case_id -eq $CaseId -and $_.topology -eq $Topology -and
                $_.code -in @('STEADY_BOUNDARY_GAMEPLAY_MISSING','STEADY_GAMEPLAY_GAP')
        }).Count -gt 0) {
        return $false
    }

    $steadyPackets = @($packets | Where-Object {
        [uint64]$_.ts_ns -ge $startNs -and [uint64]$_.ts_ns -le $sliceEndNs
    })
    $initialPackets = @(Get-InitialProtocolPrefixPackets $Facts)
    $initialFrames = @{}
    foreach ($packet in $initialPackets) {
        $initialFrames["$($packet.frame)"] = $true
    }
    # Generic wire parity covers the complete connection through the declared
    # steady interval.  The readiness gate intentionally observes roughly ten
    # seconds of ordinary traffic before steady starts; dropping that warmup
    # would let delayed or transient OpenNova-only records evade the RR diff.
    # Steady liveness/density and dynamic-state checks remain sliced below.
    $comparisonPackets = @($packets | Where-Object {
        [uint64]$_.ts_ns -le $sliceEndNs
    })
    $selectedFrames = @{}
    foreach ($packet in $comparisonPackets) {
        $selectedFrames["$($packet.frame)"] = $true
    }
    $events = @((Get-Field $Facts 'events' @()))
    $entities = @((Get-Field $Facts 'entities' @()))
    $steadyEvents = @($events | Where-Object {
        [uint64]$_.ts_ns -ge $startNs -and [uint64]$_.ts_ns -le $sliceEndNs
    })
    $steadyStates = @($states | Where-Object {
        [uint64]$_.ts_ns -ge $startNs -and [uint64]$_.ts_ns -le $sliceEndNs
    })
    $steadyEntities = @($entities | Where-Object {
        [uint64]$_.ts_ns -ge $startNs -and [uint64]$_.ts_ns -le $sliceEndNs
    })
    $initialEvents = @($events | Where-Object {
        $initialFrames.ContainsKey("$($_.frame)")
    })
    $comparisonEvents = @($events | Where-Object {
        $selectedFrames.ContainsKey("$($_.frame)")
    })
    $comparisonStates = @($states | Where-Object {
        $selectedFrames.ContainsKey("$($_.frame)")
    })
    $comparisonEntities = @($entities | Where-Object {
        $selectedFrames.ContainsKey("$($_.frame)")
    })
    $datagrams = @((Get-Field $Facts 'datagrams' @()))
    $steadyDatagrams = @($datagrams | Where-Object {
        [uint64]$_.ts_ns -ge $startNs -and [uint64]$_.ts_ns -le $sliceEndNs
    })
    # The external pcapng tail marker is deliberately sent after endpoint
    # shutdown and therefore after the verdict interval. Everything through
    # the declared steady slice must still be classified and decoded.
    $comparisonDatagrams = @($datagrams | Where-Object {
        [uint64]$_.ts_ns -le $sliceEndNs
    })
    $comparisonHistogram = @{}
    foreach ($event in $comparisonEvents) {
        $key = "$($event.dir):$($event.identity)"
        if (-not $comparisonHistogram.ContainsKey($key)) { $comparisonHistogram[$key] = 0 }
        $comparisonHistogram[$key] = [int]$comparisonHistogram[$key] + 1
    }
    $comparisonFirstNs = if ($comparisonPackets.Count -gt 0) {
        [uint64]($comparisonPackets | Measure-Object ts_ns -Minimum).Minimum
    } else { $sliceEndNs }
    $comparisonDurationMs = if ($sliceEndNs -ge $comparisonFirstNs) {
        [double]($sliceEndNs - $comparisonFirstNs) / 1000000.0
    } else { 0.0 }

    $Facts | Add-Member -NotePropertyName capture_duration_ms `
        -NotePropertyValue ([double](Get-Field $Facts 'duration_ms' 0)) -Force
    $Facts | Add-Member -NotePropertyName steady_started_ns -NotePropertyValue $startNs -Force
    $Facts | Add-Member -NotePropertyName steady_completed_ns -NotePropertyValue $sliceEndNs -Force
    $Facts | Add-Member -NotePropertyName steady_survived_until_ns -NotePropertyValue $completedNs -Force
    $Facts | Add-Member -NotePropertyName steady_duration_ms -NotePropertyValue $declaredMs -Force
    $Facts | Add-Member -NotePropertyName steady_survival_ms -NotePropertyValue $survivalMs -Force
    $Facts | Add-Member -NotePropertyName duration_ms -NotePropertyValue $declaredMs -Force
    $Facts | Add-Member -NotePropertyName comparison_duration_ms `
        -NotePropertyValue $comparisonDurationMs -Force
    $Facts | Add-Member -NotePropertyName steady_packets -NotePropertyValue $steadyPackets -Force
    $Facts | Add-Member -NotePropertyName steady_events -NotePropertyValue $steadyEvents -Force
    $Facts | Add-Member -NotePropertyName steady_states -NotePropertyValue $steadyStates -Force
    $Facts | Add-Member -NotePropertyName steady_entities -NotePropertyValue $steadyEntities -Force
    $Facts | Add-Member -NotePropertyName steady_datagrams -NotePropertyValue $steadyDatagrams -Force
    $Facts | Add-Member -NotePropertyName initial_packets -NotePropertyValue $initialPackets -Force
    $Facts | Add-Member -NotePropertyName initial_events -NotePropertyValue $initialEvents -Force
    $Facts | Add-Member -NotePropertyName comparison_packets -NotePropertyValue $comparisonPackets -Force
    $Facts | Add-Member -NotePropertyName comparison_events -NotePropertyValue $comparisonEvents -Force
    $Facts | Add-Member -NotePropertyName comparison_states -NotePropertyValue $comparisonStates -Force
    $Facts | Add-Member -NotePropertyName comparison_entities -NotePropertyValue $comparisonEntities -Force
    $Facts | Add-Member -NotePropertyName comparison_datagrams -NotePropertyValue $comparisonDatagrams -Force
    $Facts | Add-Member -NotePropertyName comparison_histogram -NotePropertyValue $comparisonHistogram -Force
    return $true
}

function Test-RawDatagramContract {
    param($Facts, [string] $CaseId, [string] $Topology)

    $datagrams = @((Get-FactCollection $Facts 'comparison_datagrams' 'datagrams'))
    if ($datagrams.Count -eq 0) {
        Add-ParityFailure 'DATAGRAM_ACCOUNTING_MISSING' $CaseId $Topology `
            'No per-UDP-datagram accounting records cover the comparison interval.'
        return $false
    }
    $duplicateFrames = @($datagrams | Group-Object frame | Where-Object {
        $_.Count -ne 1
    })
    if ($duplicateFrames.Count -gt 0) {
        Add-ParityFailure 'DATAGRAM_ACCOUNTING_DUPLICATE' $CaseId $Topology `
            "Per-UDP-datagram accounting repeats capture frame(s): $(@($duplicateFrames.Name | Select-Object -First 8) -join ',')."
    }
    $bad = @($datagrams | Where-Object {
        -not [bool](Get-Field $_ 'outer_decoded' $false) -or
            -not [bool](Get-Field $_ 'decode' $false) -or
            "$(Get-Field $_ 'class' '')" -in @('invalid','unknown')
    })
    if ($bad.Count -gt 0) {
        $examples = @($bad | Select-Object -First 8 | ForEach-Object {
            "frame=$(Get-Field $_ 'frame' -1)/class=$(Get-Field $_ 'class' '')/opcode=$(if ($null -eq (Get-Field $_ 'opcode' $null)) { '--' } else { '0x{0:x2}' -f [byte]$_.opcode })"
        }) -join ', '
        Add-ParityFailure 'RAW_DATAGRAM_DECODE_FAILED' $CaseId $Topology `
            "$($bad.Count) UDP datagram(s) in the comparison interval were malformed, unknown, or not opcode-specifically decoded: $examples."
    }

    $protocolFrames = @($datagrams | Where-Object {
        "$(Get-Field $_ 'class' '')" -in @('client_protocol','server_protocol')
    } | ForEach-Object { [int]$_.frame } | Sort-Object)
    $packetFrames = @((Get-FactCollection $Facts 'comparison_packets' 'packets') |
        ForEach-Object { [int]$_.frame } | Sort-Object)
    if (($protocolFrames -join ',') -cne ($packetFrames -join ',')) {
        Add-ParityFailure 'DATAGRAM_PACKET_ACCOUNTING_MISMATCH' $CaseId $Topology `
            "Decoded protocol datagram frames do not exactly account for PARITY_PACKET frames (datagrams=$($protocolFrames.Count), packets=$($packetFrames.Count))."
    }
    return $bad.Count -eq 0 -and $duplicateFrames.Count -eq 0 -and
        (($protocolFrames -join ',') -ceq ($packetFrames -join ','))
}

function Get-InteriorSteadyPacketKeys {
    param($Facts, [string] $Direction)
    [uint64]$start = [uint64](Get-Field $Facts 'steady_started_ns' 0)
    [uint64]$end = [uint64](Get-Field $Facts 'steady_completed_ns' 0)
    $guard = [uint64]500000000
    if ($start -eq 0 -or $end -le $start + (2 * $guard)) { return @() }
    return @((Get-FactCollection $Facts 'steady_packets' 'packets') |
        Where-Object {
            "$(Get-Field $_ 'dir' '')" -ceq $Direction -and
                [uint64](Get-Field $_ 'ts_ns' 0) -ge $start + $guard -and
                [uint64](Get-Field $_ 'ts_ns' 0) -le $end - $guard
        } | ForEach-Object {
            "$Direction|sid=$('{0:x8}' -f [uint32](Get-Field $_ 'sid' 0))" +
                "|seq=$(Get-Field $_ 'sequence' 0)|ack=$(Get-Field $_ 'ack' 0)" +
                "|flags=$(Get-Field $_ 'flags' '')|records=$(Get-Field $_ 'records' -1)" +
                "|wire=$(Get-Field $_ 'wire_key' '')"
        } | Sort-Object -Unique)
}

function Test-ExternalCaptureCrossCheck {
    param($PrimaryFacts, $ExternalFacts, [string] $CaseId, [string] $Topology)
    $valid = $true
    foreach ($direction in @('C','S')) {
        $primaryKeys = @(Get-InteriorSteadyPacketKeys $PrimaryFacts $direction)
        $externalKeys = @(Get-InteriorSteadyPacketKeys $ExternalFacts $direction)
        if ($primaryKeys.Count -eq 0 -or $externalKeys.Count -eq 0 -or
                ($primaryKeys -join "`n") -cne ($externalKeys -join "`n")) {
            Add-ParityFailure 'EXTERNAL_CAPTURE_WIRE_MISMATCH' $CaseId $Topology `
                "$direction interior steady packet identities differ between the hook-owned evidence and independently captured pcapng (primary=$($primaryKeys.Count), external=$($externalKeys.Count))."
            $valid = $false
        }
    }
    return $valid
}

function Test-SemanticContract {
    param($Facts, [string] $CaseId, [string] $Topology)
    $valid = $true
    $uplinkEvents = @($Facts.events | Where-Object { $_.dir -eq 'C' -and $_.identity -eq '0c:0' })
    $frameEvents = @($Facts.events | Where-Object { $_.dir -eq 'S' -and $_.identity -eq '0a:0' })
    $uplinks = @($Facts.states | Where-Object { $_.kind -eq 'uplink' })
    $frames = @($Facts.states | Where-Object { $_.kind -eq 'frame' })
    if ($uplinkEvents.Count -eq 0 -or $frameEvents.Count -eq 0) {
        Add-ParityFailure "SEMANTIC_SURFACE_MISSING" $CaseId $Topology `
            "A gameplay capture must contain both C2S 0x0C uplinks and S2C 0x0A frames."
        $valid = $false
    }
    $gameplayConnections = @($uplinkEvents + $frameEvents |
        ForEach-Object { $_.connection_key } | Select-Object -Unique)
    if ($gameplayConnections.Count -ne 1) {
        Add-ParityFailure "SEMANTIC_CONNECTION_AMBIGUOUS" $CaseId $Topology `
            "A parity run must contain one SID-bound gameplay connection; saw $($gameplayConnections.Count)."
        $valid = $false
    }
    if ($uplinks.Count -ne $uplinkEvents.Count -or $frames.Count -ne $frameEvents.Count) {
        Add-ParityFailure "SEMANTIC_EVENT_COUNT" $CaseId $Topology `
            "Every C2S 0x0C/S2C 0x0A event must have exactly one decoded PARITY_STATE record."
        $valid = $false
    }

    $eventIndex = @{}
    $semanticOwnerKeys = [System.Collections.Generic.HashSet[string]]::new()
    foreach ($event in @($uplinkEvents + $frameEvents)) {
        $key = "$($event.frame)|$($event.dir)|$($event.connection_key)|$($event.sid)|$($event.tag)|$($event.ts_ns)"
        if (-not $eventIndex.ContainsKey($key)) { $eventIndex[$key] = 0 }
        $eventIndex[$key]++
        $null = $semanticOwnerKeys.Add($key)
    }
    $stateIndex = @{}
    foreach ($state in $Facts.states) {
        $key = "$($state.frame)|$($state.dir)|$($state.connection_key)|$($state.sid)|$($state.tag)|$($state.ts_ns)"
        if (-not $stateIndex.ContainsKey($key)) { $stateIndex[$key] = 0 }
        $stateIndex[$key]++
        $null = $semanticOwnerKeys.Add($key)
    }
    $correlationFailures = 0
    foreach ($key in $semanticOwnerKeys) {
        $eventCount = if ($eventIndex.ContainsKey($key)) { [int]$eventIndex[$key] } else { 0 }
        $stateCount = if ($stateIndex.ContainsKey($key)) { [int]$stateIndex[$key] } else { 0 }
        if ($eventCount -ne $stateCount) {
            $correlationFailures++
        }
    }
    if ($correlationFailures -gt 0) {
        Add-ParityFailure "SEMANTIC_EVENT_CORRELATION" $CaseId $Topology `
            "$correlationFailures packet/tag group(s) have unequal ordinary gameplay event/state multiplicities."
        $valid = $false
    }
    foreach ($group in @($Facts.states | Where-Object { -not $_.decode } | Group-Object dir,tag)) {
        Add-ParityFailure "SEMANTIC_DECODE_FAILED" $CaseId $Topology `
            "$($group.Count) $($group.Name) semantic record(s) did not decode completely; the hash-bound corpus classifier or semantic codec is unsupported."
        $valid = $false
    }

    $entityIndex = @{}
    $shapeOwnerKeys = [System.Collections.Generic.HashSet[string]]::new()
    foreach ($entity in $Facts.entities) {
        $key = "$($entity.frame)|$($entity.connection_key)|$($entity.sid)|$($entity.ts_ns)"
        if (-not $entityIndex.ContainsKey($key)) {
            $entityIndex[$key] = [System.Collections.Generic.List[object]]::new()
        }
        $entityIndex[$key].Add($entity)
        $null = $shapeOwnerKeys.Add($key)
    }
    $frameIndex = @{}
    foreach ($frame in $frames) {
        $key = "$($frame.frame)|$($frame.connection_key)|$($frame.sid)|$($frame.ts_ns)"
        if (-not $frameIndex.ContainsKey($key)) {
            $frameIndex[$key] = [System.Collections.Generic.List[object]]::new()
        }
        $frameIndex[$key].Add($frame)
        $null = $shapeOwnerKeys.Add($key)
    }
    $shapeFailures = 0
    foreach ($key in $shapeOwnerKeys) {
        $atStates = @(if ($frameIndex.ContainsKey($key)) { $frameIndex[$key].ToArray() })
        $atFrame = @(if ($entityIndex.ContainsKey($key)) { $entityIndex[$key].ToArray() })
        $classCounts = @{
            P = @($atFrame | Where-Object { $_.class -eq 'P' }).Count
            V = @($atFrame | Where-Object { $_.class -eq 'V' }).Count
            I = @($atFrame | Where-Object { $_.class -eq 'I' }).Count
            N = @($atFrame | Where-Object { $_.class -eq 'N' }).Count
        }
        $declared = @{
            records = [int](($atStates | Measure-Object -Property records -Sum).Sum)
            P = [int](($atStates | Measure-Object -Property players -Sum).Sum)
            V = [int](($atStates | Measure-Object -Property vehicles -Sum).Sum)
            I = [int](($atStates | Measure-Object -Property infantry -Sum).Sum)
            N = [int](($atStates | Measure-Object -Property none -Sum).Sum)
        }
        if (@($atStates | Where-Object { -not $_.local_tail }).Count -gt 0 -or
                $atFrame.Count -ne $declared.records -or
                $classCounts.P -ne $declared.P -or $classCounts.V -ne $declared.V -or
                $classCounts.I -ne $declared.I -or $classCounts.N -ne $declared.N) {
            $shapeFailures++
        }
    }
    if ($shapeFailures -gt 0) {
        Add-ParityFailure "SEMANTIC_SHAPE_MISMATCH" $CaseId $Topology `
            "$shapeFailures S2C 0x0A packet group(s) have inconsistent aggregate state/entity counts or fixed local tails."
        $valid = $false
    }
    return $valid
}

function Test-CaptureStatsDeclaration {
    param($Run, [string] $CaseId, [string] $Topology)
    $available = Get-Field $Run 'capture_stats_available' $null
    if ($available -isnot [bool]) {
        Add-ParityFailure "CAPTURE_STATS_TYPE" $CaseId $Topology `
            "capture_stats_available must be a JSON boolean."
        return
    }
    $counters = @('dropped','truncated','write_errors','unsupported_async')
    if (-not $available) {
        $perspective = "$(Get-Field $Run 'perspective' '')"
        if ($Topology -ne 'OO' -or $perspective -ne 'external') {
            Add-ParityFailure "CAPTURE_STATS_REQUIRED" $CaseId $Topology `
                "Hook captures RR/RO/OR must publish capture stats; unavailable stats are permitted only for OO/external."
        }
        if (@($counters | Where-Object { Test-Field $Run $_ }).Count -gt 0) {
            Add-ParityFailure "CAPTURE_STATS_COUNTERS_INVENTED" $CaseId $Topology `
                "A run with capture_stats_available=false must omit unavailable counter fields."
        }
        return
    }
    foreach ($counter in $counters) {
        if (-not (Test-Field $Run $counter)) {
            Add-ParityFailure "CAPTURE_STATS_MISSING" $CaseId $Topology `
                "Capture stats are available but counter '$counter' is absent."
        } elseif ([int64](Get-Field $Run $counter -1) -ne 0) {
            Add-ParityFailure "CAPTURE_COUNTER" $CaseId $Topology `
                "Capture counter '$counter' is nonzero."
        }
    }
}

function Test-ExternalCaptureCompleteness {
    param($Facts, [string] $CaseId, [string] $Topology)
    $packets = @($Facts.packets)
    $events = @($Facts.events)
    if ($packets.Count -eq 0 -or $events.Count -eq 0) {
        Add-ParityFailure "EXTERNAL_CAPTURE_EMPTY" $CaseId $Topology `
            "Counter-less external evidence must decode nonempty SID-bearing packets and events."
        return
    }

    $sequenceGroups = @{}
    foreach ($packet in $packets) {
        $connection = "$(Get-Field $packet 'connection_key' '')"
        $sid = [uint32](Get-Field $packet 'sid' 0)
        if ([string]::IsNullOrWhiteSpace($connection) -or $sid -eq 0) {
            Add-ParityFailure "EXTERNAL_CAPTURE_SID_AMBIGUOUS" $CaseId $Topology `
                "Every external-capture packet must bind to a nonzero wire SID and connection epoch."
            continue
        }
        $key = "$connection|$($packet.dir)|$sid"
        if (-not $sequenceGroups.ContainsKey($key)) {
            $sequenceGroups[$key] = [System.Collections.Generic.List[uint32]]::new()
        }
        $sequenceGroups[$key].Add([uint32]$packet.sequence)
    }
    foreach ($key in $sequenceGroups.Keys) {
        $sequences = @($sequenceGroups[$key].ToArray() | Sort-Object -Unique)
        for ($i = 1; $i -lt $sequences.Count; $i++) {
            $expected = [uint32]((
                [uint64]$sequences[$i - 1] + [uint64]1) -band [uint64]4294967295)
            if ([uint32]$sequences[$i] -ne $expected) {
                Add-ParityFailure "EXTERNAL_CAPTURE_SEQUENCE_GAP" $CaseId $Topology `
                    "External capture sequence $key jumps from $($sequences[$i - 1]) to $($sequences[$i])."
                break
            }
        }
    }

    $eventCounts = @{}
    foreach ($event in $events) {
        $key = "$($event.frame)|$($event.ts_ns)|$($event.dir)|$($event.connection_key)|$($event.sid)|$($event.identity)"
        if (-not $eventCounts.ContainsKey($key)) { $eventCounts[$key] = 0 }
        $eventCounts[$key]++
    }
    $groupMismatch = $false
    foreach ($packet in $packets) {
        $packetCounts = @{}
        foreach ($identity in @($packet.tag_identities)) {
            if (-not $packetCounts.ContainsKey($identity)) { $packetCounts[$identity] = 0 }
            $packetCounts[$identity]++
        }
        if ([int](Get-Field $packet 'records' -1) -ne @($packet.tag_identities).Count) {
            $groupMismatch = $true
            break
        }
        foreach ($identity in $packetCounts.Keys) {
            $key = "$($packet.frame)|$($packet.ts_ns)|$($packet.dir)|$($packet.connection_key)|$($packet.sid)|$identity"
            $actual = if ($eventCounts.ContainsKey($key)) { [int]$eventCounts[$key] } else { 0 }
            if ($actual -ne [int]$packetCounts[$identity]) {
                $groupMismatch = $true
                break
            }
        }
        if ($groupMismatch) { break }
    }
    if ($groupMismatch) {
        Add-ParityFailure "EXTERNAL_CAPTURE_GROUP_INCOMPLETE" $CaseId $Topology `
            "External capture packet record groups do not have a one-for-one decoded event image."
    }
}

function Read-ExternalPcapngUInt32 {
    param(
        [Parameter(Mandatory = $true)] [byte[]] $Bytes,
        [Parameter(Mandatory = $true)] [int] $Offset,
        [Parameter(Mandatory = $true)] [bool] $LittleEndian
    )
    if ($Offset -lt 0 -or $Offset + 4 -gt $Bytes.Length) {
        throw 'pcapng uint32 lies outside its block'
    }
    [byte[]]$word = @(
        $Bytes[$Offset], $Bytes[$Offset + 1],
        $Bytes[$Offset + 2], $Bytes[$Offset + 3]
    )
    if ([BitConverter]::IsLittleEndian -ne $LittleEndian) {
        [Array]::Reverse($word)
    }
    return [BitConverter]::ToUInt32($word, 0)
}

function Read-ExternalPcapngExactly {
    param(
        [Parameter(Mandatory = $true)] [IO.Stream] $Stream,
        [Parameter(Mandatory = $true)] [byte[]] $Buffer,
        [Parameter(Mandatory = $true)] [int] $Offset,
        [Parameter(Mandatory = $true)] [int] $Count,
        [Parameter(Mandatory = $true)] [string] $Context
    )
    $readTotal = 0
    while ($readTotal -lt $Count) {
        $readNow = $Stream.Read($Buffer, $Offset + $readTotal, $Count - $readTotal)
        if ($readNow -le 0) { throw "Truncated pcapng while reading $Context" }
        $readTotal += $readNow
    }
}

function Find-ExternalPcapngBytes {
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

# Parse the persisted artifact independently of capture.ps1 and of the
# generator. Every topology gets structural exact-EOF validation. When a tail
# marker is supplied (OO), it must occur inside the last packet block: the
# runner sends it only after both endpoints are dead and the game port is
# unowned, durably ordering all endpoint traffic before EOF.
function Get-ExternalPcapngTailFacts {
    param(
        [Parameter(Mandatory = $true)] [string] $Path,
        [byte[]] $TailMarker = [byte[]]@()
    )
    $requireTailMarker = $TailMarker.Count -gt 0
    $stream = [IO.File]::Open(
        $Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        $fileLength = $stream.Length
        if ($fileLength -le 0) { throw 'pcapng artifact is empty' }
        $blockCount = 0
        $packetBlockCount = 0
        $sectionCount = 0
        $littleEndian = $null
        $markerBlockIndex = -1
        $lastPacketBlockIndex = -1

        while ($stream.Position -lt $fileLength) {
            $blockOffset = $stream.Position
            $remaining = $fileLength - $blockOffset
            if ($remaining -lt 12) {
                throw "Truncated pcapng tail: $remaining byte(s) remain after block $blockCount"
            }
            [byte[]]$header = [byte[]]::new(12)
            Read-ExternalPcapngExactly $stream $header 0 12 "block $blockCount header"
            $isSectionHeader = $header[0] -eq 0x0a -and $header[1] -eq 0x0d -and
                $header[2] -eq 0x0d -and $header[3] -eq 0x0a
            if ($blockCount -eq 0 -and -not $isSectionHeader) {
                throw 'pcapng does not begin with a Section Header Block'
            }
            if ($isSectionHeader) {
                if ($header[8] -eq 0x4d -and $header[9] -eq 0x3c -and
                        $header[10] -eq 0x2b -and $header[11] -eq 0x1a) {
                    $littleEndian = $true
                } elseif ($header[8] -eq 0x1a -and $header[9] -eq 0x2b -and
                        $header[10] -eq 0x3c -and $header[11] -eq 0x4d) {
                    $littleEndian = $false
                } else {
                    throw "Section Header Block $blockCount has invalid byte-order magic"
                }
                $sectionCount++
            } elseif ($null -eq $littleEndian) {
                throw "pcapng block $blockCount precedes a Section Header Block"
            }

            $blockLength = [long](Read-ExternalPcapngUInt32 $header 4 ([bool]$littleEndian))
            if ($blockLength -lt 12 -or ($blockLength % 4) -ne 0) {
                throw "pcapng block $blockCount has invalid total length $blockLength"
            }
            if ($isSectionHeader -and $blockLength -lt 28) {
                throw "Section Header Block $blockCount is shorter than 28 bytes"
            }
            if ($blockLength -gt $remaining -or $blockLength -gt [int]::MaxValue) {
                throw "pcapng block $blockCount length $blockLength exceeds the remaining artifact"
            }
            [byte[]]$block = [byte[]]::new([int]$blockLength)
            [Array]::Copy($header, 0, $block, 0, 12)
            if ($blockLength -gt 12) {
                Read-ExternalPcapngExactly $stream $block 12 ([int]$blockLength - 12) `
                    "block $blockCount body"
            }
            $trailingLength = [long](Read-ExternalPcapngUInt32 `
                $block ([int]$blockLength - 4) ([bool]$littleEndian))
            if ($trailingLength -ne $blockLength) {
                throw "pcapng block $blockCount header/trailer length mismatch"
            }

            $blockType = Read-ExternalPcapngUInt32 $block 0 ([bool]$littleEndian)
            $packetDataOffset = -1
            $packetDataLength = 0
            if ($blockType -eq 6 -or $blockType -eq 2) {
                if ($blockLength -lt 32) {
                    throw "pcapng packet block $blockCount is shorter than 32 bytes"
                }
                $capturedLength = [long](Read-ExternalPcapngUInt32 `
                    $block 20 ([bool]$littleEndian))
                if ($capturedLength -gt $blockLength - 32) {
                    throw "pcapng packet block $blockCount captured length exceeds its body"
                }
                $packetDataOffset = 28
                $packetDataLength = [int]$capturedLength
            } elseif ($blockType -eq 3) {
                if ($blockLength -lt 16) {
                    throw "pcapng simple packet block $blockCount is shorter than 16 bytes"
                }
                $originalLength = [long](Read-ExternalPcapngUInt32 `
                    $block 8 ([bool]$littleEndian))
                $packetDataOffset = 12
                $packetDataLength = [int][Math]::Min($originalLength, [int]$blockLength - 16)
            }
            if ($packetDataOffset -ge 0) {
                $packetBlockCount++
                $lastPacketBlockIndex = $blockCount
                if ($requireTailMarker -and
                        (Find-ExternalPcapngBytes $block $packetDataOffset `
                            $packetDataLength $TailMarker) -ge 0) {
                    $markerBlockIndex = $blockCount
                }
            }
            $blockCount++
        }
        if ($stream.Position -ne $fileLength) { throw 'pcapng parser did not end at exact EOF' }
        if ($sectionCount -le 0) { throw 'pcapng contains no Section Header Block' }
        if ($packetBlockCount -le 0) { throw 'pcapng contains no packet blocks' }
        if ($requireTailMarker -and $markerBlockIndex -lt 0) {
            throw 'tail marker is absent from packet data'
        }
        if ($requireTailMarker -and $markerBlockIndex -ne $lastPacketBlockIndex) {
            throw 'tail marker is not in the final packet block'
        }
        return [pscustomobject]@{
            block_count = $blockCount
            packet_block_count = $packetBlockCount
            marker_block_index = $markerBlockIndex
            exact_eof = $true
        }
    } finally {
        $stream.Dispose()
    }
}

function Test-ExternalCaptureTailProof {
    param(
        $Run, [string] $Capture, [string] $CaptureSha256,
        [string] $CaseId, [string] $Topology
    )
    if (-not (Test-Field $Run 'external_capture_proof')) {
        Add-ParityFailure 'EXTERNAL_CAPTURE_PROOF_MISSING' $CaseId $Topology `
            'Every topology requires an independently verified external pcapng proof.'
        return $false
    }
    $proof = Get-Field $Run 'external_capture_proof' $null
    try {
        $tailRequiredValue = Get-Field $proof 'tail_marker_required' $null
        $tailFoundValue = Get-Field $proof 'tail_marker_found' $null
        $expectTail = $Topology -eq 'OO'
        if ((Get-Field $proof 'valid' $null) -isnot [bool] -or
                -not [bool](Get-Field $proof 'valid' $false) -or
                (Get-Field $proof 'exact_eof' $null) -isnot [bool] -or
                -not [bool](Get-Field $proof 'exact_eof' $false) -or
                $tailRequiredValue -isnot [bool] -or
                $tailFoundValue -isnot [bool] -or
                [bool]$tailRequiredValue -ne $expectTail -or
                [bool]$tailFoundValue -ne $expectTail) {
            throw 'valid/exact_eof must be literal true, and tail-marker booleans must exactly match the topology contract'
        }
        $artifactHash = Normalize-Sha256 "$(Get-Field $proof 'artifact_sha256' '')"
        if ($artifactHash -ne $CaptureSha256) {
            throw 'artifact_sha256 does not match the verified capture bytes'
        }
        $actual = Get-ExternalPcapngTailFacts $Capture
        if ([int64](Get-Field $proof 'block_count' -1) -ne
                    [int64]$actual.block_count -or
                [int64](Get-Field $proof 'packet_block_count' -1) -ne
                    [int64]$actual.packet_block_count) {
            throw 'declared pcapng block counts do not match an independent parse'
        }
        if ($expectTail) {
            $markerHex = "$(Get-Field $proof 'tail_marker_hex' '')"
            if ($markerHex -cne $markerHex.ToLowerInvariant() -or
                    $markerHex -notmatch '^(?:[0-9a-f]{2}){8,}$') {
                throw 'tail_marker_hex must be canonical lowercase hex containing at least 8 bytes'
            }
            [byte[]]$marker = ConvertFrom-HexBytes $markerHex
            $runId = "$(Get-Field $Run 'run_id' '')"
            if ($runId -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$') {
                throw 'OO run_id is absent or is not a canonical identifier'
            }
            $strictUtf8 = [Text.UTF8Encoding]::new($false, $true)
            $markerText = $strictUtf8.GetString($marker)
            $markerPattern = '^OPENNOVA-PCAPNG-TAIL\|' +
                [regex]::Escape($runId) + '\|[0-9a-f]{32}$'
            if ($markerText -cnotmatch $markerPattern) {
                throw 'tail marker bytes are not uniquely bound to this OO run_id'
            }
            if ("$(Get-Field $proof 'tail_marker_text' '')" -cne $markerText) {
                throw 'tail_marker_text does not exactly match the decoded marker bytes'
            }
            $sha = [Security.Cryptography.SHA256]::Create()
            try { $markerDigest = $sha.ComputeHash($marker) }
            finally { $sha.Dispose() }
            $actualMarkerHash = ([BitConverter]::ToString(
                $markerDigest)).Replace('-', '').ToLowerInvariant()
            if ((Normalize-Sha256 "$(Get-Field $proof 'tail_marker_sha256' '')") -ne
                    $actualMarkerHash) {
                throw 'tail_marker_sha256 does not match tail_marker_hex'
            }
            [uint64]$markerSentNs = ConvertFrom-StrictUtcIsoTimestamp `
                "$(Get-Field $proof 'tail_marker_sent_utc' '')"
            [uint64]$steadyCompletedNs = ConvertFrom-StrictUtcIsoTimestamp `
                "$(Get-Field $Run 'steady_completed_utc' '')"
            $joinerShutdown = Get-Field $Run 'opennova_joiner_shutdown_witness' $null
            $hostShutdown = Get-Field $Run 'opennova_host_shutdown_witness' $null
            [uint64]$joinerCompletedNs = ConvertFrom-StrictUtcIsoTimestamp `
                "$(Get-Field $joinerShutdown 'completed_utc' '')"
            [uint64]$hostCompletedNs = ConvertFrom-StrictUtcIsoTimestamp `
                "$(Get-Field $hostShutdown 'completed_utc' '')"
            if ($markerSentNs -lt $steadyCompletedNs -or
                    $markerSentNs -lt $joinerCompletedNs -or
                    $markerSentNs -lt $hostCompletedNs) {
                throw 'tail marker was sent before steady completion and both OpenNova endpoint shutdowns were recorded'
            }
            $null = Get-ExternalPcapngTailFacts $Capture $marker
        }
    } catch {
        Add-ParityFailure 'EXTERNAL_CAPTURE_PROOF_INVALID' $CaseId $Topology $_.Exception.Message
        return $false
    }
    return $true
}

function Normalize-Sha256 {
    param([string] $Value)
    $normalized = $Value.Trim().ToLowerInvariant()
    if ($normalized.StartsWith("sha256:")) { $normalized = $normalized.Substring(7) }
    return $normalized
}

function Register-UniqueMissionHash {
    param(
        [hashtable] $Seen,
        [string] $Sha256,
        [string] $CaseId,
        [string] $Mission = $CaseId
    )
    if ($Seen.ContainsKey($Sha256)) {
        $prior = $Seen[$Sha256]
        if ([string]::Equals(
                "$(Get-Field $prior 'mission' '')", $Mission,
                [StringComparison]::OrdinalIgnoreCase)) {
            return $true
        }
        Add-ParityFailure "MISSION_BYTES_REUSED" $CaseId "" `
            "Mission '$Mission' duplicates BMS bytes bound to distinct mission '$(Get-Field $prior 'mission' '')' in case $(Get-Field $prior 'case_id' '')."
        return $false
    }
    $Seen[$Sha256] = [pscustomobject]@{case_id=$CaseId;mission=$Mission}
    return $true
}

function Test-MissionArtifactBinding {
    param(
        [string] $Mission,
        [string] $Path,
        [string] $Sha256,
        [string] $ExpectedSha256,
        [string] $CaseId,
        [string] $Topology
    )
    $isRun = -not [string]::IsNullOrWhiteSpace($Topology)
    $nameCode = if ($isRun) { "RUN_MISSION_ARTIFACT_NAME" } else { "MISSION_ARTIFACT_NAME" }
    $hashCode = if ($isRun) { "RUN_MISSION_ARTIFACT_HASH" } else { "MISSION_ARTIFACT_HASH" }
    $normalized = Normalize-Sha256 $Sha256
    $leaf = ""
    if (-not [string]::IsNullOrWhiteSpace($Path)) {
        $leaf = [IO.Path]::GetFileName($Path.Replace('/', '\'))
    }
    if ([string]::IsNullOrWhiteSpace($Mission) -or
            -not $Mission.EndsWith('.bms', [StringComparison]::OrdinalIgnoreCase) -or
            -not $leaf.Equals($Mission, [StringComparison]::OrdinalIgnoreCase)) {
        Add-ParityFailure $nameCode $CaseId $Topology `
            "Mission artifact leaf '$leaf' must exactly identify case mission '$Mission' as a .bms file."
    }
    if ($normalized -notmatch '^[0-9a-f]{64}$' -or $normalized -eq ('0' * 64)) {
        Add-ParityFailure $hashCode $CaseId $Topology `
            "Mission artifact SHA-256 must be a pinned nonzero digest."
    }
    if ($isRun -and -not [string]::IsNullOrWhiteSpace($ExpectedSha256)) {
        $expected = Normalize-Sha256 $ExpectedSha256
        if ($normalized -ne $expected) {
            Add-ParityFailure "RUN_MISSION_HASH_MISMATCH" $CaseId $Topology `
                "Run mission SHA-256 does not match its case's BMS artifact."
        }
    }
    return $normalized
}

function Get-StringSha256 {
    param([Parameter(Mandatory = $true)] [string] $Value)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [Text.Encoding]::UTF8.GetBytes($Value)
        return ([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
    } finally {
        $sha.Dispose()
    }
}

function Test-EffectiveRetailConfigs {
    param(
        $Run, $Case, [string] $ManifestDir,
        [string] $CaseId, [string] $Topology, $Corpus,
        [hashtable] $ArtifactPaths
    )
    $expectedRoles = @(switch ($Topology) {
        'RR' { 'client'; 'server' }
        'RO' { 'server' }
        'OR' { 'client' }
    }) | Sort-Object
    $configs = @(Get-Field $Run 'effective_configs' @())
    $actualRoles = @($configs | ForEach-Object {
        "$(Get-Field $_ 'role' '')".Trim().ToLowerInvariant()
    } | Sort-Object)
    if (($actualRoles -join ',') -ne ($expectedRoles -join ',')) {
        Add-ParityFailure 'EFFECTIVE_CONFIG_ROLES' $CaseId $Topology `
            "Effective retail cfg roles differ: expected=$($expectedRoles -join ',') actual=$($actualRoles -join ',')."
        return
    }
    foreach ($config in $configs) {
        $role = "$(Get-Field $config 'role' '')".Trim().ToLowerInvariant()
        $path = Resolve-ManifestPath $ManifestDir "$(Get-Field $config 'path' '')"
        $declaredHash = Normalize-Sha256 "$(Get-Field $config 'sha256' '')"
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            Add-ParityFailure 'EFFECTIVE_CONFIG_MISSING' $CaseId $Topology `
                "Effective retail $role cfg is missing: $path"
            continue
        }
        $actualHash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($declaredHash -notmatch '^[0-9a-f]{64}$' -or
                $declaredHash -eq ('0' * 64) -or $declaredHash -ne $actualHash) {
            Add-ParityFailure 'EFFECTIVE_CONFIG_HASH' $CaseId $Topology `
                "Effective retail $role cfg SHA-256 does not match its bytes."
            continue
        }
        $expectedTemplateRole = "${role}_cfg_template"
        $templateRole = "$(Get-Field $config 'template_role' '')".Trim().ToLowerInvariant()
        $declaredBaseHash = Normalize-Sha256 "$(Get-Field $config 'base_sha256' '')"
        if ($templateRole -ne $expectedTemplateRole -or
                -not $ArtifactPaths.ContainsKey($expectedTemplateRole)) {
            Add-ParityFailure 'EFFECTIVE_CONFIG_TEMPLATE' $CaseId $Topology `
                "Effective retail $role cfg is not bound to corpus artifact '$expectedTemplateRole'."
            continue
        }
        try {
            $effectiveBase = Get-OnHookConfigBase -Path $path
            $templateBase = Get-OnHookConfigBase -Path $ArtifactPaths[$expectedTemplateRole]
        } catch {
            Add-ParityFailure 'EFFECTIVE_CONFIG_BASE' $CaseId $Topology $_.Exception.Message
            continue
        }
        if ($declaredBaseHash -notmatch '^[0-9a-f]{64}$' -or
                $declaredBaseHash -eq ('0' * 64) -or
                $declaredBaseHash -ne [string] $effectiveBase.Sha256 -or
                [string] $effectiveBase.Sha256 -ne [string] $templateBase.Sha256 -or
                [string] $effectiveBase.Canonical -ne [string] $templateBase.Canonical) {
            Add-ParityFailure 'EFFECTIVE_CONFIG_BASE' $CaseId $Topology `
                "Effective retail $role fixed settings differ from the hash-bound tracked template."
        }
        $text = Get-Content -LiteralPath $path -Raw
        $required = if ($role -eq 'server') {
            @{
                LanHostMission = "$(Get-Field $Case 'mission' '')"
                LanHostPort = "$(Get-Field $Case 'port' '')"
                LanHostCallsign = "$(Get-Field $Case 'host_callsign' '')"
                LanHostGameType = "$(Get-Field $Case 'game_type' '')"
                LanHostMaxPlayers = '4'
                OpenNovaExpansion = "$(Get-Field $Corpus 'expansion' '')"
            }
        } else {
            @{
                LanJoinAddress = '127.0.0.1'
                LanJoinPort = "$(Get-Field $Case 'port' '')"
                LanJoinCallsign = "$(Get-Field $Case 'joiner_callsign' '')"
                OpenNovaExpansion = "$(Get-Field $Corpus 'expansion' '')"
            }
        }
        foreach ($key in $required.Keys) {
            $pattern = "(?m)^\s*$([regex]::Escape($key))\s+$([regex]::Escape($required[$key]))\s*$"
            if ([regex]::Matches($text, $pattern).Count -ne 1) {
                Add-ParityFailure 'EFFECTIVE_CONFIG_METADATA' $CaseId $Topology `
                    "Effective retail $role cfg lacks one exact $key=$($required[$key]) setting."
            }
        }
    }
}

function Test-MotionGateWitness {
    param(
        $Run, $Case, [string] $ManifestDir,
        [string] $CaseId, [string] $Topology
    )
    $expected = [bool](Get-Field $Case 'exercise_input' $false) -and
        $Topology -in @('RO','OO')
    if (-not $expected) {
        if (Test-Field $Run 'motion_gate_witness') {
            Add-ParityFailure 'MOTION_GATE_UNEXPECTED' $CaseId $Topology `
                'Only exercised OpenNova-joiner runs may carry motion_gate_witness.'
            return $false
        }
        return $true
    }
    if (-not (Test-Field $Run 'motion_gate_witness')) {
        Add-ParityFailure 'MOTION_GATE_MISSING' $CaseId $Topology `
            'Exercised OpenNova-joiner evidence requires a steady-window motion gate.'
        return $false
    }
    $witness = Get-Field $Run 'motion_gate_witness' $null
    try {
        $runId = "$(Get-Field $Run 'run_id' '')"
        $path = Resolve-ManifestPath $ManifestDir "$(Get-Field $witness 'path' '')"
        $expectedPath = [IO.Path]::GetFullPath((Join-Path (Get-RepoRoot) `
            ".scratch\runs\$runId\joiner\motion-gate.json"))
        if ($path -ne $expectedPath -or -not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw 'motion gate is not the owned create-new run artifact'
        }
        [byte[]]$raw = [IO.File]::ReadAllBytes($path)
        if ($raw.Length -eq 0 -or ($raw.Length -ge 3 -and
                $raw[0] -eq 0xef -and $raw[1] -eq 0xbb -and $raw[2] -eq 0xbf)) {
            throw 'motion gate JSON is empty or has a UTF-8 BOM'
        }
        $actualHash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        if ((Normalize-Sha256 "$(Get-Field $witness 'sha256' '')") -ne $actualHash) {
            throw 'motion gate SHA-256 does not match its bytes'
        }
        $gate = [Text.Encoding]::UTF8.GetString($raw) | ConvertFrom-Json
        $expectedFields = @{
            schema=1
            run_id=$runId
            topology=$Topology
            joiner_pid=[int](Get-Field $witness 'joiner_pid' 0)
            steady_started_utc="$(Get-Field $Run 'steady_started_utc' '')"
            created_utc="$(Get-Field $witness 'created_utc' '')"
            pre_roll_ms=250
            inter_phase_gap_ms=250
            look_samples_per_phase=20
            forward_ms=1800
            forward_look=320
            strafe_ms=1200
            strafe_look=-320
            return_ms=900
            return_look=160
        }
        if ([int]$expectedFields.joiner_pid -le 0) {
            throw 'motion gate joiner_pid must be positive'
        }
        foreach ($field in $expectedFields.Keys) {
            if (-not (Test-Field $gate $field) -or
                    "$(Get-Field $gate $field '')" -cne "$($expectedFields[$field])" -or
                    ($field -ne 'schema' -and (
                        -not (Test-Field $witness $field) -or
                        "$(Get-Field $witness $field '')" -cne "$($expectedFields[$field])"))) {
                throw "motion gate field '$field' differs from the run/witness contract"
            }
        }
        [uint64]$startNs = ConvertFrom-StrictUtcIsoTimestamp `
            "$(Get-Field $Run 'steady_started_utc' '')"
        [uint64]$completedNs = ConvertFrom-StrictUtcIsoTimestamp `
            "$(Get-Field $Run 'steady_completed_utc' '')"
        [uint64]$createdNs = ConvertFrom-StrictUtcIsoTimestamp `
            "$(Get-Field $witness 'created_utc' '')"
        [uint64]$declaredWriteNs = ConvertFrom-StrictUtcIsoTimestamp `
            "$(Get-Field $witness 'file_last_write_utc' '')"
        $actualWriteText = (Get-Item -LiteralPath $path).LastWriteTimeUtc.ToString('o')
        [uint64]$actualWriteNs = ConvertFrom-StrictUtcIsoTimestamp $actualWriteText
        if ($createdNs -lt $startNs -or $createdNs -gt $startNs + [uint64]2000000000 -or
                $createdNs -ge $completedNs -or $declaredWriteNs -ne $actualWriteNs -or
                $actualWriteNs -lt $createdNs - [uint64]1000000000 -or
                $actualWriteNs -gt $startNs + [uint64]3000000000) {
            throw 'motion gate was not durably published at steady-window start'
        }
    } catch {
        Add-ParityFailure 'MOTION_GATE_INVALID' $CaseId $Topology $_.Exception.Message
        return $false
    }
    return $true
}

function Test-RetailInputWitness {
    param(
        $Run, $Case, [string] $ManifestDir,
        [string] $CaseId, [string] $Topology
    )
    $expected = [bool](Get-Field $Case 'exercise_input' $false) -and
        $Topology -in @('RR','OR')
    if (-not $expected) {
        if (Test-Field $Run 'retail_input_witness') {
            Add-ParityFailure 'RETAIL_INPUT_WITNESS_UNEXPECTED' $CaseId $Topology `
                'Only exercised retail-joiner RR/OR runs may carry retail_input_witness.'
            return $false
        }
        return $true
    }
    if (-not (Test-Field $Run 'retail_input_witness')) {
        Add-ParityFailure 'RETAIL_INPUT_WITNESS_MISSING' $CaseId $Topology `
            'Exercised retail-joiner RR/OR evidence requires retail_input_witness.'
        return $false
    }
    $witness = Get-Field $Run 'retail_input_witness' $null
    try {
        Assert-ExactWireReadyProperties $witness @(
            'path','sha256','schema','complete','run_id','topology','pid',
            'process_start_utc','window_handle','started_utc','completed_utc',
            'forward_ms','strafe_ms','return_ms','turn_pixels','injected_events',
            'focus_attempts','foreground_checks'
        ) 'manifest retail-input witness'
        $runId = "$(Get-Field $Run 'run_id' '')"
        $declaredPath = "$(Get-Field $witness 'path' '')"
        $expectedDeclaredPath = "repo:.scratch/runs/$runId/retail-input.json"
        $path = Resolve-ManifestPath $ManifestDir $declaredPath
        $expectedPath = [IO.Path]::GetFullPath((Join-Path (Get-RepoRoot) `
            ".scratch\runs\$runId\retail-input.json"))
        if ($declaredPath -cne $expectedDeclaredPath -or $path -ne $expectedPath -or
                -not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw 'retail-input witness is not the owned repo-relative create-new run artifact'
        }
        [byte[]]$raw = [IO.File]::ReadAllBytes($path)
        if ($raw.Length -eq 0 -or ($raw.Length -ge 3 -and
                $raw[0] -eq 0xef -and $raw[1] -eq 0xbb -and $raw[2] -eq 0xbf)) {
            throw 'retail-input JSON is empty or has a UTF-8 BOM'
        }
        $actualHash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        $declaredHash = "$(Get-Field $witness 'sha256' '')"
        if ($declaredHash -cnotmatch '^[0-9a-f]{64}$' -or $declaredHash -cne $actualHash) {
            throw 'retail-input SHA-256 does not match its bytes'
        }
        $disk = [Text.Encoding]::UTF8.GetString($raw) | ConvertFrom-Json
        Assert-ExactWireReadyProperties $disk @(
            'schema','complete','run_id','topology','pid','process_start_utc',
            'window_handle','started_utc','completed_utc','forward_ms','strafe_ms',
            'return_ms','turn_pixels','injected_events','focus_attempts',
            'foreground_checks'
        ) 'retail-input artifact'
        foreach ($field in @(
            'schema','run_id','topology','process_start_utc','started_utc','completed_utc'
        )) {
            if ((Get-Field $witness $field $null) -isnot [string] -or
                    (Get-Field $disk $field $null) -isnot [string] -or
                    "$(Get-Field $witness $field '')" -cne "$(Get-Field $disk $field '')") {
                throw "retail-input string field '$field' differs from the manifest binding"
            }
        }
        if ((Get-Field $witness 'complete' $null) -isnot [bool] -or
                (Get-Field $disk 'complete' $null) -isnot [bool] -or
                -not [bool](Get-Field $witness 'complete' $false) -or
                -not [bool](Get-Field $disk 'complete' $false)) {
            throw 'retail-input complete must be the JSON boolean true'
        }
        $numericFields = @(
            'pid','window_handle','forward_ms','strafe_ms','return_ms','turn_pixels',
            'injected_events','focus_attempts','foreground_checks'
        )
        foreach ($field in $numericFields) {
            $manifestValue = Get-Field $witness $field $null
            $diskValue = Get-Field $disk $field $null
            if ($manifestValue -is [string] -or $manifestValue -is [bool] -or
                    $diskValue -is [string] -or $diskValue -is [bool] -or
                    [int64]$manifestValue -ne [int64]$diskValue) {
                throw "retail-input numeric field '$field' differs from the manifest binding"
            }
        }
        if ([string]$disk.schema -cne 'opennova.retail-input.v1' -or
                [string]$disk.run_id -cne $runId -or
                [string]$disk.topology -cne $Topology -or
                [int]$disk.pid -le 0 -or [int64]$disk.window_handle -le 0 -or
                [int]$disk.forward_ms -ne 1800 -or
                [int]$disk.strafe_ms -ne 1200 -or
                [int]$disk.return_ms -ne 900 -or
                [int]$disk.turn_pixels -ne 320 -or
                [int]$disk.injected_events -ne 66 -or
                [int]$disk.focus_attempts -lt 3 -or [int]$disk.focus_attempts -gt 30 -or
                [int]$disk.foreground_checks -lt 195 -or
                [int]$disk.foreground_checks -gt 222) {
            throw 'retail-input run/topology/PID/window/trajectory contract drifted'
        }
        [uint64]$steadyStartNs = ConvertFrom-StrictUtcIsoTimestamp `
            "$(Get-Field $Run 'steady_started_utc' '')"
        [uint64]$steadyCompletedNs = ConvertFrom-StrictUtcIsoTimestamp `
            "$(Get-Field $Run 'steady_completed_utc' '')"
        [uint64]$processStartNs = ConvertFrom-StrictUtcIsoTimestamp `
            "$(Get-Field $disk 'process_start_utc' '')"
        [uint64]$inputStartNs = ConvertFrom-StrictUtcIsoTimestamp `
            "$(Get-Field $disk 'started_utc' '')"
        [uint64]$inputCompletedNs = ConvertFrom-StrictUtcIsoTimestamp `
            "$(Get-Field $disk 'completed_utc' '')"
        if ($processStartNs -gt $steadyStartNs -or $processStartNs -gt $inputStartNs -or
                $inputStartNs -lt $steadyStartNs -or
                $inputCompletedNs -le $inputStartNs -or
                $inputCompletedNs -gt $steadyCompletedNs) {
            throw 'retail-input exercise interval is not wholly within the steady window'
        }
    } catch {
        Add-ParityFailure 'RETAIL_INPUT_WITNESS_INVALID' $CaseId $Topology $_.Exception.Message
        return $false
    }
    return $true
}

function Test-OpenNovaJoinerReadinessWitness {
    param(
        $Run, $Case, [string] $CaseId, [string] $Topology
    )
    $expected = $Topology -in @('RO', 'OO')
    $hasWitness = Test-Field $Run 'opennova_joiner_readiness_witness'
    if (-not $expected) {
        if ($hasWitness) {
            Add-ParityFailure 'OPENNOVA_JOINER_READINESS_UNEXPECTED' `
                $CaseId $Topology `
                'Only RO/OO runs may carry an OpenNova joiner readiness witness.'
            return $false
        }
        return $true
    }
    if (-not $hasWitness) {
        Add-ParityFailure 'OPENNOVA_JOINER_READINESS_MISSING' $CaseId $Topology `
            'RO/OO evidence requires initial and final OpenNova joiner heartbeats.'
        return $false
    }

    $witness = Get-Field $Run 'opennova_joiner_readiness_witness' $null
    $initial = Get-Field $witness 'initial' $null
    $final = Get-Field $witness 'final' $null
    $readinessMode = "$(Get-Field $Case 'readiness_mode' '')"
    $allowPostDeath = $readinessMode -eq 'in_match'
    $valid = "$(Get-Field $witness 'schema' '')" -ceq
                'opennova.parity-joiner-readiness.v2' -and
            "$(Get-Field $witness 'readiness_mode' '')" -ceq $readinessMode -and
            $null -ne $initial -and $null -ne $final -and
            (Test-OpenNovaJoinerReadinessTransition -Initial $initial `
                -Final $final -ReadinessMode $readinessMode `
                -AllowPostDeathRedeploy:$allowPostDeath `
                -RequireMotionComplete)
    $finalClass = if ($valid) {
        Get-OpenNovaJoinerReadinessClass -State $final `
            -ReadinessMode $readinessMode `
            -AllowPostDeathRedeploy:$allowPostDeath
    } else {
        'invalid'
    }
    if (-not $valid -or
            "$(Get-Field $witness 'final_class' '')" -cne $finalClass) {
        Add-ParityFailure 'OPENNOVA_JOINER_READINESS_INVALID' $CaseId $Topology `
            'OpenNova joiner readiness does not prove an admitted, live initial-to-final transition.'
        return $false
    }

    $launchProofs = Get-Field $Run 'opennova_launch_proofs' $null
    $joinerProof = Get-Field $launchProofs 'joiner' $null
    $runtimePid = [int](Get-Field $joinerProof 'runtime_pid' 0)
    if ($runtimePid -le 0 -or
            [int](Get-Field $initial 'process_id' 0) -ne $runtimePid -or
            [int](Get-Field $final 'process_id' 0) -ne $runtimePid) {
        Add-ParityFailure 'OPENNOVA_JOINER_READINESS_PID' $CaseId $Topology `
            'OpenNova joiner heartbeats do not belong to the launched runtime PID.'
        return $false
    }
    return $true
}

function Test-OpenNovaJoinerShutdownWitness {
    param(
        $Run, [string] $ManifestDir,
        [string] $CaseId, [string] $Topology
    )
    $expected = $Topology -in @('RO','OO')
    $hasWitness = Test-Field $Run 'opennova_joiner_shutdown_witness'
    $hasFallback = Test-Field $Run 'opennova_joiner_exact_fallback_required'
    if (-not $expected) {
        if ($hasWitness -or $hasFallback) {
            Add-ParityFailure 'OPENNOVA_JOINER_SHUTDOWN_UNEXPECTED' $CaseId $Topology `
                'Only RO/OO runs may carry OpenNova joiner shutdown evidence.'
            return $false
        }
        return $true
    }
    if (-not $hasWitness -or -not $hasFallback) {
        Add-ParityFailure 'OPENNOVA_JOINER_SHUTDOWN_MISSING' $CaseId $Topology `
            'RO/OO evidence requires a cooperative OpenNova joiner shutdown witness and fallback declaration.'
        return $false
    }
    if ((Get-Field $Run 'opennova_joiner_exact_fallback_required' $null) -isnot [bool] -or
            [bool](Get-Field $Run 'opennova_joiner_exact_fallback_required' $true)) {
        Add-ParityFailure 'OPENNOVA_JOINER_SHUTDOWN_FALLBACK' $CaseId $Topology `
            'Verdict-bearing RO/OO runs may not require exact-process shutdown fallback.'
        return $false
    }
    $witness = Get-Field $Run 'opennova_joiner_shutdown_witness' $null
    try {
        Assert-ExactWireReadyProperties $witness @(
            'request_path','request_sha256','path','sha256','schema','run_id',
            'topology','process_id','requested_utc','completed_utc','clean'
        ) 'manifest OpenNova joiner shutdown witness'
        $runId = "$(Get-Field $Run 'run_id' '')"
        $expectedRequestValue = "repo:.scratch/runs/$runId/joiner/joiner-stop-request.json"
        $expectedAckValue = "repo:.scratch/runs/$runId/joiner/joiner-shutdown.json"
        $requestValue = "$(Get-Field $witness 'request_path' '')"
        $ackValue = "$(Get-Field $witness 'path' '')"
        $requestPath = Resolve-ManifestPath $ManifestDir $requestValue
        $ackPath = Resolve-ManifestPath $ManifestDir $ackValue
        if ($requestValue -cne $expectedRequestValue -or $ackValue -cne $expectedAckValue -or
                -not (Test-Path -LiteralPath $requestPath -PathType Leaf) -or
                -not (Test-Path -LiteralPath $ackPath -PathType Leaf)) {
            throw 'OpenNova joiner shutdown artifacts are not the exact repo-owned run paths'
        }
        [byte[]]$requestRaw = [IO.File]::ReadAllBytes($requestPath)
        [byte[]]$ackRaw = [IO.File]::ReadAllBytes($ackPath)
        foreach ($artifact in @(
            [pscustomobject]@{name='request';raw=$requestRaw},
            [pscustomobject]@{name='ack';raw=$ackRaw}
        )) {
            if ($artifact.raw.Length -eq 0 -or ($artifact.raw.Length -ge 3 -and
                    $artifact.raw[0] -eq 0xef -and $artifact.raw[1] -eq 0xbb -and
                    $artifact.raw[2] -eq 0xbf)) {
                throw "OpenNova joiner shutdown $($artifact.name) is empty or has a UTF-8 BOM"
            }
        }
        $actualRequestHash = (Get-FileHash -LiteralPath $requestPath `
            -Algorithm SHA256).Hash.ToLowerInvariant()
        $actualAckHash = (Get-FileHash -LiteralPath $ackPath `
            -Algorithm SHA256).Hash.ToLowerInvariant()
        if ("$(Get-Field $witness 'request_sha256' '')" -cne $actualRequestHash -or
                "$(Get-Field $witness 'sha256' '')" -cne $actualAckHash) {
            throw 'OpenNova joiner shutdown request/ack SHA-256 does not match its bytes'
        }
        $request = [Text.Encoding]::UTF8.GetString($requestRaw) | ConvertFrom-Json
        $ack = [Text.Encoding]::UTF8.GetString($ackRaw) | ConvertFrom-Json
        Assert-ExactWireReadyProperties $request @(
            'schema','run_id','topology','pid','requested_utc'
        ) 'OpenNova joiner stop request'
        Assert-ExactWireReadyProperties $ack @(
            'schema','run_id','topology','process_id','requested_utc',
            'completed_utc','clean'
        ) 'OpenNova joiner shutdown acknowledgement'
        $launchProofs = Get-Field $Run 'opennova_launch_proofs' ([pscustomobject]@{})
        $joinerProof = Get-Field $launchProofs 'joiner' $null
        $expectedPid = [int](Get-Field $joinerProof 'runtime_pid' 0)
        if ($expectedPid -le 0 -or
                (Get-Field $witness 'request_path' $null) -isnot [string] -or
                (Get-Field $witness 'request_sha256' $null) -isnot [string] -or
                (Get-Field $witness 'path' $null) -isnot [string] -or
                (Get-Field $witness 'sha256' $null) -isnot [string] -or
                (Get-Field $witness 'schema' $null) -isnot [string] -or
                (Get-Field $witness 'run_id' $null) -isnot [string] -or
                (Get-Field $witness 'topology' $null) -isnot [string] -or
                -not ((Get-Field $witness 'process_id' $null) -is [int] -or
                    (Get-Field $witness 'process_id' $null) -is [long]) -or
                (Get-Field $witness 'requested_utc' $null) -isnot [string] -or
                (Get-Field $witness 'completed_utc' $null) -isnot [string] -or
                (Get-Field $witness 'clean' $null) -isnot [bool] -or
                -not [bool](Get-Field $witness 'clean' $false) -or
                (Get-Field $request 'schema' $null) -isnot [string] -or
                (Get-Field $request 'run_id' $null) -isnot [string] -or
                (Get-Field $request 'topology' $null) -isnot [string] -or
                -not ((Get-Field $request 'pid' $null) -is [int] -or
                    (Get-Field $request 'pid' $null) -is [long]) -or
                (Get-Field $request 'requested_utc' $null) -isnot [string] -or
                (Get-Field $ack 'schema' $null) -isnot [string] -or
                (Get-Field $ack 'run_id' $null) -isnot [string] -or
                (Get-Field $ack 'topology' $null) -isnot [string] -or
                -not ((Get-Field $ack 'process_id' $null) -is [int] -or
                    (Get-Field $ack 'process_id' $null) -is [long]) -or
                (Get-Field $ack 'requested_utc' $null) -isnot [string] -or
                (Get-Field $ack 'completed_utc' $null) -isnot [string] -or
                [string](Get-Field $request 'schema' '') -cne
                    'opennova.parity-joiner-stop-request.v1' -or
                [string](Get-Field $request 'run_id' '') -cne $runId -or
                [string](Get-Field $request 'topology' '') -cne $Topology -or
                [int](Get-Field $request 'pid' 0) -ne $expectedPid -or
                [string](Get-Field $ack 'schema' '') -cne
                    'opennova.parity-joiner-shutdown.v1' -or
                [string](Get-Field $ack 'run_id' '') -cne $runId -or
                [string](Get-Field $ack 'topology' '') -cne $Topology -or
                [int](Get-Field $ack 'process_id' 0) -ne $expectedPid -or
                [string](Get-Field $ack 'requested_utc' '') -cne
                    [string](Get-Field $request 'requested_utc' '') -or
                (Get-Field $ack 'clean' $null) -isnot [bool] -or
                -not [bool](Get-Field $ack 'clean' $false)) {
            throw 'OpenNova joiner shutdown request/ack does not bind the exact launched runtime'
        }
        foreach ($field in @(
            'schema','run_id','topology','process_id','requested_utc',
            'completed_utc','clean'
        )) {
            if ("$(Get-Field $witness $field '')" -cne "$(Get-Field $ack $field '')") {
                throw "OpenNova joiner shutdown manifest field '$field' differs from the ack"
            }
        }
        [uint64]$steadyCompletedNs = ConvertFrom-StrictUtcIsoTimestamp `
            "$(Get-Field $Run 'steady_completed_utc' '')"
        [uint64]$requestedNs = ConvertFrom-StrictUtcIsoTimestamp `
            "$(Get-Field $ack 'requested_utc' '')"
        [uint64]$completedNs = ConvertFrom-StrictUtcIsoTimestamp `
            "$(Get-Field $ack 'completed_utc' '')"
        if ($requestedNs -lt $steadyCompletedNs -or $completedNs -lt $requestedNs -or
                $completedNs -gt $requestedNs + [uint64]20000000000) {
            throw 'OpenNova joiner shutdown did not follow steady completion within 20 seconds'
        }
    } catch {
        Add-ParityFailure 'OPENNOVA_JOINER_SHUTDOWN_INVALID' $CaseId $Topology `
            $_.Exception.Message
        return $false
    }
    return $true
}

function Test-OpenNovaHostShutdownWitness {
    param(
        $Run, [string] $ManifestDir,
        [string] $CaseId, [string] $Topology
    )
    $expected = $Topology -in @('OR','OO')
    $hasWitness = Test-Field $Run 'opennova_host_shutdown_witness'
    $hasFallback = Test-Field $Run 'opennova_host_exact_fallback_required'
    if (-not $expected) {
        if ($hasWitness -or $hasFallback) {
            Add-ParityFailure 'OPENNOVA_HOST_SHUTDOWN_UNEXPECTED' $CaseId $Topology `
                'Only OR/OO runs may carry OpenNova host shutdown evidence.'
            return $false
        }
        return $true
    }
    if (-not $hasWitness -or -not $hasFallback) {
        Add-ParityFailure 'OPENNOVA_HOST_SHUTDOWN_MISSING' $CaseId $Topology `
            'OR/OO evidence requires an OpenNova host shutdown witness and fallback declaration.'
        return $false
    }
    if ((Get-Field $Run 'opennova_host_exact_fallback_required' $null) -isnot [bool] -or
            [bool](Get-Field $Run 'opennova_host_exact_fallback_required' $true)) {
        Add-ParityFailure 'OPENNOVA_HOST_SHUTDOWN_FALLBACK' $CaseId $Topology `
            'Verdict-bearing OR/OO runs may not require exact-process host shutdown fallback.'
        return $false
    }

    $witness = Get-Field $Run 'opennova_host_shutdown_witness' $null
    try {
        Assert-ExactWireReadyProperties $witness @(
            'schema','run_id','topology','process_id','requested_utc',
            'completed_utc','close_requested','exit_code','clean','log_path',
            'log_sha256','forbidden_diagnostics_absent'
        ) 'manifest OpenNova host shutdown witness'

        $runId = "$(Get-Field $Run 'run_id' '')"
        $expectedLogValue = "repo:.scratch/runs/$runId/host/godot.log"
        $logValue = "$(Get-Field $witness 'log_path' '')"
        $logPath = Resolve-ManifestPath $ManifestDir $logValue
        if ($logValue -cne $expectedLogValue -or
                -not (Test-Path -LiteralPath $logPath -PathType Leaf)) {
            throw 'OpenNova host shutdown log is not the exact repo-owned run path'
        }

        [byte[]]$logRaw = [IO.File]::ReadAllBytes($logPath)
        $hasUtf8Bom = $logRaw.Length -ge 3 -and
            $logRaw[0] -eq 0xef -and $logRaw[1] -eq 0xbb -and $logRaw[2] -eq 0xbf
        $hasUtf16Bom = $logRaw.Length -ge 2 -and
            (($logRaw[0] -eq 0xff -and $logRaw[1] -eq 0xfe) -or
             ($logRaw[0] -eq 0xfe -and $logRaw[1] -eq 0xff))
        $hasUtf32Bom = $logRaw.Length -ge 4 -and
            (($logRaw[0] -eq 0x00 -and $logRaw[1] -eq 0x00 -and
              $logRaw[2] -eq 0xfe -and $logRaw[3] -eq 0xff) -or
             ($logRaw[0] -eq 0xff -and $logRaw[1] -eq 0xfe -and
              $logRaw[2] -eq 0x00 -and $logRaw[3] -eq 0x00))
        if ($logRaw.Length -eq 0 -or $hasUtf8Bom -or $hasUtf16Bom -or
                $hasUtf32Bom -or [Array]::IndexOf($logRaw, [byte]0) -ge 0) {
            throw 'OpenNova host shutdown log is empty, has a BOM, or contains NUL bytes'
        }
        $actualLogHash = (Get-FileHash -LiteralPath $logPath `
            -Algorithm SHA256).Hash.ToLowerInvariant()
        if ((Get-Field $witness 'log_sha256' $null) -isnot [string] -or
                "$(Get-Field $witness 'log_sha256' '')" -cne $actualLogHash) {
            throw 'OpenNova host shutdown log SHA-256 does not match its bytes'
        }
        $strictUtf8 = [Text.UTF8Encoding]::new($false, $true)
        $logText = $strictUtf8.GetString($logRaw)
        $forbiddenDiagnostics = @(
            [pscustomobject]@{
                name='ObjectDB leak'
                pattern='(?im)\bObjectDB\b[^\r\n]*\bleak(?:ed|s|ing)?\b'
            },
            [pscustomobject]@{
                name='resources still in use'
                pattern='(?im)\bresources?\b[^\r\n]*(?:\bstill in use\b|\bleak(?:ed|s|ing)?\b)'
            },
            [pscustomobject]@{
                name='RenderingServer teardown failure'
                pattern='(?im)\bRenderingServer\b[^\r\n]*(?:\bstill in use\b|\bleak(?:ed|s|ing)?\b|get_singleton\(\))'
            },
            [pscustomobject]@{
                name='RID leak'
                pattern='(?im)\bRID(?:s|\s+allocations?)?\b[^\r\n]*\bleak(?:ed|s|ing)?\b'
            }
        )
        foreach ($diagnostic in $forbiddenDiagnostics) {
            if ([regex]::IsMatch($logText, [string]$diagnostic.pattern)) {
                throw "OpenNova host shutdown log contains forbidden $($diagnostic.name) diagnostics"
            }
        }

        $launchProofs = Get-Field $Run 'opennova_launch_proofs' ([pscustomobject]@{})
        $hostProof = Get-Field $launchProofs 'host' $null
        $expectedPid = [int64](Get-Field $hostProof 'runtime_pid' 0)
        if ((Get-Field $witness 'schema' $null) -isnot [string] -or
                (Get-Field $witness 'run_id' $null) -isnot [string] -or
                (Get-Field $witness 'topology' $null) -isnot [string] -or
                -not ((Get-Field $witness 'process_id' $null) -is [int] -or
                    (Get-Field $witness 'process_id' $null) -is [long]) -or
                (Get-Field $witness 'requested_utc' $null) -isnot [string] -or
                (Get-Field $witness 'completed_utc' $null) -isnot [string] -or
                (Get-Field $witness 'close_requested' $null) -isnot [bool] -or
                -not ((Get-Field $witness 'exit_code' $null) -is [int] -or
                    (Get-Field $witness 'exit_code' $null) -is [long]) -or
                (Get-Field $witness 'clean' $null) -isnot [bool] -or
                (Get-Field $witness 'log_path' $null) -isnot [string] -or
                (Get-Field $witness 'forbidden_diagnostics_absent' $null) -isnot [bool] -or
                (Get-Field $hostProof 'runtime_start_utc' $null) -isnot [string] -or
                $expectedPid -le 0 -or
                [string](Get-Field $witness 'schema' '') -cne
                    'opennova.parity-host-shutdown.v1' -or
                [string](Get-Field $witness 'run_id' '') -cne $runId -or
                [string](Get-Field $witness 'topology' '') -cne $Topology -or
                [int64](Get-Field $witness 'process_id' 0) -ne $expectedPid -or
                -not [bool](Get-Field $witness 'close_requested' $false) -or
                [int64](Get-Field $witness 'exit_code' -1) -ne 0 -or
                -not [bool](Get-Field $witness 'clean' $false) -or
                -not [bool](Get-Field $witness 'forbidden_diagnostics_absent' $false)) {
            throw 'OpenNova host shutdown witness does not bind a clean close to the exact launched runtime'
        }

        [uint64]$processStartedNs = ConvertFrom-StrictUtcIsoTimestamp `
            "$(Get-Field $hostProof 'runtime_start_utc' '')"
        [uint64]$steadyCompletedNs = ConvertFrom-StrictUtcIsoTimestamp `
            "$(Get-Field $Run 'steady_completed_utc' '')"
        [uint64]$requestedNs = ConvertFrom-StrictUtcIsoTimestamp `
            "$(Get-Field $witness 'requested_utc' '')"
        [uint64]$completedNs = ConvertFrom-StrictUtcIsoTimestamp `
            "$(Get-Field $witness 'completed_utc' '')"
        if ($requestedNs -lt $processStartedNs -or
                $requestedNs -lt $steadyCompletedNs -or
                $completedNs -lt $requestedNs -or
                $completedNs -gt $requestedNs + [uint64]20000000000) {
            throw 'OpenNova host shutdown timestamps do not follow process/steady completion and finish within 20 seconds'
        }
    } catch {
        Add-ParityFailure 'OPENNOVA_HOST_SHUTDOWN_INVALID' $CaseId $Topology `
            $_.Exception.Message
        return $false
    }
    return $true
}

function Test-OpenNovaLaunchProofs {
    param(
        $Run,
        $Case,
        $Corpus,
        [string] $CaseId,
        [string] $Topology,
        [hashtable] $ArtifactPaths
    )

    $expectedRoles = @(switch ($Topology) {
        'RO' { 'joiner' }
        'OR' { 'host' }
        'OO' { 'host'; 'joiner' }
    }) | Sort-Object
    $proofs = Get-Field $Run 'opennova_launch_proofs' ([pscustomobject]@{})
    $actualRoles = @($proofs.PSObject.Properties.Name | Sort-Object)
    if (($actualRoles -join ',') -cne ($expectedRoles -join ',')) {
        Add-ParityFailure 'OPENNOVA_LAUNCH_ROLE_SET' $CaseId $Topology `
            "OpenNova launch-proof roles '$($actualRoles -join ',')' do not exactly match '$($expectedRoles -join ',')'."
        return
    }

    foreach ($role in $expectedRoles) {
        $proof = Get-Field $proofs $role $null
        $requiredFields = @(
            'schema','wrapper_used','launcher_pid','launcher_path',
            'launcher_start_utc','launcher_created_utc','launcher_command_line',
            'runtime_pid','runtime_parent_pid','runtime_path','runtime_start_utc',
            'runtime_created_utc','runtime_command_line','launcher_console_host_pids',
            'arguments','parent_verified','argv_verified','active_execution_verified',
            'proven_utc'
        )
        try {
            Assert-ExactWireReadyProperties $proof $requiredFields `
                "OpenNova $role launch proof"
        } catch {
            Add-ParityFailure 'OPENNOVA_LAUNCH_SCHEMA' $CaseId $Topology `
                $_.Exception.Message
            continue
        }
        $stringFields = @(
            'schema','launcher_path','launcher_start_utc','launcher_created_utc',
            'launcher_command_line','runtime_path','runtime_start_utc',
            'runtime_created_utc','runtime_command_line','proven_utc'
        )
        $typeErrors = [System.Collections.Generic.List[string]]::new()
        foreach ($field in $stringFields) {
            if ((Get-Field $proof $field $null) -isnot [string]) {
                $typeErrors.Add("${field}:string")
            }
        }
        foreach ($field in @('launcher_pid','runtime_pid','runtime_parent_pid')) {
            $value = Get-Field $proof $field $null
            if (-not ($value -is [int] -or $value -is [long]) -or [int64]$value -le 0) {
                $typeErrors.Add("${field}:positive-integer")
            }
        }
        if ($proof.PSObject.Properties['arguments'].Value -isnot [System.Array]) {
            $typeErrors.Add('arguments:array')
        }
        if ($proof.PSObject.Properties['launcher_console_host_pids'].Value `
                -isnot [System.Array]) {
            $typeErrors.Add('launcher_console_host_pids:array')
        }
        if ($typeErrors.Count -ne 0) {
            Add-ParityFailure 'OPENNOVA_LAUNCH_PROOF_TYPE' $CaseId $Topology `
                "OpenNova $role launch proof fields have invalid types: $($typeErrors -join ', ')."
            continue
        }
        if ("$(Get-Field $proof 'schema' '')" -cne 'opennova.godot-process-ownership.v1') {
            Add-ParityFailure 'OPENNOVA_LAUNCH_SCHEMA' $CaseId $Topology `
                "OpenNova $role launch proof has an unsupported schema."
        }
        if ((Get-Field $proof 'wrapper_used' $null) -isnot [bool] -or
                -not [bool](Get-Field $proof 'wrapper_used' $false)) {
            Add-ParityFailure 'OPENNOVA_WRAPPER_BYPASSED' $CaseId $Topology `
                "OpenNova $role was not launched through the hash-bound official Godot wrapper."
        }
        foreach ($flag in @('parent_verified','argv_verified','active_execution_verified')) {
            if ((Get-Field $proof $flag $null) -isnot [bool] -or
                    -not [bool](Get-Field $proof $flag $false)) {
                Add-ParityFailure 'OPENNOVA_LAUNCH_UNPROVEN' $CaseId $Topology `
                    "OpenNova $role launch proof does not affirm '$flag'."
            }
        }

        $launcherPid = [int64](Get-Field $proof 'launcher_pid' 0)
        $runtimePid = [int64](Get-Field $proof 'runtime_pid' 0)
        $runtimeParentPid = [int64](Get-Field $proof 'runtime_parent_pid' 0)
        if ($launcherPid -le 0 -or $runtimePid -le 0 -or
                $launcherPid -eq $runtimePid -or $runtimeParentPid -ne $launcherPid) {
            Add-ParityFailure 'OPENNOVA_LAUNCH_PARENTAGE' $CaseId $Topology `
                "OpenNova $role runtime/launcher PIDs do not prove exact wrapper parentage."
        }

        try {
            $launcherPath = [IO.Path]::GetFullPath("$(Get-Field $proof 'launcher_path' '')")
            $runtimePath = [IO.Path]::GetFullPath("$(Get-Field $proof 'runtime_path' '')")
            if (-not $ArtifactPaths.ContainsKey('godot_launcher') -or
                    -not $launcherPath.Equals(
                        [IO.Path]::GetFullPath([string]$ArtifactPaths['godot_launcher']),
                        [StringComparison]::OrdinalIgnoreCase) -or
                    -not $ArtifactPaths.ContainsKey('godot_runtime') -or
                    -not $runtimePath.Equals(
                        [IO.Path]::GetFullPath([string]$ArtifactPaths['godot_runtime']),
                        [StringComparison]::OrdinalIgnoreCase) -or
                    $launcherPath.Equals($runtimePath, [StringComparison]::OrdinalIgnoreCase)) {
                Add-ParityFailure 'OPENNOVA_LAUNCH_EXECUTABLE' $CaseId $Topology `
                    "OpenNova $role launch paths do not match the hash-bound wrapper/runtime pair."
            }
        } catch {
            Add-ParityFailure 'OPENNOVA_LAUNCH_EXECUTABLE' $CaseId $Topology $_.Exception.Message
        }

        $times = @{}
        $timestampsValid = $true
        foreach ($field in @(
            'launcher_start_utc','launcher_created_utc','runtime_start_utc',
            'runtime_created_utc','proven_utc'
        )) {
            try {
                $times[$field] = [uint64](ConvertFrom-StrictUtcIsoTimestamp `
                    "$(Get-Field $proof $field '')")
            } catch {
                $timestampsValid = $false
                Add-ParityFailure 'OPENNOVA_LAUNCH_TIMESTAMP' $CaseId $Topology `
                    "OpenNova $role $field is invalid: $($_.Exception.Message)"
            }
        }
        try {
            $times.steady_started_utc = [uint64](ConvertFrom-StrictUtcIsoTimestamp `
                "$(Get-Field $Run 'steady_started_utc' '')")
        } catch {
            $timestampsValid = $false
            Add-ParityFailure 'OPENNOVA_LAUNCH_TIMESTAMP' $CaseId $Topology `
                "OpenNova $role run steady_started_utc is invalid: $($_.Exception.Message)"
        }
        if ($timestampsValid) {
            $launcherClockDelta = [Math]::Abs(
                [decimal]$times.launcher_start_utc -
                [decimal]$times.launcher_created_utc)
            $runtimeClockDelta = [Math]::Abs(
                [decimal]$times.runtime_start_utc -
                [decimal]$times.runtime_created_utc)
            if ($launcherClockDelta -gt [decimal]2000000000 -or
                    $runtimeClockDelta -gt [decimal]2000000000 -or
                    $times.launcher_start_utc -gt $times.runtime_start_utc -or
                    $times.launcher_created_utc -gt $times.runtime_created_utc -or
                    $times.runtime_start_utc -gt $times.proven_utc -or
                    $times.runtime_created_utc -gt $times.proven_utc -or
                    $times.proven_utc -gt $times.steady_started_utc) {
                Add-ParityFailure 'OPENNOVA_LAUNCH_TIMESTAMP' $CaseId $Topology `
                    "OpenNova $role launch times do not prove process creation within two seconds and completion before steady traffic."
            }
        }

        $arguments = @($proof.arguments)
        $argumentsTyped = @($arguments | Where-Object { $_ -isnot [string] -or
            [string]::IsNullOrEmpty([string]$_) }).Count -eq 0
        $repo = [IO.Path]::GetFullPath((Get-RepoRoot))
        $expectedProject = [IO.Path]::GetFullPath((Join-Path $repo 'godot'))
        $expectedJoinerDriver = [IO.Path]::GetFullPath((Join-Path `
            $repo 'godot\tests\net\parity_joiner_driver.gd'))
        $expectedRunRoot = [IO.Path]::GetFullPath((Join-Path `
            $repo ".scratch\runs\$(Get-Field $Run 'run_id' '')"))
        $hostResourceDir = if ($ArtifactPaths.ContainsKey('retail_exe')) {
            [IO.Path]::GetFullPath((Split-Path -Parent `
                ([string]$ArtifactPaths['retail_exe'])))
        } else { '' }
        $resourceArgumentIndex = if ($role -eq 'joiner') { 11 } else { 9 }
        $resourceDir = ''
        if ($argumentsTyped -and $arguments.Count -gt $resourceArgumentIndex -and
                [IO.Path]::IsPathRooted([string]$arguments[$resourceArgumentIndex])) {
            try { $resourceDir = [IO.Path]::GetFullPath(
                    [string]$arguments[$resourceArgumentIndex]) }
            catch { $resourceDir = '' }
        }
        $resourceValid = -not [string]::IsNullOrWhiteSpace($resourceDir) -and
            (Test-Path -LiteralPath $resourceDir -PathType Container)
        if ($resourceValid -and $role -eq 'host') {
            $resourceValid = $resourceDir.Equals(
                $hostResourceDir, [StringComparison]::OrdinalIgnoreCase)
        } elseif ($resourceValid -and $role -eq 'joiner') {
            $resourceValid = -not $resourceDir.Equals(
                $hostResourceDir, [StringComparison]::OrdinalIgnoreCase)
            $hostPrefix = $hostResourceDir.TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
            foreach ($artifactRole in @(
                'retail_exe','hook','resource','localres','language','med',
                'expansion_pff','expansion_language_pff','expansion_bin'
            )) {
                if (-not $resourceValid -or -not $ArtifactPaths.ContainsKey($artifactRole)) {
                    $resourceValid = $false
                    break
                }
                $source = [IO.Path]::GetFullPath([string]$ArtifactPaths[$artifactRole])
                if (-not $source.StartsWith(
                        $hostPrefix, [StringComparison]::OrdinalIgnoreCase)) {
                    $resourceValid = $false
                    break
                }
                $relative = $source.Substring($hostPrefix.Length)
                $mirror = Join-Path $resourceDir $relative
                if (-not (Test-Path -LiteralPath $mirror -PathType Leaf) -or
                        (Get-FileHash -LiteralPath $mirror -Algorithm SHA256).Hash -cne
                            (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash) {
                    $resourceValid = $false
                    break
                }
            }
        }
        if (-not $resourceValid) {
            Add-ParityFailure 'OPENNOVA_LAUNCH_RESOURCE_CORPUS' $CaseId $Topology `
                "OpenNova $role resource directory is not the exact hash-bound retail corpus or its isolated byte-identical mirror."
        }
        $expectedArguments = @(
            '--path', $expectedProject,
            '--windowed', '--resolution', "$(Get-Field $Case 'resolution' '')",
            '--log-file', [IO.Path]::GetFullPath((Join-Path $expectedRunRoot "$role\godot.log"))
        )
        if ($role -eq 'joiner') {
            $expectedArguments += @('--script', $expectedJoinerDriver)
        }
        $expectedArguments += @(
            '--', '--resource-dir', $resourceDir,
            '/exp', "$(Get-Field $Corpus 'expansion' '')"
        )
        if (-not $argumentsTyped -or $arguments.Count -ne $expectedArguments.Count -or
                ($arguments -join "`n") -cne ($expectedArguments -join "`n") -or
                [string]::IsNullOrWhiteSpace([string]$proof.launcher_command_line) -or
                [string]::IsNullOrWhiteSpace([string]$proof.runtime_command_line)) {
            Add-ParityFailure 'OPENNOVA_LAUNCH_ARGV' $CaseId $Topology `
                "OpenNova $role launch proof differs from the exact case-bound argument vector."
        }
        if ($role -eq 'joiner' -and
                (-not $ArtifactPaths.ContainsKey('auto_joiner') -or
                    -not ([IO.Path]::GetFullPath(
                        [string]$ArtifactPaths['auto_joiner'])).Equals(
                            $expectedJoinerDriver,
                            [StringComparison]::OrdinalIgnoreCase))) {
            Add-ParityFailure 'OPENNOVA_JOINER_DRIVER_BINDING' $CaseId $Topology `
                'OpenNova joiner is not bound to the tracked parity driver artifact.'
        }

        $consoleHostPids = @($proof.launcher_console_host_pids)
        $consolePidsValid = $true
        foreach ($consolePid in $consoleHostPids) {
            if (-not ($consolePid -is [int] -or $consolePid -is [long]) -or
                    [int64]$consolePid -le 0 -or
                    [int64]$consolePid -in @($launcherPid,$runtimePid)) {
                $consolePidsValid = $false
            }
        }
        if (-not $consolePidsValid -or
                @($consoleHostPids | ForEach-Object { [int64]$_ } |
                    Select-Object -Unique).Count -ne $consoleHostPids.Count) {
            Add-ParityFailure 'OPENNOVA_LAUNCH_CONSOLE_OWNERSHIP' $CaseId $Topology `
                "OpenNova $role launch proof contains invalid console-host PIDs."
        }

        if ($role -eq 'joiner' -and (Test-Field $Run 'motion_gate_witness')) {
            $motionGate = Get-Field $Run 'motion_gate_witness' $null
            if ([int64](Get-Field $motionGate 'joiner_pid' 0) -ne $runtimePid) {
                Add-ParityFailure 'OPENNOVA_LAUNCH_PID_BINDING' $CaseId $Topology `
                    "OpenNova joiner runtime PID does not match its motion-gate witness."
            }
        }
    }
}

function Get-RunFacts {
    param(
        [Parameter(Mandatory = $true)] $Run,
        [Parameter(Mandatory = $true)] $Case,
        [Parameter(Mandatory = $true)] $Corpus,
        [Parameter(Mandatory = $true)] [string] $ManifestDir,
        [Parameter(Mandatory = $true)] [string] $Topology,
        [Parameter(Mandatory = $true)] [string] $NwPp,
        [Parameter(Mandatory = $true)] [hashtable] $ArtifactPaths,
        [Parameter(Mandatory = $true)] [string] $SuiteId,
        [string] $ItemsPath = ""
    )
    $caseId = "$(Get-Field $Case 'id' '')"
    $requiredFields = @(
        "capture", "sha256", "external_capture", "external_capture_sha256",
        "external_capture_proof", "perspective", "mission", "game_type", "resolution", "port",
        "mission_path", "mission_sha256",
        "corpus", "expansion", "host_callsign", "host_session_name", "server_name", "joiner_callsign", "steady_seconds",
        "steady_started_utc", "steady_completed_utc",
        "capture_complete", "capture_stats_available", "effective_configs", "opennova_launch_proofs",
        "wire_ready_witness", "run_id", "suite_id"
    )
    foreach ($field in $requiredFields) {
        if (-not (Test-Field $Run $field)) {
            Add-ParityFailure "RUN_METADATA_MISSING" $caseId $Topology "Run metadata is missing '$field'."
        }
    }
    $runId = "$(Get-Field $Run 'run_id' '')"
    if ($runId -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$') {
        Add-ParityFailure 'RUN_ID_INVALID' $caseId $Topology `
            'run_id must be a nonempty canonical identifier.'
    }
    if ("$(Get-Field $Run 'suite_id' '')" -ne $SuiteId) {
        Add-ParityFailure 'RUN_SUITE_MISMATCH' $caseId $Topology `
            'Run suite_id does not match the enclosing manifest suite_id.'
    }
    Test-EffectiveRetailConfigs $Run $Case $ManifestDir $caseId $Topology $Corpus $ArtifactPaths
    $null = Test-MotionGateWitness $Run $Case $ManifestDir $caseId $Topology
    $null = Test-RetailInputWitness $Run $Case $ManifestDir $caseId $Topology
    Test-OpenNovaLaunchProofs $Run $Case $Corpus $caseId $Topology $ArtifactPaths
	$null = Test-OpenNovaJoinerReadinessWitness $Run $Case $caseId $Topology
    $null = Test-OpenNovaHostShutdownWitness $Run $ManifestDir $caseId $Topology
    $null = Test-OpenNovaJoinerShutdownWitness $Run $ManifestDir $caseId $Topology

    $expectedPerspective = @{ RR="retail-client"; RO="retail-host"; OR="retail-client"; OO="external" }[$Topology]
    if ("$(Get-Field $Run 'perspective' '')" -ne $expectedPerspective) {
        Add-ParityFailure "PERSPECTIVE_MISMATCH" $caseId $Topology `
            "Expected capture perspective '$expectedPerspective'."
    }
    foreach ($field in @("mission", "resolution", "host_callsign", "host_session_name", "server_name", "joiner_callsign")) {
        $runBinding = "$(Get-Field $Run $field '')"
        $caseBinding = "$(Get-Field $Case $field '')"
        if (-not $runBinding.Equals($caseBinding, [StringComparison]::Ordinal)) {
            Add-ParityFailure "RUN_BINDING_MISMATCH" $caseId $Topology `
                "Run '$field' does not match its case."
        }
    }
    $caseMissionArtifact = Get-Field $Case 'mission_artifact' ([pscustomobject]@{})
    $caseMissionHash = Normalize-Sha256 "$(Get-Field $caseMissionArtifact 'sha256' '')"
    $runMissionPathValue = "$(Get-Field $Run 'mission_path' '')"
    $runMissionHash = Test-MissionArtifactBinding `
        "$(Get-Field $Case 'mission' '')" $runMissionPathValue `
        "$(Get-Field $Run 'mission_sha256' '')" $caseMissionHash $caseId $Topology
    $runMissionPath = Resolve-ManifestPath $ManifestDir $runMissionPathValue
    if ([string]::IsNullOrWhiteSpace($runMissionPath) -or
            -not (Test-Path -LiteralPath $runMissionPath -PathType Leaf)) {
        Add-ParityFailure "RUN_MISSION_ARTIFACT_MISSING" $caseId $Topology `
            "Run mission BMS is missing: $runMissionPath"
    } else {
        $actualRunMissionHash = (Get-FileHash -LiteralPath $runMissionPath -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($runMissionHash -ne $actualRunMissionHash) {
            Add-ParityFailure "RUN_MISSION_ARTIFACT_HASH" $caseId $Topology `
                "Run mission SHA-256 does not match the actual BMS bytes."
        }
    }
    $runGameTypeValid = Test-UInt32JsonField $Run 'game_type'
    $caseGameTypeValid = Test-UInt32JsonField $Case 'game_type'
    $runGameType = if ($runGameTypeValid) { [uint32]$Run.game_type } else { [uint32]0 }
    $caseGameType = if ($caseGameTypeValid) { [uint32]$Case.game_type } else { [uint32]0 }
    if (-not $runGameTypeValid) {
        Add-ParityFailure "GAME_TYPE_UNPINNED" $caseId $Topology `
            "Run game_type must be an explicit unsigned 32-bit numeric wire value; zero is valid."
    }
    if ($runGameTypeValid -and $caseGameTypeValid -and $runGameType -ne $caseGameType) {
        Add-ParityFailure "RUN_BINDING_MISMATCH" $caseId $Topology `
            "Run 'game_type' does not numerically match its case."
    }
    $runPort = [int](Get-Field $Run "port" 0)
    $casePort = [int](Get-Field $Case "port" 0)
    if ($runPort -ne $casePort) {
        Add-ParityFailure "RUN_BINDING_MISMATCH" $caseId $Topology `
            "Run 'port' does not numerically match its case."
    }
    if ("$(Get-Field $Run 'corpus' '')" -ne "$(Get-Field $Corpus 'fingerprint' '')" -or
            "$(Get-Field $Run 'expansion' '')" -ne "$(Get-Field $Corpus 'expansion' '')") {
        Add-ParityFailure "CORPUS_MISMATCH" $caseId $Topology `
            "Run corpus/expansion does not match the suite corpus."
    }
    Test-CaptureStatsDeclaration $Run $caseId $Topology
    $captureComplete = Get-Field $Run "capture_complete" $false
    if ($captureComplete -isnot [bool]) {
        Add-ParityFailure "RUN_METADATA_TYPE" $caseId $Topology `
            "capture_complete must be a JSON boolean, not a truthy string/number."
    } elseif (-not $captureComplete) {
        Add-ParityFailure "CAPTURE_INCOMPLETE" $caseId $Topology "Capture was not drained cleanly."
    }

    $capture = Resolve-ManifestPath $ManifestDir "$(Get-Field $Run 'capture' '')"
    if (-not (Test-Path -LiteralPath $capture -PathType Leaf)) {
        Add-ParityFailure "CAPTURE_MISSING" $caseId $Topology "Capture not found: $capture"
        return $null
    }
    $actualHash = (Get-FileHash -LiteralPath $capture -Algorithm SHA256).Hash.ToLowerInvariant()
    $expectedHash = Normalize-Sha256 "$(Get-Field $Run 'sha256' '')"
    if ($expectedHash -notmatch '^[0-9a-f]{64}$' -or $actualHash -ne $expectedHash) {
        Add-ParityFailure "CAPTURE_HASH_MISMATCH" $caseId $Topology `
            "Capture SHA-256 does not match the manifest (stale/replaced evidence)."
        return $null
    }
    $externalCapture = Resolve-ManifestPath $ManifestDir `
        "$(Get-Field $Run 'external_capture' '')"
    if (-not (Test-Path -LiteralPath $externalCapture -PathType Leaf)) {
        Add-ParityFailure 'EXTERNAL_CAPTURE_MISSING' $caseId $Topology `
            "Independent external capture not found: $externalCapture"
        return $null
    }
    $externalActualHash = (Get-FileHash -LiteralPath $externalCapture `
        -Algorithm SHA256).Hash.ToLowerInvariant()
    $externalExpectedHash = Normalize-Sha256 `
        "$(Get-Field $Run 'external_capture_sha256' '')"
    if ($externalExpectedHash -notmatch '^[0-9a-f]{64}$' -or
            $externalActualHash -cne $externalExpectedHash) {
        Add-ParityFailure 'EXTERNAL_CAPTURE_HASH_MISMATCH' $caseId $Topology `
            'Independent external pcapng SHA-256 does not match the manifest.'
        return $null
    }
    if ($Topology -eq 'OO' -and
            (([IO.Path]::GetFullPath($externalCapture) -cne
                [IO.Path]::GetFullPath($capture)) -or
             $externalActualHash -cne $actualHash)) {
        Add-ParityFailure 'EXTERNAL_CAPTURE_OWNERSHIP' $caseId $Topology `
            'OO primary evidence must be the same hash-bound external pcapng artifact.'
        return $null
    }
    if (-not (Test-ExternalCaptureTailProof `
            $Run $externalCapture $externalActualHash $caseId $Topology)) {
        return $null
    }

    $facts = Invoke-CaptureDecode $NwPp $capture $caseId $Topology $ItemsPath
    if ($null -eq $facts) { return $null }
    $packetIndex = @{}
    foreach ($packet in $facts.packets) {
        $indexKey = "$($packet.frame)|$($packet.dir)|$($packet.connection_key)|$($packet.sid)"
        if (-not $packetIndex.ContainsKey($indexKey)) {
            $packetIndex[$indexKey] = [System.Collections.Generic.List[object]]::new()
        }
        $packetIndex[$indexKey].Add($packet)
    }
    foreach ($event in $facts.events) {
        $indexKey = "$($event.frame)|$($event.dir)|$($event.connection_key)|$($event.sid)"
        $owners = @(if ($packetIndex.ContainsKey($indexKey)) {
            $packetIndex[$indexKey].ToArray() | Where-Object {
                $_.tag_identities -contains $event.identity
            }
        })
        if ($owners.Count -ne 1 -or $owners[0].ts_ns -ne $event.ts_ns) {
            Add-ParityFailure "EVENT_PACKET_CORRELATION" $caseId $Topology `
                "Event $($event.dir) 0x$($event.tag) frame $($event.frame) does not bind to exactly one timestamp-identical packet."
            return $null
        }
    }
    $null = Test-SemanticContract $facts $caseId $Topology
    if ((Get-Field $Run 'capture_stats_available' $true) -is [bool] -and
            -not [bool](Get-Field $Run 'capture_stats_available' $true)) {
        Test-ExternalCaptureCompleteness $facts $caseId $Topology
    }
    if (-not (Set-SteadyWindowFacts $facts $Run $caseId $Topology)) { return $null }
    if (-not (Test-RawDatagramContract $facts $caseId $Topology)) { return $null }
    if ($Topology -ne 'OO') {
        $externalFacts = Invoke-CaptureDecode $NwPp $externalCapture `
            $caseId "$Topology-external" $ItemsPath
        if ($null -eq $externalFacts) { return $null }
        $null = Test-SemanticContract $externalFacts $caseId "$Topology-external"
        Test-ExternalCaptureCompleteness $externalFacts $caseId "$Topology-external"
        if (-not (Set-SteadyWindowFacts $externalFacts $Run `
                    $caseId "$Topology-external")) { return $null }
        if (-not (Test-RawDatagramContract $externalFacts `
                    $caseId "$Topology-external")) { return $null }
        if (-not (Test-ExternalCaptureCrossCheck $facts $externalFacts `
                    $caseId $Topology)) { return $null }
        $externalFacts | Add-Member -NotePropertyName capture `
            -NotePropertyValue $externalCapture -Force
        $externalFacts | Add-Member -NotePropertyName sha256 `
            -NotePropertyValue $externalActualHash -Force
        $facts | Add-Member -NotePropertyName external_facts `
            -NotePropertyValue $externalFacts -Force
    }
    $facts | Add-Member -NotePropertyName capture -NotePropertyValue $capture
    $facts | Add-Member -NotePropertyName sha256 -NotePropertyValue $actualHash
    $facts | Add-Member -NotePropertyName external_capture `
        -NotePropertyValue $externalCapture -Force
    $facts | Add-Member -NotePropertyName external_sha256 `
        -NotePropertyValue $externalActualHash -Force
    if (-not (Test-WireReadyWitness $Run $Case $Corpus $facts $ManifestDir `
            $Topology $NwPp $ArtifactPaths $ItemsPath)) {
        return $null
    }
    return $facts
}

function Get-EventBody {
    param($Facts, [string] $Direction, [string] $Tag)
    return @($Facts.events | Where-Object {
        $_.dir -eq $Direction -and $_.identity -eq "${Tag}:0"
    })
}

function Test-WireIdentity {
    param($Facts, $Case, [string] $Topology, $Corpus, $MissionIdentity)
    $caseId = "$(Get-Field $Case 'id' '')"
    $expectedMission = "$(Get-Field $Case 'mission' '')"
    $expectedGameType = [uint32](Get-Field $Case "game_type" 0)
    $expectedHostCallsign = "$(Get-Field $Case 'host_callsign' '')"
    $expectedServerName = "$(Get-Field $Case 'server_name' '')"
    $expectedJoiner = "$(Get-Field $Case 'joiner_callsign' '')"
    $expectedExpansion = "$(Get-Field $Corpus 'expansion' '')"
    $expectedMissionDisplayName = [string] $MissionIdentity.MissionDisplayName
    $expectedMissionHeaderSha256 = [string] $MissionIdentity.Sha256

    $sessionConfigs = @(Get-EventBody $Facts "S" "08")
    if ($sessionConfigs.Count -eq 0) {
        Add-ParityFailure "SESSION_CONFIG_MISSING" $caseId $Topology "No S2C 0x08 session config was decoded."
    }
    foreach ($event in $sessionConfigs) {
        if ($event.body.Length -ne 51) {
            Add-ParityFailure "SESSION_CONFIG_SHAPE" $caseId $Topology "S2C 0x08 is not 51 bytes."
            continue
        }
        $actual = Read-U32Le $event.body 12
        if ($actual -ne $expectedGameType) {
            Add-ParityFailure "GAME_TYPE_MISMATCH" $caseId $Topology `
                "Wire game type $actual does not match $expectedGameType."
        }
    }

    $missionHeaders = @(Get-EventBody $Facts "S" "0b")
    if ($missionHeaders.Count -eq 0) {
        Add-ParityFailure "MISSION_HEADER_MISSING" $caseId $Topology `
            "No S2C 0x0B BMS header was decoded."
    } elseif ($missionHeaders.Count -ne 1) {
        Add-ParityFailure "MISSION_HEADER_COUNT" $caseId $Topology `
            "Expected exactly one S2C 0x0B BMS header; observed $($missionHeaders.Count)."
    } else {
        [byte[]] $wireHeader = $missionHeaders[0].body
        if ($wireHeader.Length -ne 616 -or
                (Get-NetHarnessBytesSha256 $wireHeader) -cne
                    $expectedMissionHeaderSha256) {
            Add-ParityFailure "MISSION_HEADER_MISMATCH" $caseId $Topology `
                "S2C 0x0B is not the exact header of the hash-bound mission BMS."
        }
    }

    $playerInfos = @(Get-EventBody $Facts "S" "7b")
    if ($playerInfos.Count -eq 0) {
        Add-ParityFailure "PLAYER_INFO_MISSING" $caseId $Topology "No S2C 0x7B map identity was decoded."
    }
    $wireNames = [System.Collections.Generic.List[string]]::new()
    $wireMissions = [System.Collections.Generic.List[string]]::new()
    foreach ($event in $playerInfos) {
        try {
            $offset = 0
            $playerName = Read-CStringAscii $event.body ([ref]$offset)
            $null = Read-CStringAscii $event.body ([ref]$offset)
            $serverName = Read-CStringAscii $event.body ([ref]$offset)
            $mission = Read-CStringAscii $event.body ([ref]$offset)
            $map = Read-CStringAscii $event.body ([ref]$offset)
            $wireGameType = Read-U32Le $event.body $offset
            $offset += 4
            $null = Read-CStringAscii $event.body ([ref]$offset)
            $wireExpansion = Read-CStringAscii $event.body ([ref]$offset)
            if ($offset -ne $event.body.Length) { throw "S2C 0x7B has trailing bytes." }
            $wireNames.Add($playerName)
            if (-not $serverName.Equals($expectedServerName, [StringComparison]::Ordinal)) {
                Add-ParityFailure "SERVER_NAME_MISMATCH" $caseId $Topology `
                    "S2C 0x7B server/game name '$serverName' does not exactly match manifest server_name '$expectedServerName'."
            }
            $wireMissions.Add($mission)
            if (-not (Test-ParityAdvertisedMissionName $mission $expectedMission `
                        $expectedMissionDisplayName)) {
                Add-ParityFailure "MISSION_MISMATCH" $caseId $Topology `
                    ("Wire mission '$mission' is neither exact map filename " +
                    "'$expectedMission' nor hash-bound BMS title " +
                    "'$expectedMissionDisplayName'.")
            }
            if (-not $map.Equals($expectedMission, [StringComparison]::Ordinal)) {
                Add-ParityFailure "MAP_MISMATCH" $caseId $Topology `
                    "Wire map '$map' does not exactly match '$expectedMission'."
            }
            if ($wireGameType -ne $expectedGameType) {
                Add-ParityFailure "PLAYER_INFO_GAME_TYPE" $caseId $Topology `
                    "S2C 0x7B game type $wireGameType does not match $expectedGameType."
            }
            if (-not $wireExpansion.Equals($expectedExpansion, [StringComparison]::OrdinalIgnoreCase)) {
                Add-ParityFailure "EXPANSION_MISMATCH" $caseId $Topology `
                    "S2C 0x7B expansion '$wireExpansion' does not match '$expectedExpansion'."
            }
        } catch {
            Add-ParityFailure "PLAYER_INFO_SHAPE" $caseId $Topology $_.Exception.Message
        }
    }
    if (@($wireMissions.ToArray() | Select-Object -Unique).Count -gt 1) {
        Add-ParityFailure "MISSION_IDENTITY_UNSTABLE" $caseId $Topology `
            "S2C 0x7B changed advertised mission identity inside one capture."
    }
    if (@($wireNames | Where-Object { $_.Equals($expectedJoiner, [StringComparison]::Ordinal) }).Count -eq 0) {
        Add-ParityFailure "JOINER_IDENTITY_MISMATCH" $caseId $Topology `
            "Recipient-scoped S2C 0x7B never exactly identifies the manifest joiner callsign '$expectedJoiner'."
    }

    # Retail's 0x7B is recipient-scoped: a client capture can legitimately
    # contain only the joiner's player-info record. The full roster identity
    # surface is the variable-field S2C 0x46 slot stream, so prove both names
    # there instead of inventing a host 0x7B requirement retail itself fails.
    $slotSync = @(Get-EventBody $Facts "S" "46")
    if ($slotSync.Count -eq 0) {
        Add-ParityFailure "PLAYER_SLOT_SYNC_MISSING" $caseId $Topology `
            "No S2C 0x46 player-slot synchronization was decoded."
    }
    foreach ($identity in @(
        [pscustomobject]@{ Name=$expectedHostCallsign; Code="HOST_PLAYER_IDENTITY_MISMATCH"; Role="host" },
        [pscustomobject]@{ Name=$expectedJoiner; Code="JOINER_SLOT_IDENTITY_MISMATCH"; Role="joiner" }
    )) {
        if (@($slotSync | Where-Object {
                Test-BodyContainsCStringAscii -Bytes $_.body -Value $identity.Name
            }).Count -eq 0) {
            Add-ParityFailure $identity.Code $caseId $Topology `
                "S2C 0x46 never exactly identifies the manifest $($identity.Role) callsign '$($identity.Name)'."
        }
    }
}

function Assert-ExactWireReadyProperties {
    param($Value, [string[]] $Expected, [string] $Context)
    if ($null -eq $Value) { throw "$Context is missing." }
    $actual = @($Value.PSObject.Properties.Name | Sort-Object)
    $wanted = @($Expected | Sort-Object)
    if (($actual -join ',') -cne ($wanted -join ',')) {
        throw "$Context property set drifted: actual=$($actual -join ',') expected=$($wanted -join ',')"
    }
}

function Read-WireReadyCStringAscii {
    param([byte[]] $Bytes, [ref] $Offset, [string] $Context)
    $start = [int]$Offset.Value
    $end = $start
    while ($end -lt $Bytes.Length -and $Bytes[$end] -ne 0) {
        if ($Bytes[$end] -gt 0x7f) { throw "$Context contains a non-ASCII string byte." }
        $end++
    }
    if ($end -ge $Bytes.Length) { throw "$Context contains an unterminated string." }
    $value = [Text.Encoding]::ASCII.GetString($Bytes, $start, $end - $start)
    $Offset.Value = $end + 1
    return $value
}

function ConvertFrom-WireReadyPlayerInfo {
    param($Event)
    [byte[]]$body = $Event.body
    $offset = 0
    $name = Read-WireReadyCStringAscii $body ([ref]$offset) 'S2C 0x7B'
    $playerId = Read-WireReadyCStringAscii $body ([ref]$offset) 'S2C 0x7B'
    $sessionName = Read-WireReadyCStringAscii $body ([ref]$offset) 'S2C 0x7B'
    $missionName = Read-WireReadyCStringAscii $body ([ref]$offset) 'S2C 0x7B'
    $mapName = Read-WireReadyCStringAscii $body ([ref]$offset) 'S2C 0x7B'
    $gameType = Read-U32Le $body $offset
    $offset += 4
    $motd = Read-WireReadyCStringAscii $body ([ref]$offset) 'S2C 0x7B'
    $expansion = Read-WireReadyCStringAscii $body ([ref]$offset) 'S2C 0x7B'
    if ($offset -ne $body.Length) { throw 'S2C 0x7B has trailing bytes.' }
    return [pscustomobject]@{
        event = $Event; name = $name; player_id = $playerId
        session_name = $sessionName; mission = $missionName; map = $mapName
        game_type = [uint32]$gameType; motd = $motd; expansion = $expansion
    }
}

function Get-WireReadyPlayerSyncName {
    param($Event)
    [byte[]]$body = $Event.body
    if ($body.Length -lt 3) { throw 'S2C 0x46 is shorter than its slot/field-flags prefix.' }
    $fieldFlags = Read-U16Le $body 1
    if (($fieldFlags -band 0x0001) -eq 0) { return $null }
    if ($body.Length -lt 4) { throw 'Name-bearing S2C 0x46 omits its entity slot.' }
    $offset = 4
    return Read-WireReadyCStringAscii $body ([ref]$offset) 'S2C 0x46 name field'
}

function Get-WireReadyTimestampSpanNs {
    param([object[]] $Values)
    if ($Values.Count -lt 2) { return [uint64]0 }
    $ordered = @($Values | Sort-Object { [uint64]$_.ts_ns })
    [uint64]$first = $ordered[0].ts_ns
    [uint64]$last = $ordered[$ordered.Count - 1].ts_ns
    if ($last -lt $first) { throw 'Wire-ready timestamps moved backwards.' }
    return [uint64]($last - $first)
}

function Get-WireReadyRttMatches {
    param([object[]] $Events, [uint64] $NotBefore)
    $pending = @{}
    $matches = [System.Collections.Generic.List[object]]::new()
    foreach ($event in @($Events | Where-Object {
            $_.identity -in @('2c:0','57:0') -and $_.ts_ns -ge $NotBefore
        } | Sort-Object ts_ns, frame)) {
        [byte[]]$body = $event.body
        if ($body.Length -ne 5 -or $body[4] -gt 1) {
            throw "$($event.dir) 0x$($event.tag) has an invalid RTT body."
        }
        $token = ConvertTo-CompactHex ([byte[]]$body[0..3])
        if ($event.dir -eq 'C') {
            if ($body[4] -eq 0) { continue }
            if (-not $pending.ContainsKey($token)) {
                $pending[$token] = [System.Collections.Generic.Queue[object]]::new()
            }
            $pending[$token].Enqueue($event)
        }
        else {
            if ($body[4] -eq 1) { continue }
            if ($pending.ContainsKey($token) -and $pending[$token].Count -gt 0) {
                $request = $pending[$token].Dequeue()
                $matches.Add([pscustomobject]@{
                    token = $token; request_frame = [int]$request.frame
                    request_ts_ns = [uint64]$request.ts_ns
                    response_frame = [int]$event.frame
                    response_ts_ns = [uint64]$event.ts_ns
                })
            }
        }
    }
    return @($matches.ToArray())
}

function Get-WireReadyEvidenceFromFacts {
    param($Facts, $Case, $Corpus, $MissionIdentity)
    $expectedMission = "$(Get-Field $Case 'mission' '')"
    $expectedGameType = [uint32](Get-Field $Case 'game_type' 0)
    $expectedHostCallsign = "$(Get-Field $Case 'host_callsign' '')"
    $expectedJoiner = "$(Get-Field $Case 'joiner_callsign' '')"
    $expectedServerName = "$(Get-Field $Case 'server_name' '')"
    $expectedExpansion = "$(Get-Field $Corpus 'expansion' '')"
    $expectedMissionDisplayName = [string] $MissionIdentity.MissionDisplayName
    $expectedMissionHeaderSha256 = [string] $MissionIdentity.Sha256

    $candidateKeys = [System.Collections.Generic.List[string]]::new()
    foreach ($event in @($Facts.events | Where-Object {
            $_.dir -eq 'S' -and $_.identity -eq '7b:0'
        })) {
        $info = ConvertFrom-WireReadyPlayerInfo $event
        if ($info.name.Equals($expectedJoiner, [StringComparison]::Ordinal) -and
                -not $candidateKeys.Contains([string]$event.connection_key)) {
            $candidateKeys.Add([string]$event.connection_key)
        }
    }
    if ($candidateKeys.Count -eq 0) {
        throw "No recipient-scoped S2C 0x7B identifies exact joiner '$expectedJoiner'."
    }

    $ready = [System.Collections.Generic.List[object]]::new()
    foreach ($connectionKey in $candidateKeys) {
        $events = @($Facts.events | Where-Object { $_.connection_key -eq $connectionKey })
        $packets = @($Facts.packets | Where-Object { $_.connection_key -eq $connectionKey })
        $states = @($Facts.states | Where-Object { $_.connection_key -eq $connectionKey })
        $entities = @($Facts.entities | Where-Object { $_.connection_key -eq $connectionKey })
        $clientSids = @($packets | Where-Object { $_.dir -eq 'C' } |
            ForEach-Object { [uint32]$_.sid } | Select-Object -Unique)
        $serverSids = @($packets | Where-Object { $_.dir -eq 'S' } |
            ForEach-Object { [uint32]$_.sid } | Select-Object -Unique)
        if ($clientSids.Count -ne 1 -or $serverSids.Count -ne 1) { continue }

        $playerInfos = [System.Collections.Generic.List[object]]::new()
        $advertisedMissions = [System.Collections.Generic.List[string]]::new()
        foreach ($event in @($events | Where-Object {
                $_.dir -eq 'S' -and $_.identity -eq '7b:0'
            } | Sort-Object ts_ns, frame)) {
            $info = ConvertFrom-WireReadyPlayerInfo $event
            foreach ($binding in @(
                @($info.session_name, $expectedServerName, 'server/game name'),
                @($info.map, $expectedMission, 'map'),
                @($info.expansion, $expectedExpansion, 'expansion')
            )) {
                if (-not ([string]$binding[0]).Equals(
                        [string]$binding[1], [StringComparison]::Ordinal)) {
                    throw "S2C 0x7B $($binding[2]) mismatch on readiness connection."
                }
            }
            if (-not (Test-ParityAdvertisedMissionName $info.mission `
                        $expectedMission $expectedMissionDisplayName)) {
                throw ("S2C 0x7B mission on readiness connection is neither " +
                    "the exact map filename nor the hash-bound BMS title.")
            }
            $advertisedMissions.Add([string] $info.mission)
            if ([uint32]$info.game_type -ne $expectedGameType) {
                throw 'S2C 0x7B game type mismatch on readiness connection.'
            }
            $playerInfos.Add($info)
        }
        $distinctAdvertisedMissions = @($advertisedMissions.ToArray() |
            Select-Object -Unique)
        if ($distinctAdvertisedMissions.Count -ne 1) {
            throw 'S2C 0x7B changed advertised mission identity on readiness connection.'
        }
        $advertisedMission = [string] $distinctAdvertisedMissions[0]
        $joinerInfos = @($playerInfos | Where-Object {
            $_.name.Equals($expectedJoiner, [StringComparison]::Ordinal)
        })
        if ($joinerInfos.Count -eq 0) { continue }

        $sessionConfigs = @($events | Where-Object {
            $_.dir -eq 'S' -and $_.identity -eq '08:0'
        } | Sort-Object ts_ns, frame)
        if ($sessionConfigs.Count -eq 0) { continue }
        foreach ($config in $sessionConfigs) {
            if ($config.body.Length -ne 51 -or
                    (Read-U32Le $config.body 12) -ne $expectedGameType) {
                throw 'S2C 0x08 readiness session config is malformed or mismatched.'
            }
        }

        $missionHeaders = @($events | Where-Object {
            $_.dir -eq 'S' -and $_.identity -eq '0b:0'
        } | Sort-Object ts_ns, frame)
        if ($missionHeaders.Count -ne 1) {
            throw "Readiness connection has $($missionHeaders.Count) S2C 0x0B BMS headers; expected exactly one."
        }
        [byte[]] $wireMissionHeader = $missionHeaders[0].body
        if ($wireMissionHeader.Length -ne 616 -or
                (Get-NetHarnessBytesSha256 $wireMissionHeader) -cne
                    $expectedMissionHeaderSha256) {
            throw 'Readiness S2C 0x0B is not the exact hash-bound mission BMS header.'
        }

        $syncNames = [System.Collections.Generic.List[object]]::new()
        foreach ($event in @($events | Where-Object {
                $_.dir -eq 'S' -and $_.identity -eq '46:0'
            })) {
            $name = Get-WireReadyPlayerSyncName $event
            if ($null -ne $name) {
                $syncNames.Add([pscustomobject]@{ name=$name; event=$event })
            }
        }
        $hostSync = @($syncNames | Where-Object {
            $_.name.Equals($expectedHostCallsign, [StringComparison]::Ordinal)
        } | Sort-Object { $_.event.ts_ns }, { $_.event.frame } | Select-Object -First 1)
        $joinerSync = @($syncNames | Where-Object {
            $_.name.Equals($expectedJoiner, [StringComparison]::Ordinal)
        } | Sort-Object { $_.event.ts_ns }, { $_.event.frame } | Select-Object -First 1)
        if ($hostSync.Count -eq 0 -or $joinerSync.Count -eq 0) { continue }

        $missionStatus = @($events | Where-Object {
            $_.dir -eq 'C' -and $_.identity -eq '0b:0'
        } | Sort-Object ts_ns, frame | Select-Object -First 1)
        $worldLoad = @($events | Where-Object {
            $_.dir -eq 'S' -and $_.identity -eq '0f:0'
        } | Sort-Object ts_ns, frame | Select-Object -First 1)
        if ($missionStatus.Count -eq 0 -or $worldLoad.Count -eq 0) { continue }
        [uint64]$notBefore = 0
        foreach ($milestone in @(
            [uint64]$hostSync[0].event.ts_ns
            [uint64]$joinerSync[0].event.ts_ns
            [uint64]$missionHeaders[0].ts_ns
            [uint64]$missionStatus[0].ts_ns
            [uint64]$worldLoad[0].ts_ns
        )) {
            if ([uint64]$milestone -gt $notBefore) {
                $notBefore = [uint64]$milestone
            }
        }

        $decodedUplinks = @($states | Where-Object {
            $_.dir -eq 'C' -and $_.kind -eq 'uplink' -and $_.decode -and
            $_.handle -ne 0 -and $_.ts_ns -ge $notBefore
        })
        $handleCandidates = [System.Collections.Generic.List[object]]::new()
        foreach ($group in @($decodedUplinks | Group-Object handle)) {
            $handle = [uint32]$group.Name
            $matchingEntities = @($entities | Where-Object {
                $_.class -eq 'P' -and $_.handle -eq $handle -and
                $_.ts_ns -ge $notBefore
            })
            if ($matchingEntities.Count -eq 0) { continue }
            $uplinks = @($group.Group | Sort-Object ts_ns, frame)
            $handleCandidates.Add([pscustomobject]@{
                handle=$handle; type_id=[uint32]$uplinks[0].type_id
                uplinks=$uplinks; entities=$matchingEntities; count=$uplinks.Count
                span_ns=(Get-WireReadyTimestampSpanNs $uplinks)
            })
        }
        $player = @($handleCandidates | Where-Object {
            $_.count -ge 40 -and $_.span_ns -ge [uint64]10000000000
        } | Sort-Object @{Expression='count';Descending=$true}, handle |
            Select-Object -First 1)
        if ($player.Count -eq 0) { continue }
        $player = $player[0]

        $localFrames = @($states | Where-Object {
            $_.dir -eq 'S' -and $_.kind -eq 'frame' -and $_.decode -and
            $_.local_tail -and $_.ts_ns -ge $notBefore
        } | Sort-Object ts_ns, frame)
        [uint64]$frameSpan = Get-WireReadyTimestampSpanNs $localFrames
        if ($localFrames.Count -lt 40 -or $frameSpan -lt [uint64]10000000000) { continue }
        $rttMatches = @(Get-WireReadyRttMatches $events $notBefore)
        if ($rttMatches.Count -lt 20) { continue }

        $firstEntity = @($player.entities | Sort-Object ts_ns, frame | Select-Object -First 1)[0]
        $ready.Add([pscustomobject]@{
            connection_key = $connectionKey
            last_rtt_response_ts_ns = [uint64]$rttMatches[$rttMatches.Count - 1].response_ts_ns
            evidence = [pscustomobject][ordered]@{
                session = [int]$joinerInfos[0].event.session
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
                gameplay_not_before_ts_ns = [uint64]$notBefore
                player_handle = [uint32]$player.handle
                player_type = [uint32]$player.type_id
                player_entity_first_frame = [int]$firstEntity.frame
                c2s_0c_count = [int]$player.count
                c2s_0c_first_ts_ns = [uint64]$player.uplinks[0].ts_ns
                c2s_0c_last_ts_ns = [uint64]$player.uplinks[$player.uplinks.Count - 1].ts_ns
                c2s_0c_span_ns = [uint64]$player.span_ns
                s2c_0a_count = [int]$localFrames.Count
                s2c_0a_first_ts_ns = [uint64]$localFrames[0].ts_ns
                s2c_0a_last_ts_ns = [uint64]$localFrames[$localFrames.Count - 1].ts_ns
                s2c_0a_span_ns = [uint64]$frameSpan
                matched_rtt_count = [int]$rttMatches.Count
                first_rtt_token = [string]$rttMatches[0].token
                last_rtt_token = [string]$rttMatches[$rttMatches.Count - 1].token
            }
        })
    }
    if ($ready.Count -ne 1) {
        throw "Expected exactly one decoded wire-ready connection; observed $($ready.Count)."
    }
    return $ready[0]
}

function Invoke-WireReadyPrefixDecode {
    param(
        [string] $NwPp, [string] $Capture, [int64] $PrefixBytes,
        [string] $ItemsPath, [string] $CaseId, [string] $Topology
    )
    $captureItem = Get-Item -LiteralPath $Capture -ErrorAction Stop
    if ($PrefixBytes -le 24 -or $PrefixBytes -gt $captureItem.Length) {
        throw "Wire-ready prefix length $PrefixBytes is invalid for $Capture."
    }
    $tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
    $tempRoot = Join-Path $tempBase ('opennova-verify-wire-' + [Guid]::NewGuid().ToString('N'))
    $null = New-Item -ItemType Directory -Path $tempRoot
    $snapshot = Join-Path $tempRoot ('prefix' + [IO.Path]::GetExtension($Capture))
    try {
        $input = [IO.File]::Open($Capture, [IO.FileMode]::Open, [IO.FileAccess]::Read,
            [IO.FileShare]::Read)
        try {
            $output = [IO.File]::Open($snapshot, [IO.FileMode]::CreateNew,
                [IO.FileAccess]::Write, [IO.FileShare]::None)
            try {
                $buffer = [byte[]]::new(1024 * 1024)
                [int64]$remaining = $PrefixBytes
                while ($remaining -gt 0) {
                    $requested = [int][Math]::Min([int64]$buffer.Length, $remaining)
                    $read = $input.Read($buffer, 0, $requested)
                    if ($read -le 0) { throw 'Final capture ended inside its declared readiness prefix.' }
                    $output.Write($buffer, 0, $read)
                    $remaining -= $read
                }
                $output.Flush($true)
            } finally { $output.Dispose() }
        } finally { $input.Dispose() }
        $hash = (Get-FileHash -LiteralPath $snapshot -Algorithm SHA256).Hash.ToLowerInvariant()
        $facts = Invoke-CaptureDecode $NwPp $snapshot $CaseId $Topology $ItemsPath
        return [pscustomobject]@{ sha256=$hash; facts=$facts }
    }
    finally {
        $resolved = [IO.Path]::GetFullPath($tempRoot)
        if (-not $resolved.StartsWith($tempBase + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing to remove unexpected wire-ready temp directory: $resolved"
        }
        if (Test-Path -LiteralPath $resolved) {
            Remove-Item -LiteralPath $resolved -Recurse -Force
        }
    }
}

function Test-WireReadyWitness {
    param(
        $Run, $Case, $Corpus, $Facts, [string] $ManifestDir,
        [string] $Topology, [string] $NwPp, [hashtable] $ArtifactPaths,
        [string] $ItemsPath
    )
    $caseId = "$(Get-Field $Case 'id' '')"
    try {
        $caseMissionArtifact = Get-Field $Case 'mission_artifact' ([pscustomobject]@{})
        $caseMissionPath = Resolve-ManifestPath $ManifestDir `
            "$(Get-Field $caseMissionArtifact 'path' '')"
        if (-not (Test-Path -LiteralPath $caseMissionPath -PathType Leaf)) {
            throw "Wire-ready case mission BMS is missing: $caseMissionPath"
        }
        $caseMissionHash = Normalize-Sha256 `
            "$(Get-Field $caseMissionArtifact 'sha256' '')"
        if ((Get-FileHash -LiteralPath $caseMissionPath -Algorithm SHA256).Hash.ToLowerInvariant() `
                -cne $caseMissionHash) {
            throw 'Wire-ready case mission BMS hash drifted.'
        }
        $missionIdentity = Get-BmsHeaderIdentity -Path $caseMissionPath
        $manifestWitness = Get-Field $Run 'wire_ready_witness' $null
        Assert-ExactWireReadyProperties $manifestWitness @(
            'path','sha256','schema','ready','created_utc','file_last_write_utc',
            'expected','thresholds','source','evidence'
        ) 'manifest wire-ready witness'
        $witnessPath = Resolve-ManifestPath $ManifestDir "$(Get-Field $manifestWitness 'path' '')"
        if (-not (Test-Path -LiteralPath $witnessPath -PathType Leaf)) {
            throw "Wire-ready witness artifact is missing: $witnessPath"
        }
        $capture = [string]$Facts.capture
        $captureParent = Split-Path -Parent $capture
        $runRoot = if ($Topology -eq 'OO') { $captureParent } else {
            Split-Path -Parent $captureParent
        }
        $expectedWitnessPath = [IO.Path]::GetFullPath((Join-Path $runRoot 'wire-ready.json'))
        if ([IO.Path]::GetFullPath($witnessPath) -ne $expectedWitnessPath) {
            throw 'Wire-ready witness is not owned by the exact run root.'
        }
        $actualWitnessHash = (Get-FileHash -LiteralPath $witnessPath -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actualWitnessHash -cne "$(Get-Field $manifestWitness 'sha256' '')") {
            throw 'Wire-ready witness SHA-256 does not match its manifest binding.'
        }
        $disk = Get-Content -LiteralPath $witnessPath -Raw | ConvertFrom-Json
        Assert-ExactWireReadyProperties $disk @(
            'schema','ready','created_utc','witness_path','expected','thresholds','source','evidence'
        ) 'wire-ready artifact'
        Assert-ExactWireReadyProperties $disk.expected @(
            'host_callsign','joiner_callsign','host_session_name','mission',
            'mission_display_name','mission_header_sha256','map','game_type','expansion'
        ) 'wire-ready expected binding'
        Assert-ExactWireReadyProperties $disk.thresholds @(
            'decoded_c2s_0c','decoded_local_s2c_0a','state_span_ns',
            'matched_c2s_0x2c_s2c_0x57'
        ) 'wire-ready thresholds'
        Assert-ExactWireReadyProperties $disk.source @(
            'capture','capture_prefix_bytes','capture_prefix_sha256','capture_last_write_utc',
            'nw_pp','nw_pp_sha256','items','items_sha256','loaded_item_count',
            'parity_packet_count','parity_event_count'
        ) 'wire-ready source'
        Assert-ExactWireReadyProperties $disk.evidence @(
            'session','client_sid','server_sid','joiner_player_info_frame',
            'advertised_mission','session_config_frame','mission_header_frame',
            'host_player_sync_frame','joiner_player_sync_frame',
            'mission_status_frame','world_load_frame','gameplay_not_before_ts_ns',
            'player_handle','player_type','player_entity_first_frame','c2s_0c_count',
            'c2s_0c_first_ts_ns','c2s_0c_last_ts_ns','c2s_0c_span_ns','s2c_0a_count',
            's2c_0a_first_ts_ns','s2c_0a_last_ts_ns','s2c_0a_span_ns',
            'matched_rtt_count','first_rtt_token','last_rtt_token'
        ) 'wire-ready evidence'
        Assert-ExactWireReadyProperties $manifestWitness.source @(
            'capture','capture_sha256','capture_prefix_bytes',
            'capture_prefix_sha256','capture_last_write_utc','nw_pp',
            'nw_pp_sha256','items','items_sha256','loaded_item_count',
            'parity_packet_count','parity_event_count'
        ) 'manifest wire-ready source'

        if ([string]$disk.schema -cne 'opennova.parity-wire-readiness.v2' -or
                [string]$manifestWitness.schema -cne [string]$disk.schema -or
                $disk.ready -isnot [bool] -or -not [bool]$disk.ready -or
                $manifestWitness.ready -isnot [bool] -or -not [bool]$manifestWitness.ready -or
                [string]$manifestWitness.created_utc -cne [string]$disk.created_utc -or
                (Resolve-ManifestPath $ManifestDir ([string]$disk.witness_path)) -ne $witnessPath) {
            throw 'Wire-ready header/path binding is invalid.'
        }
        foreach ($field in @('expected','thresholds','evidence')) {
            if (($manifestWitness.$field | ConvertTo-Json -Depth 10 -Compress) -cne
                    ($disk.$field | ConvertTo-Json -Depth 10 -Compress)) {
                throw "Manifest wire-ready $field differs from the hash-bound artifact."
            }
        }
        $expected = $disk.expected
        foreach ($binding in @(
            @([string]$expected.host_callsign, "$(Get-Field $Case 'host_callsign' '')", 'host callsign'),
            @([string]$expected.joiner_callsign, "$(Get-Field $Case 'joiner_callsign' '')", 'joiner callsign'),
            @([string]$expected.host_session_name, "$(Get-Field $Case 'server_name' '')", 'server/game name'),
            @([string]$expected.mission, "$(Get-Field $Case 'mission' '')", 'mission'),
            @([string]$expected.mission_display_name,
                [string]$missionIdentity.MissionDisplayName, 'mission display name'),
            @([string]$expected.mission_header_sha256,
                [string]$missionIdentity.Sha256, 'mission header SHA-256'),
            @([string]$expected.map, "$(Get-Field $Case 'mission' '')", 'map'),
            @([string]$expected.expansion, "$(Get-Field $Corpus 'expansion' '')", 'expansion')
        )) {
            if (-not $binding[0].Equals($binding[1], [StringComparison]::Ordinal)) {
                throw "Wire-ready expected $($binding[2]) is not exactly case-bound."
            }
        }
        if ([uint32]$expected.game_type -ne [uint32](Get-Field $Case 'game_type' 0)) {
            throw 'Wire-ready expected game type is not exactly case-bound.'
        }
        $thresholds = $disk.thresholds
        if ([int]$thresholds.decoded_c2s_0c -ne 40 -or
                [int]$thresholds.decoded_local_s2c_0a -ne 40 -or
                [uint64]$thresholds.state_span_ns -ne [uint64]10000000000 -or
                [int]$thresholds.matched_c2s_0x2c_s2c_0x57 -ne 20) {
            throw 'Wire-ready thresholds were weakened or drifted.'
        }

        $source = $disk.source
        $manifestSource = $manifestWitness.source
        $normalizedCapture = Resolve-ManifestPath $ManifestDir ([string]$manifestSource.capture)
        $expectedWireCapture = [IO.Path]::GetFullPath(
            (Join-Path $runRoot 'external.pcapng'))
        if ($normalizedCapture -ne $expectedWireCapture -or
                -not (Test-Path -LiteralPath $normalizedCapture -PathType Leaf) -or
                [string]$manifestSource.capture_sha256 -cne
                    (Get-FileHash -LiteralPath $normalizedCapture -Algorithm SHA256).Hash.ToLowerInvariant()) {
            throw 'Normalized wire-ready source is not the hash-bound run-owned external capture.'
        }
        $expectedNwPp = [string]$ArtifactPaths['nw_pp']
        $expectedItems = [string]$ArtifactPaths['items']
        if ((Resolve-ManifestPath $ManifestDir ([string]$manifestSource.nw_pp)) -ne $expectedNwPp -or
                (Resolve-ManifestPath $ManifestDir ([string]$manifestSource.items)) -ne $expectedItems -or
                (Resolve-ManifestPath $ManifestDir ([string]$source.nw_pp)) -ne $expectedNwPp -or
                (Resolve-ManifestPath $ManifestDir ([string]$source.items)) -ne $expectedItems -or
                [string]$source.nw_pp_sha256 -cne (Get-FileHash -LiteralPath $expectedNwPp -Algorithm SHA256).Hash.ToLowerInvariant() -or
                [string]$source.items_sha256 -cne (Get-FileHash -LiteralPath $expectedItems -Algorithm SHA256).Hash.ToLowerInvariant() -or
                [string]$manifestSource.nw_pp_sha256 -cne [string]$source.nw_pp_sha256 -or
                [string]$manifestSource.items_sha256 -cne [string]$source.items_sha256 -or
                [int]$source.loaded_item_count -le 0 -or
                [int]$manifestSource.loaded_item_count -ne [int]$source.loaded_item_count) {
            throw 'Wire-ready decoder/items binding is invalid.'
        }
        foreach ($field in @(
            'capture_prefix_bytes','capture_prefix_sha256','capture_last_write_utc',
            'parity_packet_count','parity_event_count'
        )) {
            if ([string]$manifestSource.$field -cne [string]$source.$field) {
                throw "Normalized wire-ready source field '$field' differs from the artifact."
            }
        }

        [uint64]$createdNs = ConvertFrom-StrictUtcIsoTimestamp ([string]$disk.created_utc)
        [uint64]$fileWriteNs = ConvertFrom-StrictUtcIsoTimestamp `
            ([string]$manifestWitness.file_last_write_utc)
        [uint64]$captureWriteNs = ConvertFrom-StrictUtcIsoTimestamp `
            ([string]$source.capture_last_write_utc)
        [uint64]$steadyStartNs = [uint64]$Facts.steady_started_ns
        [uint64]$tenSeconds = 10000000000
        if ($createdNs -gt $steadyStartNs -or
                ($steadyStartNs -gt $tenSeconds -and $createdNs -lt $steadyStartNs - $tenSeconds) -or
                $captureWriteNs -gt $createdNs + 1000000000 -or
                $fileWriteNs + 1000000000 -lt $createdNs -or
                $fileWriteNs -gt $steadyStartNs + 2000000000) {
            throw 'Wire-ready artifact was not freshly published immediately before steady.'
        }
        $actualFileWrite = (Get-Item -LiteralPath $witnessPath).LastWriteTimeUtc.ToString('o')
        if ([string]$manifestWitness.file_last_write_utc -cne $actualFileWrite) {
            throw 'Wire-ready witness file timestamp drifted after manifest generation.'
        }

        [int64]$prefixBytes = $source.capture_prefix_bytes
        $prefix = Invoke-WireReadyPrefixDecode $NwPp $normalizedCapture $prefixBytes `
            $ItemsPath $caseId $Topology
        if ($null -eq $prefix.facts) { return $false }
        if ([string]$prefix.sha256 -cne [string]$source.capture_prefix_sha256 -or
                [string]$prefix.sha256 -cne [string]$manifestSource.capture_prefix_sha256) {
            throw 'Wire-ready hash is not an exact prefix of the completed capture.'
        }
        if ($prefix.facts.packets.Count -ne [int]$source.parity_packet_count -or
                $prefix.facts.events.Count -ne [int]$source.parity_event_count) {
            throw 'Wire-ready prefix packet/event counts do not match independent decoding.'
        }
        $derived = Get-WireReadyEvidenceFromFacts $prefix.facts $Case $Corpus `
            $missionIdentity
        if (($derived.evidence | ConvertTo-Json -Depth 8 -Compress) -cne
                ($disk.evidence | ConvertTo-Json -Depth 8 -Compress)) {
            throw 'Wire-ready decoded evidence does not match independent prefix reconstruction.'
        }
        $steadyConnections = @($Facts.steady_states | Where-Object {
            $_.decode -and (($_.dir -eq 'C' -and $_.kind -eq 'uplink') -or
                ($_.dir -eq 'S' -and $_.kind -eq 'frame'))
        } | ForEach-Object { [string]$_.connection_key } | Select-Object -Unique)
        if ($steadyConnections.Count -ne 1 -or
                $steadyConnections[0] -cne [string]$derived.connection_key) {
            throw 'Wire-ready connection is not the exact steady-state connection.'
        }
        [uint64]$prefixLastNs = [uint64]$prefix.facts.last_ts_ns
        if ($prefixLastNs -gt $steadyStartNs + 1000000000) {
            throw 'Wire-ready capture prefix extends beyond steady start.'
        }
        foreach ($latest in @(
            [uint64]$derived.evidence.c2s_0c_last_ts_ns,
            [uint64]$derived.evidence.s2c_0a_last_ts_ns,
            [uint64]$derived.last_rtt_response_ts_ns
        )) {
            if ($latest -gt $createdNs + 1000000000 -or
                    $createdNs -gt $latest + 5000000000) {
                throw 'Wire-ready required traffic was stale when the witness was created.'
            }
        }
        return $true
    }
    catch {
        Add-ParityFailure 'WIRE_READY_WITNESS_INVALID' $caseId $Topology $_.Exception.Message
        return $false
    }
}

function Get-CaseException {
    param($Case, [string] $Topology, [string] $Direction, [string] $Kind, [string] $Tag)
    $parts = $Tag.Split(':')
    $wireTag = $parts[0]
    $settingsUpdate = $parts.Count -gt 1 -and $parts[1] -eq '1'
    foreach ($entry in @(Get-Field $Case "exceptions" @())) {
        $settingsMatches = -not (Test-Field $entry "settings_update") -or
            [bool](Get-Field $entry "settings_update" $false) -eq $settingsUpdate
        if ("$(Get-Field $entry 'topology' '')" -eq $Topology -and
                "$(Get-Field $entry 'direction' '')" -eq $Direction -and
                "$(Get-Field $entry 'kind' '')" -eq $Kind -and
                (ConvertTo-NormalizedTag (Get-Field $entry "tag" "-1")) -eq $wireTag -and
                $settingsMatches) {
            if ([string]::IsNullOrWhiteSpace("$(Get-Field $entry 'reason' '')")) {
                throw "Every parity exception needs a non-empty reason."
            }
            return $entry
        }
    }
    return $null
}

function Get-FirstTagOrder {
    param($Facts, [string] $Direction, [string[]] $AllowedTags)
    $seen = @{}
    $result = [System.Collections.Generic.List[string]]::new()
    $events = @(Get-FactCollection $Facts 'comparison_events' 'events')
    foreach ($event in @($events | Where-Object { $_.dir -eq $Direction })) {
        if ($AllowedTags -notcontains $event.identity -or $seen.ContainsKey($event.identity)) { continue }
        $seen[$event.identity] = $true
        $result.Add($event.identity)
    }
    return @($result.ToArray())
}

function Test-EventBodyMaterialized {
    param($Event)
    [byte[]]$body = @(Get-Field $Event 'body' ([byte[]]@()))
    $default = $body.Length -eq [int](Get-Field $Event 'length' -1)
    return [bool](Get-Field $Event 'body_materialized' $default)
}

function Test-BodyHasNonzeroByte {
    param($Event)
    [byte[]]$body = @(Get-Field $Event 'body' ([byte[]]@()))
    return @($body | Where-Object { $_ -ne 0 }).Count -gt 0
}

# The initial reliable stream is deterministic after normalizing the live tick
# at the start of S2C 0x0F. Compare every other material payload byte, not just
# tag/length/order. Explicitly dynamic cryptographic, challenge, and clock
# records are validated by their dedicated stateful checks below.
function Compare-InitialMaterialBodies {
    param($Case, [string] $Topology, [string] $Direction, $Golden, $Subject)

    $caseId = "$(Get-Field $Case 'id' '')"
    $goldEvents = @(Get-FactCollection $Golden 'initial_events' 'events' |
        Where-Object { $_.dir -eq $Direction })
    $subjectEvents = @(Get-FactCollection $Subject 'initial_events' 'events' |
        Where-Object { $_.dir -eq $Direction })
    $dynamic = if ($Direction -eq 'C') {
        @('02:0','08:0','1c:0','20:0','21:0','28:0','3d:0')
    } else {
        # 0x19 is a live GetTickCount timestamp. 0x46 is a flag-gated
        # player-slot delta whose score/timer fields legitimately vary across
        # independent processes. Their shapes and non-constant/nonzero
        # behavior are still checked by Compare-ParityDirection.
        @('02:0','19:0','30:0','31:0','39:0','43:0','46:0','68:0')
    }
    $tags = @($goldEvents.identity + $subjectEvents.identity | Sort-Object -Unique)
    foreach ($tag in $tags) {
        if ($dynamic -contains $tag) { continue }
        $goldTagEvents = @($goldEvents | Where-Object { $_.identity -eq $tag })
        $subjectTagEvents = @($subjectEvents | Where-Object { $_.identity -eq $tag })
        if ($goldTagEvents.Count -eq 0 -or $subjectTagEvents.Count -eq 0) { continue }
        if (@($goldTagEvents + $subjectTagEvents | Where-Object {
                -not (Test-EventBodyMaterialized $_)
            }).Count -ne 0) {
            Add-ParityFailure 'INITIAL_BODY_UNMATERIALIZED' $caseId $Topology `
                "$Direction 0x$($tag.Substring(0,2)) initial payload bytes were omitted by the decoder."
            continue
        }
        $goldBodies = @($goldTagEvents | ForEach-Object {
            Get-StableBodyKey $_ $Direction $tag
        })
        $subjectBodies = @($subjectTagEvents | ForEach-Object {
            Get-StableBodyKey $_ $Direction $tag
        })
        if (($goldBodies -join ',') -ne ($subjectBodies -join ',') -and
                $null -eq (Get-CaseException $Case $Topology $Direction 'body' $tag)) {
            Add-ParityFailure 'INITIAL_BODY_MISMATCH' $caseId $Topology `
                "$Direction 0x$($tag.Substring(0,2)) initial payload sequence differs byte-for-byte from same-case RR."
        }
    }
}

function Compare-ParityDirection {
    param(
        $Case, [string] $Topology, [string] $Direction,
        $Golden, $Subject, $Policy
    )
    $caseId = "$(Get-Field $Case 'id' '')"
    $goldHistogram = Get-ComparisonHistogram $Golden
    $subjectHistogram = Get-ComparisonHistogram $Subject
    $goldComparisonEvents = @(Get-FactCollection $Golden 'comparison_events' 'events')
    $subjectComparisonEvents = @(Get-FactCollection $Subject 'comparison_events' 'events')
    $goldTags = @($goldHistogram.Keys | Where-Object { $_ -like "${Direction}:*" } |
        ForEach-Object { $_.Substring(2) } | Sort-Object -Unique)
    $subjectTags = @($subjectHistogram.Keys | Where-Object { $_ -like "${Direction}:*" } |
        ForEach-Object { $_.Substring(2) } | Sort-Object -Unique)

    foreach ($tag in $goldTags) {
        if ($subjectTags -notcontains $tag -and
                $null -eq (Get-CaseException $Case $Topology $Direction "missing" $tag)) {
            Add-ParityFailure "MISSING_TAG" $caseId $Topology `
                "$Direction 0x$($tag.Substring(0,2)) settings=$($tag.Substring(3)) exists in same-case RR but not in $Topology."
        }
    }
    foreach ($tag in $subjectTags) {
        if ($goldTags -notcontains $tag -and
                $null -eq (Get-CaseException $Case $Topology $Direction "extra" $tag)) {
            $code = if ($Topology -eq "RO" -and $Direction -eq "C") { "UNEXPECTED_C2S" } else { "UNEXPECTED_TAG" }
            Add-ParityFailure $code $caseId $Topology `
                "$Direction 0x$($tag.Substring(0,2)) settings=$($tag.Substring(3)) is absent from the same-case RR golden."
        }
    }

    $common = @($goldTags | Where-Object { $subjectTags -contains $_ })
    $absoluteTolerance = [double](Get-Field $Policy "count_absolute_tolerance" 1)
    $relativeTolerance = [double](Get-Field $Policy "count_relative_tolerance" 0.20)
    foreach ($tag in $common) {
        $goldCount = [double]$goldHistogram["${Direction}:$tag"]
        $subjectCount = [double]$subjectHistogram["${Direction}:$tag"]
        $expected = $goldCount
        $goldDuration = [double](Get-Field $Golden 'comparison_duration_ms' $Golden.duration_ms)
        $subjectDuration = [double](Get-Field $Subject 'comparison_duration_ms' $Subject.duration_ms)
        if ($goldDuration -gt 0 -and $subjectDuration -gt 0 -and $goldCount -ge 20) {
            $expected = $goldCount * $subjectDuration / $goldDuration
        }
        $tolerance = [Math]::Max($absoluteTolerance, [Math]::Ceiling($expected * $relativeTolerance))
        if ([Math]::Abs($subjectCount - $expected) -gt $tolerance -and
                $null -eq (Get-CaseException $Case $Topology $Direction "count" $tag)) {
            Add-ParityFailure "COUNT_MISMATCH" $caseId $Topology `
                "$Direction 0x$($tag.Substring(0,2)) settings=$($tag.Substring(3)) count $subjectCount differs from RR-normalized $([Math]::Round($expected, 2)) (tolerance $tolerance)."
        }

        # Every event retains its declared wire length, even when its body is
        # intentionally not materialized. A tag/count/order clone with a
        # zeroed or fixed-size payload is therefore not wire parity.
        $goldEvents = @($goldComparisonEvents | Where-Object {
            $_.dir -eq $Direction -and $_.identity -eq $tag
        })
        $subjectEvents = @($subjectComparisonEvents | Where-Object {
            $_.dir -eq $Direction -and $_.identity -eq $tag
        })
        $goldLengths = Get-ExactSetKey @($goldEvents | ForEach-Object {
            [int](Get-Field $_ 'length' 0)
        })
        $subjectLengths = Get-ExactSetKey @($subjectEvents | ForEach-Object {
            [int](Get-Field $_ 'length' 0)
        })
        $dynamicFrameBody = ($Direction -eq 'C' -and $tag -eq '0c:0') -or
            ($Direction -eq 'S' -and $tag -eq '0a:0')
        if ($dynamicFrameBody) {
            $goldLengthValues = @($goldEvents | ForEach-Object { [int](Get-Field $_ 'length' 0) })
            $subjectLengthValues = @($subjectEvents | ForEach-Object { [int](Get-Field $_ 'length' 0) })
            $goldUniqueLengthCount = @($goldLengthValues | Select-Object -Unique).Count
            $subjectUniqueLengthCount = @($subjectLengthValues | Select-Object -Unique).Count
            if (($subjectLengthValues -contains 0) -and -not ($goldLengthValues -contains 0)) {
                Add-ParityFailure "LENGTH_SET_MISMATCH" $caseId $Topology `
                    "$Direction 0x$($tag.Substring(0,2)) introduced a zero-length dynamic frame absent from RR."
            }
            if ($goldUniqueLengthCount -gt 1 -and $subjectEvents.Count -ge 8 -and
                    $subjectUniqueLengthCount -eq 1) {
                Add-ParityFailure "LENGTH_CONSTANT" $caseId $Topology `
                    "$Direction 0x$($tag.Substring(0,2)) collapsed RR's variable frame sizes to one constant length."
            }
        } elseif ($goldLengths -ne $subjectLengths -and
                $null -eq (Get-CaseException $Case $Topology $Direction "length" $tag)) {
            Add-ParityFailure "LENGTH_SET_MISMATCH" $caseId $Topology `
                "$Direction 0x$($tag.Substring(0,2)) wire-length set differs from RR: RR=$goldLengths subject=$subjectLengths."
        }

        # Every ordinary body is now visible. Even when the field is dynamic
        # enough to preclude byte equality across independent runs, reject the
        # two common hardcoding failures: collapsing a varied RR stream to one
        # constant and replacing a nonzero RR stream with all-zero bytes.
        if (-not $dynamicFrameBody -and
                @($goldEvents + $subjectEvents | Where-Object {
                    -not (Test-EventBodyMaterialized $_)
                }).Count -eq 0) {
            $goldBodyKeys = @($goldEvents | ForEach-Object { ConvertTo-CompactHex $_.body })
            $subjectBodyKeys = @($subjectEvents | ForEach-Object { ConvertTo-CompactHex $_.body })
            $goldBodyDiversity = @($goldBodyKeys | Sort-Object -Unique).Count
            $subjectBodyDiversity = @($subjectBodyKeys | Sort-Object -Unique).Count
            if ($goldBodyDiversity -gt 1 -and $subjectEvents.Count -ge 8 -and
                    $subjectBodyDiversity -eq 1) {
                Add-ParityFailure 'BODY_CONSTANT' $caseId $Topology `
                    "$Direction 0x$($tag.Substring(0,2)) collapsed RR's varied payloads to one constant body."
            }
            $goldHasNonzero = @($goldEvents | Where-Object {
                Test-BodyHasNonzeroByte $_
            }).Count -gt 0
            $subjectHasNonzero = @($subjectEvents | Where-Object {
                Test-BodyHasNonzeroByte $_
            }).Count -gt 0
            if ($goldHasNonzero -and -not $subjectHasNonzero) {
                Add-ParityFailure 'BODY_ALL_ZERO' $caseId $Topology `
                    "$Direction 0x$($tag.Substring(0,2)) payloads are all zero although same-case RR carries nonzero bytes."
            }
        }

        # These payloads are stable for a same-case matrix and directly drive
        # team/deployment/loadout behavior. Compare their exact observed body
        # sets, not merely their tags and sizes, to reject same-length constants.
        $stableBodies = ($Direction -eq 'C' -and @(
                '0a:0','0e:0','2f:0','47:0'
            ) -contains $tag) -or
            ($Direction -eq 'S' -and @(
                '00:1','04:0','08:0','0b:0','0f:0','11:0','1c:0','2a:0',
                '2c:0','5a:0','60:0','66:0','75:0','76:0','7b:0'
            ) -contains $tag)
        if ($stableBodies) {
            if (@($goldEvents + $subjectEvents | Where-Object {
                    [byte[]]$eventBody = @(Get-Field $_ 'body' ([byte[]]@()))
                    $defaultMaterial = $eventBody.Length -eq [int](Get-Field $_ 'length' -1)
                    -not [bool](Get-Field $_ 'body_materialized' $defaultMaterial)
                }).Count -ne 0) {
                Add-ParityFailure "STABLE_BODY_UNMATERIALIZED" $caseId $Topology `
                    "$Direction 0x$($tag.Substring(0,2)) stable payload bytes were omitted by the decoder."
                continue
            }
            $goldBodies = Get-ExactSetKey @($goldEvents | ForEach-Object {
                Get-StableBodyKey $_ $Direction $tag
            })
            $subjectBodies = Get-ExactSetKey @($subjectEvents | ForEach-Object {
                Get-StableBodyKey $_ $Direction $tag
            })
            if ($goldBodies -ne $subjectBodies -and
                    $null -eq (Get-CaseException $Case $Topology $Direction "body" $tag)) {
                Add-ParityFailure "STABLE_BODY_MISMATCH" $caseId $Topology `
                    "$Direction 0x$($tag.Substring(0,2)) stable payload set differs from same-case RR."
            }
        }
    }

    # 0x30/0x31 are one process-global alternating phase. The phase advances
    # even with no recipient and survives a mission reset, so a remote capture
    # can legitimately enter either half depending on join timing. Their
    # alternation and scoreboard grouping are checked statefully below; do not
    # turn capture-local phase into a false first-occurrence mismatch.
    $orderedCommon = @($common | Where-Object {
        $_ -ne '30:0' -and $_ -ne '31:0'
    })
    $goldOrder = Get-FirstTagOrder $Golden $Direction $orderedCommon
    $subjectOrder = Get-FirstTagOrder $Subject $Direction $orderedCommon
    if (($goldOrder -join ',') -ne ($subjectOrder -join ',')) {
        Add-ParityFailure "FIRST_ORDER_MISMATCH" $caseId $Topology `
            "$Direction first-occurrence order differs from RR: RR=$($goldOrder -join ',') subject=$($subjectOrder -join ',')."
    }
}

function Get-ExactSetKey {
    param([object[]] $Values)
    return (@($Values | ForEach-Object { "$_" } | Sort-Object -Unique) -join ',')
}

function Get-MovingHandleCount {
    param([object[]] $Entities)
    return Get-ChangingHandleCount $Entities {
        param($entity)
        "$($entity.pos_x),$($entity.pos_y),$($entity.pos_z)"
    }
}

function Get-ChangingHandleCount {
    param([object[]] $Entities, [scriptblock] $ValueSelector)
    $positions = @{}
    foreach ($entity in $Entities) {
        $connection = "$(Get-Field $entity 'connection_key' (Get-Field $entity 'session' ''))"
        $sid = "$(Get-Field $entity 'sid' '')"
        $key = "$connection|$sid|$($entity.handle)"
        if (-not $positions.ContainsKey($key)) { $positions[$key] = @{} }
        $positions[$key]["$(& $ValueSelector $entity)"] = $true
    }
    return @($positions.Keys | Where-Object { $positions[$_].Count -gt 1 }).Count
}

function Test-ControllableInputWitness {
    param($Facts, $Case, [string] $Topology)
    $caseId = "$(Get-Field $Case 'id' '')"
    $steadyStates = @(Get-FactCollection $Facts 'steady_states' 'states')
    $uplinks = @($steadyStates | Where-Object {
        "$(Get-Field $_ 'dir' 'C')" -eq 'C' -and
            "$(Get-Field $_ 'kind' 'uplink')" -eq 'uplink' -and
            [bool](Get-Field $_ 'decode' $true)
    })
    if ($uplinks.Count -lt 8) {
        Add-ParityFailure 'INPUT_SAMPLE_COVERAGE' $caseId $Topology `
            "Controllable case decoded only $($uplinks.Count) C2S uplinks; require at least 8."
    }
    if ((Get-ChangingHandleCount $uplinks {
                param($state)
                "$($state.pos_x),$($state.pos_y),$($state.pos_z)"
            }) -lt 1) {
        Add-ParityFailure 'INPUT_POSITION_CONSTANT' $caseId $Topology `
            'Programmatically exercised client input produced no changing C2S position.'
    }
    if ((Get-ChangingHandleCount $uplinks {
                param($state)
                "$($state.orient_a),$($state.orient_b)"
            }) -lt 1) {
        Add-ParityFailure 'INPUT_ORIENTATION_CONSTANT' $caseId $Topology `
            'Programmatically exercised client input produced no changing C2S orientation.'
    }
    if (@($uplinks | Where-Object {
                [uint32]$_.move -ne 0 -or [uint32]$_.state_a -ne 0
            }).Count -eq 0) {
        Add-ParityFailure 'INPUT_STATE_IDLE' $caseId $Topology `
            'Programmatically exercised client input never produced a nonzero move/state field.'
    }
    if ([bool](Get-Field $Case 'require_analog' $false) -and
            @($uplinks | Where-Object {
                [uint32]$_.analog_x -ne 0 -or [uint32]$_.analog_y -ne 0 -or
                    [uint32]$_.analog_z -ne 0
            }).Count -eq 0) {
        Add-ParityFailure 'INPUT_ANALOG_ZERO' $caseId $Topology `
            'This vehicle/control case requires a nonzero C2S analog-axis witness.'
    }
}

function Test-MinimumDiversity {
    param(
        [string] $CaseId, [string] $Topology, [string] $Label,
        [int] $GoldenCount, [int] $SubjectCount, [double] $Ratio
    )
    if ($GoldenCount -lt 2) { return }
    $minimum = [Math]::Max(2, [Math]::Ceiling($GoldenCount * $Ratio))
    if ($SubjectCount -lt $minimum) {
        Add-ParityFailure "SEMANTIC_DIVERSITY" $CaseId $Topology `
            "$Label exposes $SubjectCount distinct values; same-case RR exposes $GoldenCount (minimum $minimum)."
    }
}

function Get-NumericPercentile {
    param([object[]] $Values, [double] $Fraction)
    $ordered = @($Values | ForEach-Object { [double]$_ } | Sort-Object)
    if ($ordered.Count -eq 0) { return 0.0 }
    $bounded = [Math]::Max(0.0, [Math]::Min(1.0, $Fraction))
    $index = [int][Math]::Floor(($ordered.Count - 1) * $bounded)
    return [double]$ordered[$index]
}

function Get-WrappedDelta {
    param([double] $Current, [double] $Previous, [double] $Modulus)
    $delta = $Current - $Previous
    if ($Modulus -gt 0) {
        $half = $Modulus / 2.0
        while ($delta -gt $half) { $delta -= $Modulus }
        while ($delta -lt -$half) { $delta += $Modulus }
    }
    return $delta
}

function Get-DynamicVectorStats {
    param(
        [object[]] $Samples,
        [string[]] $Fields,
        [string[]] $GroupFields = @(),
        [double] $CyclicModulus = 0
    )
    $ordered = @($Samples | Sort-Object `
        @{Expression={ [uint64](Get-Field $_ 'ts_ns' 0) }},
        @{Expression={ [int](Get-Field $_ 'frame' 0) }})
    $baseline = @()
    if ($ordered.Count -gt 0) {
        $baseline = @($Fields | ForEach-Object {
            [double](Get-Field $ordered[0] $_ 0)
        })
    }

    $groups = @{}
    foreach ($sample in $ordered) {
        $key = if ($GroupFields.Count -eq 0) {
            '__all__'
        } else {
            @($GroupFields | ForEach-Object { "$(Get-Field $sample $_ '')" }) -join '|'
        }
        if (-not $groups.ContainsKey($key)) {
            $groups[$key] = [System.Collections.Generic.List[object]]::new()
        }
        $groups[$key].Add($sample)
    }

    $ranges = [System.Collections.Generic.List[double]]::new()
    $displacements = [System.Collections.Generic.List[double]]::new()
    $steps = [System.Collections.Generic.List[double]]::new()
    $stepShapes = @{}
    $movingSteps = 0
    $cycleChecks = 0
    $cycleHits = 0
    $sequenceCount = 0
    foreach ($key in $groups.Keys) {
        $sequence = @($groups[$key].ToArray() | Sort-Object `
            @{Expression={ [uint64](Get-Field $_ 'ts_ns' 0) }},
            @{Expression={ [int](Get-Field $_ 'frame' 0) }})
        if ($sequence.Count -lt 2) { continue }
        $sequenceCount++
        $origin = @($Fields | ForEach-Object {
            [double](Get-Field $sequence[0] $_ 0)
        })
        $mins = @($Fields | ForEach-Object { 0.0 })
        $maxs = @($Fields | ForEach-Object { 0.0 })
        $normalizedKeys = [System.Collections.Generic.List[string]]::new()
        $previous = $null
        for ($i = 0; $i -lt $sequence.Count; $i++) {
            $current = @($Fields | ForEach-Object {
                [double](Get-Field $sequence[$i] $_ 0)
            })
            $normalized = @()
            $displacement = 0.0
            for ($axis = 0; $axis -lt $Fields.Count; $axis++) {
                $deltaFromOrigin = Get-WrappedDelta `
                    $current[$axis] $origin[$axis] $CyclicModulus
                $normalized += $deltaFromOrigin
                $displacement += [Math]::Abs($deltaFromOrigin)
                if ($deltaFromOrigin -lt $mins[$axis]) { $mins[$axis] = $deltaFromOrigin }
                if ($deltaFromOrigin -gt $maxs[$axis]) { $maxs[$axis] = $deltaFromOrigin }
            }
            $displacements.Add($displacement)
            $normalizedKey = @($normalized | ForEach-Object { '{0:R}' -f [double]$_ }) -join ','
            $normalizedKeys.Add($normalizedKey)
            if ($null -ne $previous) {
                $step = 0.0
                $shape = @()
                for ($axis = 0; $axis -lt $Fields.Count; $axis++) {
                    $component = Get-WrappedDelta `
                        $current[$axis] $previous[$axis] $CyclicModulus
                    $step += [Math]::Abs($component)
                    $shape += ('{0:R}' -f [double]$component)
                }
                if ($step -gt 0) {
                    $movingSteps++
                    $steps.Add($step)
                    $stepShapes[$shape -join ','] = $true
                    $cycleChecks++
                    $oldest = [Math]::Max(0, $i - 8)
                    for ($prior = $i - 2; $prior -ge $oldest; $prior--) {
                        if ($prior -ge 0 -and $normalizedKeys[$prior] -eq $normalizedKey) {
                            $cycleHits++
                            break
                        }
                    }
                }
            }
            $previous = $current
        }
        $range = 0.0
        for ($axis = 0; $axis -lt $Fields.Count; $axis++) {
            $range += $maxs[$axis] - $mins[$axis]
        }
        $ranges.Add($range)
    }

    return [pscustomobject]@{
        sample_count = $ordered.Count
        sequence_count = $sequenceCount
        baseline = $baseline
        range_p90 = Get-NumericPercentile $ranges.ToArray() 0.90
        range_max = Get-NumericPercentile $ranges.ToArray() 1.0
        displacement_p90 = Get-NumericPercentile $displacements.ToArray() 0.90
        step_p50 = Get-NumericPercentile $steps.ToArray() 0.50
        step_p90 = Get-NumericPercentile $steps.ToArray() 0.90
        step_max = Get-NumericPercentile $steps.ToArray() 1.0
        step_shapes = $stepShapes.Count
        moving_steps = $movingSteps
        short_cycle_fraction = if ($cycleChecks -gt 0) {
            [double]$cycleHits / [double]$cycleChecks
        } else { 0.0 }
    }
}

function Compare-DynamicVectorAgreement {
    param(
        [string] $CaseId,
        [string] $Topology,
        [string] $Label,
        [object[]] $GoldenSamples,
        [object[]] $SubjectSamples,
        [string[]] $Fields,
        $Policy,
        [bool] $CompareBaseline = $false,
        [string[]] $GroupFields = @(),
        [double] $CyclicModulus = 0
    )
    $minimumSamples = [int](Get-Field $Policy 'dynamic_min_samples' 8)
    if ($GoldenSamples.Count -lt $minimumSamples -or
            $SubjectSamples.Count -lt $minimumSamples) { return }
    $gold = Get-DynamicVectorStats $GoldenSamples $Fields $GroupFields $CyclicModulus
    $subject = Get-DynamicVectorStats $SubjectSamples $Fields $GroupFields $CyclicModulus

    if ($CompareBaseline -and $gold.baseline.Count -eq $Fields.Count -and
            $subject.baseline.Count -eq $Fields.Count) {
        $baselineDifference = 0.0
        for ($axis = 0; $axis -lt $Fields.Count; $axis++) {
            $difference = [Math]::Abs((Get-WrappedDelta `
                $subject.baseline[$axis] $gold.baseline[$axis] $CyclicModulus))
            $baselineDifference = [Math]::Max($baselineDifference, $difference)
        }
        $baselineTolerance = [double](Get-Field $Policy `
            'spatial_baseline_tolerance_fixed16' 4194304)
        if ($baselineDifference -gt $baselineTolerance) {
            Add-ParityFailure "DYNAMIC_BASELINE_MISMATCH" $CaseId $Topology `
                "$Label initial baseline differs from same-case RR by $baselineDifference (tolerance $baselineTolerance)."
        }
    }

    $ratioLimit = [double](Get-Field $Policy 'dynamic_scale_ratio_limit' 8.0)
    foreach ($metric in @(
        [pscustomobject]@{ name='range_p90'; code='DYNAMIC_RANGE_MISMATCH'; label='origin-normalized range' },
        [pscustomobject]@{ name='displacement_p90'; code='DYNAMIC_RANGE_MISMATCH'; label='origin-normalized displacement' },
        [pscustomobject]@{ name='step_p90'; code='DYNAMIC_STEP_MISMATCH'; label='nonzero step p90' }
    )) {
        $goldValue = [double]$gold.($metric.name)
        $subjectValue = [double]$subject.($metric.name)
        if ($goldValue -gt 0 -and
                ($subjectValue -lt $goldValue / $ratioLimit -or
                 $subjectValue -gt $goldValue * $ratioLimit)) {
            Add-ParityFailure $metric.code $CaseId $Topology `
                "$Label $($metric.label) $subjectValue is outside the RR-relative 1/$ratioLimit..${ratioLimit}x band around $goldValue."
        }
    }

    $diversityRatio = [double](Get-Field $Policy 'semantic_diversity_ratio' 0.25)
    if ($gold.step_shapes -ge 2) {
        $minimumShapes = [Math]::Max(2, [Math]::Ceiling($gold.step_shapes * $diversityRatio))
        if ($subject.step_shapes -lt $minimumShapes) {
            Add-ParityFailure "DYNAMIC_STEP_SHAPE_MISMATCH" $CaseId $Topology `
                "$Label exposes $($subject.step_shapes) distinct nonzero step vectors; RR exposes $($gold.step_shapes) (minimum $minimumShapes)."
        }
    }

    $cycleLimit = [double](Get-Field $Policy 'dynamic_short_cycle_limit' 0.60)
    if ($subject.moving_steps -ge $minimumSamples -and
            $subject.short_cycle_fraction -gt $cycleLimit -and
            $subject.short_cycle_fraction -gt $gold.short_cycle_fraction + 0.20) {
        Add-ParityFailure "DYNAMIC_SHORT_CYCLE" $CaseId $Topology `
            "$Label revisits a 2..8-sample origin-normalized cycle in $([Math]::Round(100 * $subject.short_cycle_fraction,1))% of moving steps versus $([Math]::Round(100 * $gold.short_cycle_fraction,1))% in RR."
    }
}

function Get-OriginNormalizedScalarKey {
    param(
        [object[]] $Items,
        [string] $Field,
        [int64] $Modulus = 0,
        [int] $PrefixSamples = 16
    )
    $keys = [System.Collections.Generic.List[string]]::new()
    foreach ($group in @($Items | Group-Object {
                "$(Get-Field $_ 'connection_key' '')|$(Get-Field $_ 'sid' 0)|$(Get-Field $_ 'session' 0)"
            })) {
        $ordered = @($group.Group | Sort-Object frame,ts_ns)
        if ($ordered.Count -eq 0) { continue }
        [int64[]]$values = @($ordered | ForEach-Object {
            [int64](Get-Field $_ $Field 0)
        })
        $distinct = @($values | Sort-Object -Unique)
        if ($distinct.Count -eq 1) {
            $keys.Add("constant:$($distinct[0])")
            continue
        }
        $steps = [System.Collections.Generic.List[int64]]::new()
        $prefix = [System.Collections.Generic.List[int64]]::new()
        [int64]$cumulative = 0
        $prefix.Add(0)
        for ($i = 1; $i -lt $values.Count; $i++) {
            [int64]$step = if ($Modulus -gt 0) {
                Get-WrappedDelta $values[$i - 1] $values[$i] $Modulus
            } else {
                $values[$i] - $values[$i - 1]
            }
            $steps.Add($step)
            $cumulative += $step
            if ($prefix.Count -lt $PrefixSamples) { $prefix.Add($cumulative) }
        }
        $stepKey = @($steps.ToArray() | Sort-Object -Unique) -join ','
        $prefixKey = @($prefix.ToArray()) -join ','
        $keys.Add("dynamic:steps=${stepKey}:prefix=$prefixKey")
    }
    return @($keys.ToArray() | Sort-Object) -join ';'
}

function Compare-SemanticDirection {
    param(
        $Case, [string] $Topology, [string] $Direction,
        $Golden, $Subject, $Policy
    )
    $caseId = "$(Get-Field $Case 'id' '')"
    $diversityRatio = [double](Get-Field $Policy 'semantic_diversity_ratio' 0.25)
    $goldSteadyStates = @(Get-FactCollection $Golden 'steady_states' 'states')
    $subjectSteadyStates = @(Get-FactCollection $Subject 'steady_states' 'states')
    $goldSteadyEntities = @(Get-FactCollection $Golden 'steady_entities' 'entities')
    $subjectSteadyEntities = @(Get-FactCollection $Subject 'steady_entities' 'entities')
    if ($Direction -eq 'C') {
        $goldStates = @($goldSteadyStates | Where-Object { $_.kind -eq 'uplink' -and $_.decode })
        $subjectStates = @($subjectSteadyStates | Where-Object { $_.kind -eq 'uplink' -and $_.decode })
        $goldShapes = Get-ExactSetKey @($goldStates | ForEach-Object { "$($_.length)|$($_.type_id)|$($_.sub_op)" })
        $subjectShapes = Get-ExactSetKey @($subjectStates | ForEach-Object { "$($_.length)|$($_.type_id)|$($_.sub_op)" })
        if ($goldShapes -ne $subjectShapes) {
            Add-ParityFailure "UPLINK_SHAPE_MISMATCH" $caseId $Topology `
                "C2S 0x0C decoded length/type/sub-op shapes differ from same-case RR."
        }
        $goldAdm = Get-ExactSetKey @($goldStates | ForEach-Object { $_.adm })
        $subjectAdm = Get-ExactSetKey @($subjectStates | ForEach-Object { $_.adm })
        if ($goldAdm -ne $subjectAdm) {
            Add-ParityFailure "UPLINK_LOADOUT_MISMATCH" $caseId $Topology `
                "C2S 0x0C equipped ADM values differ from same-case RR."
        }
        $goldState = Get-ExactSetKey @($goldStates | ForEach-Object { $_.state_a })
        $subjectState = Get-ExactSetKey @($subjectStates | ForEach-Object { $_.state_a })
        if ($goldState -ne $subjectState) {
            Add-ParityFailure "UPLINK_STATE_MISMATCH" $caseId $Topology `
                "C2S 0x0C player-state flag values differ from same-case RR."
        }
        $goldMove = Get-ExactSetKey @($goldStates | ForEach-Object { $_.move })
        $subjectMove = Get-ExactSetKey @($subjectStates | ForEach-Object { $_.move })
        if ($goldMove -ne $subjectMove) {
            Add-ParityFailure "UPLINK_MOVE_MISMATCH" $caseId $Topology `
                "C2S 0x0C movement-state values differ from same-case RR."
        }
        # Carrier is a session-local wire handle. Compare the semantic
        # none/bound state while deliberately normalizing the handle itself.
        $goldCarrier = Get-ExactSetKey @($goldStates | ForEach-Object {
            if ($_.carrier -eq 0xffff) { 'none' } else { 'bound' }
        })
        $subjectCarrier = Get-ExactSetKey @($subjectStates | ForEach-Object {
            if ($_.carrier -eq 0xffff) { 'none' } else { 'bound' }
        })
        if ($goldCarrier -ne $subjectCarrier) {
            Add-ParityFailure "UPLINK_CARRIER_MISMATCH" $caseId $Topology `
                "C2S 0x0C carrier presence differs from same-case RR."
        }
        $goldPositions = @($goldStates | ForEach-Object { "$($_.pos_x),$($_.pos_y),$($_.pos_z)" } | Sort-Object -Unique)
        $subjectPositions = @($subjectStates | ForEach-Object { "$($_.pos_x),$($_.pos_y),$($_.pos_z)" } | Sort-Object -Unique)
        Test-MinimumDiversity $caseId $Topology 'C2S 0x0C player position' `
            $goldPositions.Count $subjectPositions.Count $diversityRatio
        $goldNonzero = @($goldStates | Where-Object { $_.pos_x -ne 0 -or $_.pos_y -ne 0 -or $_.pos_z -ne 0 }).Count
        $subjectNonzero = @($subjectStates | Where-Object { $_.pos_x -ne 0 -or $_.pos_y -ne 0 -or $_.pos_z -ne 0 }).Count
        if ($goldNonzero -gt 0 -and $subjectNonzero -eq 0) {
            Add-ParityFailure "UPLINK_ALL_ZERO" $caseId $Topology `
                "C2S 0x0C positions are all zero although same-case RR carries nonzero player state."
        }
        $goldMoving = Get-MovingHandleCount $goldStates
        $subjectMoving = Get-MovingHandleCount $subjectStates
        if ($goldMoving -gt 0 -and $subjectMoving -eq 0) {
            Add-ParityFailure "UPLINK_CONSTANT" $caseId $Topology `
                "C2S 0x0C never changes position for a player handle although same-case RR does."
        }
        $goldOrientation = @($goldStates | ForEach-Object { "$($_.orient_a),$($_.orient_b)" } | Sort-Object -Unique)
        $subjectOrientation = @($subjectStates | ForEach-Object { "$($_.orient_a),$($_.orient_b)" } | Sort-Object -Unique)
        Test-MinimumDiversity $caseId $Topology 'C2S 0x0C orientation' `
            $goldOrientation.Count $subjectOrientation.Count $diversityRatio
        $goldOrientationNonzero = @($goldStates | Where-Object {
            $_.orient_a -ne 0 -or $_.orient_b -ne 0
        }).Count
        $subjectOrientationNonzero = @($subjectStates | Where-Object {
            $_.orient_a -ne 0 -or $_.orient_b -ne 0
        }).Count
        if ($goldOrientationNonzero -gt 0 -and $subjectOrientationNonzero -eq 0) {
            Add-ParityFailure "UPLINK_ORIENTATION_ZERO" $caseId $Topology `
                "C2S 0x0C orientation is fixed at zero although same-case RR carries nonzero values."
        }
        $goldTurning = Get-ChangingHandleCount $goldStates {
            param($state)
            "$($state.orient_a),$($state.orient_b)"
        }
        $subjectTurning = Get-ChangingHandleCount $subjectStates {
            param($state)
            "$($state.orient_a),$($state.orient_b)"
        }
        if ($goldTurning -gt 0 -and $subjectTurning -eq 0) {
            Add-ParityFailure "UPLINK_ORIENTATION_CONSTANT" $caseId $Topology `
                "C2S 0x0C orientation never changes for a player handle although same-case RR does."
        }
        $goldAnalog = @($goldStates | ForEach-Object {
            "$($_.analog_x),$($_.analog_y),$($_.analog_z)"
        } | Sort-Object -Unique)
        $subjectAnalog = @($subjectStates | ForEach-Object {
            "$($_.analog_x),$($_.analog_y),$($_.analog_z)"
        } | Sort-Object -Unique)
        Test-MinimumDiversity $caseId $Topology 'C2S 0x0C analog input' `
            $goldAnalog.Count $subjectAnalog.Count $diversityRatio
        $goldAnalogNonzero = @($goldStates | Where-Object {
            $_.analog_x -ne 0 -or $_.analog_y -ne 0 -or $_.analog_z -ne 0
        }).Count
        $subjectAnalogNonzero = @($subjectStates | Where-Object {
            $_.analog_x -ne 0 -or $_.analog_y -ne 0 -or $_.analog_z -ne 0
        }).Count
        if ($goldAnalogNonzero -gt 0 -and $subjectAnalogNonzero -eq 0) {
            Add-ParityFailure "UPLINK_ANALOG_ZERO" $caseId $Topology `
                "C2S 0x0C analog input is fixed at zero although same-case RR carries nonzero values."
        }
        $goldPriority = @($goldStates | Where-Object { $_.priority_nonzero -gt 0 }).Count
        $subjectPriority = @($subjectStates | Where-Object { $_.priority_nonzero -gt 0 }).Count
        if ($goldPriority -gt 0 -and $subjectPriority -eq 0) {
            Add-ParityFailure "UPLINK_PRIORITY_EMPTY" $caseId $Topology `
                "C2S 0x0C entity-priority feedback is always empty although same-case RR populates it."
        }
        # Priority handles are allocated inside one live session and cannot be
        # compared numerically across independent captures. Preserve the four
        # wire ordinals while reducing each handle to occupied/empty; scores
        # remain exact evidence and therefore expose swapped, omitted, or
        # hardcoded priority slots.
        $goldPriorityFingerprints = Get-ExactSetKey @($goldStates | ForEach-Object {
            $state = $_
            $slots = for ($slot = 0; $slot -lt 4; $slot++) {
                $handle = [uint32](Get-Field $state "priority_handle_$slot" 0)
                $score = [uint32](Get-Field $state "priority_score_$slot" 0)
                "$(if ($handle -ne 0 -or $score -ne 0) { 'occupied' } else { 'empty' }):$score"
            }
            $slots -join ','
        })
        $subjectPriorityFingerprints = Get-ExactSetKey @($subjectStates | ForEach-Object {
            $state = $_
            $slots = for ($slot = 0; $slot -lt 4; $slot++) {
                $handle = [uint32](Get-Field $state "priority_handle_$slot" 0)
                $score = [uint32](Get-Field $state "priority_score_$slot" 0)
                "$(if ($handle -ne 0 -or $score -ne 0) { 'occupied' } else { 'empty' }):$score"
            }
            $slots -join ','
        })
        if ($goldPriorityFingerprints -ne $subjectPriorityFingerprints) {
            Add-ParityFailure "UPLINK_PRIORITY_MISMATCH" $caseId $Topology `
                "C2S 0x0C ordered priority occupancy/scores differ from same-case RR after normalizing session-local handles."
        }
        $goldPriorityDiversity = @($goldStates | ForEach-Object {
            $state = $_
            (0..3 | ForEach-Object {
                [uint32](Get-Field $state "priority_score_$_" 0)
            }) -join ','
        } | Sort-Object -Unique).Count
        $subjectPriorityDiversity = @($subjectStates | ForEach-Object {
            $state = $_
            (0..3 | ForEach-Object {
                [uint32](Get-Field $state "priority_score_$_" 0)
            }) -join ','
        } | Sort-Object -Unique).Count
        Test-MinimumDiversity $caseId $Topology 'C2S 0x0C ordered priority scores' `
            $goldPriorityDiversity $subjectPriorityDiversity $diversityRatio
        Compare-DynamicVectorAgreement $caseId $Topology 'C2S 0x0C position' `
            $goldStates $subjectStates @('pos_x','pos_y','pos_z') $Policy $true
        Compare-DynamicVectorAgreement $caseId $Topology 'C2S 0x0C orientation' `
            $goldStates $subjectStates @('orient_a','orient_b') $Policy $false @() 65536
        Compare-DynamicVectorAgreement $caseId $Topology 'C2S 0x0C analog input' `
            $goldStates $subjectStates @('analog_x','analog_y','analog_z') $Policy
        return
    }

    $goldFrames = @($goldSteadyStates | Where-Object { $_.kind -eq 'frame' -and $_.decode })
    $subjectFrames = @($subjectSteadyStates | Where-Object { $_.kind -eq 'frame' -and $_.decode })
    $goldSubs = Get-ExactSetKey @($goldFrames | ForEach-Object { $_.sub_block })
    $subjectSubs = Get-ExactSetKey @($subjectFrames | ForEach-Object { $_.sub_block })
    if ($goldSubs -ne $subjectSubs) {
        Add-ParityFailure "FRAME_SUBBLOCK_MISMATCH" $caseId $Topology `
            "S2C 0x0A sub-block coverage differs from same-case RR."
    }
    $goldFlags1 = Get-ExactSetKey @($goldFrames | ForEach-Object { $_.flags1 })
    $subjectFlags1 = Get-ExactSetKey @($subjectFrames | ForEach-Object { $_.flags1 })
    if ($goldFlags1 -ne $subjectFlags1) {
        Add-ParityFailure "FRAME_HEADER_FLAGS_MISMATCH" $caseId $Topology `
            "S2C 0x0A flags1 values differ from same-case RR."
    }
    $goldFlags2 = Get-ExactSetKey @($goldFrames | ForEach-Object { $_.flags2 })
    $subjectFlags2 = Get-ExactSetKey @($subjectFrames | ForEach-Object { $_.flags2 })
    if ($goldFlags2 -ne $subjectFlags2) {
        Add-ParityFailure "FRAME_HEADER_FLAGS_MISMATCH" $caseId $Topology `
            "S2C 0x0A flags2 values differ from same-case RR."
    }
    $goldLocalMount = Get-ExactSetKey @($goldFrames | ForEach-Object {
        if ([uint32](Get-Field $_ 'local_mount' 0xffff) -eq 0xffff) { 'none' } else { 'bound' }
    })
    $subjectLocalMount = Get-ExactSetKey @($subjectFrames | ForEach-Object {
        if ([uint32](Get-Field $_ 'local_mount' 0xffff) -eq 0xffff) { 'none' } else { 'bound' }
    })
    if ($goldLocalMount -ne $subjectLocalMount) {
        Add-ParityFailure "FRAME_LOCAL_MOUNT_MISMATCH" $caseId $Topology `
            "S2C 0x0A local mount presence differs from same-case RR after normalizing session-local handles."
    }
    $goldWeapon = Get-ExactSetKey @($goldFrames | Where-Object {
        [bool](Get-Field $_ 'weapon_present' $false)
    } | ForEach-Object {
        @(
            Get-Field $_ 'weapon_preround_timer' 0
            Get-Field $_ 'weapon_slot_state360' 0
            Get-Field $_ 'weapon_slot_state368' 0
            Get-Field $_ 'weapon_slot_state364' 0
            Get-Field $_ 'weapon_slot_state356' 0
            Get-Field $_ 'weapon_slot_state460' 0
            Get-Field $_ 'weapon_reload_seconds' 0
            Get-Field $_ 'weapon_uniform_team_mask' 0
        ) -join ','
    })
    $subjectWeapon = Get-ExactSetKey @($subjectFrames | Where-Object {
        [bool](Get-Field $_ 'weapon_present' $false)
    } | ForEach-Object {
        @(
            Get-Field $_ 'weapon_preround_timer' 0
            Get-Field $_ 'weapon_slot_state360' 0
            Get-Field $_ 'weapon_slot_state368' 0
            Get-Field $_ 'weapon_slot_state364' 0
            Get-Field $_ 'weapon_slot_state356' 0
            Get-Field $_ 'weapon_slot_state460' 0
            Get-Field $_ 'weapon_reload_seconds' 0
            Get-Field $_ 'weapon_uniform_team_mask' 0
        ) -join ','
    })
    $goldWeaponPresence = Get-ExactSetKey @($goldFrames | ForEach-Object {
        if ([bool](Get-Field $_ 'weapon_present' $false)) { 1 } else { 0 }
    })
    $subjectWeaponPresence = Get-ExactSetKey @($subjectFrames | ForEach-Object {
        if ([bool](Get-Field $_ 'weapon_present' $false)) { 1 } else { 0 }
    })
    if ($goldWeaponPresence -ne $subjectWeaponPresence -or
            $goldWeapon -ne $subjectWeapon) {
        Add-ParityFailure "FRAME_WEAPON_PHASE_MISMATCH" $caseId $Topology `
            "S2C 0x0A phase-1 weapon/mount-ammo payload values differ from same-case RR."
    }
    $goldTimerFrames = @($goldFrames | Where-Object {
        [bool](Get-Field $_ 'timer_present' $false)
    })
    $subjectTimerFrames = @($subjectFrames | Where-Object {
        [bool](Get-Field $_ 'timer_present' $false)
    })
    $goldTimerPresence = Get-ExactSetKey @($goldFrames | ForEach-Object {
        if ([bool](Get-Field $_ 'timer_present' $false)) { 1 } else { 0 }
    })
    $subjectTimerPresence = Get-ExactSetKey @($subjectFrames | ForEach-Object {
        if ([bool](Get-Field $_ 'timer_present' $false)) { 1 } else { 0 }
    })
    $goldTimerState = Get-ExactSetKey @($goldTimerFrames | ForEach-Object {
        @(
            Get-Field $_ 'timer_state0' 0
            Get-Field $_ 'timer_state1' 0
            Get-Field $_ 'timer_state2' 0
            Get-Field $_ 'timer_state3' 0
        ) -join ','
    })
    $subjectTimerState = Get-ExactSetKey @($subjectTimerFrames | ForEach-Object {
        @(
            Get-Field $_ 'timer_state0' 0
            Get-Field $_ 'timer_state1' 0
            Get-Field $_ 'timer_state2' 0
            Get-Field $_ 'timer_state3' 0
        ) -join ','
    })
    $goldTimerProgression = Get-OriginNormalizedScalarKey `
        $goldTimerFrames 'timer_seconds'
    $subjectTimerProgression = Get-OriginNormalizedScalarKey `
        $subjectTimerFrames 'timer_seconds'
    if ($goldTimerPresence -ne $subjectTimerPresence -or
            $goldTimerState -ne $subjectTimerState -or
            $goldTimerProgression -ne $subjectTimerProgression) {
        Add-ParityFailure "FRAME_TIMER_PHASE_MISMATCH" $caseId $Topology `
            "S2C 0x0A phase-0 timer presence/state/progression differs from same-case RR."
    }
    $goldEnvFrames = @($goldFrames | Where-Object {
        [bool](Get-Field $_ 'env_present' $false)
    })
    $subjectEnvFrames = @($subjectFrames | Where-Object {
        [bool](Get-Field $_ 'env_present' $false)
    })
    $goldEnvPresence = Get-ExactSetKey @($goldFrames | ForEach-Object {
        if ([bool](Get-Field $_ 'env_present' $false)) { 1 } else { 0 }
    })
    $subjectEnvPresence = Get-ExactSetKey @($subjectFrames | ForEach-Object {
        if ([bool](Get-Field $_ 'env_present' $false)) { 1 } else { 0 }
    })
    $goldEnvStable = Get-ExactSetKey @($goldEnvFrames | ForEach-Object {
        @(
            Get-Field $_ 'env_fog_dist' 0
            Get-Field $_ 'env_fog_accel' 0
            Get-Field $_ 'env_quake_ticks' 0
            Get-Field $_ 'env_cloud_scroll' 0
            Get-Field $_ 'env_rain_pct' 0
            Get-Field $_ 'env_overcast' 0
            Get-Field $_ 'env_param' 0
        ) -join ','
    })
    $subjectEnvStable = Get-ExactSetKey @($subjectEnvFrames | ForEach-Object {
        @(
            Get-Field $_ 'env_fog_dist' 0
            Get-Field $_ 'env_fog_accel' 0
            Get-Field $_ 'env_quake_ticks' 0
            Get-Field $_ 'env_cloud_scroll' 0
            Get-Field $_ 'env_rain_pct' 0
            Get-Field $_ 'env_overcast' 0
            Get-Field $_ 'env_param' 0
        ) -join ','
    })
    $goldEnvTod = Get-OriginNormalizedScalarKey `
        $goldEnvFrames 'env_tod_fixed' 65536
    $subjectEnvTod = Get-OriginNormalizedScalarKey `
        $subjectEnvFrames 'env_tod_fixed' 65536
    if ($goldEnvPresence -ne $subjectEnvPresence -or
            $goldEnvStable -ne $subjectEnvStable -or
            $goldEnvTod -ne $subjectEnvTod) {
        Add-ParityFailure "FRAME_ENV_PHASE_MISMATCH" $caseId $Topology `
            "S2C 0x0A phase-2 environment presence/weather/TOD progression differs from same-case RR."
    }
    $goldObjectivePresence = Get-ExactSetKey @($goldFrames | ForEach-Object {
        if ([bool](Get-Field $_ 'objective_present' $false)) { 1 } else { 0 }
    })
    $subjectObjectivePresence = Get-ExactSetKey @($subjectFrames | ForEach-Object {
        if ([bool](Get-Field $_ 'objective_present' $false)) { 1 } else { 0 }
    })
    $goldObjective = Get-ExactSetKey @($goldFrames | Where-Object {
        [bool](Get-Field $_ 'objective_present' $false)
    } | ForEach-Object {
        @(
            Get-Field $_ 'objective_state_0' 0
            Get-Field $_ 'objective_state_1' 0
            Get-Field $_ 'objective_state_2' 0
            Get-Field $_ 'objective_state_3' 0
        ) -join ','
    })
    $subjectObjective = Get-ExactSetKey @($subjectFrames | Where-Object {
        [bool](Get-Field $_ 'objective_present' $false)
    } | ForEach-Object {
        @(
            Get-Field $_ 'objective_state_0' 0
            Get-Field $_ 'objective_state_1' 0
            Get-Field $_ 'objective_state_2' 0
            Get-Field $_ 'objective_state_3' 0
        ) -join ','
    })
    if ($goldObjectivePresence -ne $subjectObjectivePresence -or
            $goldObjective -ne $subjectObjective) {
        Add-ParityFailure "FRAME_OBJECTIVE_PHASE_MISMATCH" $caseId $Topology `
            "S2C 0x0A phase-3 objective presence/words differ from same-case RR."
    }
    $goldPassengerPresence = Get-ExactSetKey @($goldFrames | ForEach-Object {
        if ([bool](Get-Field $_ 'passenger_present' $false)) { 1 } else { 0 }
    })
    $subjectPassengerPresence = Get-ExactSetKey @($subjectFrames | ForEach-Object {
        if ([bool](Get-Field $_ 'passenger_present' $false)) { 1 } else { 0 }
    })
    $goldPassenger = Get-ExactSetKey @($goldFrames | Where-Object {
        [bool](Get-Field $_ 'passenger_present' $false)
    } | ForEach-Object {
        $bound = [uint32](Get-Field $_ 'passenger_mount_handle' 0xffff) -ne 0xffff
        $hasMount = [bool](Get-Field $_ 'passenger_has_mount' $false)
        $clip = if ($bound) { [uint32](Get-Field $_ 'passenger_clip' 0) } else { '-' }
        $reserve = if ($bound) { [uint32](Get-Field $_ 'passenger_reserve' 0) } else { '-' }
        "$(if ($bound) { 'bound' } else { 'none' }):$(if ($hasMount) { 1 } else { 0 }):${clip}:$reserve"
    })
    $subjectPassenger = Get-ExactSetKey @($subjectFrames | Where-Object {
        [bool](Get-Field $_ 'passenger_present' $false)
    } | ForEach-Object {
        $bound = [uint32](Get-Field $_ 'passenger_mount_handle' 0xffff) -ne 0xffff
        $hasMount = [bool](Get-Field $_ 'passenger_has_mount' $false)
        $clip = if ($bound) { [uint32](Get-Field $_ 'passenger_clip' 0) } else { '-' }
        $reserve = if ($bound) { [uint32](Get-Field $_ 'passenger_reserve' 0) } else { '-' }
        "$(if ($bound) { 'bound' } else { 'none' }):$(if ($hasMount) { 1 } else { 0 }):${clip}:$reserve"
    })
    if ($goldPassengerPresence -ne $subjectPassengerPresence -or
            $goldPassenger -ne $subjectPassenger) {
        Add-ParityFailure "FRAME_PASSENGER_PHASE_MISMATCH" $caseId $Topology `
            "S2C 0x0A phase-8 mount presence/clip/reserve differs from same-case RR after normalizing session-local handles."
    }
    $goldLengths = @($goldFrames | ForEach-Object { $_.length } | Sort-Object -Unique)
    $subjectLengths = @($subjectFrames | ForEach-Object { $_.length } | Sort-Object -Unique)
    Test-MinimumDiversity $caseId $Topology 'S2C 0x0A body length' `
        $goldLengths.Count $subjectLengths.Count $diversityRatio
    $goldShapes = @($goldFrames | ForEach-Object {
        "$($_.length)|$($_.sub_block)|$($_.players)|$($_.vehicles)|$($_.infantry)|$($_.none)|$($_.rounds)"
    } | Sort-Object -Unique)
    $subjectShapes = @($subjectFrames | ForEach-Object {
        "$($_.length)|$($_.sub_block)|$($_.players)|$($_.vehicles)|$($_.infantry)|$($_.none)|$($_.rounds)"
    } | Sort-Object -Unique)
    Test-MinimumDiversity $caseId $Topology 'S2C 0x0A decoded shape fingerprint' `
        $goldShapes.Count $subjectShapes.Count $diversityRatio

    $goldAnchors = @($goldFrames | ForEach-Object {
        "$($_.anchor_x),$($_.anchor_y),$($_.anchor_z)"
    } | Sort-Object -Unique)
    $subjectAnchors = @($subjectFrames | ForEach-Object {
        "$($_.anchor_x),$($_.anchor_y),$($_.anchor_z)"
    } | Sort-Object -Unique)
    Test-MinimumDiversity $caseId $Topology 'S2C 0x0A frame anchor' `
        $goldAnchors.Count $subjectAnchors.Count $diversityRatio
    $goldAnchorNonzero = @($goldFrames | Where-Object {
        $_.anchor_x -ne 0 -or $_.anchor_y -ne 0 -or $_.anchor_z -ne 0
    }).Count
    $subjectAnchorNonzero = @($subjectFrames | Where-Object {
        $_.anchor_x -ne 0 -or $_.anchor_y -ne 0 -or $_.anchor_z -ne 0
    }).Count
    if ($goldAnchorNonzero -gt 0 -and $subjectAnchorNonzero -eq 0) {
        Add-ParityFailure "FRAME_ANCHOR_ZERO" $caseId $Topology `
            "S2C 0x0A frame anchors are fixed at zero although same-case RR carries nonzero values."
    }
    Compare-DynamicVectorAgreement $caseId $Topology 'S2C 0x0A frame anchor' `
        $goldFrames $subjectFrames @('anchor_x','anchor_y','anchor_z') $Policy $true
    $goldLocalState = Get-ExactSetKey @($goldFrames | ForEach-Object { $_.state_a })
    $subjectLocalState = Get-ExactSetKey @($subjectFrames | ForEach-Object { $_.state_a })
    if ($goldLocalState -ne $subjectLocalState) {
        Add-ParityFailure "FRAME_LOCAL_STATE_MISMATCH" $caseId $Topology `
            "S2C 0x0A local-tail state values differ from same-case RR."
    }
    foreach ($field in @(
        [pscustomobject]@{ name='health'; label='local-tail health' },
        [pscustomobject]@{ name='state_word'; label='local-tail state word' }
    )) {
        $fieldName = $field.name
        $goldValues = @($goldFrames | ForEach-Object { $_.$fieldName } | Sort-Object -Unique)
        $subjectValues = @($subjectFrames | ForEach-Object { $_.$fieldName } | Sort-Object -Unique)
        Test-MinimumDiversity $caseId $Topology "S2C 0x0A $($field.label)" `
            $goldValues.Count $subjectValues.Count $diversityRatio
        $goldFieldNonzero = @($goldFrames | Where-Object { $_.$fieldName -ne 0 }).Count
        $subjectFieldNonzero = @($subjectFrames | Where-Object { $_.$fieldName -ne 0 }).Count
        if ($goldFieldNonzero -gt 0 -and $subjectFieldNonzero -eq 0) {
            Add-ParityFailure "FRAME_LOCAL_FIELD_ZERO" $caseId $Topology `
                "S2C 0x0A $($field.label) is fixed at zero although same-case RR carries nonzero values."
        }
    }

    $goldTypes = Get-ExactSetKey @($goldSteadyEntities | ForEach-Object { "$($_.class):$($_.type_id)" })
    $subjectTypes = Get-ExactSetKey @($subjectSteadyEntities | ForEach-Object { "$($_.class):$($_.type_id)" })
    if ($goldTypes -ne $subjectTypes) {
        Add-ParityFailure "FRAME_ENTITY_TYPES_MISMATCH" $caseId $Topology `
            "S2C 0x0A entity class/type coverage differs from same-case RR."
    }
    # Dynamic semantics and rate/count comparisons use the exact declared
    # steady interval. Initial admission semantics are checked separately, so
    # pre-window motion can never satisfy a parity witness.
    $goldTypeGroups = @($goldSteadyEntities | Group-Object { "$($_.class):$($_.type_id)" })
    $subjectTypeCounts = @{}
    foreach ($group in @($subjectSteadyEntities | Group-Object { "$($_.class):$($_.type_id)" })) {
        $subjectTypeCounts[$group.Name] = $group.Count
    }
    $absoluteTolerance = [double](Get-Field $Policy 'count_absolute_tolerance' 1)
    $relativeTolerance = [double](Get-Field $Policy 'count_relative_tolerance' 0.20)
    foreach ($group in $goldTypeGroups) {
        if (-not $subjectTypeCounts.ContainsKey($group.Name)) { continue }
        $expected = [double]$group.Count
        if ($Golden.duration_ms -gt 0 -and $Subject.duration_ms -gt 0 -and $group.Count -ge 20) {
            $expected = $group.Count * $Subject.duration_ms / $Golden.duration_ms
        }
        $tolerance = [Math]::Max($absoluteTolerance, [Math]::Ceiling($expected * $relativeTolerance))
        if ([Math]::Abs([double]$subjectTypeCounts[$group.Name] - $expected) -gt $tolerance) {
            Add-ParityFailure "FRAME_ENTITY_COUNT_MISMATCH" $caseId $Topology `
                "S2C 0x0A entity $($group.Name) count $($subjectTypeCounts[$group.Name]) differs from RR-normalized $([Math]::Round($expected,2))."
        }
    }
    foreach ($class in @('P','V','I')) {
        $goldEntities = @($goldSteadyEntities | Where-Object { $_.class -eq $class })
        $subjectEntities = @($subjectSteadyEntities | Where-Object { $_.class -eq $class })
        $goldNonzero = @($goldEntities | Where-Object { $_.pos_x -ne 0 -or $_.pos_y -ne 0 -or $_.pos_z -ne 0 }).Count
        $subjectNonzero = @($subjectEntities | Where-Object { $_.pos_x -ne 0 -or $_.pos_y -ne 0 -or $_.pos_z -ne 0 }).Count
        if ($goldNonzero -gt 0 -and $subjectNonzero -eq 0) {
            Add-ParityFailure "FRAME_ENTITY_ALL_ZERO" $caseId $Topology `
                "S2C 0x0A class $class positions are all zero although same-case RR carries nonzero values."
        }
        $goldMoving = Get-MovingHandleCount $goldEntities
        $subjectMoving = Get-MovingHandleCount $subjectEntities
        if ($goldMoving -gt 0 -and $subjectMoving -eq 0) {
            Add-ParityFailure "FRAME_ENTITY_CONSTANT" $caseId $Topology `
                "S2C 0x0A class $class never changes position for any handle although same-case RR does."
        }
        $goldOrientation = @($goldEntities | ForEach-Object {
            "$($_.orient_a),$($_.orient_b)"
        } | Sort-Object -Unique)
        $subjectOrientation = @($subjectEntities | ForEach-Object {
            "$($_.orient_a),$($_.orient_b)"
        } | Sort-Object -Unique)
        Test-MinimumDiversity $caseId $Topology "S2C 0x0A class $class orientation" `
            $goldOrientation.Count $subjectOrientation.Count $diversityRatio
        $goldOrientationNonzero = @($goldEntities | Where-Object {
            $_.orient_a -ne 0 -or $_.orient_b -ne 0
        }).Count
        $subjectOrientationNonzero = @($subjectEntities | Where-Object {
            $_.orient_a -ne 0 -or $_.orient_b -ne 0
        }).Count
        if ($goldOrientationNonzero -gt 0 -and $subjectOrientationNonzero -eq 0) {
            Add-ParityFailure "FRAME_ENTITY_ORIENTATION_ZERO" $caseId $Topology `
                "S2C 0x0A class $class orientation is fixed at zero although same-case RR carries nonzero values."
        }
        $goldTurning = Get-ChangingHandleCount $goldEntities {
            param($entity)
            "$($entity.orient_a),$($entity.orient_b)"
        }
        $subjectTurning = Get-ChangingHandleCount $subjectEntities {
            param($entity)
            "$($entity.orient_a),$($entity.orient_b)"
        }
        if ($goldTurning -gt 0 -and $subjectTurning -eq 0) {
            Add-ParityFailure "FRAME_ENTITY_ORIENTATION_CONSTANT" $caseId $Topology `
                "S2C 0x0A class $class orientation never changes for any handle although same-case RR does."
        }
        $goldEntityState = @($goldEntities | ForEach-Object {
            "$($_.state_a),$($_.state_b)"
        } | Sort-Object -Unique)
        $subjectEntityState = @($subjectEntities | ForEach-Object {
            "$($_.state_a),$($_.state_b)"
        } | Sort-Object -Unique)
        Test-MinimumDiversity $caseId $Topology "S2C 0x0A class $class state" `
            $goldEntityState.Count $subjectEntityState.Count $diversityRatio
        $goldStateNonzero = @($goldEntities | Where-Object {
            $_.state_a -ne 0 -or $_.state_b -ne 0
        }).Count
        $subjectStateNonzero = @($subjectEntities | Where-Object {
            $_.state_a -ne 0 -or $_.state_b -ne 0
        }).Count
        if ($goldStateNonzero -gt 0 -and $subjectStateNonzero -eq 0) {
            Add-ParityFailure "FRAME_ENTITY_STATE_ZERO" $caseId $Topology `
                "S2C 0x0A class $class state is fixed at zero although same-case RR carries nonzero values."
        }
        $goldVital = @($goldEntities | Where-Object { $_.vital -gt 0 }).Count
        $subjectVital = @($subjectEntities | Where-Object { $_.vital -gt 0 }).Count
        if ($goldVital -gt 0 -and $subjectVital -eq 0) {
            Add-ParityFailure "FRAME_ENTITY_VITAL_ZERO" $caseId $Topology `
                "S2C 0x0A class $class vital fields are all zero although same-case RR carries nonzero values."
        }
        $entityGroups = @('connection_key','sid','handle')
        Compare-DynamicVectorAgreement $caseId $Topology `
            "S2C 0x0A class $class entity position" `
            $goldEntities $subjectEntities @('pos_x','pos_y','pos_z') `
            $Policy $false $entityGroups
        $entityOrientationModulus = if ($class -eq 'V') { 65536 } else { 256 }
        Compare-DynamicVectorAgreement $caseId $Topology `
            "S2C 0x0A class $class entity orientation" `
            $goldEntities $subjectEntities @('orient_a','orient_b') `
            $Policy $false $entityGroups $entityOrientationModulus
    }
}

function Get-HandshakeFieldKey {
    param($Handshake, $Field, [bool] $NormalizeDynamic = $true)
    $opcode = [byte](Get-Field $Handshake 'opcode' 0)
    $name = "$(Get-Field $Field 'name' '')"
    $kind = "$(Get-Field $Field 'kind' '')"
    $length = [int](Get-Field $Field 'length' -1)
    $dynamicField = switch ($opcode) {
        0x41 { $name -in @('CI','EIP','EPN') }
        0x42 { $name -in @('CI','HK','CK','SIP','SPN','SCRK') }
        0x81 { $name -in @('CI','UT','HK','RIP','RPN','EIP','EPN','SUS1') }
        0x82 { $name -in @('CI','MI','CK','SK','SCRK','RIP','RPN') }
        default { $false }
    }
    if ($kind -ceq 'cu') {
        $cuName = "$(Get-Field $Field 'cu_name' '')"
        $cuType = [int](Get-Field $Field 'cu_type' -1)
        $cuLength = [int](Get-Field $Field 'cu_value_length' -1)
        $dynamicCu = ($opcode -eq 0x42 -and
                $cuName -cin @('UdpCode1','UdpCode2')) -or
            ($opcode -eq 0x82 -and $cuName -ceq 'NWUID')
        $value = if ($NormalizeDynamic -and $dynamicCu) {
            '<dynamic>'
        } else {
            ConvertTo-CompactHex ([byte[]](Get-Field $Field 'cu_value' ([byte[]]@())))
        }
        return "$name|$kind|$length|type=$cuType|name=$cuName|value_length=$cuLength|value=$value"
    }
    if ($kind -ceq 'cs') {
        return "$name|$kind|$length|direction=$(Get-Field $Field 'cs_direction' -1)|index=$(Get-Field $Field 'cs_index' -1)|value=$(Get-Field $Field 'cs_value' -1)"
    }
    $valueKey = if ($NormalizeDynamic -and $dynamicField) {
        '<dynamic>'
    } else {
        ConvertTo-CompactHex ([byte[]](Get-Field $Field 'value' ([byte[]]@())))
    }
    return "$name|$kind|$length|value=$valueKey"
}

function Get-NormalizedHandshakeKey {
    param($Handshake)
    $fieldKeys = @((Get-Field $Handshake 'fields' @()) | ForEach-Object {
        Get-HandshakeFieldKey $Handshake $_
    })
    return ('0x{0:x2}|{1}' -f [byte](Get-Field $Handshake 'opcode' 0),
        ($fieldKeys -join '>'))
}

function Get-ExactHandshakeKey {
    param($Handshake)
    $fieldKeys = @((Get-Field $Handshake 'fields' @()) | ForEach-Object {
        Get-HandshakeFieldKey $Handshake $_ $false
    })
    $bodyKey = ConvertTo-CompactHex ([byte[]](Get-Field $Handshake 'body' ([byte[]]@())))
    return ('0x{0:x2}|{1}|body={2}' -f [byte](Get-Field $Handshake 'opcode' 0),
        ($fieldKeys -join '>'),$bodyKey)
}

function Resolve-OuterHandshakeSession {
    param($Facts)
    $allHandshakes = @(Get-FactCollection $Facts 'handshakes' 'handshakes')
    $handshakeSessions = @($allHandshakes | ForEach-Object {
        [int](Get-Field $_ 'session' 0)
    } | Sort-Object -Unique)
    $protocolSessions = @(
        @((Get-FactCollection $Facts 'packets' 'packets') | ForEach-Object {
            [int](Get-Field $_ 'session' 0)
        }) +
        @((Get-FactCollection $Facts 'events' 'events') | Where-Object {
            "$(Get-Field $_ 'identity' '')" -in @('0c:0','0a:0')
        } | ForEach-Object { [int](Get-Field $_ 'session' 0) }) |
        Sort-Object -Unique
    )
    if ($protocolSessions.Count -eq 1) {
        return [pscustomobject]@{
            valid=$true;session=[int]$protocolSessions[0]
            code='';message='';handshakes=$allHandshakes
        }
    }
    if ($protocolSessions.Count -gt 1) {
        return [pscustomobject]@{
            valid=$false;session=$null;code='HANDSHAKE_SESSION_BINDING'
            message='Decoded protocol/gameplay traffic does not identify exactly one client-port session.'
            handshakes=$allHandshakes
        }
    }
    if ($handshakeSessions.Count -eq 1) {
        # Minimal structural/self-test facts can omit protocol traffic; they
        # still bind unambiguously when every handshake record has one session.
        return [pscustomobject]@{
            valid=$true;session=[int]$handshakeSessions[0]
            code='';message='';handshakes=$allHandshakes
        }
    }
    return [pscustomobject]@{
        valid=$false;session=$null;code='HANDSHAKE_SESSION_AMBIGUOUS'
        message='No unique gameplay or handshake session is available for the four-message outer handshake.'
        handshakes=$allHandshakes
    }
}

function Get-FirstHandshake {
    param($Facts, [byte] $Opcode, [int] $Session)
    $matches = @((Get-FactCollection $Facts 'handshakes' 'handshakes') |
        Where-Object {
            $_.opcode -eq $Opcode -and $_.decode -and
                [int](Get-Field $_ 'session' 0) -eq $Session
        } |
        Sort-Object frame | Select-Object -First 1)
    if ($matches.Count -eq 0) { return $null }
    return $matches[0]
}

function Get-UniqueHandshakeField {
    param($Handshake, [string] $Name)
    if ($null -eq $Handshake) { return $null }
    $matches = @((Get-Field $Handshake 'fields' @()) | Where-Object {
        $_.name -ceq $Name
    })
    if ($matches.Count -ne 1) { return $null }
    return $matches[0]
}

function Test-OuterHandshakeContract {
    param($Facts, $Case, [string] $Topology)
    $caseId = "$(Get-Field $Case 'id' '')"
    $binding = Resolve-OuterHandshakeSession $Facts
    $allHandshakes = @($binding.handshakes)
    $boundSession = if ($binding.valid) { [int]$binding.session } else { $null }
    if (-not $binding.valid) {
        Add-ParityFailure $binding.code $caseId $Topology $binding.message
    }
    $handshakes = if ($null -eq $boundSession) {
        @($allHandshakes)
    } else {
        @($allHandshakes | Where-Object {
            [int](Get-Field $_ 'session' 0) -eq [int]$boundSession
        })
    }
    $sessionBindingReported = $false
    if ($null -ne $boundSession) {
        $splicedOpcodes = @()
        foreach ($opcode in @(0x41,0x81,0x42,0x82)) {
            if (@($handshakes | Where-Object { $_.opcode -eq $opcode }).Count -eq 0 -and
                    @($allHandshakes | Where-Object { $_.opcode -eq $opcode }).Count -gt 0) {
                $splicedOpcodes += ('0x{0:x2}' -f $opcode)
            }
        }
        if ($splicedOpcodes.Count -gt 0) {
            Add-ParityFailure 'HANDSHAKE_SESSION_BINDING' $caseId $Topology `
                "Outer handshake opcode(s) $($splicedOpcodes -join ',') exist only on a different session than gameplay session $boundSession."
            $sessionBindingReported = $true
        }

        # External captures legitimately contain the runner's readiness probe:
        # an incomplete 0x41/0x81-only exchange on a different client port.
        # Exclude only that explicit shape. A foreign 0x42/0x82 (or any other
        # handshake shape) is another auth/session attempt and fails closed.
        $foreignGroups = @($allHandshakes | Where-Object {
            [int](Get-Field $_ 'session' 0) -ne [int]$boundSession
        } | Group-Object session)
        foreach ($foreignGroup in $foreignGroups) {
            $foreignOpcodes = @($foreignGroup.Group | ForEach-Object {
                [byte](Get-Field $_ 'opcode' 0)
            } | Sort-Object -Unique)
            $isIncompleteReadinessProbe = $foreignOpcodes.Count -gt 0 -and
                @($foreignOpcodes | Where-Object { $_ -notin @(0x41,0x81) }).Count -eq 0
            if (-not $isIncompleteReadinessProbe) {
                if (-not $sessionBindingReported) {
                    Add-ParityFailure 'HANDSHAKE_SESSION_BINDING' $caseId $Topology `
                        "Foreign session $($foreignGroup.Name) is not an incomplete 0x41/0x81 readiness probe."
                    $sessionBindingReported = $true
                }
                continue
            }
            foreach ($probeOpcode in $foreignOpcodes) {
                $probeRetries = @($foreignGroup.Group | Where-Object {
                    $_.opcode -eq $probeOpcode
                } | Sort-Object frame)
                if (@($probeRetries | Where-Object { -not $_.decode }).Count -gt 0) {
                    Add-ParityFailure 'HANDSHAKE_DECODE_FAILED' $caseId $Topology `
                        ('Readiness-probe 0x{0:x2} retry on session {1} did not decode.' -f
                            $probeOpcode,$foreignGroup.Name)
                }
                $probeKeys = @($probeRetries | Where-Object { $_.decode } |
                    ForEach-Object { Get-ExactHandshakeKey $_ } |
                    Sort-Object -Unique)
                if ($probeKeys.Count -ne 1) {
                    Add-ParityFailure 'HANDSHAKE_RETRY_MISMATCH' $caseId $Topology `
                        ('Readiness-probe 0x{0:x2} retries on session {1} are not byte-field stable.' -f
                            $probeOpcode,$foreignGroup.Name)
                }
            }
        }
    }
    $first = @{}
    foreach ($opcode in @(0x41,0x81,0x42,0x82)) {
        $matches = @($handshakes | Where-Object { $_.opcode -eq $opcode } |
            Sort-Object frame)
        $decoded = @($matches | Where-Object { $_.decode })
        if (@($matches | Where-Object { -not $_.decode }).Count -gt 0) {
            Add-ParityFailure 'HANDSHAKE_DECODE_FAILED' $caseId $Topology `
                ('At least one outer 0x{0:x2} retry did not decode.' -f $opcode)
        }
        if ($matches.Count -eq 0 -or $decoded.Count -eq 0) {
            Add-ParityFailure 'HANDSHAKE_MISSING' $caseId $Topology `
                ('Outer 0x{0:x2} handshake is missing or did not decode.' -f $opcode)
            continue
        }
        $first[$opcode] = $decoded[0]
        $retryKeys = @($decoded | ForEach-Object {
            Get-ExactHandshakeKey $_
        } | Sort-Object -Unique)
        if ($retryKeys.Count -ne 1) {
            Add-ParityFailure 'HANDSHAKE_RETRY_MISMATCH' $caseId $Topology `
                ('Outer 0x{0:x2} retransmissions are not byte-field stable.' -f $opcode)
        }
    }
    if ($first.Count -ne 4) { return $false }
    if (-not ($first[0x41].frame -lt $first[0x81].frame -and
            $first[0x81].frame -lt $first[0x42].frame -and
            $first[0x42].frame -lt $first[0x82].frame)) {
        Add-ParityFailure 'HANDSHAKE_ORDER' $caseId $Topology `
            'Outer handshake order is not 0x41,0x81,0x42,0x82.'
    }
    $required = @{
        0x41=@('NVS','CO','AP','BDAT','PN','PG','PV1','PV2','CI')
        0x42=@('NVS','CO','AP','BDAT','PN','PG','PV1','PV2','CI','HK','CK','NA','SCRK')
        0x81=@('CI','CO','AP','BDAT','PN','PG','PV1','PV2','PV3','HK','SN','SF','NC','RIP','RPN','EIP','EPN')
        0x82=@('CI','MI','CK','CR','SK','SCRK','NA','RIP','RPN')
    }
    foreach ($opcode in $required.Keys) {
        foreach ($name in $required[$opcode]) {
            if ($null -eq (Get-UniqueHandshakeField $first[$opcode] $name)) {
                Add-ParityFailure 'HANDSHAKE_FIELD_SET' $caseId $Topology `
                    ('Outer 0x{0:x2} requires exactly one {1} field.' -f $opcode,$name)
            }
        }
    }
    $serverCs = @($first[0x82].fields | Where-Object { $_.name -ceq 'CS' })
    if ($serverCs.Count -eq 0) {
        Add-ParityFailure 'HANDSHAKE_FIELD_SET' $caseId $Topology `
            'Outer 0x82 carries no CS control settings.'
    }
    $echoPairs = @(
        @(0x41,'NVS',0x42,'NVS'), @(0x41,'CO',0x42,'CO'),
        @(0x41,'AP',0x42,'AP'), @(0x41,'BDAT',0x42,'BDAT'),
        @(0x41,'PN',0x42,'PN'), @(0x41,'PG',0x42,'PG'),
        @(0x41,'PV1',0x42,'PV1'), @(0x41,'PV2',0x42,'PV2'),
        @(0x41,'CI',0x81,'CI'), @(0x41,'CI',0x42,'CI'),
        @(0x42,'CI',0x82,'CI'), @(0x41,'PN',0x81,'PN'),
        @(0x41,'PG',0x81,'PG'), @(0x41,'PV1',0x81,'PV1'),
        @(0x41,'PV2',0x81,'PV2'), @(0x81,'HK',0x42,'HK'),
        @(0x42,'CK',0x82,'CK'), @(0x42,'NA',0x82,'NA'),
        @(0x81,'RIP',0x82,'RIP'), @(0x81,'RPN',0x82,'RPN')
    )
    foreach ($pair in $echoPairs) {
        $left = Get-UniqueHandshakeField $first[$pair[0]] $pair[1]
        $right = Get-UniqueHandshakeField $first[$pair[2]] $pair[3]
        if ($null -eq $left -or $null -eq $right) { continue }
        if ((ConvertTo-CompactHex ([byte[]]$left.value)) -cne
                (ConvertTo-CompactHex ([byte[]]$right.value))) {
            Add-ParityFailure 'HANDSHAKE_ECHO_MISMATCH' $caseId $Topology `
                ('Outer 0x{0:x2}.{1} does not echo 0x{2:x2}.{3}.' -f
                    $pair[2],$pair[3],$pair[0],$pair[1])
        }
    }
    foreach ($optionalEcho in @(
        @(0x41,'EIP',0x81,'EIP'), @(0x41,'EPN',0x81,'EPN')
    )) {
        $left = Get-UniqueHandshakeField $first[$optionalEcho[0]] $optionalEcho[1]
        $right = Get-UniqueHandshakeField $first[$optionalEcho[2]] $optionalEcho[3]
        if (($null -eq $left) -ne ($null -eq $right) -or
                ($null -ne $left -and
                    (ConvertTo-CompactHex ([byte[]]$left.value)) -cne
                    (ConvertTo-CompactHex ([byte[]]$right.value)))) {
            Add-ParityFailure 'HANDSHAKE_ECHO_MISMATCH' $caseId $Topology `
                "Outer ServerHello does not preserve ClientHello $($optionalEcho[1]) presence/value."
        }
    }
    $clientKey = Get-UniqueHandshakeField $first[0x42] 'CK'
    $serverKey = Get-UniqueHandshakeField $first[0x82] 'SK'
    if ($null -ne $clientKey -and $null -ne $serverKey) {
        $expectedS = [uint32](Read-U32Le ([byte[]]$clientKey.value) 0)
        $expectedC = [uint32](Read-U32Le ([byte[]]$serverKey.value) 0)
        $session = [int]$first[0x42].session
        $cSids = @($Facts.packets | Where-Object {
            $_.dir -eq 'C' -and $_.session -eq $session
        } | ForEach-Object { [uint32]$_.sid } | Sort-Object -Unique)
        $sSids = @($Facts.packets | Where-Object {
            $_.dir -eq 'S' -and $_.session -eq $session
        } | ForEach-Object { [uint32]$_.sid } | Sort-Object -Unique)
        if ($cSids.Count -ne 1 -or $cSids[0] -ne $expectedC -or
                $sSids.Count -ne 1 -or $sSids[0] -ne $expectedS) {
            Add-ParityFailure 'HANDSHAKE_SESSION_KEY_ECHO' $caseId $Topology `
                'Outer CK/SK values do not become the opposite directional protocol packet SIDs.'
        }
    }
    return $true
}

function Get-UniqueFramePhaseStates {
    param($Facts, [bool] $SteadyOnly = $false)
    $states = if ($SteadyOnly) {
        @(Get-FactCollection $Facts 'steady_states' 'states')
    } else {
        @((Get-Field $Facts 'states' @()))
    }
    $states = @($states | Where-Object {
        "$(Get-Field $_ 'dir' '')" -eq 'S' -and
            "$(Get-Field $_ 'kind' '')" -eq 'frame' -and
            [bool](Get-Field $_ 'decode' $false)
    })
    $phasePackets = @((Get-Field $Facts 'packets' @()) | Where-Object {
        "$(Get-Field $_ 'dir' '')" -eq 'S' -and
            @((Get-Field $_ 'tag_identities' @()) | ForEach-Object {
                "$($_)".ToLowerInvariant()
            }) -contains '0a:0'
    } | Sort-Object frame)
    if ($phasePackets.Count -eq 0) { return $states }

    # Ignore physical retransmissions without hiding two genuine 0x0A records
    # batched into the same original datagram. State order inside the retained
    # frame remains the shared decoder's semantic order.
    $firstByOrigin = @{}
    foreach ($packet in $phasePackets) {
        $connection = "$(Get-Field $packet 'connection_key' '')"
        if ([string]::IsNullOrWhiteSpace($connection)) {
            $connection = "$(Get-Field $packet 'session' 0)|sid=$(Get-Field $packet 'sid' 0)"
        }
        $origin = "$connection|seq=$(Get-Field $packet 'sequence' 0)"
        if (-not $firstByOrigin.ContainsKey($origin)) {
            $firstByOrigin[$origin] = $packet
        }
    }
    $firstFrames = @{}
    foreach ($packet in $firstByOrigin.Values) {
        $firstFrames["$(Get-Field $packet 'frame' -1)"] = $true
    }
    return @($states | Where-Object {
        $firstFrames.ContainsKey("$(Get-Field $_ 'frame' -1)")
    })
}

function Test-FramePhaseStream {
    param($Facts, $Case, [string] $Topology)
    $caseId = "$(Get-Field $Case 'id' '')"
    [uint32]$gameType = [uint32](Get-Field $Case 'game_type' 0)
    $allFrames = @(Get-UniqueFramePhaseStates $Facts $false)
    $steadyFrames = @(Get-UniqueFramePhaseStates $Facts $true)
    if ($allFrames.Count -eq 0) {
        Add-ParityFailure 'FRAME_PHASE_COVERAGE' $caseId $Topology `
            'No uniquely-originated decoded S2C 0x0A frames are available for flags2 validation.'
        return
    }
    $groups = @($allFrames | Group-Object {
        $connection = "$(Get-Field $_ 'connection_key' '')"
        if ([string]::IsNullOrWhiteSpace($connection)) {
            $connection = "$(Get-Field $_ 'session' 0)|sid=$(Get-Field $_ 'sid' 0)"
        }
        $connection
    })
    foreach ($group in $groups) {
        $frames = @($group.Group)
        if ([uint32](Get-Field $frames[0] 'flags2' 0xffffffff) -ne 1) {
            Add-ParityFailure 'FRAME_PHASE_START' $caseId $Topology `
                "S2C 0x0A flags2 for connection $($group.Name) does not start at retail's pre-incremented value 1."
        }
        $sequenceMismatch = $null
        for ($i = 1; $i -lt $frames.Count; $i++) {
            [uint32]$previous = [uint32](Get-Field $frames[$i - 1] 'flags2' 0xffffffff)
            [uint32]$actual = [uint32](Get-Field $frames[$i] 'flags2' 0xffffffff)
            [uint32]$expected = ($previous + 1) -band 0xff
            if ($actual -ne $expected) {
                $sequenceMismatch = "frame $(Get-Field $frames[$i] 'frame' -1): expected $expected after $previous, observed $actual"
                break
            }
        }
        if ($null -ne $sequenceMismatch) {
            Add-ParityFailure 'FRAME_PHASE_SEQUENCE' $caseId $Topology `
                "S2C 0x0A flags2 is not an exact modulo-256 sequence for connection $($group.Name) ($sequenceMismatch)."
        }

        $associationMismatch = $null
        foreach ($frame in $frames) {
            [uint32]$flags2 = [uint32](Get-Field $frame 'flags2' 0xffffffff)
            [uint32]$subBlock = $flags2 -band 3
            $expectObjective = $subBlock -eq 3 -and ($gameType -band 0x20000) -ne 0
            $expectPassenger = ($flags2 -band 0x0f) -eq 8
            if ([uint32](Get-Field $frame 'sub_block' 0xffffffff) -ne $subBlock -or
                    [bool](Get-Field $frame 'weapon_present' $false) -ne ($subBlock -eq 0) -or
                    [bool](Get-Field $frame 'timer_present' $false) -ne ($subBlock -eq 1) -or
                    [bool](Get-Field $frame 'env_present' $false) -ne ($subBlock -eq 2) -or
                    [bool](Get-Field $frame 'objective_present' $false) -ne $expectObjective -or
                    [bool](Get-Field $frame 'passenger_present' $false) -ne $expectPassenger) {
                $associationMismatch = "frame $(Get-Field $frame 'frame' -1), flags2=$flags2"
                break
            }
        }
        if ($null -ne $associationMismatch) {
            Add-ParityFailure 'FRAME_PHASE_ASSOCIATION' $caseId $Topology `
                "S2C 0x0A decoded sub-block/phase-8 presence disagrees with flags2 and game_type for connection $($group.Name) ($associationMismatch)."
        }

        $steady = @($steadyFrames | Where-Object {
            $connection = "$(Get-Field $_ 'connection_key' '')"
            if ([string]::IsNullOrWhiteSpace($connection)) {
                $connection = "$(Get-Field $_ 'session' 0)|sid=$(Get-Field $_ 'sid' 0)"
            }
            $connection -ceq $group.Name
        })
        [uint32[]]$steadyValues = @($steady | ForEach-Object {
            [uint32](Get-Field $_ 'flags2' 0xffffffff)
        })
        $uniqueValues = @($steadyValues | Sort-Object -Unique)
        $hasWrap = $false
        for ($i = 1; $i -lt $steadyValues.Count; $i++) {
            if ($steadyValues[$i - 1] -eq 255 -and $steadyValues[$i] -eq 0) {
                $hasWrap = $true
                break
            }
        }
        # Canonical LAN mode 1 sends at about 5.2 Hz. The 35/45-second cases
        # cannot physically cover all 256 values, while the 80-second case can.
        # Require a broad 128-value witness in every run and require the
        # explicit 255->0 edge whenever this steady slice contains >=257
        # ordered samples. Exact +1 sequencing above rejects any short cycle.
        if ($uniqueValues.Count -lt 128 -or
                ($steadyValues.Count -ge 257 -and -not $hasWrap)) {
            Add-ParityFailure 'FRAME_PHASE_COVERAGE' $caseId $Topology `
                "Steady S2C 0x0A flags2 for connection $($group.Name) covers $($uniqueValues.Count) unique values across $($steadyValues.Count) samples; require at least 128 values and a 255-to-0 wrap when 257 or more samples are present."
        }
    }
}

function Test-AckStream {
    param($Facts, $Case, [string] $Topology)
    $caseId = "$(Get-Field $Case 'id' '')"
    $packets = @((Get-Field $Facts 'packets' @()) | Sort-Object frame)
    $groups = @($packets | Group-Object {
        $connection = "$(Get-Field $_ 'connection_key' '')"
        if ([string]::IsNullOrWhiteSpace($connection)) {
            $connection = "$(Get-Field $_ 'session' 0)"
        }
        $connection
    })
    foreach ($group in $groups) {
        $seen = @{ S=@{}; C=@{} }
        $lastAck = @{ S=$null; C=$null }
        $ackValues = @{
            S=[System.Collections.Generic.List[uint32]]::new()
            C=[System.Collections.Generic.List[uint32]]::new()
        }
        $sequenceValues = @{
            S=[System.Collections.Generic.List[uint32]]::new()
            C=[System.Collections.Generic.List[uint32]]::new()
        }
        $unknownReported = @{ S=$false; C=$false }
        $regressionReported = @{ S=$false; C=$false }
        foreach ($packet in @($group.Group | Sort-Object frame)) {
            $direction = "$(Get-Field $packet 'dir' '')"
            if ($direction -notin @('S','C')) { continue }
            $opposite = if ($direction -eq 'S') { 'C' } else { 'S' }
            [uint32]$sequence = [uint32](Get-Field $packet 'sequence' 0)
            [uint32]$ack = [uint32](Get-Field $packet 'ack' 0)
            if ($ack -ne 0 -and -not $seen[$opposite].ContainsKey("$ack") -and
                    -not $unknownReported[$direction]) {
                Add-ParityFailure 'ACK_UNKNOWN_SEQUENCE' $caseId $Topology `
                    "$direction packet frame $(Get-Field $packet 'frame' -1) ACKs opposite sequence $ack before that sequence appears on connection $($group.Name)."
                $unknownReported[$direction] = $true
            }
            if ($null -ne $lastAck[$direction] -and
                    $ack -lt [uint32]$lastAck[$direction] -and
                    -not $regressionReported[$direction]) {
                Add-ParityFailure 'ACK_REGRESSION' $caseId $Topology `
                    "$direction ACK regresses from $($lastAck[$direction]) to $ack on connection $($group.Name)."
                $regressionReported[$direction] = $true
            }
            $lastAck[$direction] = $ack
            $ackValues[$direction].Add($ack)
            $sequenceValues[$direction].Add($sequence)
            $seen[$direction]["$sequence"] = $true
        }

        foreach ($direction in @('S','C')) {
            $opposite = if ($direction -eq 'S') { 'C' } else { 'S' }
            [uint32[]]$ownSequences = @($sequenceValues[$direction].ToArray() |
                Sort-Object -Unique)
            [uint32[]]$oppositeSequences = @($sequenceValues[$opposite].ToArray() |
                Sort-Object -Unique)
            if ($ownSequences.Count -lt 8 -or $oppositeSequences.Count -lt 8) {
                continue
            }
            [uint32[]]$uniqueAcks = @($ackValues[$direction].ToArray() |
                Sort-Object -Unique)
            [uint32]$maximumAck = [uint32](
                $ackValues[$direction].ToArray() | Measure-Object -Maximum).Maximum
            [uint32]$minimumOpposite = $oppositeSequences[0]
            [uint32]$maximumOpposite = $oppositeSequences[$oppositeSequences.Count - 1]
            [double]$requiredProgress = [double]$minimumOpposite +
                (0.8 * [double]($maximumOpposite - $minimumOpposite))
            if ($uniqueAcks.Count -lt 2 -or $maximumAck -lt $requiredProgress) {
                Add-ParityFailure 'ACK_NO_PROGRESS' $caseId $Topology `
                    "$direction ACKs do not progress through the opposite sequence stream on connection $($group.Name) (max ACK $maximumAck, opposite range $minimumOpposite..$maximumOpposite)."
            }
        }
    }
}

function Get-OccupiedMountCoverageReport {
    param([hashtable] $DecodedCases, [string[]] $Topologies)
    $topologyReports = [System.Collections.Generic.List[object]]::new()
    $totalPhase8 = 0
    $totalBound = 0
    foreach ($topology in $Topologies) {
        $phase8Samples = 0
        $boundSamples = 0
        foreach ($caseId in $DecodedCases.Keys) {
            $entry = $DecodedCases[$caseId]
            $factsByTopology = Get-Field $entry 'facts' @{}
            if (-not $factsByTopology.ContainsKey($topology)) { continue }
            $steadyStates = @(Get-FactCollection $factsByTopology[$topology] `
                'steady_states' 'states')
            foreach ($state in @($steadyStates | Where-Object {
                    "$(Get-Field $_ 'dir' '')" -eq 'S' -and
                    "$(Get-Field $_ 'kind' '')" -eq 'frame' -and
                    [bool](Get-Field $_ 'decode' $false) -and
                    [bool](Get-Field $_ 'passenger_present' $false)
                })) {
                $phase8Samples++
                if ([bool](Get-Field $state 'passenger_has_mount' $false) -and
                        [uint32](Get-Field $state 'passenger_mount_handle' 0xffff) -ne 0xffff) {
                    $boundSamples++
                }
            }
        }
        $totalPhase8 += $phase8Samples
        $totalBound += $boundSamples
        $topologyReports.Add([pscustomobject]@{
            topology = $topology
            phase8_samples = $phase8Samples
            observed_bound_samples = $boundSamples
        })
    }
    return [pscustomobject]@{
        # The canonical input drivers exercise W/A/D + relative look only and
        # have no map-independent seat-acquisition/attach action. Incidental
        # bound samples are disclosed but cannot upgrade this claim.
        status = 'structural-only'
        deterministic_live_control = $false
        phase8_samples = $totalPhase8
        observed_bound_samples = $totalBound
        structural_test = 'netsim_two_peer_fanout'
        topologies = @($topologyReports.ToArray())
    }
}

function Compare-OuterHandshakeDirection {
    param($Case, [string] $Topology, [string] $Direction, $Golden, $Subject)
    $caseId = "$(Get-Field $Case 'id' '')"
    $goldBinding = Resolve-OuterHandshakeSession $Golden
    $subjectBinding = Resolve-OuterHandshakeSession $Subject
    if (-not $goldBinding.valid -or -not $subjectBinding.valid) {
        $bindingCode = if (-not $goldBinding.valid) {
            $goldBinding.code
        } else {
            $subjectBinding.code
        }
        $bindingDetail = @(
            if (-not $goldBinding.valid) { "RR: $($goldBinding.message)" }
            if (-not $subjectBinding.valid) { "$Topology`: $($subjectBinding.message)" }
        ) -join ' '
        Add-ParityFailure $bindingCode $caseId $Topology `
            "Cannot compare outer $Direction handshake without a unique gameplay session. $bindingDetail"
        return
    }
    $opcodes = if ($Direction -ceq 'C') { @(0x41,0x42) } else { @(0x81,0x82) }
    foreach ($opcode in $opcodes) {
        $goldHandshake = Get-FirstHandshake $Golden $opcode ([int]$goldBinding.session)
        $subjectHandshake = Get-FirstHandshake $Subject $opcode `
            ([int]$subjectBinding.session)
        if ($null -eq $goldHandshake -or $null -eq $subjectHandshake) {
            Add-ParityFailure 'HANDSHAKE_MISSING' $caseId $Topology `
                ('Cannot compare outer 0x{0:x2}: RR or subject fact is missing.' -f $opcode)
            continue
        }
        if ((Get-NormalizedHandshakeKey $goldHandshake) -cne
                (Get-NormalizedHandshakeKey $subjectHandshake)) {
            Add-ParityFailure 'HANDSHAKE_MISMATCH' $caseId $Topology `
                ('Outer 0x{0:x2} field/CU order, kinds, lengths, or stable values differ from same-case RR.' -f $opcode)
        }
    }
}

function Compare-OuterHandshakeParity {
    param($Case, [string] $Topology, $Golden, $Subject)
    foreach ($direction in @('C','S')) {
        Compare-OuterHandshakeDirection $Case $Topology $direction $Golden $Subject
    }
}

function Get-InitialPacketGroups {
    param($Facts, [string] $Direction)
    $boundaryTag = if ($Direction -eq 'C') { '0c' } else { '0a' }
    $earliest = @{}
    foreach ($packet in @($Facts.packets | Where-Object { $_.dir -eq $Direction })) {
        # The capture's `session` is the client UDP port, not the encrypted
        # protocol SID. A reconnect can reuse both port and sequence, so all
        # origin/replay identities include the wire SID.
        $key = "$($packet.session)|$($packet.sid)|$($packet.sequence)"
        if (-not $earliest.ContainsKey($key) -or $packet.frame -lt $earliest[$key]) {
            $earliest[$key] = $packet.frame
        }
    }
    $groups = [System.Collections.Generic.List[string]]::new()
    foreach ($packet in @($Facts.packets | Where-Object { $_.dir -eq $Direction } | Sort-Object frame)) {
        $key = "$($packet.session)|$($packet.sid)|$($packet.sequence)"
        if ($packet.frame -ne $earliest[$key] -or $packet.records -eq 0) { continue }
        $groups.Add("$(Get-Field $packet 'flags' '0x00')|$($packet.tag_identities -join ',')|wire=$(Get-Field $packet 'wire_key' '')")
        if ($packet.tags -contains $boundaryTag) { break }
    }
    return @($groups.ToArray())
}

function Compare-InitialPacketGrouping {
    param($Case, [string] $Topology, [string] $Direction, $Golden, $Subject)
    $caseId = "$(Get-Field $Case 'id' '')"
    $goldGroups = @(Get-InitialPacketGroups $Golden $Direction)
    $subjectGroups = @(Get-InitialPacketGroups $Subject $Direction)
    if (($goldGroups -join '>') -ne ($subjectGroups -join '>')) {
        Add-ParityFailure "INITIAL_PACKET_GROUPING" $caseId $Topology `
            "$Direction initial reliable record grouping through the first gameplay packet differs from same-case RR."
    }
}

function Add-HistogramValue {
    param([hashtable] $Histogram, [string] $Key)
    if (-not $Histogram.ContainsKey($Key)) { $Histogram[$Key] = 0 }
    $Histogram[$Key]++
}

function Get-SteadyGameplayProfile {
    param($Facts, [string] $Direction)
    $boundaryTag = if ($Direction -eq 'C') { '0c' } else { '0a' }
    $connections = @{}
    $steadyPackets = @(Get-FactCollection $Facts 'steady_packets' 'packets')
    foreach ($packet in @($steadyPackets | Where-Object { $_.dir -eq $Direction })) {
        $connection = "$(Get-Field $packet 'connection_key' '')"
        if ([string]::IsNullOrWhiteSpace($connection)) {
            $connection = "$(Get-Field $packet 'session' '')|sid=$(Get-Field $packet 'sid' '')"
        }
        # Retain the wire SID in the key even for synthetic/self-test facts;
        # a reconnect must never create a transition across protocol sessions.
        $connection = "$connection|dir=$Direction|sid=$(Get-Field $packet 'sid' '')"
        if (-not $connections.ContainsKey($connection)) {
            $connections[$connection] = [System.Collections.Generic.List[object]]::new()
        }
        $connections[$connection].Add($packet)
    }

    $groupHistogram = @{}
    $orderHistogram = @{}
    $packetCount = 0
    $transitionCount = 0
    $connectionCount = 0
    foreach ($connection in $connections.Keys) {
        $earliest = @{}
        foreach ($packet in @($connections[$connection].ToArray() | Sort-Object frame)) {
            $origin = "$connection|seq=$(Get-Field $packet 'sequence' -1)"
            if (-not $earliest.ContainsKey($origin)) { $earliest[$origin] = $packet }
        }
        $packets = @($earliest.Values | Sort-Object frame)
        $boundary = -1
        for ($i = 0; $i -lt $packets.Count; $i++) {
            if (@($packets[$i].tags | ForEach-Object { "$($_)".ToLowerInvariant() }) -contains $boundaryTag) {
                $boundary = $i
                break
            }
        }
        if ($boundary -lt 0) { continue }
        $connectionCount++
        $previousIdentity = $null
        for ($i = $boundary + 1; $i -lt $packets.Count; $i++) {
            $packet = $packets[$i]
            if ([int](Get-Field $packet 'records' 0) -le 0) { continue }
            $identities = @($packet.tag_identities | ForEach-Object { "$($_)".ToLowerInvariant() })
            if ($identities.Count -eq 0) { continue }
            Add-HistogramValue $groupHistogram `
                "$(Get-Field $packet 'flags' '0x00')|$($identities -join ',')|wire=$(Get-Field $packet 'wire_key' '')"
            $packetCount++
            foreach ($identity in $identities) {
                if ($null -ne $previousIdentity) {
                    Add-HistogramValue $orderHistogram "$previousIdentity>$identity"
                    $transitionCount++
                }
                $previousIdentity = $identity
            }
        }
    }
    return [pscustomobject]@{
        connections = $connectionCount
        packets = $packetCount
        transitions = $transitionCount
        group_histogram = $groupHistogram
        order_histogram = $orderHistogram
    }
}

function Compare-NormalizedGameplayDistribution {
    param(
        [string] $CaseId,
        [string] $Topology,
        [string] $Code,
        [string] $Label,
        [hashtable] $GoldenHistogram,
        [int] $GoldenTotal,
        [hashtable] $SubjectHistogram,
        [int] $SubjectTotal,
        $Policy
    )
    $minimumSamples = [int](Get-Field $Policy 'gameplay_distribution_min_samples' 8)
    if ($GoldenTotal -lt $minimumSamples -or $SubjectTotal -lt $minimumSamples) {
        Add-ParityFailure "GAMEPLAY_DISTRIBUTION_UNEXERCISED" $CaseId $Topology `
            "$Label has RR/subject sample totals $GoldenTotal/$SubjectTotal; require at least $minimumSamples after the first gameplay packet."
        return
    }
    $minimumShare = [double](Get-Field $Policy 'gameplay_distribution_min_share' 0.02)
    $absoluteTolerance = [double](Get-Field $Policy `
        'gameplay_distribution_absolute_tolerance' 0.08)
    $relativeTolerance = [double](Get-Field $Policy `
        'gameplay_distribution_relative_tolerance' 0.50)
    $keys = @($GoldenHistogram.Keys + $SubjectHistogram.Keys | Sort-Object -Unique)
    $mismatches = [System.Collections.Generic.List[string]]::new()
    foreach ($key in $keys) {
        $goldCount = if ($GoldenHistogram.ContainsKey($key)) {
            [double]$GoldenHistogram[$key]
        } else { 0.0 }
        $subjectCount = if ($SubjectHistogram.ContainsKey($key)) {
            [double]$SubjectHistogram[$key]
        } else { 0.0 }
        $goldShare = $goldCount / [double]$GoldenTotal
        $subjectShare = $subjectCount / [double]$SubjectTotal
        if ([Math]::Max($goldShare, $subjectShare) -lt $minimumShare) { continue }
        $tolerance = [Math]::Max($absoluteTolerance, $goldShare * $relativeTolerance)
        if ([Math]::Abs($subjectShare - $goldShare) -gt $tolerance) {
            $mismatches.Add(
                "$key RR=$([Math]::Round($goldShare,3)) subject=$([Math]::Round($subjectShare,3))")
        }
    }
    if ($mismatches.Count -gt 0) {
        Add-ParityFailure $Code $CaseId $Topology `
            "$Label distribution differs after the first gameplay packet: $(@($mismatches | Select-Object -First 6) -join '; ')."
    }
}

function Compare-SteadyGameplayDistributions {
    param(
        $Case, [string] $Topology, [string] $Direction,
        $Golden, $Subject, $Policy
    )
    $caseId = "$(Get-Field $Case 'id' '')"
    $goldProfile = Get-SteadyGameplayProfile $Golden $Direction
    $subjectProfile = Get-SteadyGameplayProfile $Subject $Direction
    Compare-NormalizedGameplayDistribution $caseId $Topology `
        'GAMEPLAY_GROUP_DISTRIBUTION' "$Direction packet-group" `
        $goldProfile.group_histogram $goldProfile.packets `
        $subjectProfile.group_histogram $subjectProfile.packets $Policy
    Compare-NormalizedGameplayDistribution $caseId $Topology `
        'GAMEPLAY_ORDER_DISTRIBUTION' "$Direction adjacent-event order" `
        $goldProfile.order_histogram $goldProfile.transitions `
        $subjectProfile.order_histogram $subjectProfile.transitions $Policy
}

function Get-SessionPacketEnvelopeProfile {
    param($Facts, [string] $Direction)

    $packets = @((Get-FactCollection $Facts 'comparison_packets' 'packets') |
        Where-Object { "$(Get-Field $_ 'dir' '')" -ceq $Direction } |
        Sort-Object ts_ns, frame)
    $shapeHistogram = @{}
    $headerFlags = [System.Collections.Generic.List[string]]::new()
    $headerOnly = 0
    foreach ($packet in $packets) {
        $records = [int](Get-Field $packet 'records' 0)
        $flags = "$(Get-Field $packet 'flags' '0x00')".ToLowerInvariant()
        $shape = if ($records -eq 0) { 'header-only' } else { 'records' }
        Add-HistogramValue $shapeHistogram "$shape|flags=$flags"
        if ($records -eq 0) {
            $headerOnly++
            $headerFlags.Add($flags)
        }
    }
    $durationMs = [double](Get-Field $Facts 'comparison_duration_ms' `
        (Get-Field $Facts 'duration_ms' 0))
    return [pscustomobject]@{
        packets = $packets.Count
        header_only = $headerOnly
        duration_ms = $durationMs
        packets_per_second = if ($durationMs -gt 0) {
            1000.0 * [double]$packets.Count / $durationMs
        } else { 0.0 }
        header_only_per_second = if ($durationMs -gt 0) {
            1000.0 * [double]$headerOnly / $durationMs
        } else { 0.0 }
        header_flags = @($headerFlags.ToArray() | Sort-Object -Unique)
        shape_histogram = $shapeHistogram
    }
}

function Compare-SessionPacketCadenceCount {
    param(
        [string] $CaseId, [string] $Topology, [string] $Direction,
        [string] $Label, [int] $GoldenCount, [double] $GoldenDurationMs,
        [int] $SubjectCount, [double] $SubjectDurationMs, $Policy
    )
    if ($GoldenDurationMs -le 0 -or $SubjectDurationMs -le 0) {
        Add-ParityFailure 'SESSION_PACKET_CADENCE' $CaseId $Topology `
            "$Direction $Label cannot be rate-normalized because a comparison duration is zero."
        return
    }
    $expected = [double]$GoldenCount * $SubjectDurationMs / $GoldenDurationMs
    $absoluteTolerance = [double](Get-Field $Policy 'count_absolute_tolerance' 1)
    $relativeTolerance = [double](Get-Field $Policy 'count_relative_tolerance' 0.20)
    $tolerance = [Math]::Max(
        $absoluteTolerance, [Math]::Ceiling($expected * $relativeTolerance))
    # Absence is structural, not statistical: one side cannot invent or omit
    # an entire packet class merely because the generic count allowance is 1.
    $classAbsent = ($GoldenCount -eq 0) -xor ($SubjectCount -eq 0)
    if ($classAbsent -or [Math]::Abs([double]$SubjectCount - $expected) -gt $tolerance) {
        Add-ParityFailure 'SESSION_PACKET_CADENCE' $CaseId $Topology `
            "$Direction $Label count $SubjectCount differs from same-case RR rate-normalized $([Math]::Round($expected, 2)) (tolerance $tolerance)."
    }
}

function Compare-SessionPacketEnvelope {
    param(
        $Case, [string] $Topology, [string] $Direction,
        $Golden, $Subject, $Policy
    )
    $caseId = "$(Get-Field $Case 'id' '')"
    $gold = Get-SessionPacketEnvelopeProfile $Golden $Direction
    $subject = Get-SessionPacketEnvelopeProfile $Subject $Direction
    Compare-SessionPacketCadenceCount $caseId $Topology $Direction `
        'session-packet cadence' $gold.packets $gold.duration_ms `
        $subject.packets $subject.duration_ms $Policy
    Compare-SessionPacketCadenceCount $caseId $Topology $Direction `
        'header/ACK-only cadence' $gold.header_only $gold.duration_ms `
        $subject.header_only $subject.duration_ms $Policy

    if (($gold.header_flags -join ',') -cne ($subject.header_flags -join ',')) {
        Add-ParityFailure 'SESSION_PACKET_HEADER_FLAGS' $caseId $Topology `
            "$Direction header/ACK-only flag coverage differs from same-case RR (RR=$($gold.header_flags -join ',') subject=$($subject.header_flags -join ','))."
    }
    Compare-NormalizedGameplayDistribution $caseId $Topology `
        'SESSION_PACKET_SHAPE_DISTRIBUTION' "$Direction session-packet shape/flags" `
        $gold.shape_histogram $gold.packets `
        $subject.shape_histogram $subject.packets $Policy
}

function Get-IntegrityTransactions {
    param($Facts)
    $events = @($Facts.events)
    $transactions = [System.Collections.Generic.List[object]]::new()
    for ($i = 0; $i -lt $events.Count; $i++) {
        $challenge = $events[$i]
        if ($challenge.dir -ne "S" -or $challenge.identity -ne "31:0") { continue }
        $reply = $null
        for ($j = $i + 1; $j -lt $events.Count; $j++) {
            $candidate = $events[$j]
            if ($candidate.connection_key -ne $challenge.connection_key) { continue }
            if ($candidate.dir -eq "S" -and $candidate.identity -eq "31:0") { break }
            if ($candidate.dir -eq "C" -and $candidate.identity -eq "21:0") {
                $reply = $candidate
                break
            }
        }
        if ($challenge.body.Length -ne 3 -or $null -eq $reply -or $reply.body.Length -ne 9) {
            $transactions.Add([pscustomobject]@{ valid=$false; challenge=$challenge; reply=$reply })
            continue
        }
        $row = [int]$challenge.body[0]
        $key = [uint32](Read-U16Le $challenge.body 1)
        $replyRow = [int]$reply.body[0]
        $replyCrc = Read-U32Le $reply.body 1
        $replyKey = Read-U32Le $reply.body 5
        $transactions.Add([pscustomobject]@{
            valid = $replyRow -eq $row -and $replyKey -eq $key
            challenge = $challenge
            reply = $reply
            row = $row
            key = $key
            reply_crc = $replyCrc
            # Retail sends xorKey ^ sanitized ammo-row CRC and also echoes the
            # key in the trailing dword. Recover the corpus-stable RR oracle.
            base_crc = [uint32]($replyCrc -bxor $key)
        })
    }
    return @($transactions.ToArray())
}

function Get-EntityIntegrityTransactions {
    param($Facts)
    $events = @($Facts.events)
    $transactions = [System.Collections.Generic.List[object]]::new()
    for ($i = 0; $i -lt $events.Count; $i++) {
        $challenge = $events[$i]
        if ($challenge.dir -ne "S" -or $challenge.identity -ne "30:0") { continue }
        $reply = $null
        for ($j = $i + 1; $j -lt $events.Count; $j++) {
            $candidate = $events[$j]
            if ($candidate.connection_key -ne $challenge.connection_key) { continue }
            if ($candidate.dir -eq "S" -and $candidate.identity -eq "30:0") { break }
            if ($candidate.dir -eq "C" -and $candidate.identity -eq "20:0") {
                $reply = $candidate
                break
            }
        }
        if ($challenge.body.Length -ne 3 -or $null -eq $reply -or $reply.body.Length -ne 5) {
            $transactions.Add([pscustomobject]@{ valid=$false; challenge=$challenge; reply=$reply })
            continue
        }
        $row = [int]$challenge.body[0]
        $key = [uint32](Read-U16Le $challenge.body 1)
        $replyRow = [int]$reply.body[0]
        $replyCrc = Read-U32Le $reply.body 1
        $transactions.Add([pscustomobject]@{
            valid = $replyRow -eq $row
            challenge = $challenge
            reply = $reply
            row = $row
            key = $key
            reply_crc = $replyCrc
            base_crc = [uint32]($replyCrc -bxor $key)
        })
    }
    return @($transactions.ToArray())
}

function Compare-SameCaseEntityIntegrityRows {
    param(
        [string] $CaseId, [string] $Topology,
        [object[]] $GoldenTransactions, [object[]] $SubjectTransactions
    )
    $goldRows = @($GoldenTransactions | Where-Object { $_.valid } |
        ForEach-Object { [int]$_.row } | Sort-Object -Unique)
    $subjectRows = @($SubjectTransactions | Where-Object { $_.valid } |
        ForEach-Object { [int]$_.row } | Sort-Object -Unique)
    if (($goldRows -join ',') -cne ($subjectRows -join ',')) {
        Add-ParityFailure 'HOST_DYNAMIC_ENTITY_ROW_MISMATCH' $CaseId $Topology `
            "OpenNova host 0x30 ADM/entity challenge rows do not match the same case's RR rows (RR=$($goldRows -join ',') subject=$($subjectRows -join ','))."
    }
}

function Test-IntegrityStream {
    param($Facts, $Case, [string] $Topology, $Oracle, $EntityOracle, [double] $TimingToleranceMs)
    $caseId = "$(Get-Field $Case 'id' '')"
    $transactions = @(Get-IntegrityTransactions $Facts)
    if ($transactions.Count -eq 0) {
        Add-ParityFailure "INTEGRITY_UNEXERCISED" $caseId $Topology "No S2C 0x31/C2S 0x21 transaction was captured."
    }
    foreach ($transaction in $transactions) {
        if (-not $transaction.valid) {
            Add-ParityFailure "INTEGRITY_PAIR_INVALID" $caseId $Topology `
                "S2C 0x31 did not receive a shape/row/key-correct C2S 0x21 reply."
            continue
        }
        $rowKey = "{0:x2}" -f $transaction.row
        if (-not $Oracle.ContainsKey($rowKey)) {
            Add-ParityFailure "INTEGRITY_ROW_UNWITNESSED" $caseId $Topology `
                "Ammo row 0x$rowKey has no retail-retail CRC oracle in this suite."
        } elseif ([uint32]$Oracle[$rowKey] -ne [uint32]$transaction.base_crc) {
            Add-ParityFailure "INTEGRITY_CRC_MISMATCH" $caseId $Topology `
                "Ammo row 0x$rowKey CRC does not match the RR-derived corpus oracle."
        }
    }
    $entityTransactions = @(Get-EntityIntegrityTransactions $Facts)
    if ($entityTransactions.Count -eq 0) {
        Add-ParityFailure "ENTITY_INTEGRITY_UNEXERCISED" $caseId $Topology `
            "No S2C 0x30/C2S 0x20 transaction was captured."
    }
    foreach ($transaction in $entityTransactions) {
        if (-not $transaction.valid) {
            Add-ParityFailure "ENTITY_INTEGRITY_PAIR_INVALID" $caseId $Topology `
                "S2C 0x30 did not receive a shape/id-correct C2S 0x20 reply."
            continue
        }
        $rowKey = "{0:x2}" -f $transaction.row
        if (-not $EntityOracle.ContainsKey($rowKey)) {
            Add-ParityFailure "ENTITY_INTEGRITY_ROW_UNWITNESSED" $caseId $Topology `
                "ADM/entity row 0x$rowKey has no retail-retail checksum oracle in this suite."
        } elseif ([uint32]$EntityOracle[$rowKey] -ne [uint32]$transaction.base_crc) {
            Add-ParityFailure "ENTITY_INTEGRITY_CRC_MISMATCH" $caseId $Topology `
                "ADM/entity row 0x$rowKey checksum does not match the RR-derived corpus oracle."
        }
    }

    $challenges = @($Facts.events | Where-Object {
        $_.dir -eq "S" -and ($_.identity -eq "30:0" -or $_.identity -eq "31:0")
    })
    foreach ($challengeGroup in @($challenges | Group-Object connection_key)) {
        $stream = @($challengeGroup.Group | Sort-Object frame)
        for ($i = 1; $i -lt $stream.Count; $i++) {
            if ($stream[$i].tag -eq $stream[$i - 1].tag) {
                Add-ParityFailure "INTEGRITY_NOT_ALTERNATING" $caseId $Topology `
                    "S2C 0x30/0x31 did not alternate within SID-bound connection $($challengeGroup.Name)."
                break
            }
        }
    }
    $badGrouping = 0
    foreach ($challenge in $challenges) {
        $owners = @($Facts.packets | Where-Object {
            $_.dir -eq 'S' -and $_.frame -eq $challenge.frame -and
                $_.connection_key -eq $challenge.connection_key -and
                $_.sid -eq $challenge.sid -and
                $_.tag_identities -contains $challenge.identity
        })
        if ($owners.Count -ne 1 -or
                (($owners[0].tag_identities -join ',') -notmatch "(?:^|,)16:0,$($challenge.tag):0(?:,|$)")) {
            $badGrouping++
        }
    }
    if ($badGrouping -gt 0) {
        Add-ParityFailure "INTEGRITY_SCOREBOARD_ORDER" $caseId $Topology `
            "$badGrouping integrity challenge(s) were not immediately preceded by transient S2C 0x16 in the same packet."
    }
    if ([bool](Get-Field $Case "maintenance" $false)) {
        $minimum = [Math]::Max(4, [int](Get-Field $Case "minimum_integrity_events" 4))
        if ($challenges.Count -lt $minimum) {
            Add-ParityFailure "INTEGRITY_WINDOW_SHORT" $caseId $Topology `
                "Maintenance case needs at least $minimum integrity events; saw $($challenges.Count)."
        }
        $expectedMs = 311.0 / 62.0 * 1000.0
        foreach ($challengeGroup in @($challenges | Group-Object connection_key)) {
            $stream = @($challengeGroup.Group | Sort-Object frame)
            for ($i = 1; $i -lt $stream.Count; $i++) {
                if ($stream[$i].ts_ns -eq 0 -or $stream[$i - 1].ts_ns -eq 0) {
                    Add-ParityFailure "TIMESTAMP_MISSING" $caseId $Topology "Capture has no integrity timestamps."
                    break
                }
                $actualMs = [double]($stream[$i].ts_ns - $stream[$i - 1].ts_ns) / 1000000.0
                if ([Math]::Abs($actualMs - $expectedMs) -gt $TimingToleranceMs) {
                    Add-ParityFailure "INTEGRITY_CADENCE" $caseId $Topology `
                        "Integrity interval $([Math]::Round($actualMs))ms is outside 311/62Hz expectation $([Math]::Round($expectedMs))ms +/- ${TimingToleranceMs}ms."
                }
            }
        }
    }
    return $transactions
}

function Get-EventAtFrame {
    param($Facts, [int] $Frame, [string] $ConnectionKey, [uint32] $Sid, [string] $Tag)
    return @($Facts.events | Where-Object {
        $_.dir -eq "S" -and $_.frame -eq $Frame -and
            $_.connection_key -eq $ConnectionKey -and $_.sid -eq $Sid -and
            $_.identity -eq "${Tag}:0"
    } | Select-Object -First 1)[0]
}

function Get-OriginPacketForEvent {
    param($Facts, $Event)
    $owners = @($Facts.packets | Where-Object {
        $_.frame -eq $Event.frame -and $_.dir -eq $Event.dir -and
            $_.connection_key -eq $Event.connection_key -and $_.sid -eq $Event.sid -and
            $_.tag_identities -contains $Event.identity
    })
    if ($owners.Count -ne 1) { return $null }
    $owner = $owners[0]
    $prior = @($Facts.packets | Where-Object {
        $_.dir -eq $owner.dir -and $_.session -eq $owner.session -and
            $_.sid -eq $owner.sid -and $_.sequence -eq $owner.sequence -and
            $_.frame -lt $owner.frame
    })
    if ($prior.Count -ne 0) { return $null }
    return $owner
}

function Test-ControlStream {
    param(
        $Facts, $Case, [string] $Topology, [double] $TimingToleranceMs,
        [object[]] $CharattrOracle = @()
    )
    $caseId = "$(Get-Field $Case 'id' '')"
    $controlCandidates = @($Facts.packets | Where-Object {
        $_.dir -eq "S" -and ($_.tag_identities -contains "39:0" -or
            $_.tag_identities -contains "43:0" -or $_.tag_identities -contains "68:0")
    })
    $controlPackets = @($controlCandidates | Where-Object {
        $candidate = $_
        @($Facts.packets | Where-Object {
            $_.dir -eq $candidate.dir -and $_.session -eq $candidate.session -and
            $_.sid -eq $candidate.sid -and $_.sequence -eq $candidate.sequence -and
            $_.frame -lt $candidate.frame
        }).Count -eq 0
    })
    $mode = "$(Get-Field $Case 'control_mode' 'observe')"
    if ($mode -eq "forbidden") {
        if ($controlPackets.Count -gt 0) {
            Add-ParityFailure "CONTROL_WHILE_HELD" $caseId $Topology `
                "Control quartet appeared in a manifest-declared deployment-hold case."
        }
        return @()
    }
    $required = [bool](Get-Field $Case "maintenance" $false) -and $mode -eq "required"
    $minimum = [Math]::Max(4, [int](Get-Field $Case "minimum_control_quartets" 4))
    if ($required -and $controlPackets.Count -lt $minimum) {
        Add-ParityFailure "CONTROL_WINDOW_SHORT" $caseId $Topology `
            "Maintenance case needs at least $minimum control quartets; saw $($controlPackets.Count)."
    }
    if ($controlPackets.Count -eq 0) {
        return @()
    }
    $timeValues = [System.Collections.Generic.List[uint32]]::new()
    $clientTimeValues = [System.Collections.Generic.List[uint32]]::new()
    $cursorValues = [System.Collections.Generic.List[uint32]]::new()
    $seeds = [System.Collections.Generic.List[uint32]]::new()
    $charattrBases = [System.Collections.Generic.List[uint32]]::new()
    foreach ($packet in $controlPackets) {
        $joined = $packet.tag_identities -join ','
        if ($joined -notmatch '(?:^|,)39:0,42:0,43:0,68:0(?:,|$)') {
            Add-ParityFailure "CONTROL_GROUPING" $caseId $Topology `
                "Control records are not contiguous 0x39,0x42,0x43,0x68 in one packet (frame $($packet.frame))."
            continue
        }
        $seed = Get-EventAtFrame $Facts $packet.frame $packet.connection_key $packet.sid "39"
        $flags = Get-EventAtFrame $Facts $packet.frame $packet.connection_key $packet.sid "42"
        $time = Get-EventAtFrame $Facts $packet.frame $packet.connection_key $packet.sid "43"
        $cursor = Get-EventAtFrame $Facts $packet.frame $packet.connection_key $packet.sid "68"
        if ($null -eq $seed -or $seed.body.Length -ne 4 -or
                $null -eq $flags -or $flags.body.Length -ne 2 -or
                $null -eq $time -or $time.body.Length -ne 4 -or
                $null -eq $cursor -or $cursor.body.Length -ne 4) {
            Add-ParityFailure "CONTROL_SHAPE" $caseId $Topology `
                "Control packet frame $($packet.frame) lacks exact decoded bodies."
            continue
        }
        $seedValue = Read-U32Le $seed.body 0
        $flagValue = Read-U16Le $flags.body 0
        $timeValue = Read-U32Le $time.body 0
        $cursorValue = Read-U32Le $cursor.body 0
        $seeds.Add($seedValue)
        $timeValues.Add($timeValue)
        $cursorValues.Add($cursorValue)
        if ($flagValue -ne 0) {
            Add-ParityFailure "CONTROL_FLAGS" $caseId $Topology "Periodic S2C 0x42 is not 0x0000."
        }

        $nextControl = @($controlPackets | Where-Object {
            $_.connection_key -eq $packet.connection_key -and $_.sid -eq $packet.sid -and
                $_.frame -gt $packet.frame
        } | Sort-Object frame | Select-Object -First 1)
        $nextFrame = if ($nextControl.Count -gt 0) { $nextControl[0].frame } else { [int]::MaxValue }
        $replyEvents = @($Facts.events | Where-Object {
            $_.dir -eq 'C' -and $_.connection_key -eq $packet.connection_key -and
                $_.frame -gt $packet.frame -and $_.frame -lt $nextFrame -and
                @('08:0','1c:0','3d:0') -contains $_.identity -and
                $null -ne (Get-OriginPacketForEvent $Facts $_)
        })
        $charReplies = @($replyEvents | Where-Object { $_.identity -eq '1c:0' })
        $timeReplies = @($replyEvents | Where-Object { $_.identity -eq '08:0' })
        $pageReplies = @($replyEvents | Where-Object { $_.identity -eq '3d:0' })
        $expectsPage = $packet.tag_identities -contains '68:0'
        if ($charReplies.Count -ne 1 -or $timeReplies.Count -ne 1 -or
                ($expectsPage -and $pageReplies.Count -ne 1) -or
                (-not $expectsPage -and $pageReplies.Count -ne 0)) {
            Add-ParityFailure "CONTROL_REPLY_COUNT" $caseId $Topology `
                "Control frame $($packet.frame) needs one reliable C2S 0x1C/0x08 reply$(if ($expectsPage) { ' and one transient 0x3D reply' } else { '' }) before the next quartet."
            continue
        }
        $charReply = $charReplies[0]
        $timeReply = $timeReplies[0]
        if ($charReply.body.Length -ne 4 -or $timeReply.body.Length -ne 8) {
            Add-ParityFailure "CONTROL_REPLY_SHAPE" $caseId $Topology `
                "C2S 0x1C/0x08 control replies are not exact 4/8-byte bodies."
            continue
        }
        $replyPacket = Get-OriginPacketForEvent $Facts $timeReply
        $replyPattern = if ($expectsPage) { '(?:^|,)1c:0,08:0,3d:0(?:,|$)' } else { '(?:^|,)1c:0,08:0(?:,|$)' }
        if ($null -eq $replyPacket -or (($replyPacket.tag_identities -join ',') -notmatch $replyPattern) -or
                $charReply.frame -ne $timeReply.frame -or
                ($expectsPage -and $pageReplies[0].frame -ne $timeReply.frame)) {
            Add-ParityFailure "CONTROL_REPLY_GROUPING" $caseId $Topology `
                "C2S control replies are not contiguous 0x1C,0x08$(if ($expectsPage) { ',0x3D' } else { '' }) in one origin packet."
        }
        $replySequence = Read-U32Le $timeReply.body 0
        if ($replySequence -ne $timeValue) {
            Add-ParityFailure "CONTROL_TIME_ECHO" $caseId $Topology `
                "C2S 0x08 sequence $replySequence does not echo S2C 0x43 sequence $timeValue."
        }
        $clientTimeValues.Add((Read-U32Le $timeReply.body 4))
        $charattrBases.Add([uint32]((Read-U32Le $charReply.body 0) -bxor $seedValue))
        if ($expectsPage) {
            $page = $pageReplies[0]
            if ($page.body.Length -lt 4 -or $page.body.Length -gt 204 -or
                    (($page.body.Length - 4) % 4) -ne 0 -or
                    (Read-U32Le $page.body 0) -ne $cursorValue) {
                Add-ParityFailure "CONTROL_PAGE_REPLY" $caseId $Topology `
                    "C2S 0x3D does not echo the S2C 0x68 cursor with at most fifty dword rows."
            }
        }
    }
    if (@($seeds | Select-Object -Unique).Count -ne 1) {
        Add-ParityFailure "CONTROL_SEED_CHANGED" $caseId $Topology "Retained S2C 0x39 seed changed between quartets."
    }
    if ($timeValues.Count -ge 4 -and ($timeValues[0..3] -join ',') -ne '1,1,2,2') {
        Add-ParityFailure "CONTROL_TIME_SEQUENCE" $caseId $Topology `
            "First four S2C 0x43 values are not 1,1,2,2."
    }
    for ($i = 1; $i -lt $timeValues.Count; $i++) {
        $previous = [uint32]$timeValues[$i - 1]
        $expectedNext = [uint32](([uint64]$previous + 1) % 4294967296)
        if ($timeValues[$i] -ne $previous -and $timeValues[$i] -ne $expectedNext) {
            Add-ParityFailure "CONTROL_TIME_SEQUENCE" $caseId $Topology `
                "S2C 0x43 jumps from $previous to $($timeValues[$i]); only retain or +1 is valid."
        }
        if ($timeValues[$i] -eq $expectedNext -and
                ($i -lt 2 -or $timeValues[$i - 2] -ne $previous)) {
            Add-ParityFailure "CONTROL_TIME_SEQUENCE" $caseId $Topology `
                "S2C 0x43 advanced sequence $previous before two matching reply rounds."
        }
    }
    if ($clientTimeValues.Count -ge 2 -and
            @($clientTimeValues | Select-Object -Unique).Count -lt 2) {
        Add-ParityFailure "CONTROL_CLIENT_CLOCK_CONSTANT" $caseId $Topology `
            "C2S 0x08 client GetTickCount values never change."
    }
    foreach ($base in @($charattrBases.ToArray() | Select-Object -Unique)) {
        if ($CharattrOracle.Count -gt 0 -and $CharattrOracle -notcontains $base) {
            Add-ParityFailure "CONTROL_CHARATTR_CRC_MISMATCH" $caseId $Topology `
                "C2S 0x1C seed-normalized checksum has no same-case RR oracle."
        }
    }
    $height = 0
    $resolution = "$(Get-Field $Case 'resolution' '')"
    if ($resolution -match '^[0-9]+x([0-9]+)$') { $height = [int]$Matches[1] }
    for ($i = 0; $i -lt $cursorValues.Count; $i++) {
        $expected = if ($i -eq 0) { 50 } else {
            $next = [int]$cursorValues[$i - 1] + 50
            if ($next -ge $height) { 0 } else { $next }
        }
        if ([int]$cursorValues[$i] -ne $expected) {
            Add-ParityFailure "CONTROL_CURSOR" $caseId $Topology `
                "S2C 0x68 cursor $($cursorValues[$i]) does not follow +50/wrap-at-$height sequence (expected $expected)."
        }
    }

    $firstFrame = @($Facts.events | Where-Object { $_.dir -eq "S" -and $_.identity -eq "0a:0" } |
        Select-Object -First 1)
    if ($firstFrame.Count -gt 0 -and $firstFrame[0].ts_ns -gt 0 -and $controlPackets[0].ts_ns -gt 0) {
        $firstDelayMs = [double]($controlPackets[0].ts_ns - $firstFrame[0].ts_ns) / 1000000.0
        $minimumDelayMs = 1861.0 / 62.0 * 1000.0 - $TimingToleranceMs
        if ($firstDelayMs -lt $minimumDelayMs) {
            Add-ParityFailure "CONTROL_TOO_EARLY" $caseId $Topology `
                "First quartet arrived ${firstDelayMs}ms after first gameplay frame; expected no earlier than call 1861 (pre-increment age gate) at 62Hz minus tolerance."
        }
    }
    $repeatMs = 744.0 / 62.0 * 1000.0
    for ($i = 1; $i -lt $controlPackets.Count; $i++) {
        $actualMs = [double]($controlPackets[$i].ts_ns - $controlPackets[$i - 1].ts_ns) / 1000000.0
        if ([Math]::Abs($actualMs - $repeatMs) -gt $TimingToleranceMs) {
            Add-ParityFailure "CONTROL_CADENCE" $caseId $Topology `
                "Quartet interval $([Math]::Round($actualMs))ms is outside 744/62Hz expectation ${repeatMs}ms +/- ${TimingToleranceMs}ms."
        }
    }

    # A same-sequence repeat is an actual NACK reconstruction witness. When it
    # exists, assert reliable 0x39/0x43 survive and transient 0x42/0x68 do not.
    foreach ($packet in $controlPackets) {
        $replays = @($Facts.packets | Where-Object {
            $_.dir -eq $packet.dir -and $_.session -eq $packet.session -and
            $_.sid -eq $packet.sid -and $_.sequence -eq $packet.sequence -and
            $_.frame -gt $packet.frame
        })
        foreach ($replay in $replays) {
            $script:RetentionWitnesses++
            if ($replay.tag_identities -notcontains "39:0" -or
                    $replay.tag_identities -notcontains "43:0" -or
                    $replay.tag_identities -contains "42:0" -or
                    $replay.tag_identities -contains "68:0") {
                Add-ParityFailure "RETENTION_CLASSIFICATION" $caseId $Topology `
                    "NACK replay did not preserve reliable 0x39/0x43 while pruning transient 0x42/0x68."
            }
        }
    }
    return @($charattrBases.ToArray() | Select-Object -Unique)
}

function Invoke-VerifierSelfTest {
	$initialJoin = [pscustomobject]@{
		readiness_mode='in_match'; process_id=4101; ticks_msec=1000;
		heartbeat_sequence=10; in_match=$true; local_player=$true;
		deploy_hold_ready=$false; deployment_pick_pending=$false;
		deployment_pick_sent=$true; auto_deploy=$true; self_handle=89;
		join_error=''; session_lost=$false; joiner_phase=4;
		join_admission_stage='in match'; deploy_presented=$false;
		motion_complete=$false
	}
	$postDeath = [pscustomobject]@{
		readiness_mode='in_match'; process_id=4101; ticks_msec=2000;
		heartbeat_sequence=11; in_match=$false; local_player=$true;
		deploy_hold_ready=$false; deployment_pick_pending=$true;
		deployment_pick_sent=$false; auto_deploy=$true; self_handle=89;
		join_error=''; session_lost=$false; joiner_phase=3;
		join_admission_stage="awaiting the player's deployment pick";
		deploy_presented=$true; motion_complete=$true
	}
	# The witnessed deploy hold: protocol InMatch (phase 4) with the motor
	# present while the DEATH screen holds presentation (net-re 5.61).
	$deployHold = [pscustomobject]@{
		readiness_mode='deploy_hold'; process_id=4102; ticks_msec=1000;
		heartbeat_sequence=10; in_match=$true; local_player=$true;
		deploy_hold_ready=$true; deployment_pick_pending=$true;
		deployment_pick_sent=$false; auto_deploy=$false; self_handle=90;
		join_error=''; session_lost=$false; joiner_phase=4;
		join_admission_stage="awaiting the player's deployment pick";
		deploy_presented=$true; motion_complete=$false
	}
	if ((Get-OpenNovaJoinerReadinessClass -State $initialJoin `
			-ReadinessMode in_match) -cne 'in_match' -or
		(Get-OpenNovaJoinerReadinessClass -State $postDeath `
			-ReadinessMode in_match) -cne 'invalid' -or
		(Get-OpenNovaJoinerReadinessClass -State $postDeath `
			-ReadinessMode in_match -AllowPostDeathRedeploy) -cne
				'post_death_redeploy' -or
		(Get-OpenNovaJoinerReadinessClass -State $deployHold `
			-ReadinessMode deploy_hold) -cne 'deploy_hold' -or
		-not (Test-OpenNovaJoinerReadinessTransition -Initial $initialJoin `
			-Final $postDeath -ReadinessMode in_match `
			-AllowPostDeathRedeploy -RequireMotionComplete)) {
		throw 'OpenNova joiner readiness transition positive self-test failed'
	}
	$deployHold.deploy_presented = $false
	if ((Get-OpenNovaJoinerReadinessClass -State $deployHold `
			-ReadinessMode deploy_hold) -cne 'invalid') {
		throw 'OpenNova deploy-hold presentation fail-closed self-test failed'
	}
	$deployHold.deploy_presented = $true
	$postDeath.session_lost = $true
	if ((Get-OpenNovaJoinerReadinessClass -State $postDeath `
			-ReadinessMode in_match -AllowPostDeathRedeploy) -cne 'invalid' -or
		(Test-OpenNovaJoinerReadinessTransition -Initial $initialJoin `
			-Final $postDeath -ReadinessMode in_match `
			-AllowPostDeathRedeploy -RequireMotionComplete)) {
		throw 'OpenNova joiner readiness transition fail-closed self-test failed'
	}
	$postDeath.session_lost = $false
	$readinessWitness = New-OpenNovaJoinerReadinessWitness `
		-Initial $initialJoin -Final $postDeath -ReadinessMode in_match `
		-AllowPostDeathRedeploy -RequireMotionComplete
	$readinessRun = [pscustomobject]@{
		opennova_joiner_readiness_witness = $readinessWitness
		opennova_launch_proofs = [pscustomobject]@{
			joiner = [pscustomobject]@{ runtime_pid = 4101 }
		}
	}
	$readinessCase = [pscustomobject]@{ readiness_mode = 'in_match' }
	$script:Failures.Clear()
	if (-not (Test-OpenNovaJoinerReadinessWitness $readinessRun `
			$readinessCase 'joiner-readiness' 'RO') -or
			$script:Failures.Count -ne 0) {
		throw 'OpenNova joiner readiness witness positive self-test failed'
	}
	$readinessWitness.final.session_lost = $true
	$script:Failures.Clear()
	$null = Test-OpenNovaJoinerReadinessWitness $readinessRun `
		$readinessCase 'joiner-readiness' 'RO'
	if ($script:Failures.Count -ne 1 -or
			$script:Failures[0].code -ne 'OPENNOVA_JOINER_READINESS_INVALID') {
		throw 'OpenNova joiner readiness witness tamper self-test failed'
	}
	$readinessWitness.final.session_lost = $false
	$script:Failures.Clear()

    if (-not (Test-RetailAutomationPort 32786) -or
            -not (Test-RetailAutomationPort 32789) -or
            (Test-RetailAutomationPort 32785) -or
            (Test-RetailAutomationPort 32790)) {
        throw 'retail automation port-range self-test failed'
    }
    if (-not (Test-UInt32JsonField ([pscustomobject]@{game_type=0}) 'game_type') -or
            -not (Test-UInt32JsonField `
                ([pscustomobject]@{game_type=[uint64]4294967295}) 'game_type') -or
            (Test-UInt32JsonField ([pscustomobject]@{}) 'game_type') -or
            (Test-UInt32JsonField ([pscustomobject]@{game_type='0'}) 'game_type') -or
            (Test-UInt32JsonField ([pscustomobject]@{game_type=@(0)}) 'game_type') -or
            (Test-UInt32JsonField ([pscustomobject]@{game_type=-1}) 'game_type') -or
            (Test-UInt32JsonField `
                ([pscustomobject]@{game_type=[uint64]4294967296}) 'game_type')) {
        throw 'explicit UInt32 JSON field self-test failed'
    }
    $derivedMissionRoles = @(
        Get-ParityMissionArtifactRole '01TR.bms'
        Get-ParityMissionArtifactRole 'TDH_I3A.bms'
        Get-ParityMissionArtifactRole 'CP04.bms'
        Get-ParityMissionArtifactRole '03TR.bms'
    )
    $derivedCaseSlugs = @(
        Get-ParityCaseRunSlug '01tr-waypoint'
        Get-ParityCaseRunSlug 'tdh-i3a-tdm'
        Get-ParityCaseRunSlug 'cp04-deploy-hold'
        Get-ParityCaseRunSlug '03tr-dense'
    )
    if (($derivedMissionRoles -join ',') -cne
            'mission_01tr,mission_tdh_i3a,mission_cp04,mission_03tr' -or
            ($derivedCaseSlugs -join ',') -cne '01tr,tdh,cp04,03tr') {
        throw 'canonical parity case/mission derivation self-test failed'
    }

    $sourceStateRoot = Join-Path ([IO.Path]::GetTempPath()) `
        ('opennova-source-state-selftest-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $sourceStateRoot | Out-Null
    try {
        $utf8 = [Text.UTF8Encoding]::new($false)
        [IO.File]::WriteAllText((Join-Path $sourceStateRoot '.gitignore'),
            ".scratch/`nbuild/`n", $utf8)
        [IO.File]::WriteAllText((Join-Path $sourceStateRoot 'runtime.cpp'),
            "int parity_source = 1;`n", $utf8)
        & git -C $sourceStateRoot init --quiet
        if ($LASTEXITCODE -ne 0) { throw 'source-state self-test git init failed' }
        & git -C $sourceStateRoot config core.autocrlf false
        if ($LASTEXITCODE -ne 0) { throw 'source-state self-test git config failed' }
        & git -C $sourceStateRoot add -- .gitignore runtime.cpp
        if ($LASTEXITCODE -ne 0) { throw 'source-state self-test git add failed' }
        $baselineSourceState = Get-NetHarnessRepositorySourceState `
            -RepoRoot $sourceStateRoot
        if ([string]$baselineSourceState.schema -cne
                'opennova.repository-source-state.v1' -or
                [int]$baselineSourceState.entry_count -ne 2 -or
                [string]$baselineSourceState.sha256 -cnotmatch '^[0-9a-f]{64}$') {
            throw 'repository source-state self-test emitted an invalid baseline'
        }

        foreach ($ignoredDirectory in @('.scratch', 'build')) {
            $directory = Join-Path $sourceStateRoot $ignoredDirectory
            New-Item -ItemType Directory -Path $directory | Out-Null
            [IO.File]::WriteAllText((Join-Path $directory 'ignored.bin'),
                [Guid]::NewGuid().ToString('N'), $utf8)
        }
        $ignoredOutputState = Get-NetHarnessRepositorySourceState `
            -RepoRoot $sourceStateRoot
        if ([string]$ignoredOutputState.sha256 -cne
                [string]$baselineSourceState.sha256) {
            throw 'repository source state included ignored evidence/build output'
        }

        [IO.File]::WriteAllText((Join-Path $sourceStateRoot 'runtime.cpp'),
            "int parity_source = 2;`n", $utf8)
        $editedSourceState = Get-NetHarnessRepositorySourceState `
            -RepoRoot $sourceStateRoot
        if ([string]$editedSourceState.sha256 -ceq
                [string]$baselineSourceState.sha256) {
            throw 'repository source state did not reject an unstaged source edit'
        }

        [IO.File]::WriteAllText((Join-Path $sourceStateRoot 'runtime.cpp'),
            "int parity_source = 1;`n", $utf8)
        $includeDirectory = Join-Path $sourceStateRoot 'include'
        New-Item -ItemType Directory -Path $includeDirectory | Out-Null
        [IO.File]::WriteAllText((Join-Path $includeDirectory 'new_wire.h'),
            "#pragma once`n", $utf8)
        $untrackedSourceState = Get-NetHarnessRepositorySourceState `
            -RepoRoot $sourceStateRoot
        if ([string]$untrackedSourceState.sha256 -ceq
                [string]$baselineSourceState.sha256) {
            throw 'repository source state omitted a nonignored untracked source file'
        }
        & git -C $sourceStateRoot add -- include/new_wire.h
        if ($LASTEXITCODE -ne 0) { throw 'source-state staging self-test failed' }
        $stagedSourceState = Get-NetHarnessRepositorySourceState `
            -RepoRoot $sourceStateRoot
        if ([string]$stagedSourceState.sha256 -cne
                [string]$untrackedSourceState.sha256) {
            throw 'repository source state changed when identical bytes were staged'
        }
    }
    finally {
        $resolvedSourceStateRoot = [IO.Path]::GetFullPath($sourceStateRoot)
        $resolvedTemp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
        if (-not $resolvedSourceStateRoot.StartsWith(
                $resolvedTemp + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw 'source-state self-test cleanup escaped the temp directory'
        }
        Remove-Item -LiteralPath $resolvedSourceStateRoot -Recurse -Force
    }
    $script:Failures.Clear()
    $null = Test-RepositorySourceStateBinding `
        $baselineSourceState $baselineSourceState
    if ($script:Failures.Count -ne 0) {
        throw 'repository source-state binding rejected identical source bytes'
    }
    $script:Failures.Clear()
    $null = Test-RepositorySourceStateBinding `
        $baselineSourceState $editedSourceState
    if ($script:Failures.Count -ne 1 -or
            $script:Failures[0].code -ne 'REPOSITORY_SOURCE_STATE_MISMATCH') {
        throw 'repository source-state binding did not fail closed on edited source bytes'
    }
    $script:Failures.Clear()
    $null = Test-RepositorySourceStateBinding $null $baselineSourceState
    if ($script:Failures.Count -ne 1 -or
            $script:Failures[0].code -ne 'REPOSITORY_SOURCE_STATE_INVALID') {
        throw 'repository source-state binding did not reject a missing declaration'
    }
    $script:Failures.Clear()

    $canonicalTemplatePath = [IO.Path]::GetFullPath((Join-Path `
        (Get-RepoRoot) 'scripts\net\parity-matrix.example.json'))
    $canonicalArtifacts = @{manifest_template=$canonicalTemplatePath}
    $canonicalFixture = Get-Content -LiteralPath $canonicalTemplatePath -Raw |
        ConvertFrom-Json
    $script:Failures.Clear()
    if (-not (Test-CanonicalMatrixPolicy $canonicalFixture $canonicalArtifacts) -or
            $script:Failures.Count -ne 0) {
        throw "canonical matrix positive self-test failed: $($script:Failures.code -join ',')"
    }
    $wrongEnvelopeCanonical = ($canonicalFixture | ConvertTo-Json -Depth 40) |
        ConvertFrom-Json
    $wrongEnvelopeCanonical.schema = '1'
    $wrongEnvelopeCanonical.corpus.expansion = 'revx03'
    $script:Failures.Clear()
    $null = Test-CanonicalMatrixPolicy $wrongEnvelopeCanonical $canonicalArtifacts
    foreach ($requiredCode in @(
        'CANONICAL_SCHEMA_POLICY','CANONICAL_EXPANSION_POLICY'
    )) {
        if (@($script:Failures | Where-Object {
                $_.code -eq $requiredCode
            }).Count -ne 1) {
            throw "noncanonical manifest envelope did not produce $requiredCode"
        }
    }
    $wrongSourceContract = ($canonicalFixture | ConvertTo-Json -Depth 40) |
        ConvertFrom-Json
    $wrongSourceContract.corpus.source_state.mode = 'git-index-only'
    $script:Failures.Clear()
    $null = Test-CanonicalMatrixPolicy $wrongSourceContract $canonicalArtifacts
    if (@($script:Failures | Where-Object {
            $_.code -eq 'CANONICAL_SOURCE_STATE_POLICY'
        }).Count -ne 1) {
        throw 'noncanonical repository source-state contract was not rejected'
    }
    $weakenedCanonical = ($canonicalFixture | ConvertTo-Json -Depth 40) |
        ConvertFrom-Json
    $weakenedCanonical.required_case_ids = @(
        $weakenedCanonical.required_case_ids | Select-Object -First 3)
    $weakenedCanonical.cases = @($weakenedCanonical.cases | Select-Object -First 3)
    $weakenedCanonical.minimum_distinct_missions = 2
    $weakenedCanonical.minimum_distinct_game_types = 2
    $weakenedCanonical.minimum_distinct_ports = 2
    $script:Failures.Clear()
    $null = Test-CanonicalMatrixPolicy $weakenedCanonical $canonicalArtifacts
    $canonicalCodes = @($script:Failures | ForEach-Object { $_.code } |
        Sort-Object -Unique)
    foreach ($requiredCode in @(
        'CANONICAL_CASE_SET','CANONICAL_MATRIX_POLICY','GAME_TYPE_BRANCH_COVERAGE'
    )) {
        if ($canonicalCodes -notcontains $requiredCode) {
            throw "self-weakened canonical matrix did not produce $requiredCode"
        }
    }
    $shortWindowCanonical = ($canonicalFixture | ConvertTo-Json -Depth 40) |
        ConvertFrom-Json
    foreach ($topology in @('RR','RO','OR','OO')) {
        $shortWindowCanonical.cases[0].runs.$topology.steady_seconds = 5
    }
    $script:Failures.Clear()
    $null = Test-CanonicalMatrixPolicy $shortWindowCanonical $canonicalArtifacts
    if (@($script:Failures | Where-Object {
            $_.code -eq 'CANONICAL_STEADY_WINDOW'
        }).Count -ne 4) {
        throw 'uniformly shortened canonical windows were not rejected for every topology'
    }
    $extraTopologyCanonical = ($canonicalFixture | ConvertTo-Json -Depth 40) |
        ConvertFrom-Json
    $extraTopologyCanonical.cases[0].runs | Add-Member `
        -NotePropertyName 'RX' `
        -NotePropertyValue ([pscustomobject]@{steady_seconds=35})
    $script:Failures.Clear()
    $null = Test-CanonicalMatrixPolicy $extraTopologyCanonical $canonicalArtifacts
    if (@($script:Failures | Where-Object {
            $_.code -eq 'CANONICAL_TOPOLOGY_SET'
        }).Count -ne 1) {
        throw 'extra canonical run topology was not rejected'
    }
    $weakenedComparison = ($canonicalFixture | ConvertTo-Json -Depth 40) |
        ConvertFrom-Json
    $weakenedComparison.comparison.count_absolute_tolerance = 2
    $weakenedComparison.comparison.dynamic_min_samples = 2
    $weakenedComparison.comparison.gameplay_distribution_min_samples = 2
    $script:Failures.Clear()
    $null = Test-CanonicalMatrixPolicy $weakenedComparison $canonicalArtifacts
    if (@($script:Failures | Where-Object {
            $_.code -eq 'CANONICAL_COMPARISON_POLICY'
        }).Count -ne 3) {
        throw 'weakened canonical comparison thresholds were not rejected'
    }
    $exceptionCanonical = ($canonicalFixture | ConvertTo-Json -Depth 40) |
        ConvertFrom-Json
    $exceptionCanonical.cases[0].exceptions = @([pscustomobject]@{
        topology='RO';direction='C';kind='body';tag='2f';reason='fixture'
    })
    $script:Failures.Clear()
    $null = Test-CanonicalMatrixPolicy $exceptionCanonical $canonicalArtifacts
    if (@($script:Failures | Where-Object {
            $_.code -eq 'PARITY_EXCEPTIONS_FORBIDDEN'
        }).Count -ne 1) {
        throw 'verdict parity-exception rejection self-test failed'
    }
    $runtimeProbeRoot = Join-Path (Get-ScratchDir) `
        ("godot-runtime-resolver-" + [Guid]::NewGuid().ToString('N'))
    try {
        $null = [IO.Directory]::CreateDirectory($runtimeProbeRoot)
        $consoleProbe = Join-Path $runtimeProbeRoot 'Godot_Test_console.exe'
        $runtimeProbe = Join-Path $runtimeProbeRoot 'Godot_Test.exe'
        [IO.File]::WriteAllBytes($consoleProbe, [byte[]](0x43))
        [IO.File]::WriteAllBytes($runtimeProbe, [byte[]](0x52))
        $resolvedRuntime = Resolve-GodotRuntimeBinary -Path $consoleProbe
        if ($resolvedRuntime -cne (Resolve-Path -LiteralPath $runtimeProbe).Path -or
                (Resolve-GodotRuntimeBinary -Path $runtimeProbe) -cne
                    (Resolve-Path -LiteralPath $runtimeProbe).Path) {
            throw 'Godot console-wrapper runtime resolution self-test failed'
        }
        $orphanProbe = Join-Path $runtimeProbeRoot 'Godot_Orphan_console.exe'
        [IO.File]::WriteAllBytes($orphanProbe, [byte[]](0x4f))
        $orphanRejected = $false
        try {
            $null = Resolve-GodotRuntimeBinary -Path $orphanProbe
        }
        catch {
            $orphanRejected = $_.Exception.Message -like
                'Godot console wrapper has no sibling runtime executable:*'
        }
        if (-not $orphanRejected) {
            throw 'orphan Godot console-wrapper self-test did not fail closed'
        }
    }
    finally {
        if (Test-Path -LiteralPath $runtimeProbeRoot) {
            Remove-Item -LiteralPath $runtimeProbeRoot -Recurse -Force
        }
    }
    $lines = @(
        'PARITY_HANDSHAKE frame=8 ts_ns=999999800 dir=C session=32768 opcode=0x41 decode=1 len=17 body=434900040011223344504e0003004a4f00',
        'PARITY_HANDSHAKE frame=9 ts_ns=999999900 dir=C session=32768 opcode=0x42 decode=1 len=21 body=434b00040078563412435500070001580002005900',
        'PARITY_PACKET frame=10 ts_ns=1000000000 dir=S session=32768 sid=0x00000001 seq=7 ack=6 flags=0x00 records=4 tags=0x039,0x042,0x043,0x068 wire=0x039:0x20:4:-,0x042:0x20:2:-,0x043:0x20:4:-,0x068:0x20:4:-',
        'PARITY_EVENT frame=10 ts_ns=1000000000 dir=S session=32768 tag=0x39 settings=0 len=4 body=01000000',
        'PARITY_EVENT frame=10 ts_ns=1000000000 dir=S session=32768 tag=0x42 settings=0 len=2 body=0000',
        'PARITY_EVENT frame=10 ts_ns=1000000000 dir=S session=32768 tag=0x43 settings=0 len=4 body=01000000',
        'PARITY_EVENT frame=10 ts_ns=1000000000 dir=S session=32768 tag=0x68 settings=0 len=4 body=32000000',
        'PARITY_PACKET frame=11 ts_ns=1000000100 dir=C session=32768 sid=0x00000002 seq=8 ack=7 flags=0x00 records=4 tags=0x01c,0x008,0x03d,0x00c wire=0x01c:0x20:4:-,0x008:0x20:8:-,0x03d:0x20:4:-,0x00c:0x20:48:-',
        'PARITY_EVENT frame=11 ts_ns=1000000100 dir=C session=32768 tag=0x1c settings=0 len=4 body=79563412',
        'PARITY_EVENT frame=11 ts_ns=1000000100 dir=C session=32768 tag=0x08 settings=0 len=8 body=01000000e8030000',
        'PARITY_EVENT frame=11 ts_ns=1000000100 dir=C session=32768 tag=0x3d settings=0 len=4 body=32000000',
        'PARITY_STATE frame=11 ts_ns=1000000100 dir=C session=32768 tag=0x0c kind=uplink decode=1 len=48 handle=7 type=5305 sub=10 carrier=65535 pos=1,2,3 orient=4,5 move=0 state=0 analog=0,0,0 adm=16 priority_nonzero=4 priority=60000:9,65535:0,22:7,60001:1',
        'PARITY_PACKET frame=12 ts_ns=1000000200 dir=S session=32768 sid=0x00000001 seq=8 ack=8 flags=0x00 records=1 tags=0x00a wire=0x00a:0x20:64:-',
        'PARITY_STATE frame=12 ts_ns=1000000200 dir=S session=32768 tag=0x0a kind=frame decode=1 len=64 sub=0 flags=0,4 anchor=1,2,3 local=1 local_state=0,150,0 records=1 players=0 vehicles=1 infantry=0 none=0 rounds=0 local_mount=65535 weapon=1,2,3,4,5,6,7,8,-9 timer=1,10,11,12,13,-14 env=1,20,21,22,23,24,25,26,27 objective=1,-1,2,-3,4 mount=1,65535,0,0,0',
        'PARITY_ENTITY frame=12 ts_ns=1000000200 session=32768 class=V handle=8 type=1291 pos=10,20,30 orient=40,50 state=0,0 vital=100'
    )
    $parsed = ConvertFrom-ParityLines $lines
    if ($parsed.handshakes.Count -ne 2 -or
            $parsed.handshakes[0].fields.Count -ne 2 -or
            $parsed.handshakes[0].fields[0].name -cne 'CI' -or
            $parsed.handshakes[0].fields[0].kind -cne 'u32' -or
            $parsed.handshakes[1].fields[1].kind -cne 'cu' -or
            $parsed.handshakes[1].fields[1].cu_type -ne 1 -or
            $parsed.handshakes[1].fields[1].cu_name -cne 'X' -or
            $parsed.handshakes[1].fields[1].cu_value_length -ne 2 -or
            $parsed.packets.Count -ne 3 -or $parsed.events.Count -ne 7 -or
            $parsed.packets[0].sid -ne 1 -or
            $parsed.packets[0].wire_records.Count -ne 4 -or
            $parsed.packets[0].wire_records[0].raw_flags -ne 0x20 -or
            $parsed.packets[0].wire_records[0].encoded_length -ne 4 -or
            $parsed.packets[0].wire_records[0].skip_hex -cne '-' -or
            $parsed.events[1].identity -ne '42:0' -or
            $parsed.packets[0].tag_identities[1] -ne '42:0' -or
            -not $parsed.histogram.ContainsKey('S:42:0') -or
            $parsed.events[0].connection_key -ne $parsed.events[4].connection_key -or
            $parsed.states[0].sid -ne 2 -or $parsed.entities[0].sid -ne 1 -or
            $parsed.states.Count -ne 2 -or $parsed.entities.Count -ne 1 -or
            $parsed.states[0].pos_z -ne 3 -or
            $parsed.states[0].priority_handle_0 -ne 60000 -or
            $parsed.states[0].priority_score_0 -ne 9 -or
            $parsed.states[0].priority_handle_1 -ne 65535 -or
            $parsed.states[0].priority_score_1 -ne 0 -or
            $parsed.states[0].priority_handle_2 -ne 22 -or
            $parsed.states[0].priority_score_2 -ne 7 -or
            $parsed.states[0].priority_handle_3 -ne 60001 -or
            $parsed.states[0].priority_score_3 -ne 1 -or
            $parsed.states[1].local_mount -ne 65535 -or
            $parsed.states[1].weapon_slot_state460 -ne 7 -or
            $parsed.states[1].weapon_uniform_team_mask -ne -9 -or
            $parsed.states[1].timer_seconds -ne -14 -or
            $parsed.states[1].env_param -ne 27 -or
            $parsed.states[1].objective_state_2 -ne -3 -or
            $parsed.states[1].passenger_present -ne $true -or
            $parsed.states[1].passenger_mount_handle -ne 65535 -or
            $parsed.entities[0].class -ne 'V' -or
            (Read-U32Le $parsed.events[3].body 0) -ne 50 -or
            (Read-U32Le $parsed.events[5].body 4) -ne 1000) {
        throw "parity-event parser self-test failed"
    }

    foreach ($malformedPacket in @(
        # The pre-fingerprint contract must not be accepted as current evidence.
        'PARITY_PACKET frame=1 ts_ns=1 dir=S session=32768 sid=0x00000001 seq=1 ack=0 flags=0x00 records=1 tags=0x00a',
        # Tag list and physical full-tag identity must agree positionally.
        'PARITY_PACKET frame=1 ts_ns=1 dir=S session=32768 sid=0x00000001 seq=1 ack=0 flags=0x00 records=1 tags=0x00a wire=0x00b:0x20:3:-',
        # SKIP2 needs exactly two captured bytes.
        'PARITY_PACKET frame=1 ts_ns=1 dir=S session=32768 sid=0x00000001 seq=1 ack=0 flags=0x00 records=1 tags=0x00a wire=0x00a:0x30:3:-',
        # A no-length physical record cannot declare payload bytes.
        'PARITY_PACKET frame=1 ts_ns=1 dir=S session=32768 sid=0x00000001 seq=1 ack=0 flags=0x00 records=1 tags=0x00a wire=0x00a:0x00:3:-'
    )) {
        $rejected = $false
        try { $null = ConvertFrom-ParityLines @($malformedPacket) }
        catch { $rejected = $true }
        if (-not $rejected) {
            throw 'malformed or legacy physical-record packet did not fail closed'
        }
    }

    $wireGold = ConvertFrom-ParityLines @(
        'PARITY_PACKET frame=1 ts_ns=1 dir=S session=32768 sid=0x00000001 seq=1 ack=0 flags=0x00 records=1 tags=0x00a wire=0x00a:0x34:3:aabb'
    )
    $wireCase = [pscustomobject]@{id='wire-fingerprint-self-test'}
    $wireVariants = @(
        # Same body size and fragment/SKIP semantics, different LEN encoding.
        'PARITY_PACKET frame=1 ts_ns=1 dir=S session=32768 sid=0x00000001 seq=1 ack=0 flags=0x00 records=1 tags=0x00a wire=0x00a:0x54:3:aabb',
        # Same flags and skip bytes, different encoded fragment length.
        'PARITY_PACKET frame=1 ts_ns=1 dir=S session=32768 sid=0x00000001 seq=1 ack=0 flags=0x00 records=1 tags=0x00a wire=0x00a:0x34:2:aabb',
        # Same tag/flags/length, different discarded-on-receive SKIP bytes.
        'PARITY_PACKET frame=1 ts_ns=1 dir=S session=32768 sid=0x00000001 seq=1 ack=0 flags=0x00 records=1 tags=0x00a wire=0x00a:0x34:3:aabc',
        # Same tag/LEN/SKIP/body length, missing FRAG_CONT.
        'PARITY_PACKET frame=1 ts_ns=1 dir=S session=32768 sid=0x00000001 seq=1 ack=0 flags=0x00 records=1 tags=0x00a wire=0x00a:0x30:3:aabb'
    )
    foreach ($wireVariant in $wireVariants) {
        $wireSubject = ConvertFrom-ParityLines @($wireVariant)
        $script:Failures.Clear()
        Compare-InitialPacketGrouping $wireCase 'RO' 'S' $wireGold $wireSubject
        if (@($script:Failures | Where-Object {
                    $_.code -eq 'INITIAL_PACKET_GROUPING'
                }).Count -ne 1) {
            throw 'physical LEN/length/SKIP/fragment mutation escaped packet parity'
        }
    }

    $handshake41Subject = $parsed.handshakes[0] | Select-Object *
    $handshake41Subject.fields = @($parsed.handshakes[0].fields | ForEach-Object {
        $_ | Select-Object *
    })
    $handshake42Subject = $parsed.handshakes[1] | Select-Object *
    $handshake42Subject.fields = @($parsed.handshakes[1].fields | ForEach-Object {
        $_ | Select-Object *
    })
    # CI/CK are session-generated: equal shape with different bytes must
    # normalize, while stable field order and nested CU semantics remain exact.
    $handshake41Subject.fields[0].value = [byte[]](0xde,0xad,0xbe,0xef)
    $handshake42Subject.fields[0].value = [byte[]](0x12,0x34,0x56,0x78)
    $handshakeGold = [pscustomobject]@{handshakes=@($parsed.handshakes[0],$parsed.handshakes[1])}
    $handshakeSubject = [pscustomobject]@{handshakes=@($handshake41Subject,$handshake42Subject)}
    $handshakeCase = [pscustomobject]@{id='handshake-self-test'}
    $script:Failures.Clear()
    Compare-OuterHandshakeDirection $handshakeCase 'RO' 'C' $handshakeGold $handshakeSubject
    if (@($script:Failures | Where-Object { $_.code -eq 'HANDSHAKE_MISMATCH' }).Count -ne 0) {
        throw 'outer handshake documented-dynamic normalization self-test failed'
    }
    $handshake41Subject.fields = @($handshake41Subject.fields[1],
        $handshake41Subject.fields[0])
    $script:Failures.Clear()
    Compare-OuterHandshakeDirection $handshakeCase 'RO' 'C' $handshakeGold $handshakeSubject
    if (@($script:Failures | Where-Object { $_.code -eq 'HANDSHAKE_MISMATCH' }).Count -ne 1) {
        throw 'outer handshake field-order negative self-test did not fail closed'
    }
    $handshake41Subject.fields = @($handshake41Subject.fields[1],
        $handshake41Subject.fields[0])
    $handshake42Subject.fields[1].cu_value = [byte[]](0x5a,0)
    $script:Failures.Clear()
    Compare-OuterHandshakeDirection $handshakeCase 'RO' 'C' $handshakeGold $handshakeSubject
    if (@($script:Failures | Where-Object { $_.code -eq 'HANDSHAKE_MISMATCH' }).Count -ne 1) {
        throw "outer handshake CU-value negative self-test did not fail closed: failures=$($script:Failures.code -join ',') gold=$(Get-NormalizedHandshakeKey $parsed.handshakes[1]) subject=$(Get-NormalizedHandshakeKey $handshake42Subject)"
    }
    $handshake42Subject.fields[1].cu_value = [byte[]](0x59,0)

    foreach ($malformedHandshake in @(
        'PARITY_HANDSHAKE frame=1 ts_ns=1 dir=C session=32768 opcode=0x41 decode=1 len=8 body=4349000300010203',
        'PARITY_HANDSHAKE frame=1 ts_ns=1 dir=C session=32768 opcode=0x42 decode=1 len=12 body=435500070001580003005900'
    )) {
        $rejected = $false
        try { $null = ConvertFrom-ParityLines @($malformedHandshake) }
        catch { $rejected = $true }
        if (-not $rejected) {
            throw 'malformed outer handshake field/CU length self-test did not fail closed'
        }
    }

    $retryExact = $parsed.handshakes[0] | Select-Object *
    $retryExact.frame = 9
    $retryExact.fields = @($parsed.handshakes[0].fields | ForEach-Object {
        $_ | Select-Object *
    })
    $retryFacts = [pscustomobject]@{
        handshakes=@($parsed.handshakes[0],$retryExact);packets=@()
    }
    $script:Failures.Clear()
    $null = Test-OuterHandshakeContract $retryFacts $handshakeCase 'RR'
    if (@($script:Failures | Where-Object {
                $_.code -eq 'HANDSHAKE_RETRY_MISMATCH'
            }).Count -ne 0) {
        throw 'exact outer handshake retransmission self-test was rejected'
    }
    $retryExact.fields[0].value = [byte[]](0xde,0xad,0xbe,0xef)
    $script:Failures.Clear()
    $null = Test-OuterHandshakeContract $retryFacts $handshakeCase 'RR'
    if (@($script:Failures | Where-Object {
                $_.code -eq 'HANDSHAKE_RETRY_MISMATCH'
            }).Count -ne 1) {
        throw 'dynamic-field handshake retry mutation did not fail closed'
    }
    $retryExact.fields[0].value = [byte[]](0x11,0x22,0x33,0x44)
    $retryExact.decode = $false
    $script:Failures.Clear()
    $null = Test-OuterHandshakeContract $retryFacts $handshakeCase 'RR'
    if (@($script:Failures | Where-Object {
                $_.code -eq 'HANDSHAKE_DECODE_FAILED'
            }).Count -ne 1) {
        throw 'undecoded outer handshake retry did not fail closed'
    }
    $retryExact.decode = $true
    $retryExact.fields[1].value = [byte[]](0x58,0x4f,0)
    $script:Failures.Clear()
    $null = Test-OuterHandshakeContract $retryFacts $handshakeCase 'RR'
    if (@($script:Failures | Where-Object {
                $_.code -eq 'HANDSHAKE_RETRY_MISMATCH'
            }).Count -ne 1) {
        throw 'mutated outer handshake retransmission self-test did not fail closed'
    }

    $newU32HandshakeField = {
        param([string]$Name, [uint32]$Value)
        [byte[]]$bytes = @(
            [byte]($Value -band 0xff), [byte](($Value -shr 8) -band 0xff),
            [byte](($Value -shr 16) -band 0xff), [byte](($Value -shr 24) -band 0xff)
        )
        return [pscustomobject]@{
            name=$Name;kind='u32';length=4;value=$bytes
            cu_type=$null;cu_name=$null;cu_value_length=$null;cu_value=[byte[]]@()
            cs_direction=$null;cs_index=$null;cs_value=$null
        }
    }
    $newStringHandshakeField = {
        param([string]$Name, [byte[]]$Value)
        return [pscustomobject]@{
            name=$Name;kind='string';length=$Value.Length;value=$Value
            cu_type=$null;cu_name=$null;cu_value_length=$null;cu_value=[byte[]]@()
            cs_direction=$null;cs_index=$null;cs_value=$null
        }
    }
    $echo41 = [pscustomobject]@{opcode=0x41;decode=$true;frame=1;session=32768;fields=@(
        (& $newU32HandshakeField 'CI' 1))}
    $echo81 = [pscustomobject]@{opcode=0x81;decode=$true;frame=2;session=32768;fields=@(
        (& $newU32HandshakeField 'CI' 1),(& $newU32HandshakeField 'HK' 2),
        (& $newU32HandshakeField 'RIP' 0x7f000001),
        (& $newU32HandshakeField 'RPN' 32768))}
    $echo42 = [pscustomobject]@{opcode=0x42;decode=$true;frame=3;session=32768;fields=@(
        (& $newU32HandshakeField 'CI' 1),(& $newU32HandshakeField 'HK' 2),
        (& $newU32HandshakeField 'CK' 3),
        (& $newStringHandshakeField 'NA' ([byte[]](0x4e,0))))}
    $echo82 = [pscustomobject]@{opcode=0x82;decode=$true;frame=4;session=32768;fields=@(
        (& $newU32HandshakeField 'CI' 1),(& $newU32HandshakeField 'CK' 3),
        (& $newU32HandshakeField 'SK' 4),
        (& $newStringHandshakeField 'NA' ([byte[]](0x4e,0))),
        (& $newU32HandshakeField 'RIP' 0x7f000001),
        (& $newU32HandshakeField 'RPN' 32768),
        [pscustomobject]@{name='CS';kind='cs';length=6;value=[byte[]](1,0,0,0,0,0);
            cs_direction=1;cs_index=0;cs_value=0;cu_type=$null;cu_name=$null;
            cu_value_length=$null;cu_value=[byte[]]@()})}
    $echoFacts = [pscustomobject]@{
        handshakes=@($echo41,$echo81,$echo42,$echo82)
        packets=@(
            [pscustomobject]@{dir='C';session=32768;sid=[uint32]4},
            [pscustomobject]@{dir='S';session=32768;sid=[uint32]3}
        )
        events=@([pscustomobject]@{identity='0a:0';session=32768})
    }
    $script:Failures.Clear()
    $null = Test-OuterHandshakeContract $echoFacts $handshakeCase 'OO'
    if (@($script:Failures | Where-Object {
                $_.code -in @('HANDSHAKE_ECHO_MISMATCH','HANDSHAKE_SESSION_KEY_ECHO')
            }).Count -ne 0) {
        throw 'outer handshake echo positive self-test failed'
    }
    $echo81.fields[0].value = [byte[]](9,0,0,0)
    $script:Failures.Clear()
    $null = Test-OuterHandshakeContract $echoFacts $handshakeCase 'OO'
    if (@($script:Failures | Where-Object {
                $_.code -eq 'HANDSHAKE_ECHO_MISMATCH'
            }).Count -ne 1) {
        throw 'outer handshake echo negative self-test did not fail closed'
    }
    $echo81.fields[0].value = [byte[]](1,0,0,0)
    $echo82.fields[4].value = [byte[]](2,0,0,127)
    $script:Failures.Clear()
    $null = Test-OuterHandshakeContract $echoFacts $handshakeCase 'OO'
    if (@($script:Failures | Where-Object {
                $_.code -eq 'HANDSHAKE_ECHO_MISMATCH'
            }).Count -ne 1) {
        throw 'outer RIP/RPN echo mutation did not fail closed'
    }
    $echo82.fields[4].value = [byte[]](1,0,0,127)
    $echo82.session = 32769
    $script:Failures.Clear()
    $null = Test-OuterHandshakeContract $echoFacts $handshakeCase 'OO'
    if (@($script:Failures | Where-Object {
                $_.code -eq 'HANDSHAKE_SESSION_BINDING'
            }).Count -ne 1) {
        throw 'cross-session outer handshake splice did not fail closed'
    }
    $echo82.session = 32768
    $echoFacts.events[0].session = 32769
    $script:Failures.Clear()
    $null = Test-OuterHandshakeContract $echoFacts $handshakeCase 'OO'
    if (@($script:Failures | Where-Object {
                $_.code -eq 'HANDSHAKE_SESSION_BINDING'
            }).Count -ne 1) {
        throw 'outer handshake/gameplay session mismatch did not fail closed'
    }
    $echoFacts.events[0].session = 32768
    $probe41 = $echo41 | Select-Object *
    $probe41.session = 40000
    $probe81 = $echo81 | Select-Object *
    $probe81.session = 40000
    $echoFacts.handshakes = @($probe41,$probe81,$echo41,$echo81,$echo42,$echo82)
    $script:Failures.Clear()
    $null = Test-OuterHandshakeContract $echoFacts $handshakeCase 'OO'
    if (@($script:Failures | Where-Object {
                $_.code -in @('HANDSHAKE_SESSION_AMBIGUOUS','HANDSHAKE_SESSION_BINDING')
            }).Count -ne 0) {
        throw 'unrelated pre-auth probe session contaminated the gameplay handshake binding'
    }
    $foreignComplete = @($echo41,$echo81,$echo42,$echo82 | ForEach-Object {
        $copy = $_ | Select-Object *
        $copy.session = 40001
        $copy
    })
    $echoFacts.handshakes = @($echo41,$echo81,$echo42,$echo82) + $foreignComplete
    $script:Failures.Clear()
    $null = Test-OuterHandshakeContract $echoFacts $handshakeCase 'OO'
    if (@($script:Failures | Where-Object {
                $_.code -eq 'HANDSHAKE_SESSION_BINDING'
            }).Count -ne 1) {
        throw 'foreign complete auth session was misclassified as a readiness probe'
    }
    $echoFacts.handshakes = @($echo41,$echo81,$echo42,$echo82)

    # An external capture can contain an earlier 0x41/0x81 readiness probe on a
    # different client port. Outer parity must compare the gameplay-bound
    # session in both mixed-peer directions, not whichever opcode appeared
    # first in the capture.
    $sessionBoundGold = ($echoFacts | ConvertTo-Json -Depth 40) | ConvertFrom-Json
    foreach ($handshake in @($sessionBoundGold.handshakes)) {
        $handshake.frame = [int]$handshake.frame + 10
    }
    $newSessionBoundSubject = {
        $subject = ($sessionBoundGold | ConvertTo-Json -Depth 40) | ConvertFrom-Json
        $probe41Copy = @($sessionBoundGold.handshakes | Where-Object {
            [byte]$_.opcode -eq 0x41
        })[0] | Select-Object *
        $probe41Copy.session = 40000
        $probe41Copy.frame = 1
        $probe81Copy = @($sessionBoundGold.handshakes | Where-Object {
            [byte]$_.opcode -eq 0x81
        })[0] | Select-Object *
        $probe81Copy.session = 40000
        $probe81Copy.frame = 2
        $subject.handshakes = @($probe41Copy,$probe81Copy) + @($subject.handshakes)
        return $subject
    }
    $roPeerSubject = & $newSessionBoundSubject
    $roServerHandshake = @($roPeerSubject.handshakes | Where-Object {
        [byte]$_.opcode -eq 0x81 -and [int]$_.session -eq 32768
    })[0]
    $roServerHandshake.fields = @($roServerHandshake.fields) + @(
        (& $newStringHandshakeField 'CO' ([byte[]](0x52,0))))
    $script:Failures.Clear()
    Compare-OuterHandshakeParity $handshakeCase 'RO' `
        $sessionBoundGold $roPeerSubject
    if (@($script:Failures | Where-Object {
                $_.code -eq 'HANDSHAKE_MISMATCH'
            }).Count -ne 1) {
        throw 'RO retail-host handshake half was not compared on the gameplay session'
    }

    $orPeerSubject = & $newSessionBoundSubject
    $orClientHandshake = @($orPeerSubject.handshakes | Where-Object {
        [byte]$_.opcode -eq 0x41 -and [int]$_.session -eq 32768
    })[0]
    $orClientHandshake.fields = @($orClientHandshake.fields) + @(
        (& $newStringHandshakeField 'CO' ([byte[]](0x4f,0))))
    $script:Failures.Clear()
    Compare-OuterHandshakeParity $handshakeCase 'OR' `
        $sessionBoundGold $orPeerSubject
    if (@($script:Failures | Where-Object {
                $_.code -eq 'HANDSHAKE_MISMATCH'
            }).Count -ne 1) {
        throw 'OR retail-client handshake half was not compared on the gameplay session'
    }

    $phaseStates = @()
    $phasePackets = @()
    for ($phaseIndex = 0; $phaseIndex -lt 300; $phaseIndex++) {
        [uint32]$phaseValue = [uint32](($phaseIndex + 1) -band 0xff)
        [uint32]$subBlock = $phaseValue -band 3
        $frame = 1000 + $phaseIndex
        $phaseStates += [pscustomobject]@{
            frame=$frame;ts_ns=[uint64](1000000000 + ($phaseIndex * 16000000))
            dir='S';session=32768;sid=[uint32]3
            connection_key='32768|0|S00000003|C00000004'
            kind='frame';decode=$true;flags2=$phaseValue;sub_block=$subBlock
            weapon_present=$subBlock -eq 0;timer_present=$subBlock -eq 1
            env_present=$subBlock -eq 2;objective_present=$subBlock -eq 3
            passenger_present=($phaseValue -band 0x0f) -eq 8
            passenger_mount_handle=[uint32]0xffff;passenger_has_mount=$false
        }
        $phasePackets += [pscustomobject]@{
            frame=$frame;dir='S';session=32768;sid=[uint32]3
            connection_key='32768|0|S00000003|C00000004'
            sequence=[uint32]($phaseIndex + 1);tags=@('0a')
            tag_identities=@('0a:0');records=1
        }
    }
    $phaseFacts = [pscustomobject]@{
        packets=$phasePackets;states=$phaseStates;steady_states=$phaseStates
    }
    $phaseCase = [pscustomobject]@{id='phase-stream-self-test';game_type=[uint32]0x30020}
    $script:Failures.Clear()
    Test-FramePhaseStream $phaseFacts $phaseCase 'RR'
    if ($script:Failures.Count -ne 0) {
        throw "flags2 phase-stream positive self-test failed: $($script:Failures.code -join ',')"
    }
    # Swap two complete adjacent phase records: coverage and association stay
    # valid, so only the ordered modulo-256 contract can catch the reordering.
    $phaseSwapFields = @(
        'flags2','sub_block','weapon_present','timer_present','env_present',
        'objective_present','passenger_present'
    )
    foreach ($field in $phaseSwapFields) {
        $saved = Get-Field $phaseStates[100] $field $null
        $phaseStates[100].$field = Get-Field $phaseStates[101] $field $null
        $phaseStates[101].$field = $saved
    }
    $script:Failures.Clear()
    Test-FramePhaseStream $phaseFacts $phaseCase 'RR'
    if (@($script:Failures | Where-Object {
                $_.code -eq 'FRAME_PHASE_SEQUENCE'
            }).Count -ne 1) {
        throw 'flags2 direct reordering did not fail closed'
    }
    foreach ($field in $phaseSwapFields) {
        $saved = Get-Field $phaseStates[100] $field $null
        $phaseStates[100].$field = Get-Field $phaseStates[101] $field $null
        $phaseStates[101].$field = $saved
    }
    $timerPhase = @($phaseStates | Where-Object { $_.flags2 -eq 1 })[0]
    $timerPhase.timer_present = $false
    $script:Failures.Clear()
    Test-FramePhaseStream $phaseFacts $phaseCase 'RR'
    if (@($script:Failures | Where-Object {
                $_.code -eq 'FRAME_PHASE_ASSOCIATION'
            }).Count -ne 1) {
        throw 'flags2 phase-association mutation did not fail closed'
    }
    $timerPhase.timer_present = $true
    $shortPhaseFacts = [pscustomobject]@{
        packets=$phasePackets;states=$phaseStates;steady_states=@($phaseStates[0..99])
    }
    $script:Failures.Clear()
    Test-FramePhaseStream $shortPhaseFacts $phaseCase 'RR'
    if (@($script:Failures | Where-Object {
                $_.code -eq 'FRAME_PHASE_COVERAGE'
            }).Count -ne 1) {
        throw 'incomplete flags2 coverage did not fail closed'
    }
    $phaseStates[0].flags2 = 2
    $script:Failures.Clear()
    Test-FramePhaseStream $phaseFacts $phaseCase 'RR'
    if (@($script:Failures | Where-Object {
                $_.code -eq 'FRAME_PHASE_START'
            }).Count -ne 1) {
        throw 'non-preincrement flags2 start did not fail closed'
    }
    $phaseStates[0].flags2 = 1

    $ackPackets = @()
    $ackFrame = 2000
    for ($ackSequence = 1; $ackSequence -le 12; $ackSequence++) {
        $ackPackets += [pscustomobject]@{
            frame=$ackFrame++;dir='S';session=32768;sid=[uint32]3
            connection_key='32768|0|S00000003|C00000004'
            sequence=[uint32]$ackSequence
            ack=[uint32]$(if ($ackSequence -eq 1) { 0 } else { $ackSequence - 1 })
        }
        $ackPackets += [pscustomobject]@{
            frame=$ackFrame++;dir='C';session=32768;sid=[uint32]4
            connection_key='32768|0|S00000003|C00000004'
            sequence=[uint32]$ackSequence;ack=[uint32]$ackSequence
        }
    }
    $ackFacts = [pscustomobject]@{packets=$ackPackets}
    $ackCase = [pscustomobject]@{id='ack-stream-self-test'}
    $script:Failures.Clear()
    Test-AckStream $ackFacts $ackCase 'RR'
    if ($script:Failures.Count -ne 0) {
        throw "ACK stream positive self-test failed: $($script:Failures.code -join ',')"
    }
    $savedAcks = @($ackPackets | ForEach-Object { [uint32]$_.ack })
    foreach ($packet in $ackPackets) { $packet.ack = [uint32]0 }
    $script:Failures.Clear()
    Test-AckStream $ackFacts $ackCase 'RR'
    if (@($script:Failures | Where-Object {
                $_.code -eq 'ACK_NO_PROGRESS'
            }).Count -ne 2) {
        throw 'absent ACK stream did not fail closed in both directions'
    }
    for ($i = 0; $i -lt $ackPackets.Count; $i++) {
        $ackPackets[$i].ack = $savedAcks[$i]
    }
    $ackPackets[9].ack = [uint32]99
    $script:Failures.Clear()
    Test-AckStream $ackFacts $ackCase 'RR'
    if (@($script:Failures | Where-Object {
                $_.code -eq 'ACK_UNKNOWN_SEQUENCE'
            }).Count -ne 1) {
        throw 'unknown ACK sequence did not fail closed'
    }
    $ackPackets[9].ack = $savedAcks[9]
    $ackPackets[13].ack = [uint32]5
    $script:Failures.Clear()
    Test-AckStream $ackFacts $ackCase 'RR'
    if (@($script:Failures | Where-Object {
                $_.code -eq 'ACK_REGRESSION'
            }).Count -ne 1) {
        throw 'regressing ACK stream did not fail closed'
    }
    $ackPackets[13].ack = $savedAcks[13]

    $mountCoverageFixture = @{
        fixture = [pscustomobject]@{facts=@{
            RR = [pscustomobject]@{steady_states=@(
                [pscustomobject]@{
                    dir='S';kind='frame';decode=$true;passenger_present=$true
                    passenger_has_mount=$true;passenger_mount_handle=[uint32]7
                }
            )}
        }}
    }
    $mountCoverage = Get-OccupiedMountCoverageReport $mountCoverageFixture @('RR')
    if ($mountCoverage.status -cne 'structural-only' -or
            $mountCoverage.deterministic_live_control -ne $false -or
            $mountCoverage.phase8_samples -ne 1 -or
            $mountCoverage.observed_bound_samples -ne 1 -or
            $mountCoverage.structural_test -cne 'netsim_two_peer_fanout') {
        throw 'occupied-mount coverage report made a false live-control claim'
    }

    # Retail can batch multiple ordinary records with the same tag into one
    # datagram. PARITY_STATE/PARITY_ENTITY lines share only the outer packet
    # identity, so correlation is cardinality- and aggregate-based at that
    # seam rather than pretending each record has a unique sub-identity.
    $batchedGameplayLines = @(
        'PARITY_PACKET frame=920 ts_ns=17000000000 dir=C session=32776 sid=0x00000002 seq=920 ack=0 flags=0x00 records=1 tags=0x00c wire=0x00c:0x20:48:-'
        'PARITY_EVENT frame=920 ts_ns=17000000000 dir=C session=32776 tag=0x0c settings=0 len=48 body=-'
        'PARITY_STATE frame=920 ts_ns=17000000000 dir=C session=32776 tag=0x0c kind=uplink decode=1 len=48 handle=136 type=5305 sub=10 carrier=65535 pos=1,2,3 orient=4,5 move=0 state=1 analog=0,0,0 adm=16 priority_nonzero=4 priority=1:1,2:1,3:1,4:1'
        'PARITY_PACKET frame=921 ts_ns=17000000100 dir=S session=32776 sid=0x00000001 seq=921 ack=0 flags=0x00 records=2 tags=0x00a,0x00a wire=0x00a:0x20:64:-,0x00a:0x20:64:-'
        'PARITY_EVENT frame=921 ts_ns=17000000100 dir=S session=32776 tag=0x0a settings=0 len=64 body=-'
        'PARITY_EVENT frame=921 ts_ns=17000000100 dir=S session=32776 tag=0x0a settings=0 len=64 body=-'
        'PARITY_STATE frame=921 ts_ns=17000000100 dir=S session=32776 tag=0x0a kind=frame decode=1 len=64 sub=0 flags=2,0 anchor=1,2,3 local=1 local_state=0,100,0 records=1 players=1 vehicles=0 infantry=0 none=0 rounds=0 local_mount=65535 weapon=0,0,0,0,0,0,0,0,0 timer=0,0,0,0,0,0 env=0,0,0,0,0,0,0,0,0 objective=0,0,0,0,0 mount=0,65535,0,0,0'
        'PARITY_STATE frame=921 ts_ns=17000000100 dir=S session=32776 tag=0x0a kind=frame decode=1 len=64 sub=0 flags=2,0 anchor=1,2,3 local=1 local_state=0,100,0 records=1 players=0 vehicles=1 infantry=0 none=0 rounds=0 local_mount=65535 weapon=0,0,0,0,0,0,0,0,0 timer=0,0,0,0,0,0 env=0,0,0,0,0,0,0,0,0 objective=0,0,0,0,0 mount=0,65535,0,0,0'
        'PARITY_ENTITY frame=921 ts_ns=17000000100 session=32776 class=P handle=136 type=5305 pos=10,20,30 orient=40,50 state=0,0 vital=100'
        'PARITY_ENTITY frame=921 ts_ns=17000000100 session=32776 class=V handle=137 type=1291 pos=11,21,31 orient=41,51 state=0,0 vital=100'
    )
    $batchedGameplay = ConvertFrom-ParityLines $batchedGameplayLines
    $script:Failures.Clear()
    if (-not (Test-SemanticContract $batchedGameplay 'batched-gameplay' 'RR') -or
            $script:Failures.Count -ne 0) {
        throw "batched-gameplay semantic self-test rejected valid multiplicities: $(@($script:Failures | ForEach-Object { $_.code }) -join ',')"
    }
    $missingBatchedState = ConvertFrom-ParityLines @($batchedGameplayLines | Where-Object {
        $_ -notmatch 'PARITY_STATE frame=921.*players=0 vehicles=1'
    })
    $script:Failures.Clear()
    $null = Test-SemanticContract $missingBatchedState 'batched-gameplay-negative' 'RR'
    if (@($script:Failures | Where-Object { $_.code -eq 'SEMANTIC_EVENT_CORRELATION' }).Count -ne 1 -or
            @($script:Failures | Where-Object { $_.code -eq 'SEMANTIC_EVENT_COUNT' }).Count -ne 1) {
        throw 'batched-gameplay missing-state self-test did not fail closed on count and owner correlation'
    }
    $script:Failures.Clear()

    $newSteadyFacts = {
        $specs = @(
            [pscustomobject]@{ frame=1; ts=[uint64]1000000000; dir='C'; identities=@('22:0') },
            [pscustomobject]@{ frame=2; ts=[uint64]2000000000; dir='S'; identities=@('08:0') },
            [pscustomobject]@{ frame=3; ts=[uint64]3000000000; dir='C'; identities=@('0c:0') },
            [pscustomobject]@{ frame=4; ts=[uint64]3100000000; dir='S'; identities=@('0a:0') },
            [pscustomobject]@{ frame=5; ts=[uint64]5000000000; dir='C'; identities=@('0c:0','47:0') },
            [pscustomobject]@{ frame=6; ts=[uint64]5100000000; dir='S'; identities=@('0a:0','75:0') },
            [pscustomobject]@{ frame=7; ts=[uint64]10500000000; dir='C'; identities=@('0c:0') },
            [pscustomobject]@{ frame=8; ts=[uint64]10600000000; dir='S'; identities=@('0a:0') },
            [pscustomobject]@{ frame=9; ts=[uint64]19500000000; dir='C'; identities=@('0c:0') },
            [pscustomobject]@{ frame=10; ts=[uint64]19600000000; dir='S'; identities=@('0a:0') },
            [pscustomobject]@{ frame=11; ts=[uint64]24000000000; dir='C'; identities=@('0c:0') },
            [pscustomobject]@{ frame=12; ts=[uint64]24100000000; dir='S'; identities=@('0a:0') }
        )
        $packets = @()
        $events = @()
        $states = @()
        $histogram = @{}
        foreach ($spec in $specs) {
            $tags = @($spec.identities | ForEach-Object { $_.Substring(0, 2) })
            $sid = if ($spec.dir -eq 'C') { [uint32]2 } else { [uint32]1 }
            $packets += [pscustomobject]@{
                frame=$spec.frame;ts_ns=$spec.ts;dir=$spec.dir;session=32768;sid=$sid
                connection_key='32768|0|S00000001|C00000002';sequence=[uint32]$spec.frame
                records=$spec.identities.Count;tags=$tags;tag_identities=$spec.identities
            }
            foreach ($identity in $spec.identities) {
                $key = "$($spec.dir):$identity"
                if (-not $histogram.ContainsKey($key)) { $histogram[$key] = 0 }
                $histogram[$key]++
                $events += [pscustomobject]@{
                    frame=$spec.frame;ts_ns=$spec.ts;dir=$spec.dir;session=32768;sid=$sid
                    connection_key='32768|0|S00000001|C00000002'
                    tag=$identity.Substring(0,2);identity=$identity;length=1;body=[byte[]](1)
                }
            }
        }
        for ($i = 0; $i -lt 20; $i++) {
            $states += [pscustomobject]@{
                frame=100 + ($i * 2);ts_ns=[uint64](10100000000 + ($i * 500000000))
                dir='C';kind='uplink';decode=$true
            }
            $states += [pscustomobject]@{
                frame=101 + ($i * 2);ts_ns=[uint64](10200000000 + ($i * 500000000))
                dir='S';kind='frame';decode=$true
            }
        }
        return [pscustomobject]@{
            packets=$packets;events=$events;states=$states;entities=@();histogram=$histogram
            first_ts_ns=[uint64]1000000000;last_ts_ns=[uint64]24100000000
            duration_ms=23100.0
        }
    }
    $validSteadyRun = [pscustomobject]@{
        steady_started_utc='1970-01-01T00:00:10.0000000Z'
        steady_completed_utc='1970-01-01T00:00:20.0000000Z'
        steady_seconds=10
    }
    $steadyFacts = & $newSteadyFacts
    $script:Failures.Clear()
    $steadySet = Set-SteadyWindowFacts $steadyFacts $validSteadyRun 'steady' 'RR'
    if (-not $steadySet -or $script:Failures.Count -ne 0) {
        throw "steady-window positive self-test rejected valid facts: $(@($script:Failures | ForEach-Object { "$($_.code):$($_.message)" }) -join '; ')"
    }
    if (
            $steadyFacts.packets.Count -ne 12 -or
            $steadyFacts.steady_packets.Count -ne 4 -or
            $steadyFacts.comparison_packets.Count -ne 10 -or
            @($steadyFacts.comparison_packets | Where-Object { $_.frame -eq 5 -or $_.frame -eq 6 }).Count -ne 2 -or
            [int]$steadyFacts.comparison_histogram['C:0c:0'] -ne 4 -or
            [int]$steadyFacts.comparison_histogram['S:0a:0'] -ne 4 -or
            [int]$steadyFacts.comparison_histogram['C:47:0'] -ne 1 -or
            [int]$steadyFacts.comparison_histogram['S:75:0'] -ne 1 -or
            [double]$steadyFacts.duration_ms -ne 10000.0 -or
            [double]$steadyFacts.comparison_duration_ms -ne 19000.0 -or
            [double]$steadyFacts.capture_duration_ms -ne 23100.0) {
        throw "steady-window slicing self-test failed: full=$($steadyFacts.packets.Count) steady=$($steadyFacts.steady_packets.Count) comparison=$($steadyFacts.comparison_packets.Count) warmup=$(@($steadyFacts.comparison_packets | Where-Object { $_.frame -eq 5 -or $_.frame -eq 6 }).Count) C0C=$($steadyFacts.comparison_histogram['C:0c:0']) S0A=$($steadyFacts.comparison_histogram['S:0a:0']) C47=$($steadyFacts.comparison_histogram['C:47:0']) S75=$($steadyFacts.comparison_histogram['S:75:0']) duration=$($steadyFacts.duration_ms) comparison_duration=$($steadyFacts.comparison_duration_ms) capture=$($steadyFacts.capture_duration_ms)"
    }

    $invalidUtcRun = [pscustomobject]@{
        steady_started_utc='1970-01-01T00:00:10+00:00'
        steady_completed_utc='1970-01-01T00:00:20.0000000Z'
        steady_seconds=10
    }
    $script:Failures.Clear()
    $null = Set-SteadyWindowFacts (& $newSteadyFacts) $invalidUtcRun 'steady' 'RR'
    if ($script:Failures.Count -ne 1 -or
            $script:Failures[0].code -ne 'STEADY_TIMESTAMP_INVALID') {
        throw 'strict UTC timestamp negative self-test failed'
    }

    $shortSteadyRun = [pscustomobject]@{
        steady_started_utc='1970-01-01T00:00:10.0000000Z'
        steady_completed_utc='1970-01-01T00:00:19.9990000Z'
        steady_seconds=10
    }
    $script:Failures.Clear()
    $null = Set-SteadyWindowFacts (& $newSteadyFacts) $shortSteadyRun 'steady' 'RR'
    if ($script:Failures.Count -ne 1 -or
            $script:Failures[0].code -ne 'STEADY_WINDOW_TOO_SHORT') {
        throw 'steady-window duration lower-bound self-test failed'
    }

    $longSteadyRun = [pscustomobject]@{
        steady_started_utc='1970-01-01T00:00:10.0000000Z'
        steady_completed_utc='1970-01-01T00:00:36.0000000Z'
        steady_seconds=10
    }
    $script:Failures.Clear()
    $null = Set-SteadyWindowFacts (& $newSteadyFacts) $longSteadyRun 'steady' 'RR'
    if ($script:Failures.Count -ne 1 -or
            $script:Failures[0].code -ne 'STEADY_WINDOW_TOO_LONG') {
        throw 'steady-window duration upper-bound self-test failed'
    }

    $delayedCompletionRun = [pscustomobject]@{
        steady_started_utc='1970-01-01T00:00:10.0000000Z'
        steady_completed_utc='1970-01-01T00:00:25.0000000Z'
        steady_seconds=10
    }
    $delayedFacts = & $newSteadyFacts
    $script:Failures.Clear()
    if (-not (Set-SteadyWindowFacts $delayedFacts $delayedCompletionRun 'steady' 'RR') -or
            $script:Failures.Count -ne 0 -or
            [double]$delayedFacts.duration_ms -ne 10000.0 -or
            [double]$delayedFacts.steady_survival_ms -ne 15000.0 -or
            $delayedFacts.steady_packets.Count -ne 4) {
        throw 'post-window completion work contaminated the exact steady slice self-test'
    }

    $untimestampedFacts = & $newSteadyFacts
    $untimestampedFacts.events[0].ts_ns = [uint64]0
    $script:Failures.Clear()
    $null = Set-SteadyWindowFacts $untimestampedFacts $validSteadyRun 'steady' 'RR'
    if ($script:Failures.Count -ne 1 -or $script:Failures[0].code -ne 'TIMESTAMP_MISSING') {
        throw 'all-record timestamp negative self-test failed'
    }

    $staleBoundaryFacts = & $newSteadyFacts
    foreach ($state in @($staleBoundaryFacts.states | Where-Object {
                $_.dir -eq 'S' -and [uint64]$_.ts_ns -ge [uint64]19500000000
            })) {
        $state.ts_ns = [uint64]17000000000
    }
    $script:Failures.Clear()
    $null = Set-SteadyWindowFacts $staleBoundaryFacts $validSteadyRun 'steady' 'RR'
    $boundaryFailures = @($script:Failures | Where-Object {
        $_.code -eq 'STEADY_BOUNDARY_GAMEPLAY_MISSING'
    })
    if ($boundaryFailures.Count -ne 1 -or
            $boundaryFailures[0].message -notlike 'S decoded gameplay state*last*') {
        throw 'steady-window end-boundary gameplay negative self-test failed'
    }

    $sparseFacts = & $newSteadyFacts
    $sparseFacts.states = @($sparseFacts.states | Where-Object {
        [uint64]$_.ts_ns -lt [uint64]10300000000 -or
            [uint64]$_.ts_ns -gt [uint64]19500000000
    })
    $script:Failures.Clear()
    $null = Set-SteadyWindowFacts $sparseFacts $validSteadyRun 'steady' 'RR'
    if (@($script:Failures | Where-Object { $_.code -eq 'STEADY_GAMEPLAY_GAP' }).Count -ne 2) {
        throw 'sparse steady-window middle-gap negative self-test failed'
    }
    $script:Failures.Clear()

    $gold = [pscustomobject]@{
        events = @([pscustomobject]@{dir='C';tag='0a';identity='0a:0';length=0;body=[byte[]]@();body_materialized=$true})
        histogram = @{'C:0a:0'=1}
        duration_ms = 1000.0
    }
    $subject = [pscustomobject]@{
        events = @(
            [pscustomobject]@{dir='C';tag='0a';identity='0a:0';length=0;body=[byte[]]@();body_materialized=$true},
            [pscustomobject]@{dir='C';tag='55';identity='55:0'}
        )
        histogram = @{'C:0a:0'=1;'C:55:0'=1}
        duration_ms = 1000.0
    }
    $case = [pscustomobject]@{ id='self-test'; resolution='1920x1080'; maintenance=$false; control_mode='observe'; exceptions=@() }
    $policy = [pscustomobject]@{ count_absolute_tolerance=0; count_relative_tolerance=0 }

    $motionRunId = "verifier-motion-$([Guid]::NewGuid().ToString('N'))"
    $motionRoot = Join-Path (Get-RepoRoot) ".scratch\runs\$motionRunId"
    $motionJoinerRoot = Join-Path $motionRoot 'joiner'
    $motionPath = Join-Path $motionJoinerRoot 'motion-gate.json'
    try {
        $null = [IO.Directory]::CreateDirectory($motionJoinerRoot)
        $motionStart = [DateTime]::UtcNow.AddMilliseconds(-100)
        $motionCreated = $motionStart.AddMilliseconds(50)
        $motionCompleted = $motionStart.AddSeconds(10)
        $motionGate = [ordered]@{
            schema=1;run_id=$motionRunId;topology='RO';joiner_pid=1234
            steady_started_utc=$motionStart.ToString('o')
            created_utc=$motionCreated.ToString('o')
            pre_roll_ms=250;inter_phase_gap_ms=250;look_samples_per_phase=20
            forward_ms=1800;forward_look=320;strafe_ms=1200;strafe_look=-320
            return_ms=900;return_look=160
        }
        [IO.File]::WriteAllText($motionPath, ($motionGate | ConvertTo-Json -Compress),
            [Text.UTF8Encoding]::new($false))
        [IO.File]::SetLastWriteTimeUtc($motionPath, $motionCreated)
        $motionWrite = (Get-Item -LiteralPath $motionPath).LastWriteTimeUtc.ToString('o')
        $motionHash = (Get-FileHash -LiteralPath $motionPath -Algorithm SHA256).Hash.ToLowerInvariant()
        $motionWitness = [pscustomobject]@{
            path="repo:.scratch/runs/$motionRunId/joiner/motion-gate.json"
            sha256=$motionHash;run_id=$motionRunId;topology='RO';joiner_pid=1234
            created_utc=$motionCreated.ToString('o')
            steady_started_utc=$motionStart.ToString('o')
            file_last_write_utc=$motionWrite
            pre_roll_ms=250;inter_phase_gap_ms=250;look_samples_per_phase=20
            forward_ms=1800;forward_look=320;strafe_ms=1200;strafe_look=-320
            return_ms=900;return_look=160
        }
        $motionRun = [pscustomobject]@{
            run_id=$motionRunId
            steady_started_utc=$motionStart.ToString('o')
            steady_completed_utc=$motionCompleted.ToString('o')
            motion_gate_witness=$motionWitness
        }
        $motionCase = [pscustomobject]@{exercise_input=$true}
        $script:Failures.Clear()
        if (-not (Test-MotionGateWitness $motionRun $motionCase `
                    (Get-RepoRoot) 'motion' 'RO') -or $script:Failures.Count -ne 0) {
            throw "motion-gate witness positive self-test failed: $(@($script:Failures | ForEach-Object { $_.message }) -join '; ')"
        }
        $motionWitness.look_samples_per_phase = 19
        $script:Failures.Clear()
        $null = Test-MotionGateWitness $motionRun $motionCase `
            (Get-RepoRoot) 'motion' 'RO'
        if ($script:Failures.Count -ne 1 -or
                $script:Failures[0].code -ne 'MOTION_GATE_INVALID') {
            throw 'motion-gate trajectory negative self-test failed'
        }
    } finally {
        if (Test-Path -LiteralPath $motionRoot -PathType Container) {
            [IO.Directory]::Delete($motionRoot, $true)
        }
    }

    $retailInputRunId = "verifier-retail-input-$([Guid]::NewGuid().ToString('N'))"
    $retailInputRoot = Join-Path (Get-RepoRoot) ".scratch\runs\$retailInputRunId"
    $retailInputPath = Join-Path $retailInputRoot 'retail-input.json'
    try {
        $null = [IO.Directory]::CreateDirectory($retailInputRoot)
        $retailSteadyStart = [DateTime]::UtcNow.AddSeconds(-20)
        $retailInputStart = $retailSteadyStart.AddSeconds(1)
        $retailInputCompleted = $retailInputStart.AddSeconds(6)
        $retailSteadyCompleted = $retailSteadyStart.AddSeconds(10)
        $retailInputArtifact = [ordered]@{
            schema='opennova.retail-input.v1';complete=$true
            run_id=$retailInputRunId;topology='RR';pid=2468
            process_start_utc=$retailSteadyStart.AddMinutes(-1).ToString('o')
            window_handle=97531;started_utc=$retailInputStart.ToString('o')
            completed_utc=$retailInputCompleted.ToString('o')
            forward_ms=1800;strafe_ms=1200;return_ms=900;turn_pixels=320
            injected_events=66;focus_attempts=3;foreground_checks=195
        }
        [IO.File]::WriteAllText(
            $retailInputPath, ($retailInputArtifact | ConvertTo-Json -Compress),
            [Text.UTF8Encoding]::new($false))
        $retailInputHash = (Get-FileHash -LiteralPath $retailInputPath `
            -Algorithm SHA256).Hash.ToLowerInvariant()
        $retailInputWitness = [pscustomobject]@{
            path="repo:.scratch/runs/$retailInputRunId/retail-input.json"
            sha256=$retailInputHash;schema='opennova.retail-input.v1';complete=$true
            run_id=$retailInputRunId;topology='RR';pid=2468
            process_start_utc=$retailInputArtifact.process_start_utc
            window_handle=97531;started_utc=$retailInputArtifact.started_utc
            completed_utc=$retailInputArtifact.completed_utc
            forward_ms=1800;strafe_ms=1200;return_ms=900;turn_pixels=320
            injected_events=66;focus_attempts=3;foreground_checks=195
        }
        $retailInputRun = [pscustomobject]@{
            run_id=$retailInputRunId
            steady_started_utc=$retailSteadyStart.ToString('o')
            steady_completed_utc=$retailSteadyCompleted.ToString('o')
            retail_input_witness=$retailInputWitness
        }
        $retailInputCase = [pscustomobject]@{exercise_input=$true}
        $script:Failures.Clear()
        if (-not (Test-RetailInputWitness $retailInputRun $retailInputCase `
                    (Get-RepoRoot) 'retail-input' 'RR') -or $script:Failures.Count -ne 0) {
            throw "retail-input witness positive self-test failed: $(@($script:Failures | ForEach-Object { $_.message }) -join '; ')"
        }
        $retailInputWitness.injected_events = 65
        $script:Failures.Clear()
        $null = Test-RetailInputWitness $retailInputRun $retailInputCase `
            (Get-RepoRoot) 'retail-input' 'RR'
        if ($script:Failures.Count -ne 1 -or
                $script:Failures[0].code -ne 'RETAIL_INPUT_WITNESS_INVALID') {
            throw 'retail-input manifest/artifact mismatch negative self-test failed'
        }
        $retailInputWitness.injected_events = 66
        $retailInputRun.steady_started_utc = $retailInputStart.AddSeconds(1).ToString('o')
        $script:Failures.Clear()
        $null = Test-RetailInputWitness $retailInputRun $retailInputCase `
            (Get-RepoRoot) 'retail-input' 'RR'
        if ($script:Failures.Count -ne 1 -or
                $script:Failures[0].code -ne 'RETAIL_INPUT_WITNESS_INVALID') {
            throw 'retail-input steady-window negative self-test failed'
        }
        $retailInputRun.steady_started_utc = $retailSteadyStart.ToString('o')
        $script:Failures.Clear()
        $null = Test-RetailInputWitness ([pscustomobject]@{
                run_id=$retailInputRunId
                steady_started_utc=$retailSteadyStart.ToString('o')
                steady_completed_utc=$retailSteadyCompleted.ToString('o')
            }) $retailInputCase (Get-RepoRoot) 'retail-input' 'RR'
        if ($script:Failures.Count -ne 1 -or
                $script:Failures[0].code -ne 'RETAIL_INPUT_WITNESS_MISSING') {
            throw 'retail-input missing-witness negative self-test failed'
        }
        $script:Failures.Clear()
        $null = Test-RetailInputWitness $retailInputRun $retailInputCase `
            (Get-RepoRoot) 'retail-input' 'RO'
        if ($script:Failures.Count -ne 1 -or
                $script:Failures[0].code -ne 'RETAIL_INPUT_WITNESS_UNEXPECTED') {
            throw 'retail-input non-retail-joiner negative self-test failed'
        }
        $script:Failures.Clear()
        $null = Test-RetailInputWitness $retailInputRun $retailInputCase `
            (Get-RepoRoot) 'retail-input' 'OR'
        if ($script:Failures.Count -ne 1 -or
                $script:Failures[0].code -ne 'RETAIL_INPUT_WITNESS_INVALID') {
            throw 'retail-input topology-binding negative self-test failed'
        }
    } finally {
        if (Test-Path -LiteralPath $retailInputRoot -PathType Container) {
            [IO.Directory]::Delete($retailInputRoot, $true)
        }
    }

    $shutdownRunId = "verifier-joiner-shutdown-$([Guid]::NewGuid().ToString('N'))"
    $shutdownRunRoot = Join-Path (Get-RepoRoot) ".scratch\runs\$shutdownRunId"
    $shutdownJoinerRoot = Join-Path $shutdownRunRoot 'joiner'
    $shutdownRequestPath = Join-Path $shutdownJoinerRoot 'joiner-stop-request.json'
    $shutdownAckPath = Join-Path $shutdownJoinerRoot 'joiner-shutdown.json'
    try {
        $null = [IO.Directory]::CreateDirectory($shutdownJoinerRoot)
        $shutdownSteadyCompleted = [DateTime]::UtcNow.AddSeconds(-2)
        $shutdownRequested = $shutdownSteadyCompleted.AddMilliseconds(100)
        $shutdownCompleted = $shutdownRequested.AddMilliseconds(500)
        $shutdownRequest = [ordered]@{
            schema='opennova.parity-joiner-stop-request.v1'
            run_id=$shutdownRunId;topology='RO';pid=2468
            requested_utc=$shutdownRequested.ToString('o')
        }
        $shutdownAck = [ordered]@{
            schema='opennova.parity-joiner-shutdown.v1'
            run_id=$shutdownRunId;topology='RO';process_id=2468
            requested_utc=$shutdownRequest.requested_utc
            completed_utc=$shutdownCompleted.ToString('o');clean=$true
        }
        [IO.File]::WriteAllText(
            $shutdownRequestPath, ($shutdownRequest | ConvertTo-Json -Compress),
            [Text.UTF8Encoding]::new($false))
        [IO.File]::WriteAllText(
            $shutdownAckPath, ($shutdownAck | ConvertTo-Json -Compress),
            [Text.UTF8Encoding]::new($false))
        $shutdownRequestHash = (Get-FileHash -LiteralPath $shutdownRequestPath `
            -Algorithm SHA256).Hash.ToLowerInvariant()
        $shutdownAckHash = (Get-FileHash -LiteralPath $shutdownAckPath `
            -Algorithm SHA256).Hash.ToLowerInvariant()
        $shutdownWitness = [pscustomobject]@{
            request_path="repo:.scratch/runs/$shutdownRunId/joiner/joiner-stop-request.json"
            request_sha256=$shutdownRequestHash
            path="repo:.scratch/runs/$shutdownRunId/joiner/joiner-shutdown.json"
            sha256=$shutdownAckHash;schema=$shutdownAck.schema
            run_id=$shutdownRunId;topology='RO';process_id=2468
            requested_utc=$shutdownAck.requested_utc
            completed_utc=$shutdownAck.completed_utc;clean=$true
        }
        $shutdownJoinerProof = [pscustomobject]@{runtime_pid=2468}
        $shutdownRun = [pscustomobject]@{
            run_id=$shutdownRunId
            steady_completed_utc=$shutdownSteadyCompleted.ToString('o')
            opennova_launch_proofs=[pscustomobject]@{joiner=$shutdownJoinerProof}
            opennova_joiner_shutdown_witness=$shutdownWitness
            opennova_joiner_exact_fallback_required=$false
        }
        $script:Failures.Clear()
        if (-not (Test-OpenNovaJoinerShutdownWitness $shutdownRun `
                    (Get-RepoRoot) 'joiner-shutdown' 'RO') -or
                $script:Failures.Count -ne 0) {
            throw "OpenNova joiner shutdown positive self-test failed: $(@($script:Failures | ForEach-Object { $_.message }) -join '; ')"
        }

        $script:Failures.Clear()
        $null = Test-OpenNovaJoinerShutdownWitness ([pscustomobject]@{
                run_id=$shutdownRunId
                steady_completed_utc=$shutdownSteadyCompleted.ToString('o')
                opennova_launch_proofs=[pscustomobject]@{joiner=$shutdownJoinerProof}
            }) (Get-RepoRoot) 'joiner-shutdown' 'RO'
        if ($script:Failures.Count -ne 1 -or
                $script:Failures[0].code -ne 'OPENNOVA_JOINER_SHUTDOWN_MISSING') {
            throw 'OpenNova joiner shutdown missing-witness self-test failed'
        }

        $shutdownWitness.sha256 = 'f' * 64
        $script:Failures.Clear()
        $null = Test-OpenNovaJoinerShutdownWitness $shutdownRun `
            (Get-RepoRoot) 'joiner-shutdown' 'RO'
        if ($script:Failures.Count -ne 1 -or
                $script:Failures[0].code -ne 'OPENNOVA_JOINER_SHUTDOWN_INVALID') {
            throw 'OpenNova joiner shutdown tampered-hash self-test failed'
        }
        $shutdownWitness.sha256 = $shutdownAckHash

        $script:Failures.Clear()
        $null = Test-OpenNovaJoinerShutdownWitness $shutdownRun `
            (Get-RepoRoot) 'joiner-shutdown' 'RR'
        if ($script:Failures.Count -ne 1 -or
                $script:Failures[0].code -ne 'OPENNOVA_JOINER_SHUTDOWN_UNEXPECTED') {
            throw 'OpenNova joiner shutdown unexpected-topology self-test failed'
        }

        $shutdownJoinerProof.runtime_pid = 2469
        $script:Failures.Clear()
        $null = Test-OpenNovaJoinerShutdownWitness $shutdownRun `
            (Get-RepoRoot) 'joiner-shutdown' 'RO'
        if ($script:Failures.Count -ne 1 -or
                $script:Failures[0].code -ne 'OPENNOVA_JOINER_SHUTDOWN_INVALID') {
            throw 'OpenNova joiner shutdown launch-PID mismatch self-test failed'
        }
        $shutdownJoinerProof.runtime_pid = 2468

        $shutdownRun.opennova_joiner_exact_fallback_required = $true
        $script:Failures.Clear()
        $null = Test-OpenNovaJoinerShutdownWitness $shutdownRun `
            (Get-RepoRoot) 'joiner-shutdown' 'RO'
        if ($script:Failures.Count -ne 1 -or
                $script:Failures[0].code -ne 'OPENNOVA_JOINER_SHUTDOWN_FALLBACK') {
            throw 'OpenNova joiner shutdown exact-fallback self-test failed'
        }
        $shutdownRun.opennova_joiner_exact_fallback_required = $false
    } finally {
        if (Test-Path -LiteralPath $shutdownRunRoot -PathType Container) {
            [IO.Directory]::Delete($shutdownRunRoot, $true)
        }
    }

    $hostShutdownRunId = "verifier-host-shutdown-$([Guid]::NewGuid().ToString('N'))"
    $hostShutdownRunRoot = Join-Path (Get-RepoRoot) ".scratch\runs\$hostShutdownRunId"
    $hostShutdownRoot = Join-Path $hostShutdownRunRoot 'host'
    $hostShutdownLogPath = Join-Path $hostShutdownRoot 'godot.log'
    try {
        $null = [IO.Directory]::CreateDirectory($hostShutdownRoot)
        $hostProcessStarted = [DateTime]::UtcNow.AddSeconds(-3)
        $hostShutdownRequested = $hostProcessStarted.AddSeconds(1)
        $hostShutdownCompleted = $hostShutdownRequested.AddMilliseconds(500)
        $cleanHostLog = "Godot Engine verifier host shutdown fixture`nOpenNova clean shutdown`n"
        [IO.File]::WriteAllText(
            $hostShutdownLogPath, $cleanHostLog, [Text.UTF8Encoding]::new($false))
        $cleanHostLogHash = (Get-FileHash -LiteralPath $hostShutdownLogPath `
            -Algorithm SHA256).Hash.ToLowerInvariant()
        $hostShutdownWitness = [pscustomobject]@{
            schema='opennova.parity-host-shutdown.v1'
            run_id=$hostShutdownRunId;topology='OR';process_id=3579
            requested_utc=$hostShutdownRequested.ToString('o')
            completed_utc=$hostShutdownCompleted.ToString('o')
            close_requested=$true;exit_code=0;clean=$true
            log_path="repo:.scratch/runs/$hostShutdownRunId/host/godot.log"
            log_sha256=$cleanHostLogHash
            forbidden_diagnostics_absent=$true
        }
        $hostShutdownProof = [pscustomobject]@{
            runtime_pid=3579
            runtime_start_utc=$hostProcessStarted.ToString('o')
        }
        $hostShutdownRun = [pscustomobject]@{
            run_id=$hostShutdownRunId
            steady_completed_utc=$hostProcessStarted.AddMilliseconds(500).ToString('o')
            opennova_launch_proofs=[pscustomobject]@{host=$hostShutdownProof}
            opennova_host_shutdown_witness=$hostShutdownWitness
            opennova_host_exact_fallback_required=$false
        }
        $script:Failures.Clear()
        if (-not (Test-OpenNovaHostShutdownWitness $hostShutdownRun `
                    (Get-RepoRoot) 'host-shutdown' 'OR') -or
                $script:Failures.Count -ne 0) {
            throw "OpenNova host shutdown positive self-test failed: $(@($script:Failures | ForEach-Object { $_.message }) -join '; ')"
        }

        $script:Failures.Clear()
        $null = Test-OpenNovaHostShutdownWitness ([pscustomobject]@{
                run_id=$hostShutdownRunId
                opennova_launch_proofs=[pscustomobject]@{host=$hostShutdownProof}
            }) (Get-RepoRoot) 'host-shutdown' 'OR'
        if ($script:Failures.Count -ne 1 -or
                $script:Failures[0].code -ne 'OPENNOVA_HOST_SHUTDOWN_MISSING') {
            throw 'OpenNova host shutdown missing-witness self-test failed'
        }

        $hostShutdownWitness.log_sha256 = 'f' * 64
        $script:Failures.Clear()
        $null = Test-OpenNovaHostShutdownWitness $hostShutdownRun `
            (Get-RepoRoot) 'host-shutdown' 'OR'
        if ($script:Failures.Count -ne 1 -or
                $script:Failures[0].code -ne 'OPENNOVA_HOST_SHUTDOWN_INVALID') {
            throw 'OpenNova host shutdown tampered-hash self-test failed'
        }
        $hostShutdownWitness.log_sha256 = $cleanHostLogHash

        $script:Failures.Clear()
        $null = Test-OpenNovaHostShutdownWitness $hostShutdownRun `
            (Get-RepoRoot) 'host-shutdown' 'RR'
        if ($script:Failures.Count -ne 1 -or
                $script:Failures[0].code -ne 'OPENNOVA_HOST_SHUTDOWN_UNEXPECTED') {
            throw 'OpenNova host shutdown unexpected-topology self-test failed'
        }

        $hostShutdownProof.runtime_pid = 3580
        $script:Failures.Clear()
        $null = Test-OpenNovaHostShutdownWitness $hostShutdownRun `
            (Get-RepoRoot) 'host-shutdown' 'OR'
        if ($script:Failures.Count -ne 1 -or
                $script:Failures[0].code -ne 'OPENNOVA_HOST_SHUTDOWN_INVALID') {
            throw 'OpenNova host shutdown launch-PID mismatch self-test failed'
        }
        $hostShutdownProof.runtime_pid = 3579

        $hostShutdownRun.opennova_host_exact_fallback_required = $true
        $script:Failures.Clear()
        $null = Test-OpenNovaHostShutdownWitness $hostShutdownRun `
            (Get-RepoRoot) 'host-shutdown' 'OR'
        if ($script:Failures.Count -ne 1 -or
                $script:Failures[0].code -ne 'OPENNOVA_HOST_SHUTDOWN_FALLBACK') {
            throw 'OpenNova host shutdown exact-fallback self-test failed'
        }
        $hostShutdownRun.opennova_host_exact_fallback_required = $false

        $hostShutdownWitness.exit_code = 1
        $script:Failures.Clear()
        $null = Test-OpenNovaHostShutdownWitness $hostShutdownRun `
            (Get-RepoRoot) 'host-shutdown' 'OR'
        if ($script:Failures.Count -ne 1 -or
                $script:Failures[0].code -ne 'OPENNOVA_HOST_SHUTDOWN_INVALID') {
            throw 'OpenNova host shutdown nonzero-exit self-test failed'
        }
        $hostShutdownWitness.exit_code = 0

        foreach ($hostForbiddenDiagnostic in @(
            'ERROR: ObjectDB instances leaked at exit',
            'WARNING: Resources still in use at exit',
            'ERROR: Parameter "RenderingServer::get_singleton()" is null',
            "ERROR: 2 RID allocations of type 'Texture' were leaked at exit"
        )) {
            [IO.File]::WriteAllText(
                $hostShutdownLogPath,
                "Godot Engine verifier host shutdown fixture`n$hostForbiddenDiagnostic`n",
                [Text.UTF8Encoding]::new($false))
            $hostShutdownWitness.log_sha256 = (Get-FileHash `
                -LiteralPath $hostShutdownLogPath -Algorithm SHA256).Hash.ToLowerInvariant()
            $script:Failures.Clear()
            $null = Test-OpenNovaHostShutdownWitness $hostShutdownRun `
                (Get-RepoRoot) 'host-shutdown' 'OR'
            if ($script:Failures.Count -ne 1 -or
                    $script:Failures[0].code -ne 'OPENNOVA_HOST_SHUTDOWN_INVALID') {
                throw "OpenNova host shutdown forbidden-log self-test failed for: $hostForbiddenDiagnostic"
            }
        }
    } finally {
        if (Test-Path -LiteralPath $hostShutdownRunRoot -PathType Container) {
            [IO.Directory]::Delete($hostShutdownRunRoot, $true)
        }
    }
    $script:Failures.Clear()
    Compare-ParityDirection $case 'RO' 'C' $gold $subject $policy
    if ($script:Failures.Count -ne 1 -or $script:Failures[0].code -ne 'UNEXPECTED_C2S') {
        throw "strict RO client-direction self-test failed"
    }
    $settingsSubject = [pscustomobject]@{
        events = @([pscustomobject]@{dir='C';tag='0a';identity='0a:1'})
        histogram = @{'C:0a:1'=1}
        duration_ms = 1000.0
    }
    $script:Failures.Clear()
    Compare-ParityDirection $case 'RO' 'C' $gold $settingsSubject $policy
    $settingsCodes = @($script:Failures | ForEach-Object { $_.code } | Sort-Object)
    if (($settingsCodes -join ',') -ne 'MISSING_TAG,UNEXPECTED_C2S') {
        throw "settings-update identity self-test failed"
    }
    $shapeGold = [pscustomobject]@{
        events=@([pscustomobject]@{dir='C';tag='2f';identity='2f:0';length=2;body=[byte[]](1,2)})
        histogram=@{'C:2f:0'=1};duration_ms=1000.0
    }
    $shapeSubject = [pscustomobject]@{
        events=@([pscustomobject]@{dir='C';tag='2f';identity='2f:0';length=1;body=[byte[]](9)})
        histogram=@{'C:2f:0'=1};duration_ms=1000.0
    }
    $script:Failures.Clear()
    Compare-ParityDirection $case 'RO' 'C' $shapeGold $shapeSubject $policy
    $shapeCodes = @($script:Failures | ForEach-Object { $_.code } | Sort-Object -Unique)
    foreach ($requiredCode in @('LENGTH_SET_MISMATCH','STABLE_BODY_MISMATCH')) {
        if ($shapeCodes -notcontains $requiredCode) {
            throw "wire-shape/body negative self-test did not produce $requiredCode"
        }
    }
    $seenHashes = @{}
    $script:Failures.Clear()
    $hash = 'a' * 64
    $null = Register-UniqueCaptureHash $seenHashes $hash 'self-a' 'RR'
    $null = Register-UniqueCaptureHash $seenHashes $hash 'self-b' 'RO'
    if ($script:Failures.Count -ne 1 -or
            $script:Failures[0].code -ne 'CAPTURE_BYTES_REUSED') {
        throw "duplicate capture SHA-256 self-test failed"
    }

    $seenMissionHashes = @{}
    $script:Failures.Clear()
    $missionHash = 'b' * 64
    $null = Test-MissionArtifactBinding `
        'map-a.bms' '.scratch/parity/corpus/renamed.bms' $missionHash $missionHash `
        'mission-a' ''
    $null = Register-UniqueMissionHash $seenMissionHashes $missionHash 'mission-a'
    $null = Register-UniqueMissionHash $seenMissionHashes $missionHash 'mission-b'
    $null = Test-MissionArtifactBinding `
        'map-a.bms' '.scratch/parity/corpus/map-a.bms' ('c' * 64) $missionHash `
        'mission-a' 'RO'
    $missionCodes = @($script:Failures | ForEach-Object { $_.code } | Sort-Object -Unique)
    foreach ($requiredCode in @(
        'MISSION_ARTIFACT_NAME', 'MISSION_BYTES_REUSED', 'RUN_MISSION_HASH_MISMATCH'
    )) {
        if ($missionCodes -notcontains $requiredCode) {
            throw "mission artifact negative self-test did not produce $requiredCode"
        }
    }
    $sameMissionHashes = @{}
    $script:Failures.Clear()
    if (-not (Register-UniqueMissionHash $sameMissionHashes $missionHash `
                'mission-case-a' 'map-a.bms') -or
            -not (Register-UniqueMissionHash $sameMissionHashes $missionHash `
                'mission-case-b' 'map-a.bms') -or
            $script:Failures.Count -ne 0 -or $sameMissionHashes.Count -ne 1) {
        throw 'same-mission crossover artifact reuse positive self-test failed'
    }

    $script:Failures.Clear()
    $hookWithoutStats = [pscustomobject]@{
        perspective='retail-host';capture_stats_available=$false
    }
    $externalWithInventedCounters = [pscustomobject]@{
        perspective='external';capture_stats_available=$false
        dropped=0;truncated=0;write_errors=0;unsupported_async=0
    }
    Test-CaptureStatsDeclaration $hookWithoutStats 'stats' 'RO'
    Test-CaptureStatsDeclaration $externalWithInventedCounters 'stats' 'OO'
    $captureStatsCodes = @($script:Failures | ForEach-Object { $_.code } | Sort-Object -Unique)
    foreach ($requiredCode in @('CAPTURE_STATS_REQUIRED','CAPTURE_STATS_COUNTERS_INVENTED')) {
        if ($captureStatsCodes -notcontains $requiredCode) {
            throw "capture-stats negative self-test did not produce $requiredCode"
        }
    }
    $script:Failures.Clear()
    Test-CaptureStatsDeclaration ([pscustomobject]@{
        perspective='external';capture_stats_available=$false
    }) 'stats' 'OO'
    if ($script:Failures.Count -ne 0) {
        throw "counter-less external capture-stats self-test failed"
    }

    $identityCase = [pscustomobject]@{
        id='identity';mission='01TR.bms';game_type=[uint32]65568
        host_callsign='ExpectedHost';host_session_name='ExpectedServer '
        server_name='ExpectedServer ';joiner_callsign='ExpectedJoin'
    }
    $identityHeader = [byte[]]::new(616)
    $identityHeader[0] = [byte][char]'B'; $identityHeader[1] = [byte][char]'M'
    $identityHeader[2] = [byte][char]'S'; $identityHeader[3] = 19
    [Text.Encoding]::ASCII.GetBytes('Training: Land Vehicles').CopyTo($identityHeader, 4)
    $identityMission = [pscustomobject]@{
        MissionDisplayName='Training: Land Vehicles'
        Bytes=$identityHeader
        Sha256=(Get-NetHarnessBytesSha256 $identityHeader)
    }
    $newIdentityBody = {
        param([string] $PlayerName, [string] $SessionName,
            [string] $AdvertisedMission = '01TR.bms')
        $body = [System.Collections.Generic.List[byte]]::new()
        foreach ($value in @($PlayerName, '', $SessionName, $AdvertisedMission, '01TR.bms')) {
            $body.AddRange([Text.Encoding]::ASCII.GetBytes($value))
            $body.Add(0)
        }
        $body.AddRange([BitConverter]::GetBytes([uint32]65568))
        foreach ($value in @('', 'revx02')) {
            $body.AddRange([Text.Encoding]::ASCII.GetBytes($value))
            $body.Add(0)
        }
        $body.ToArray()
    }
    $joinWrongSessionBody = [byte[]]@(& $newIdentityBody 'ExpectedJoin' 'WrongSession')
    $newSlotBody = {
        param([string] $PlayerName)
        [byte[]]@([byte]0, [byte]0xf7, [byte]0x5c, [byte]0x88) +
            [Text.Encoding]::ASCII.GetBytes($PlayerName) + [byte]0
    }
    $identityConfig = [byte[]]::new(51)
    [BitConverter]::GetBytes([uint32]65568).CopyTo($identityConfig, 12)
    $identityFacts = [pscustomobject]@{ events=@(
        [pscustomobject]@{dir='S';identity='08:0';body=$identityConfig},
        [pscustomobject]@{dir='S';identity='0b:0';body=$identityHeader},
        [pscustomobject]@{dir='S';identity='7b:0';body=$joinWrongSessionBody},
        [pscustomobject]@{dir='S';identity='46:0';body=[byte[]]@(& $newSlotBody 'ExpectedHost')},
        [pscustomobject]@{dir='S';identity='46:0';body=[byte[]]@(& $newSlotBody 'ExpectedJoin')}
    ) }
    $script:Failures.Clear()
    Test-WireIdentity $identityFacts $identityCase 'OR' `
        ([pscustomobject]@{ expansion='revx02' }) $identityMission
    $identityCodes = @($script:Failures | ForEach-Object { $_.code } | Sort-Object -Unique)
    if (($identityCodes -join ',') -ne 'SERVER_NAME_MISMATCH') {
        throw "server-name wire-identity negative self-test failed: $($identityCodes -join ',')"
    }

    $missingHostFacts = [pscustomobject]@{ events=@(
        [pscustomobject]@{dir='S';identity='08:0';body=$identityConfig},
        [pscustomobject]@{dir='S';identity='0b:0';body=$identityHeader},
        [pscustomobject]@{
            dir='S';identity='7b:0'
            body=[byte[]]@(& $newIdentityBody 'ExpectedJoin' 'ExpectedServer ')
        },
        [pscustomobject]@{dir='S';identity='46:0';body=[byte[]]@(& $newSlotBody 'ExpectedJoin')}
    ) }
    $script:Failures.Clear()
    Test-WireIdentity $missingHostFacts $identityCase 'OR' `
        ([pscustomobject]@{ expansion='revx02' }) $identityMission
    $identityCodes = @($script:Failures | ForEach-Object { $_.code } | Sort-Object -Unique)
    if (($identityCodes -join ',') -ne 'HOST_PLAYER_IDENTITY_MISMATCH') {
        throw "host-player wire-identity negative self-test failed: $($identityCodes -join ',')"
    }

    $titleIdentityFacts = [pscustomobject]@{ events=@(
        [pscustomobject]@{dir='S';identity='08:0';body=$identityConfig},
        [pscustomobject]@{dir='S';identity='0b:0';body=$identityHeader},
        [pscustomobject]@{
            dir='S';identity='7b:0'
            body=[byte[]]@(& $newIdentityBody 'ExpectedJoin' 'ExpectedServer ' `
                $identityMission.MissionDisplayName)
        },
        [pscustomobject]@{dir='S';identity='46:0';body=[byte[]]@(& $newSlotBody 'ExpectedHost')},
        [pscustomobject]@{dir='S';identity='46:0';body=[byte[]]@(& $newSlotBody 'ExpectedJoin')}
    ) }
    $script:Failures.Clear()
    Test-WireIdentity $titleIdentityFacts $identityCase 'OR' `
        ([pscustomobject]@{ expansion='revx02' }) $identityMission
    if ($script:Failures.Count -ne 0) {
        throw "BMS-title wire-identity self-test failed: $($script:Failures.code -join ',')"
    }

    $titleIdentityFacts.events[2].body = [byte[]]@(
        & $newIdentityBody 'ExpectedJoin' 'ExpectedServer ' 'Unbound Mission Title')
    $script:Failures.Clear()
    Test-WireIdentity $titleIdentityFacts $identityCase 'OR' `
        ([pscustomobject]@{ expansion='revx02' }) $identityMission
    $identityCodes = @($script:Failures | ForEach-Object { $_.code } | Sort-Object -Unique)
    if (($identityCodes -join ',') -ne 'MISSION_MISMATCH') {
        throw "unbound mission wire-identity negative self-test failed: $($identityCodes -join ',')"
    }
    $titleIdentityFacts.events[2].body = [byte[]]@(
        & $newIdentityBody 'ExpectedJoin' 'ExpectedServer ' `
            $identityMission.MissionDisplayName)
    [byte[]] $wrongIdentityHeader = [byte[]] $identityHeader.Clone()
    $wrongIdentityHeader[578] = $wrongIdentityHeader[578] -bxor 0x01
    $titleIdentityFacts.events[1].body = $wrongIdentityHeader
    $script:Failures.Clear()
    Test-WireIdentity $titleIdentityFacts $identityCase 'OR' `
        ([pscustomobject]@{ expansion='revx02' }) $identityMission
    $identityCodes = @($script:Failures | ForEach-Object { $_.code } | Sort-Object -Unique)
    if (($identityCodes -join ',') -ne 'MISSION_HEADER_MISMATCH') {
        throw "mutated BMS-header wire-identity negative self-test failed: $($identityCodes -join ',')"
    }

    $launchRepo = [IO.Path]::GetFullPath((Get-RepoRoot))
    $launchDriver = [IO.Path]::GetFullPath((Join-Path `
        $launchRepo 'godot\tests\net\parity_joiner_driver.gd'))
    $launchFixtureRoot = Join-Path ([IO.Path]::GetTempPath()) `
        "opennova-launch-proof-$([Guid]::NewGuid().ToString('N'))"
    $launchHostRoot = Join-Path $launchFixtureRoot 'SERVER'
    $launchClientRoot = Join-Path $launchFixtureRoot 'CLIENT'
    $launchFileRoles = [ordered]@{
        retail_exe='Jointops.exe';hook='binkw32.dll';resource='resource.pff'
        localres='localres.pff';language='language.pff';med='med.pff'
        expansion_pff='expansion\revx02\RevX02.pff'
        expansion_language_pff='expansion\revx02\RevX02L.pff'
        expansion_bin='expansion\revx02\revx02.bin'
    }
    try {
        foreach ($resourceRoot in @($launchHostRoot,$launchClientRoot)) {
            foreach ($artifactRole in $launchFileRoles.Keys) {
                $resourcePath = Join-Path $resourceRoot $launchFileRoles[$artifactRole]
                $null = [IO.Directory]::CreateDirectory((Split-Path -Parent $resourcePath))
                [IO.File]::WriteAllBytes($resourcePath,
                    [Text.Encoding]::UTF8.GetBytes("launch-fixture-$artifactRole"))
            }
        }
        $launchArtifacts = @{
            godot_launcher='C:\proof\Godot_v4.6.1-stable_win64_console.exe'
            godot_runtime='C:\proof\Godot_v4.6.1-stable_win64.exe'
            auto_joiner=$launchDriver
        }
        foreach ($artifactRole in $launchFileRoles.Keys) {
            $launchArtifacts[$artifactRole] = Join-Path `
                $launchHostRoot $launchFileRoles[$artifactRole]
        }
        $launchRunId = 'verifier-launch-proof'
        $launchLog = [IO.Path]::GetFullPath((Join-Path $launchRepo `
            ".scratch\runs\$launchRunId\joiner\godot.log"))
        $joinerLaunchProof = [pscustomobject]@{
            schema='opennova.godot-process-ownership.v1';wrapper_used=$true
            launcher_pid=101;launcher_path=$launchArtifacts.godot_launcher
            launcher_start_utc='2026-08-02T12:00:00.0000000Z'
            launcher_created_utc='2026-08-02T12:00:00.0000000Z'
            launcher_command_line='Godot_console.exe --path verifier-project'
            runtime_pid=102;runtime_parent_pid=101;runtime_path=$launchArtifacts.godot_runtime
            runtime_start_utc='2026-08-02T12:00:00.1000000Z'
            runtime_created_utc='2026-08-02T12:00:00.1000000Z'
            runtime_command_line='Godot.exe --path verifier-project'
            launcher_console_host_pids=@(103)
            arguments=@(
                '--path',(Join-Path $launchRepo 'godot'),'--windowed',
                '--resolution','1920x1080','--log-file',$launchLog,
                '--script',$launchDriver,'--','--resource-dir',$launchClientRoot,
                '/exp','revx02'
            )
            parent_verified=$true;argv_verified=$true;active_execution_verified=$true
            proven_utc='2026-08-02T12:00:00.2000000Z'
        }
        $launchRun = [pscustomobject]@{
            run_id=$launchRunId
            steady_started_utc='2026-08-02T12:00:01.0000000Z'
            opennova_launch_proofs=[pscustomobject]@{ joiner=$joinerLaunchProof }
        }
        $launchCase = [pscustomobject]@{ resolution='1920x1080' }
        $launchCorpus = [pscustomobject]@{ expansion='revx02' }
        $script:Failures.Clear()
        Test-OpenNovaLaunchProofs $launchRun $launchCase $launchCorpus `
            'launch' 'RO' $launchArtifacts
        if ($script:Failures.Count -ne 0) {
            throw "OpenNova wrapper launch-proof positive self-test failed: $(@($script:Failures | ForEach-Object { $_.message }) -join '; ')"
        }
        $joinerLaunchProof.wrapper_used = $false
        $script:Failures.Clear()
        Test-OpenNovaLaunchProofs $launchRun $launchCase $launchCorpus `
            'launch' 'RO' $launchArtifacts
        $launchCodes = @($script:Failures | ForEach-Object { $_.code } | Sort-Object -Unique)
        if (($launchCodes -join ',') -ne 'OPENNOVA_WRAPPER_BYPASSED') {
            throw "OpenNova wrapper bypass negative self-test failed: $($launchCodes -join ',')"
        }
        $joinerLaunchProof.wrapper_used = $true
        $joinerLaunchProof.arguments[8] = 'C:\proof\untracked_joiner_driver.gd'
        $script:Failures.Clear()
        Test-OpenNovaLaunchProofs $launchRun $launchCase $launchCorpus `
            'launch' 'RO' $launchArtifacts
        $launchCodes = @($script:Failures | ForEach-Object { $_.code } | Sort-Object -Unique)
        if (($launchCodes -join ',') -ne 'OPENNOVA_LAUNCH_ARGV') {
            throw "OpenNova exact argv negative self-test failed: $($launchCodes -join ',')"
        }
        $joinerLaunchProof.arguments[8] = $launchDriver
        $joinerLaunchProof.proven_utc = '2026-08-02T12:00:01.0010000Z'
        $script:Failures.Clear()
        Test-OpenNovaLaunchProofs $launchRun $launchCase $launchCorpus `
            'launch' 'RO' $launchArtifacts
        $launchCodes = @($script:Failures | ForEach-Object { $_.code } | Sort-Object -Unique)
        if (($launchCodes -join ',') -ne 'OPENNOVA_LAUNCH_TIMESTAMP') {
            throw "OpenNova proof-before-steady negative self-test failed: $($launchCodes -join ',')"
        }
        $joinerLaunchProof.proven_utc = '2026-08-02T12:00:00.2000000Z'
        $joinerLaunchProof | Add-Member -NotePropertyName unexpected `
            -NotePropertyValue $true
        $script:Failures.Clear()
        Test-OpenNovaLaunchProofs $launchRun $launchCase $launchCorpus `
            'launch' 'RO' $launchArtifacts
        $launchCodes = @($script:Failures | ForEach-Object { $_.code } | Sort-Object -Unique)
        if (($launchCodes -join ',') -ne 'OPENNOVA_LAUNCH_SCHEMA') {
            throw "OpenNova exact launch schema negative self-test failed: $($launchCodes -join ',')"
        }
        $joinerLaunchProof.PSObject.Properties.Remove('unexpected')
    } finally {
        if (Test-Path -LiteralPath $launchFixtureRoot -PathType Container) {
            [IO.Directory]::Delete($launchFixtureRoot, $true)
        }
    }

    $externalIncomplete = [pscustomobject]@{
        packets=@(
            [pscustomobject]@{frame=1;ts_ns=[uint64]1;dir='C';session=1;sid=[uint32]2;connection_key='external';sequence=[uint32]1;records=1;tag_identities=@('0c:0')},
            [pscustomobject]@{frame=2;ts_ns=[uint64]2;dir='C';session=1;sid=[uint32]2;connection_key='external';sequence=[uint32]3;records=2;tag_identities=@('0c:0','09:0')}
        )
        events=@(
            [pscustomobject]@{frame=1;ts_ns=[uint64]1;dir='C';session=1;sid=[uint32]2;connection_key='external';identity='0c:0'},
            [pscustomobject]@{frame=2;ts_ns=[uint64]2;dir='C';session=1;sid=[uint32]2;connection_key='external';identity='0c:0'}
        )
    }
    $script:Failures.Clear()
    Test-ExternalCaptureCompleteness $externalIncomplete 'stats' 'OO'
    $externalCodes = @($script:Failures | ForEach-Object { $_.code } | Sort-Object -Unique)
    foreach ($requiredCode in @('EXTERNAL_CAPTURE_SEQUENCE_GAP','EXTERNAL_CAPTURE_GROUP_INCOMPLETE')) {
        if ($externalCodes -notcontains $requiredCode) {
            throw "external completeness self-test did not produce $requiredCode"
        }
    }

    [byte[]]$tailMarker = [Text.Encoding]::UTF8.GetBytes(
        'OPENNOVA-PCAPNG-TAIL|verifier-self-test-run|0123456789abcdef0123456789abcdef')
    $pcapngBytes = [System.Collections.Generic.List[byte]]::new()
    $appendU16 = {
        param([uint16]$Value)
        $pcapngBytes.AddRange([BitConverter]::GetBytes($Value))
    }
    $appendU32 = {
        param([uint32]$Value)
        $pcapngBytes.AddRange([BitConverter]::GetBytes($Value))
    }
    & $appendU32 ([uint32]0x0a0d0d0a)
    & $appendU32 28
    & $appendU32 ([uint32]0x1a2b3c4d)
    & $appendU16 1
    & $appendU16 0
    $pcapngBytes.AddRange([byte[]](0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff))
    & $appendU32 28
    & $appendU32 1
    & $appendU32 20
    & $appendU16 1
    & $appendU16 0
    & $appendU32 65535
    & $appendU32 20
    $paddedMarkerLength = [int](($tailMarker.Length + 3) -band -4)
    $packetBlockLength = 32 + $paddedMarkerLength
    & $appendU32 6
    & $appendU32 ([uint32]$packetBlockLength)
    & $appendU32 0
    & $appendU32 0
    & $appendU32 1
    & $appendU32 ([uint32]$tailMarker.Length)
    & $appendU32 ([uint32]$tailMarker.Length)
    $pcapngBytes.AddRange($tailMarker)
    if ($paddedMarkerLength -gt $tailMarker.Length) {
        $pcapngBytes.AddRange([byte[]]::new($paddedMarkerLength - $tailMarker.Length))
    }
    & $appendU32 ([uint32]$packetBlockLength)
    $pcapngPath = Join-Path ([IO.Path]::GetTempPath()) `
        "opennova-parity-$([Guid]::NewGuid().ToString('N')).pcapng"
    try {
        [IO.File]::WriteAllBytes($pcapngPath, $pcapngBytes.ToArray())
        $pcapngHash = (Get-FileHash -LiteralPath $pcapngPath -Algorithm SHA256).Hash.ToLowerInvariant()
        $markerSha = [Security.Cryptography.SHA256]::Create()
        try { $markerDigest = $markerSha.ComputeHash($tailMarker) }
        finally { $markerSha.Dispose() }
        $tailMarkerHash = ([BitConverter]::ToString($markerDigest)).Replace('-', '').ToLowerInvariant()
        $proofRun = [pscustomobject]@{
            run_id='verifier-self-test-run'
            suite_id='verifier-self-test-suite'
            steady_completed_utc='1970-01-01T00:00:20.0000000Z'
            opennova_joiner_shutdown_witness=[pscustomobject]@{
                completed_utc='1970-01-01T00:00:20.5000000Z'
            }
            opennova_host_shutdown_witness=[pscustomobject]@{
                completed_utc='1970-01-01T00:00:20.7500000Z'
            }
            external_capture_proof=[pscustomobject]@{
                artifact_sha256=$pcapngHash
                valid=$true
                exact_eof=$true
                block_count=3
                packet_block_count=1
                tail_marker_required=$true
                tail_marker_hex=(ConvertTo-CompactHex $tailMarker)
                tail_marker_text=[Text.Encoding]::UTF8.GetString($tailMarker)
                tail_marker_sha256=$tailMarkerHash
                tail_marker_sent_utc='1970-01-01T00:00:21.0000000Z'
                tail_marker_found=$true
            }
        }
        $script:Failures.Clear()
        if (-not (Test-ExternalCaptureTailProof `
                    $proofRun $pcapngPath $pcapngHash 'pcapng' 'OO') -or
                $script:Failures.Count -ne 0) {
            throw 'independent pcapng tail-proof positive self-test failed'
        }
        $proofRun.external_capture_proof.tail_marker_required = $false
        $proofRun.external_capture_proof.tail_marker_found = $false
        $script:Failures.Clear()
        if (-not (Test-ExternalCaptureTailProof `
                    $proofRun $pcapngPath $pcapngHash 'pcapng' 'RR') -or
                $script:Failures.Count -ne 0) {
            throw 'non-OO independent pcapng structural-proof positive self-test failed'
        }
        $proofRun.external_capture_proof.tail_marker_required = $true
        $proofRun.external_capture_proof.tail_marker_found = $true
        $proofRun.opennova_host_shutdown_witness.completed_utc =
            '1970-01-01T00:00:21.1000000Z'
        $script:Failures.Clear()
        $null = Test-ExternalCaptureTailProof `
            $proofRun $pcapngPath $pcapngHash 'pcapng' 'OO'
        if ($script:Failures.Count -ne 1 -or
                $script:Failures[0].code -ne 'EXTERNAL_CAPTURE_PROOF_INVALID') {
            throw 'pcapng host-shutdown ordering negative self-test failed'
        }
        $proofRun.opennova_host_shutdown_witness.completed_utc =
            '1970-01-01T00:00:20.7500000Z'
        $proofRun.opennova_joiner_shutdown_witness.completed_utc =
            '1970-01-01T00:00:21.2000000Z'
        $script:Failures.Clear()
        $null = Test-ExternalCaptureTailProof `
            $proofRun $pcapngPath $pcapngHash 'pcapng' 'OO'
        if ($script:Failures.Count -ne 1 -or
                $script:Failures[0].code -ne 'EXTERNAL_CAPTURE_PROOF_INVALID') {
            throw 'pcapng joiner-shutdown ordering negative self-test failed'
        }
        $proofRun.opennova_joiner_shutdown_witness.completed_utc =
            '1970-01-01T00:00:20.5000000Z'
        $proofRun.external_capture_proof.packet_block_count = 2
        $script:Failures.Clear()
        $null = Test-ExternalCaptureTailProof `
            $proofRun $pcapngPath $pcapngHash 'pcapng' 'OO'
        if ($script:Failures.Count -ne 1 -or
                $script:Failures[0].code -ne 'EXTERNAL_CAPTURE_PROOF_INVALID') {
            throw 'independent pcapng declared-count negative self-test failed'
        }
        [IO.File]::WriteAllBytes(
            $pcapngPath, [byte[]]$pcapngBytes.ToArray()[0..($pcapngBytes.Count - 2)])
        $truncationRejected = $false
        try { $null = Get-ExternalPcapngTailFacts $pcapngPath $tailMarker }
        catch { $truncationRejected = $true }
        if (-not $truncationRejected) {
            throw 'independent pcapng exact-EOF negative self-test failed'
        }
    } finally {
        Remove-Item -LiteralPath $pcapngPath -Force -ErrorAction SilentlyContinue
    }
    $script:Failures.Clear()
    $controlBases = @(Test-ControlStream $parsed $case 'RR' 1250 @([uint32]0x12345678))
    if ($script:Failures.Count -ne 0 -or $controlBases.Count -ne 1 -or
            $controlBases[0] -ne [uint32]0x12345678) {
        throw "control request/reply state self-test failed"
    }
    $parsed.events[5].body[0] = 2
    $script:Failures.Clear()
    $null = Test-ControlStream $parsed $case 'OR' 1250 @([uint32]0x12345678)
    if (@($script:Failures | Where-Object { $_.code -eq 'CONTROL_TIME_ECHO' }).Count -ne 1) {
        throw "control time-echo negative self-test failed"
    }
    $parsed.events[5].body[0] = 1
    $parsed.packets[0].tag_identities = @('39:0','42:1','43:0','68:0')
    $parsed.events[1].identity = '42:1'
    $script:Failures.Clear()
    $null = Test-ControlStream $parsed $case 'OR' 1250 @([uint32]0x12345678)
    if (@($script:Failures | Where-Object { $_.code -eq 'CONTROL_GROUPING' }).Count -ne 1) {
        throw "high-table control identity negative self-test failed"
    }
    $parsed.packets[0].tag_identities = @('39:0','42:0','43:0','68:0')
    $parsed.events[1].identity = '42:0'

    # A subject that preserves tag/count parity but hardcodes decoded player
    # state must fail. This is the tight regression loop for the semantic
    # false-pass class the matrix exists to prevent.
    $semanticGold = [pscustomobject]@{
        states = @(
            [pscustomobject]@{kind='uplink';decode=$true;length=48;type_id=5305;sub_op=10;adm=16;state_a=1;move=2;carrier=65535;session=1;handle=7;pos_x=1;pos_y=2;pos_z=3;orient_a=4;orient_b=5;analog_x=0;analog_y=0;analog_z=0;priority_nonzero=2},
            [pscustomobject]@{kind='uplink';decode=$true;length=48;type_id=5305;sub_op=10;adm=16;state_a=1;move=2;carrier=65535;session=1;handle=7;pos_x=2;pos_y=2;pos_z=3;orient_a=6;orient_b=5;analog_x=1;analog_y=0;analog_z=0;priority_nonzero=2}
        )
        entities = @()
        duration_ms = 1000.0
    }
    $semanticSubject = [pscustomobject]@{
        states = @(
            [pscustomobject]@{kind='uplink';decode=$true;length=48;type_id=5305;sub_op=10;adm=16;state_a=1;move=2;carrier=65535;session=2;handle=9;pos_x=1;pos_y=2;pos_z=3;orient_a=0;orient_b=0;analog_x=0;analog_y=0;analog_z=0;priority_nonzero=2},
            [pscustomobject]@{kind='uplink';decode=$true;length=48;type_id=5305;sub_op=10;adm=16;state_a=1;move=2;carrier=65535;session=2;handle=9;pos_x=1;pos_y=2;pos_z=3;orient_a=0;orient_b=0;analog_x=0;analog_y=0;analog_z=0;priority_nonzero=2}
        )
        entities = @()
        duration_ms = 1000.0
    }
    $semanticPolicy = [pscustomobject]@{
        count_absolute_tolerance=0
        count_relative_tolerance=0
        semantic_diversity_ratio=0.25
    }
    foreach ($semanticTopology in @('RO','OO')) {
        $script:Failures.Clear()
        Compare-SemanticDirection $case $semanticTopology 'C' `
            $semanticGold $semanticSubject $semanticPolicy
        $semanticCodes = @($script:Failures | ForEach-Object { $_.code } |
            Sort-Object -Unique)
        foreach ($requiredCode in @(
            'UPLINK_CONSTANT','UPLINK_ORIENTATION_ZERO','UPLINK_ANALOG_ZERO'
        )) {
            if ($semanticCodes -notcontains $requiredCode) {
                throw "$semanticTopology semantic hardcoding self-test did not produce $requiredCode"
            }
        }
    }

    $priorityGoldState = [pscustomobject]@{
        kind='uplink';decode=$true;length=48;type_id=5305;sub_op=10;adm=16
        state_a=1;move=2;carrier=65535;connection_key='gold';sid=1;session=1;handle=7
        frame=1;ts_ns=[uint64]1000;pos_x=1;pos_y=2;pos_z=3;orient_a=4;orient_b=5
        analog_x=0;analog_y=0;analog_z=0;priority_nonzero=2
        priority_handle_0=101;priority_score_0=11
        priority_handle_1=0;priority_score_1=0
        priority_handle_2=202;priority_score_2=22
        priority_handle_3=0;priority_score_3=0
    }
    $prioritySubjectState = $priorityGoldState | Select-Object *
    $prioritySubjectState.connection_key = 'subject'
    $prioritySubjectState.sid = 2
    $prioritySubjectState.session = 2
    $prioritySubjectState.handle = 9
    $prioritySubjectState.priority_handle_0 = 900
    $prioritySubjectState.priority_handle_2 = 901
    $priorityGold = [pscustomobject]@{
        states=@($priorityGoldState);entities=@();duration_ms=1000.0
    }
    $prioritySubject = [pscustomobject]@{
        states=@($prioritySubjectState);entities=@();duration_ms=1000.0
    }
    $script:Failures.Clear()
    Compare-SemanticDirection $case 'RO' 'C' `
        $priorityGold $prioritySubject $semanticPolicy
    if (@($script:Failures | Where-Object {
                $_.code -like 'UPLINK_PRIORITY*'
            }).Count -ne 0) {
        throw 'priority raw-handle normalization positive self-test failed'
    }
    $prioritySubjectState.priority_score_2 = 23
    $script:Failures.Clear()
    Compare-SemanticDirection $case 'RO' 'C' `
        $priorityGold $prioritySubject $semanticPolicy
    if (@($script:Failures | Where-Object {
                $_.code -eq 'UPLINK_PRIORITY_MISMATCH'
            }).Count -ne 1) {
        throw 'ordered priority score negative self-test did not fail closed'
    }
    $prioritySubjectState.priority_score_2 = 22

    $weaponGoldState = [pscustomobject]@{
        kind='frame';decode=$true;length=64;sub_block=1;flags1=0;flags2=1
        connection_key='gold';sid=1;session=1;frame=1;ts_ns=[uint64]1000
        anchor_x=1;anchor_y=2;anchor_z=3;state_a=0;health=100;state_word=0
        players=0;vehicles=0;infantry=0;none=0;rounds=0
        local_mount=101
        weapon_present=$true;weapon_preround_timer=2;weapon_slot_state360=3
        weapon_slot_state368=4;weapon_slot_state364=5;weapon_slot_state356=6
        weapon_slot_state460=7;weapon_reload_seconds=8;weapon_uniform_team_mask=-9
        timer_present=$false;timer_state0=0;timer_state1=0;timer_state2=0
        timer_state3=0;timer_seconds=0
        env_present=$false;env_fog_dist=0;env_fog_accel=0;env_tod_fixed=0
        env_quake_ticks=0;env_cloud_scroll=0;env_rain_pct=0;env_overcast=0;env_param=0
        objective_present=$false;objective_state_0=0;objective_state_1=0
        objective_state_2=0;objective_state_3=0
        passenger_present=$false;passenger_mount_handle=65535
        passenger_has_mount=$false;passenger_clip=0;passenger_reserve=0
    }
    $weaponSubjectState = $weaponGoldState | Select-Object *
    $weaponSubjectState.local_mount = 900
    $weaponGold = [pscustomobject]@{
        states=@($weaponGoldState);entities=@();duration_ms=1000.0
    }
    $weaponSubject = [pscustomobject]@{
        states=@($weaponSubjectState);entities=@();duration_ms=1000.0
    }
    $script:Failures.Clear()
    Compare-SemanticDirection $case 'OR' 'S' `
        $weaponGold $weaponSubject $semanticPolicy
    if (@($script:Failures | Where-Object {
                $_.code -like 'FRAME_*PHASE_MISMATCH' -or
                $_.code -eq 'FRAME_LOCAL_MOUNT_MISMATCH'
            }).Count -ne 0) {
        throw 'phase payload raw-handle normalization positive self-test failed'
    }
    $weaponSubjectState.weapon_slot_state460 = 70
    $script:Failures.Clear()
    Compare-SemanticDirection $case 'OR' 'S' `
        $weaponGold $weaponSubject $semanticPolicy
    if (@($script:Failures | Where-Object {
                $_.code -eq 'FRAME_WEAPON_PHASE_MISMATCH'
            }).Count -ne 1) {
        throw 'weapon phase payload negative self-test did not fail closed'
    }
    $weaponSubjectState.weapon_slot_state460 = 7

    $timerGoldStates = @()
    $timerSubjectStates = @()
    for ($timerIndex = 0; $timerIndex -lt 3; $timerIndex++) {
        $goldTimer = $weaponGoldState | Select-Object *
        $goldTimer.frame = $timerIndex
        $goldTimer.flags2 = [uint32](16 * $timerIndex)
        $goldTimer.weapon_present = $false
        $goldTimer.timer_present = $true
        $goldTimer.timer_state0 = 10
        $goldTimer.timer_state1 = 11
        $goldTimer.timer_state2 = 12
        $goldTimer.timer_state3 = 13
        $goldTimer.timer_seconds = 100 - $timerIndex
        $goldTimer.connection_key = 'gold'
        $goldTimer.sid = 1
        $goldTimer.session = 1
        $timerGoldStates += $goldTimer
        $subjectTimer = $goldTimer | Select-Object *
        $subjectTimer.timer_seconds = 900 - $timerIndex
        $subjectTimer.connection_key = 'subject'
        $subjectTimer.sid = 2
        $subjectTimer.session = 2
        $timerSubjectStates += $subjectTimer
    }
    $timerGold = [pscustomobject]@{
        states=$timerGoldStates;entities=@();duration_ms=1000.0
    }
    $timerSubject = [pscustomobject]@{
        states=$timerSubjectStates;entities=@();duration_ms=1000.0
    }
    $script:Failures.Clear()
    Compare-SemanticDirection $case 'OR' 'S' `
        $timerGold $timerSubject $semanticPolicy
    if (@($script:Failures | Where-Object {
                $_.code -eq 'FRAME_TIMER_PHASE_MISMATCH'
            }).Count -ne 0) {
        throw 'timer origin normalization positive self-test failed'
    }
    $timerSubjectStates[1].timer_seconds = 897
    $script:Failures.Clear()
    Compare-SemanticDirection $case 'OR' 'S' `
        $timerGold $timerSubject $semanticPolicy
    if (@($script:Failures | Where-Object {
                $_.code -eq 'FRAME_TIMER_PHASE_MISMATCH'
            }).Count -ne 1) {
        throw 'timer progression negative self-test did not fail closed'
    }
    $timerSubjectStates[1].timer_seconds = 899

    $envGoldStates = @()
    $envSubjectStates = @()
    for ($envIndex = 0; $envIndex -lt 3; $envIndex++) {
        $goldEnv = $weaponGoldState | Select-Object *
        $goldEnv.frame = $envIndex
        $goldEnv.flags2 = [uint32](2 + (16 * $envIndex))
        $goldEnv.weapon_present = $false
        $goldEnv.env_present = $true
        $goldEnv.env_fog_dist = 380
        $goldEnv.env_fog_accel = 65280
        $goldEnv.env_tod_fixed = 1000 + (5 * $envIndex)
        $goldEnv.env_quake_ticks = 3
        $goldEnv.env_cloud_scroll = 15
        $goldEnv.env_rain_pct = 6
        $goldEnv.env_overcast = 7
        $goldEnv.env_param = 8
        $goldEnv.connection_key = 'gold'
        $goldEnv.sid = 1
        $goldEnv.session = 1
        $envGoldStates += $goldEnv
        $subjectEnv = $goldEnv | Select-Object *
        $subjectEnv.env_tod_fixed = 50000 + (5 * $envIndex)
        $subjectEnv.connection_key = 'subject'
        $subjectEnv.sid = 2
        $subjectEnv.session = 2
        $envSubjectStates += $subjectEnv
    }
    $envGold = [pscustomobject]@{
        states=$envGoldStates;entities=@();duration_ms=1000.0
    }
    $envSubject = [pscustomobject]@{
        states=$envSubjectStates;entities=@();duration_ms=1000.0
    }
    $script:Failures.Clear()
    Compare-SemanticDirection $case 'OR' 'S' `
        $envGold $envSubject $semanticPolicy
    if (@($script:Failures | Where-Object {
                $_.code -eq 'FRAME_ENV_PHASE_MISMATCH'
            }).Count -ne 0) {
        throw 'environment TOD-origin normalization positive self-test failed'
    }
    foreach ($envField in @(
        'env_fog_dist','env_fog_accel','env_quake_ticks','env_cloud_scroll',
        'env_rain_pct','env_overcast','env_param'
    )) {
        $envSubjectStates[0].$envField++
        $script:Failures.Clear()
        Compare-SemanticDirection $case 'OR' 'S' `
            $envGold $envSubject $semanticPolicy
        if (@($script:Failures | Where-Object {
                    $_.code -eq 'FRAME_ENV_PHASE_MISMATCH'
                }).Count -ne 1) {
            throw "environment $envField negative self-test did not fail closed"
        }
        $envSubjectStates[0].$envField--
    }
    $envSubjectStates[1].env_tod_fixed = 50007
    $script:Failures.Clear()
    Compare-SemanticDirection $case 'OR' 'S' `
        $envGold $envSubject $semanticPolicy
    if (@($script:Failures | Where-Object {
                $_.code -eq 'FRAME_ENV_PHASE_MISMATCH'
            }).Count -ne 1) {
        throw 'environment TOD progression negative self-test did not fail closed'
    }
    $envSubjectStates[1].env_tod_fixed = 50005

    $objectiveGoldState = $weaponGoldState | Select-Object *
    $objectiveGoldState.flags2 = 3
    $objectiveGoldState.weapon_present = $false
    $objectiveGoldState.objective_present = $true
    $objectiveGoldState.objective_state_0 = -1
    $objectiveGoldState.objective_state_1 = 2
    $objectiveGoldState.objective_state_2 = -3
    $objectiveGoldState.objective_state_3 = 4
    $objectiveSubjectState = $objectiveGoldState | Select-Object *
    $objectiveGold = [pscustomobject]@{
        states=@($objectiveGoldState);entities=@();duration_ms=1000.0
    }
    $objectiveSubject = [pscustomobject]@{
        states=@($objectiveSubjectState);entities=@();duration_ms=1000.0
    }
    foreach ($objectiveField in @(
        'objective_state_0','objective_state_1',
        'objective_state_2','objective_state_3'
    )) {
        $objectiveSubjectState.$objectiveField++
        $script:Failures.Clear()
        Compare-SemanticDirection $case 'OR' 'S' `
            $objectiveGold $objectiveSubject $semanticPolicy
        if (@($script:Failures | Where-Object {
                    $_.code -eq 'FRAME_OBJECTIVE_PHASE_MISMATCH'
                }).Count -ne 1) {
            throw "objective $objectiveField negative self-test did not fail closed"
        }
        $objectiveSubjectState.$objectiveField--
    }

    $passengerGoldState = $weaponGoldState | Select-Object *
    $passengerGoldState.flags2 = 8
    $passengerGoldState.weapon_present = $false
    $passengerGoldState.local_mount = 101
    $passengerGoldState.passenger_present = $true
    $passengerGoldState.passenger_mount_handle = 202
    $passengerGoldState.passenger_has_mount = $true
    $passengerGoldState.passenger_clip = 30
    $passengerGoldState.passenger_reserve = 40
    $passengerSubjectState = $passengerGoldState | Select-Object *
    $passengerSubjectState.local_mount = 900
    $passengerSubjectState.passenger_mount_handle = 901
    $passengerGold = [pscustomobject]@{
        states=@($passengerGoldState);entities=@();duration_ms=1000.0
    }
    $passengerSubject = [pscustomobject]@{
        states=@($passengerSubjectState);entities=@();duration_ms=1000.0
    }
    $script:Failures.Clear()
    Compare-SemanticDirection $case 'OR' 'S' `
        $passengerGold $passengerSubject $semanticPolicy
    if (@($script:Failures | Where-Object {
                $_.code -in @('FRAME_LOCAL_MOUNT_MISMATCH',
                    'FRAME_PASSENGER_PHASE_MISMATCH')
            }).Count -ne 0) {
        throw 'phase-8 raw mount-handle normalization positive self-test failed'
    }
    foreach ($passengerField in @('passenger_clip','passenger_reserve')) {
        $passengerSubjectState.$passengerField++
        $script:Failures.Clear()
        Compare-SemanticDirection $case 'OR' 'S' `
            $passengerGold $passengerSubject $semanticPolicy
        if (@($script:Failures | Where-Object {
                    $_.code -eq 'FRAME_PASSENGER_PHASE_MISMATCH'
                }).Count -ne 1) {
            throw "phase-8 $passengerField negative self-test did not fail closed"
        }
        $passengerSubjectState.$passengerField--
    }
    $passengerSubjectState.passenger_has_mount = $false
    $passengerSubjectState.passenger_mount_handle = 65535
    $script:Failures.Clear()
    Compare-SemanticDirection $case 'OR' 'S' `
        $passengerGold $passengerSubject $semanticPolicy
    if (@($script:Failures | Where-Object {
                $_.code -eq 'FRAME_PASSENGER_PHASE_MISMATCH'
            }).Count -ne 1) {
        throw 'phase-8 mount-presence negative self-test did not fail closed'
    }
    $passengerSubjectState.passenger_has_mount = $true
    $passengerSubjectState.passenger_mount_handle = 901
    $passengerSubjectState.local_mount = 65535
    $script:Failures.Clear()
    Compare-SemanticDirection $case 'OR' 'S' `
        $passengerGold $passengerSubject $semanticPolicy
    if (@($script:Failures | Where-Object {
                $_.code -eq 'FRAME_LOCAL_MOUNT_MISMATCH'
            }).Count -ne 1) {
        throw 'local mount presence negative self-test did not fail closed'
    }
    $passengerSubjectState.local_mount = 900
    $constantInput = [pscustomobject]@{
        states=@($semanticSubject.states[0],$semanticSubject.states[0],
            $semanticSubject.states[0],$semanticSubject.states[0],
            $semanticSubject.states[0],$semanticSubject.states[0],
            $semanticSubject.states[0],$semanticSubject.states[0])
    }
    $script:Failures.Clear()
    Test-ControllableInputWitness $constantInput `
        ([pscustomobject]@{id='input';require_analog=$false}) 'RO'
    $inputCodes = @($script:Failures | ForEach-Object { $_.code } | Sort-Object -Unique)
    foreach ($requiredCode in @('INPUT_POSITION_CONSTANT','INPUT_ORIENTATION_CONSTANT')) {
        if ($inputCodes -notcontains $requiredCode) {
            throw "absolute input-witness self-test did not produce $requiredCode"
        }
    }
    $preWindowMoving = @($semanticGold.states)
    $steadyFrozen = @(
        $semanticSubject.states[0],$semanticSubject.states[0],
        $semanticSubject.states[0],$semanticSubject.states[0],
        $semanticSubject.states[0],$semanticSubject.states[0],
        $semanticSubject.states[0],$semanticSubject.states[0]
    )
    $contaminatedInput = [pscustomobject]@{
        states=@($preWindowMoving + $steadyFrozen)
        steady_states=$steadyFrozen
    }
    $script:Failures.Clear()
    Test-ControllableInputWitness $contaminatedInput `
        ([pscustomobject]@{id='input-window';require_analog=$false}) 'RO'
    $windowInputCodes = @($script:Failures | ForEach-Object { $_.code } | Sort-Object -Unique)
    foreach ($requiredCode in @('INPUT_POSITION_CONSTANT','INPUT_ORIENTATION_CONSTANT')) {
        if ($windowInputCodes -notcontains $requiredCode) {
            throw "pre-steady motion contaminated the input witness self-test: missing $requiredCode"
        }
    }
    $windowGolden = [pscustomobject]@{
        states=$semanticGold.states;steady_states=$semanticGold.states
        entities=@();steady_entities=@();duration_ms=1000.0
    }
    $windowSubject = [pscustomobject]@{
        states=@($preWindowMoving + $steadyFrozen);steady_states=$steadyFrozen
        entities=@();steady_entities=@();duration_ms=1000.0
    }
    $script:Failures.Clear()
    Compare-SemanticDirection $case 'RO' 'C' `
        $windowGolden $windowSubject $semanticPolicy
    if (@($script:Failures | Where-Object { $_.code -eq 'UPLINK_CONSTANT' }).Count -ne 1) {
        throw 'pre-steady motion contaminated the semantic comparison self-test'
    }

    # Four distinct values clear the legacy 25%-diversity threshold, but the
    # subject is on the wrong map origin and loops the same four-state cycle.
    $dynamicGoldStates = @()
    $dynamicSubjectStates = @()
    for ($i = 0; $i -lt 12; $i++) {
        $dynamicGoldStates += [pscustomobject]@{
            kind='uplink';decode=$true;length=48;type_id=5305;sub_op=10;adm=16
            state_a=1;move=2;carrier=65535;connection_key='gold';sid=1;session=1;handle=7
            frame=$i;ts_ns=[uint64](1000 + $i)
            pos_x=$i * 65536;pos_y=$i * 32768;pos_z=0
            orient_a=$i * 256;orient_b=$i * 128
            analog_x=$i % 8;analog_y=($i * 3) % 8;analog_z=0;priority_nonzero=2
        }
        $cycle = $i % 4
        $dynamicSubjectStates += [pscustomobject]@{
            kind='uplink';decode=$true;length=48;type_id=5305;sub_op=10;adm=16
            state_a=1;move=2;carrier=65535;connection_key='subject';sid=2;session=2;handle=9
            frame=$i;ts_ns=[uint64](1000 + $i)
            pos_x=(256 * 65536) + ($cycle * 65536);pos_y=$cycle * 32768;pos_z=0
            orient_a=$cycle * 256;orient_b=$cycle * 128
            analog_x=$cycle;analog_y=$cycle;analog_z=0;priority_nonzero=2
        }
    }
    $dynamicGold = [pscustomobject]@{
        states=$dynamicGoldStates;entities=@();duration_ms=1000.0
    }
    $dynamicSubject = [pscustomobject]@{
        states=$dynamicSubjectStates;entities=@();duration_ms=1000.0
    }
    $script:Failures.Clear()
    Compare-SemanticDirection $case 'RO' 'C' `
        $dynamicGold $dynamicSubject $semanticPolicy
    $dynamicCodes = @($script:Failures | ForEach-Object { $_.code } | Sort-Object -Unique)
    foreach ($requiredCode in @('DYNAMIC_BASELINE_MISMATCH','DYNAMIC_SHORT_CYCLE')) {
        if ($dynamicCodes -notcontains $requiredCode) {
            throw "RR-relative dynamic-state self-test did not produce $requiredCode"
        }
    }

    $groupGold = [pscustomobject]@{
        packets = @(
            [pscustomobject]@{dir='C';session=1000;sid=1;sequence=1;frame=1;records=2;tags=@('22','09');tag_identities=@('22:0','09:0')},
            [pscustomobject]@{dir='C';session=1000;sid=1;sequence=2;frame=2;records=1;tags=@('0c');tag_identities=@('0c:0')}
        )
    }
    $groupSubject = [pscustomobject]@{
        packets = @(
            [pscustomobject]@{dir='C';session=1000;sid=2;sequence=1;frame=1;records=1;tags=@('22');tag_identities=@('22:0')},
            [pscustomobject]@{dir='C';session=1000;sid=2;sequence=2;frame=2;records=2;tags=@('09','0c');tag_identities=@('09:0','0c:0')}
        )
    }
    $script:Failures.Clear()
    Compare-InitialPacketGrouping $case 'RO' 'C' $groupGold $groupSubject
    if ($script:Failures.Count -ne 1 -or $script:Failures[0].code -ne 'INITIAL_PACKET_GROUPING') {
        throw "initial packet grouping self-test failed"
    }

    $steadyGoldPackets = @(
        [pscustomobject]@{dir='C';session=1000;sid=1;connection_key='gold';sequence=1;frame=1;records=1;tags=@('0c');tag_identities=@('0c:0')}
    )
    $steadySubjectPackets = @(
        [pscustomobject]@{dir='C';session=1000;sid=2;connection_key='subject';sequence=1;frame=1;records=1;tags=@('0c');tag_identities=@('0c:0')}
    )
    for ($i = 2; $i -le 13; $i++) {
        $steadyGoldPackets += [pscustomobject]@{
            dir='C';session=1000;sid=1;connection_key='gold';sequence=$i;frame=$i
            records=3;tags=@('0c','09','2c');tag_identities=@('0c:0','09:0','2c:0')
        }
        $steadySubjectPackets += [pscustomobject]@{
            dir='C';session=1000;sid=2;connection_key='subject';sequence=$i;frame=$i
            records=3;tags=@('2c','09','0c');tag_identities=@('2c:0','09:0','0c:0')
        }
    }
    $steadyGold = [pscustomobject]@{ packets=$steadyGoldPackets }
    $steadySubject = [pscustomobject]@{ packets=$steadySubjectPackets }
    $steadyPolicy = [pscustomobject]@{
        gameplay_distribution_min_samples=8
        gameplay_distribution_min_share=0.02
        gameplay_distribution_absolute_tolerance=0.08
        gameplay_distribution_relative_tolerance=0.50
    }
    $script:Failures.Clear()
    Compare-SteadyGameplayDistributions `
        $case 'RO' 'C' $steadyGold $steadySubject $steadyPolicy
    $steadyCodes = @($script:Failures | ForEach-Object { $_.code } | Sort-Object -Unique)
    foreach ($requiredCode in @('GAMEPLAY_GROUP_DISTRIBUTION','GAMEPLAY_ORDER_DISTRIBUTION')) {
        if ($steadyCodes -notcontains $requiredCode) {
            throw "steady gameplay distribution self-test did not produce $requiredCode"
        }
    }
    $sidReuse = [pscustomobject]@{
        packets = @(
            [pscustomobject]@{dir='C';session=1000;sid=1;sequence=1;frame=1;records=1;tags=@('47');tag_identities=@('47:0')},
            [pscustomobject]@{dir='C';session=1000;sid=2;sequence=1;frame=2;records=1;tags=@('0c');tag_identities=@('0c:0')}
        )
    }
    if (@(Get-InitialPacketGroups $sidReuse 'C').Count -ne 2) {
        throw "wire SID reconnect identity self-test failed"
    }

    $integrityEvents = @(
        [pscustomobject]@{frame=20;ts_ns=[uint64]1000000000;dir='S';session=1;sid=[uint32]1;connection_key='1|0|S00000001|C00000002';tag='31';identity='31:0';body=[byte[]](1,0,0)},
        [pscustomobject]@{frame=21;ts_ns=[uint64]1000000100;dir='C';session=1;sid=[uint32]2;connection_key='1|0|S00000001|C00000002';tag='21';identity='21:0';body=[byte[]](1,0x78,0x56,0x34,0x12,0,0,0,0)},
        [pscustomobject]@{frame=30;ts_ns=[uint64]6000000000;dir='S';session=1;sid=[uint32]1;connection_key='1|0|S00000001|C00000002';tag='30';identity='30:0';body=[byte[]](2,0,0)},
        [pscustomobject]@{frame=31;ts_ns=[uint64]6000000100;dir='C';session=1;sid=[uint32]2;connection_key='1|0|S00000001|C00000002';tag='20';identity='20:0';body=[byte[]](2,0x21,0x43,0x65,0x87)}
    )
    $integrityFacts = [pscustomobject]@{
        events = $integrityEvents
        packets = @(
            [pscustomobject]@{dir='S';frame=20;session=1;sid=[uint32]1;connection_key='1|0|S00000001|C00000002';tags=@('16','31');tag_identities=@('16:0','31:0')},
            [pscustomobject]@{dir='S';frame=30;session=1;sid=[uint32]1;connection_key='1|0|S00000001|C00000002';tags=@('16','30');tag_identities=@('16:0','30:0')}
        )
    }
    $script:Failures.Clear()
    $null = Test-IntegrityStream $integrityFacts $case 'OR' `
        @{'01'=[uint32]0x12345678} @{'02'=[uint32]2271560481} 1250
    if ($script:Failures.Count -ne 0) {
        throw "integrity scoreboard/phase self-test rejected a valid alternating stream"
    }
    $script:Failures.Clear()
    $null = Test-IntegrityStream $integrityFacts $case 'OO' `
        @{'01'=[uint32]0x12345678} @{'02'=[uint32]2271560481} 1250
    if ($script:Failures.Count -ne 0) {
        throw "OO integrity self-test rejected a valid alternating stream"
    }
    $integrityEvents[1].body[1] = 0x79
    $script:Failures.Clear()
    $null = Test-IntegrityStream $integrityFacts $case 'OO' `
        @{'01'=[uint32]0x12345678} @{'02'=[uint32]2271560481} 1250
    if (@($script:Failures | Where-Object {
            $_.code -eq 'INTEGRITY_CRC_MISMATCH'
        }).Count -ne 1) {
        throw 'OO ammo-integrity CRC mutation self-test failed'
    }
    $integrityEvents[1].body[1] = 0x78
    $integrityEvents[3].body[1] = 0x22
    $script:Failures.Clear()
    $null = Test-IntegrityStream $integrityFacts $case 'OO' `
        @{'01'=[uint32]0x12345678} @{'02'=[uint32]2271560481} 1250
    if (@($script:Failures | Where-Object {
            $_.code -eq 'ENTITY_INTEGRITY_CRC_MISMATCH'
        }).Count -ne 1) {
        throw 'OO entity-integrity CRC mutation self-test failed'
    }
    $integrityEvents[3].body[1] = 0x21
    $integrityEvents[1].connection_key = '1|1|S00000003|C00000004'
    if (@(Get-IntegrityTransactions $integrityFacts)[0].valid) {
        throw "cross-SID integrity reply pairing self-test failed"
    }
    $integrityEvents[1].connection_key = '1|0|S00000001|C00000002'
    $integrityEvents[0].identity = '31:1'
    $integrityFacts.packets[0].tag_identities = @('16:0','31:1')
    $script:Failures.Clear()
    $null = Test-IntegrityStream $integrityFacts $case 'OR' `
        @{'01'=[uint32]0x12345678} @{'02'=[uint32]2271560481} 1250
    if (@($script:Failures | Where-Object { $_.code -eq 'INTEGRITY_UNEXERCISED' }).Count -ne 1) {
        throw "high-table integrity identity negative self-test failed"
    }
    $integrityEvents[0].identity = '31:0'
    $integrityFacts.packets[0].tag_identities = @('16:0','31:0')
    $integrityFacts.packets[0].tags = @('31')
    $integrityFacts.packets[0].tag_identities = @('31:0')
    $script:Failures.Clear()
    $null = Test-IntegrityStream $integrityFacts $case 'OR' `
        @{'01'=[uint32]0x12345678} @{'02'=[uint32]2271560481} 1250
    if ($script:Failures.Count -ne 1 -or
            $script:Failures[0].code -ne 'INTEGRITY_SCOREBOARD_ORDER') {
        throw "integrity scoreboard ordering self-test failed"
    }

    $datagramFacts = ConvertFrom-ParityLines -Lines @(
        'PARITY_DATAGRAM frame=1 ts_ns=1000000000 src=32786 dst=32787 len=12 outer=1 opcode=0x41 class=client_hello decode=1',
        'PARITY_DATAGRAM frame=2 ts_ns=1000000100 src=32786 dst=32787 len=32 outer=1 opcode=0x43 class=client_protocol decode=1'
    )
    if ($datagramFacts.datagrams.Count -ne 2 -or
            $datagramFacts.datagrams[1].class -cne 'client_protocol') {
        throw 'raw datagram parser positive self-test failed'
    }
    $badClassificationRejected = $false
    try {
        $null = ConvertFrom-ParityLines -Lines @(
            'PARITY_DATAGRAM frame=1 ts_ns=1000000000 src=32786 dst=32787 len=12 outer=1 opcode=0x41 class=unknown decode=0')
    } catch { $badClassificationRejected = $true }
    if (-not $badClassificationRejected) {
        throw 'raw datagram classification mutation self-test failed'
    }
    $rawContractFacts = [pscustomobject]@{
        comparison_datagrams=$datagramFacts.datagrams
        comparison_packets=@([pscustomobject]@{frame=2})
    }
    $script:Failures.Clear()
    if (-not (Test-RawDatagramContract $rawContractFacts 'raw' 'RR') -or
            $script:Failures.Count -ne 0) {
        throw 'raw datagram accounting positive self-test failed'
    }
    $datagramFacts.datagrams[1].decode = $false
    $script:Failures.Clear()
    $null = Test-RawDatagramContract $rawContractFacts 'raw' 'OR'
    if (@($script:Failures | Where-Object {
            $_.code -eq 'RAW_DATAGRAM_DECODE_FAILED'
        }).Count -ne 1) {
        throw 'raw datagram invisible-decode mutation self-test failed'
    }
    $datagramFacts.datagrams[1].decode = $true

    $packetGold = @()
    $packetSubject = @()
    foreach ($i in 1..20) {
        $records = if (($i % 5) -eq 0) { 0 } else { 1 }
        $packet = [pscustomobject]@{
            dir='C';frame=$i;ts_ns=[uint64](1000000000 + (100000000 * $i))
            flags='0x01';records=$records
        }
        $packetGold += $packet
        $packetSubject += [pscustomobject]@{
            dir=$packet.dir;frame=$packet.frame;ts_ns=$packet.ts_ns
            flags=$packet.flags;records=$packet.records
        }
    }
    foreach ($i in 21..26) {
        $packetSubject += [pscustomobject]@{
            dir='C';frame=$i;ts_ns=[uint64](1000000000 + (100000000 * $i))
            flags='0x01';records=0
        }
    }
    $packetGoldFacts = [pscustomobject]@{
        comparison_packets=$packetGold;comparison_duration_ms=3000.0
    }
    $packetSubjectFacts = [pscustomobject]@{
        comparison_packets=$packetSubject;comparison_duration_ms=3000.0
    }
    $packetPolicy = [pscustomobject]@{
        count_absolute_tolerance=1;count_relative_tolerance=0.20
        gameplay_distribution_min_samples=8
        gameplay_distribution_min_share=0.02
        gameplay_distribution_absolute_tolerance=0.08
        gameplay_distribution_relative_tolerance=0.50
    }
    $script:Failures.Clear()
    Compare-SessionPacketEnvelope $case 'RO' 'C' `
        $packetGoldFacts $packetSubjectFacts $packetPolicy
    if (@($script:Failures | Where-Object {
            $_.code -eq 'SESSION_PACKET_CADENCE'
        }).Count -lt 1) {
        throw 'header/ACK-only packet insertion mutation self-test failed'
    }

    $newCrossCaptureFacts = {
        param([uint32]$ServerAck)
        [pscustomobject]@{
            steady_started_ns=[uint64]1000000000
            steady_completed_ns=[uint64]4000000000
            steady_packets=@(
                [pscustomobject]@{
                    dir='C';ts_ns=[uint64]2000000000;sid=[uint32]0x11111111
                    sequence=[uint32]7;ack=[uint32]6;flags='0x01';records=0;wire_key=''
                },
                [pscustomobject]@{
                    dir='S';ts_ns=[uint64]2100000000;sid=[uint32]0x22222222
                    sequence=[uint32]8;ack=$ServerAck;flags='0x01';records=1
                    wire_key='0a:0|raw=20|len=1|skip=-'
                }
            )
        }
    }
    $crossPrimary = & $newCrossCaptureFacts ([uint32]7)
    $crossExternal = & $newCrossCaptureFacts ([uint32]7)
    $script:Failures.Clear()
    if (-not (Test-ExternalCaptureCrossCheck $crossPrimary $crossExternal `
                'cross-capture' 'RR') -or $script:Failures.Count -ne 0) {
        throw 'external capture packet-identity cross-check positive self-test failed'
    }
    $crossExternal.steady_packets[1].ack = [uint32]6
    $script:Failures.Clear()
    $null = Test-ExternalCaptureCrossCheck $crossPrimary $crossExternal `
        'cross-capture' 'RR'
    if ($script:Failures.Count -ne 1 -or
            $script:Failures[0].code -ne 'EXTERNAL_CAPTURE_WIRE_MISMATCH') {
        throw 'external capture packet-identity mutation self-test failed'
    }

    $rrEntityRows = @(
        [pscustomobject]@{valid=$true;row=2},
        [pscustomobject]@{valid=$true;row=5})
    $subjectEntityRows = @([pscustomobject]@{valid=$true;row=2})
    $script:Failures.Clear()
    Compare-SameCaseEntityIntegrityRows 'entity-row' 'OO' `
        $rrEntityRows $subjectEntityRows
    if ($script:Failures.Count -ne 1 -or
            $script:Failures[0].code -ne 'HOST_DYNAMIC_ENTITY_ROW_MISMATCH') {
        throw 'same-case 0x30 entity-row mutation self-test failed'
    }
    $script:Failures.Clear()
    Write-Host "PARITY_SELF_TEST=PASS"
}

if ($SelfTest) {
    try {
        Invoke-VerifierSelfTest
        exit 0
    } catch {
        Write-Error "PARITY_SELF_TEST=FAIL $($_.Exception.Message)`n$($_.ScriptStackTrace)"
        exit 1
    }
}

if ([string]::IsNullOrWhiteSpace($Manifest)) {
    Write-Error "-Manifest is required unless -SelfTest is used."
    exit 2
}
if (-not (Test-Path -LiteralPath $Manifest -PathType Leaf)) {
    Write-Error "Manifest not found: $Manifest"
    exit 2
}
$Manifest = (Resolve-Path -LiteralPath $Manifest).Path
$manifestDir = Split-Path -Parent $Manifest
if ([string]::IsNullOrWhiteSpace($Output)) { $Output = "$Manifest.parity.json" }
$Output = Resolve-ManifestPath $manifestDir $Output
if (Test-Path -LiteralPath $Output) {
    if (-not $Force) {
        Write-Error "Output already exists; use a create-new path or -Force: $Output"
        exit 2
    }
}
$outputParent = Split-Path -Parent $Output
if (-not (Test-Path -LiteralPath $outputParent -PathType Container)) {
    Write-Error "Output parent directory does not exist: $outputParent"
    exit 2
}

$manifestHash = (Get-FileHash -LiteralPath $Manifest -Algorithm SHA256).Hash.ToLowerInvariant()
$document = Get-Content -LiteralPath $Manifest -Raw | ConvertFrom-Json
$nwpp = Find-NwPp
if (-not $nwpp) {
    Write-Error "nw_pp.exe was not found under build; build the Release target first."
    exit 2
}

$schema = [int](Get-Field $document "schema" 0)
if ($schema -ne 1) { Add-ParityFailure "MANIFEST_SCHEMA" "" "" "Expected manifest schema 1." }
$suiteId = "$(Get-Field $document 'suite_id' '')"
if ($suiteId -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$') {
    Add-ParityFailure 'SUITE_ID_INVALID' '' '' `
        'suite_id must be a nonempty canonical identifier.'
}
$corpus = Get-Field $document "corpus" $null
$corpusFingerprint = "$(Get-Field $corpus 'fingerprint' '')".ToLowerInvariant()
$declaredRepositorySourceState = Get-Field $corpus 'source_state' $null
$currentRepositorySourceState = $null
try {
    $currentRepositorySourceState = Get-NetHarnessRepositorySourceState `
        -RepoRoot (Get-RepoRoot)
}
catch {
    Add-ParityFailure 'REPOSITORY_SOURCE_STATE_UNAVAILABLE' '' '' `
        "Could not bind the current repository source state: $($_.Exception.Message)"
}
if ($null -ne $currentRepositorySourceState) {
    $null = Test-RepositorySourceStateBinding `
        $declaredRepositorySourceState $currentRepositorySourceState
}
if ([string]::IsNullOrWhiteSpace("$(Get-Field $corpus 'expansion' '')")) {
    Add-ParityFailure "EXPANSION_UNPINNED" "" "" "corpus.expansion must be pinned."
}
$artifactReports = [System.Collections.Generic.List[object]]::new()
$artifactPaths = @{}
$artifactLines = [System.Collections.Generic.List[string]]::new()
$artifacts = @((Get-Field $corpus 'artifacts' @()))
foreach ($artifact in $artifacts) {
    $role = "$(Get-Field $artifact 'role' '')".Trim().ToLowerInvariant()
    if ($role -notmatch '^[a-z][a-z0-9_-]*$' -or $artifactPaths.ContainsKey($role)) {
        Add-ParityFailure "CORPUS_ARTIFACT_ROLE" "" "" `
            "Corpus artifact roles must be unique lowercase identifiers."
        continue
    }
    $artifactPath = Resolve-ManifestPath $manifestDir "$(Get-Field $artifact 'path' '')"
    if (-not (Test-Path -LiteralPath $artifactPath -PathType Leaf)) {
        Add-ParityFailure "CORPUS_ARTIFACT_MISSING" "" "" `
            "Corpus artifact '$role' is missing: $artifactPath"
        continue
    }
    $actualArtifactHash = (Get-FileHash -LiteralPath $artifactPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $declaredArtifactHash = Normalize-Sha256 "$(Get-Field $artifact 'sha256' '')"
    if ($declaredArtifactHash -notmatch '^[0-9a-f]{64}$' -or
            $declaredArtifactHash -eq ('0' * 64) -or $declaredArtifactHash -ne $actualArtifactHash) {
        Add-ParityFailure "CORPUS_ARTIFACT_HASH" "" "" `
            "Corpus artifact '$role' SHA-256 does not match the actual file."
    }
    $artifactPaths[$role] = $artifactPath
    $artifactLines.Add("$role=$actualArtifactHash")
    $artifactReports.Add([pscustomobject]@{ role=$role; sha256=$actualArtifactHash })
}
foreach ($requiredRole in @(
    'items','ammo','retail_exe','hook','resource','localres','language','med',
    'expansion_pff','expansion_language_pff','expansion_bin','opennova_runtime',
    'godot_launcher','godot_runtime','nw_pp','lan_probe','onhook_mcp','parity_runner',
    'matrix_runner','manifest_generator','auto_joiner','capture_script',
    'host_script','join_script','net_lib','retail_launcher','verifier',
    'manifest_template','corpus_exporter','retail_input_script','wire_ready_script',
    'server_cfg_template','client_cfg_template'
)) {
    if (-not $artifactPaths.ContainsKey($requiredRole)) {
        Add-ParityFailure "CORPUS_ARTIFACT_REQUIRED" "" "" `
            "Corpus artifacts must include a hash-bound '$requiredRole' file."
    }
}
$null = Test-CanonicalMatrixPolicy $document $artifactPaths
$expectedRepoArtifactPaths = [ordered]@{
    parity_runner='scripts\net\run_parity_topology.ps1'
    matrix_runner='scripts\net\run_parity_matrix.ps1'
    manifest_generator='scripts\net\generate_parity_manifest.ps1'
    auto_joiner='godot\tests\net\parity_joiner_driver.gd'
    capture_script='scripts\net\capture.ps1'
    host_script='scripts\net\host_opennova.ps1'
    join_script='scripts\net\join_opennova.ps1'
    net_lib='scripts\net\lib.ps1'
    retail_launcher='scripts\net\launch_retail_cfg.ps1'
    verifier='scripts\net\verify_parity_matrix.ps1'
    manifest_template='scripts\net\parity-matrix.example.json'
    corpus_exporter='scripts\net\export_parity_corpus.py'
    retail_input_script='scripts\net\exercise_retail_input.ps1'
    wire_ready_script='scripts\net\wait_parity_wire_ready.ps1'
    server_cfg_template='scripts\net\configs\retail-lan-server.onhook.cfg'
    client_cfg_template='scripts\net\configs\retail-lan-client.onhook.cfg'
    opennova_runtime='godot\bin\libopennova.windows.template_debug.x86_64.dll'
    lan_probe='build\apps\nw_lan_probe\Release\nw-lan-probe.exe'
}
foreach ($role in $expectedRepoArtifactPaths.Keys) {
    $expectedPath = [IO.Path]::GetFullPath((Join-Path `
        (Get-RepoRoot) $expectedRepoArtifactPaths[$role]))
    if ($artifactPaths.ContainsKey($role) -and
            -not ([IO.Path]::GetFullPath(
                [string]$artifactPaths[$role])).Equals(
                    $expectedPath, [StringComparison]::OrdinalIgnoreCase)) {
        Add-ParityFailure 'CORPUS_ARTIFACT_PATH' '' '' `
            "Corpus artifact '$role' must bind its current canonical repository path."
    }
}
$currentGodotLauncher = Find-GodotBinary
if (-not $currentGodotLauncher) {
    Add-ParityFailure "GODOT_EXECUTABLE_MISSING" "" "" `
        "The current Godot launcher executable could not be resolved."
} else {
    $currentGodotRuntime = Resolve-GodotRuntimeBinary -Path $currentGodotLauncher
    foreach ($binding in @(
        [pscustomobject]@{ Role='godot_launcher'; Path=$currentGodotLauncher },
        [pscustomobject]@{ Role='godot_runtime'; Path=$currentGodotRuntime }
    )) {
        if ($artifactPaths.ContainsKey($binding.Role) -and
                -not ([IO.Path]::GetFullPath([string] $artifactPaths[$binding.Role])).Equals(
                    [IO.Path]::GetFullPath([string] $binding.Path),
                    [StringComparison]::OrdinalIgnoreCase)) {
            Add-ParityFailure "GODOT_EXECUTABLE_PATH" "" "" `
                "Corpus artifact '$($binding.Role)' is not the executable selected by the LAN launcher."
        }
    }
}
$currentGodotArtifacts = Get-GodotRuntimeArtifacts -RepoRoot (Get-RepoRoot)
$declaredGodotRoles = @($artifactPaths.Keys | Where-Object {
    "$_" -like 'godot_resource_*'
} | Sort-Object)
$currentGodotRoles = @($currentGodotArtifacts.Keys | Sort-Object)
if (($declaredGodotRoles -join "`n") -ne ($currentGodotRoles -join "`n")) {
    Add-ParityFailure "GODOT_PROJECT_ARTIFACT_SET" "" "" `
        "Manifest Godot res:// artifact set differs from the project currently executed."
}
foreach ($role in @($currentGodotRoles | Where-Object { $artifactPaths.ContainsKey($_) })) {
    $declaredPath = [IO.Path]::GetFullPath([string] $artifactPaths[$role])
    $currentPath = [IO.Path]::GetFullPath([string] $currentGodotArtifacts[$role])
    if (-not $declaredPath.Equals($currentPath, [StringComparison]::OrdinalIgnoreCase)) {
        Add-ParityFailure "GODOT_PROJECT_ARTIFACT_PATH" "" "" `
            "Godot res:// artifact role '$role' is bound to the wrong path."
    }
}
$fingerprintSourceHash = if ($null -ne $currentRepositorySourceState) {
    [string]$currentRepositorySourceState.sha256
} else {
    [string](Get-Field $declaredRepositorySourceState 'sha256' '')
}
$artifactLines.Add("repo_source_state=$fingerprintSourceHash")
$artifactCanonical = ((@($artifactLines.ToArray()) | Sort-Object) -join "`n") + "`n"
$derivedCorpusFingerprint = Get-StringSha256 $artifactCanonical
if ($corpusFingerprint -notmatch '^[0-9a-f]{64}$' -or
        $corpusFingerprint -eq ('0' * 64) -or $corpusFingerprint -ne $derivedCorpusFingerprint) {
    Add-ParityFailure "CORPUS_FINGERPRINT_MISMATCH" "" "" `
        "corpus.fingerprint must equal the derived artifact inventory SHA-256 $derivedCorpusFingerprint."
}
$itemsPath = if ($artifactPaths.ContainsKey('items')) { "$($artifactPaths['items'])" } else { "" }
if ($artifactPaths.ContainsKey('nw_pp') -and
        -not ([IO.Path]::GetFullPath($nwpp)).Equals(
            [IO.Path]::GetFullPath([string] $artifactPaths['nw_pp']),
            [StringComparison]::OrdinalIgnoreCase)) {
    Add-ParityFailure "DECODER_ARTIFACT_MISMATCH" "" "" `
        "Verifier selected a different nw_pp executable than the hash-bound corpus artifact."
}
$policy = Get-Field $document "comparison" ([pscustomobject]@{})
$countAbsoluteTolerance = [double](Get-Field $policy 'count_absolute_tolerance' 1)
$countRelativeTolerance = [double](Get-Field $policy 'count_relative_tolerance' 0.20)
$timingTolerancePolicy = [double](Get-Field $policy 'timing_tolerance_ms' 1250)
$semanticDiversityPolicy = [double](Get-Field $policy 'semantic_diversity_ratio' 0.25)
$spatialBaselinePolicy = [double](Get-Field $policy 'spatial_baseline_tolerance_fixed16' 4194304)
$dynamicScalePolicy = [double](Get-Field $policy 'dynamic_scale_ratio_limit' 8.0)
$dynamicCyclePolicy = [double](Get-Field $policy 'dynamic_short_cycle_limit' 0.60)
$dynamicMinSamplesPolicy = [int](Get-Field $policy 'dynamic_min_samples' 8)
$gameplayMinSamplesPolicy = [int](Get-Field $policy 'gameplay_distribution_min_samples' 8)
$gameplayMinSharePolicy = [double](Get-Field $policy 'gameplay_distribution_min_share' 0.02)
$gameplayAbsolutePolicy = [double](Get-Field $policy 'gameplay_distribution_absolute_tolerance' 0.08)
$gameplayRelativePolicy = [double](Get-Field $policy 'gameplay_distribution_relative_tolerance' 0.50)
if ($countAbsoluteTolerance -lt 0 -or $countAbsoluteTolerance -gt 2 -or
        $countRelativeTolerance -lt 0 -or $countRelativeTolerance -gt 0.20 -or
        $timingTolerancePolicy -lt 0 -or $timingTolerancePolicy -gt 1250 -or
        $semanticDiversityPolicy -lt 0.25 -or $semanticDiversityPolicy -gt 1.0 -or
        $spatialBaselinePolicy -lt 0 -or $spatialBaselinePolicy -gt 4194304 -or
        $dynamicScalePolicy -lt 1.0 -or $dynamicScalePolicy -gt 8.0 -or
        $dynamicCyclePolicy -lt 0 -or $dynamicCyclePolicy -gt 0.60 -or
        $dynamicMinSamplesPolicy -lt 2 -or $dynamicMinSamplesPolicy -gt 8 -or
        $gameplayMinSamplesPolicy -lt 2 -or $gameplayMinSamplesPolicy -gt 8 -or
        $gameplayMinSharePolicy -lt 0 -or $gameplayMinSharePolicy -gt 0.02 -or
        $gameplayAbsolutePolicy -lt 0 -or $gameplayAbsolutePolicy -gt 0.08 -or
        $gameplayRelativePolicy -lt 0 -or $gameplayRelativePolicy -gt 0.50) {
    Add-ParityFailure "COMPARISON_POLICY_WEAK" "" "" `
        "Comparison tolerances exceed the acceptance bounds for counts, timing, semantic diversity, dynamic state, or steady gameplay distributions."
}
$requiredTopologies = @("RR", "RO", "OR", "OO")
$requiredCaseIds = @((Get-Field $document "required_case_ids" @()) | ForEach-Object { "$_" })
$cases = @((Get-Field $document "cases" @()))
if ($requiredCaseIds.Count -eq 0) {
    Add-ParityFailure "REQUIRED_CASES_MISSING" "" "" "Manifest must enumerate required_case_ids."
}
$actualCaseIds = @($cases | ForEach-Object { "$(Get-Field $_ 'id' '')" })
if (@($actualCaseIds | Select-Object -Unique).Count -ne $actualCaseIds.Count) {
    Add-ParityFailure "DUPLICATE_CASE" "" "" "Case ids must be unique."
}
foreach ($caseId in $requiredCaseIds) {
    if ($actualCaseIds -notcontains $caseId) {
        Add-ParityFailure "CASE_MISSING" $caseId "" "Required case is absent from the manifest."
    }
}
foreach ($caseId in $actualCaseIds) {
    if ($requiredCaseIds -notcontains $caseId) {
        Add-ParityFailure "CASE_UNDECLARED" $caseId "" "Case is not listed in required_case_ids."
    }
}
$minimumMissions = [int](Get-Field $document "minimum_distinct_missions" 2)
$minimumGameTypes = [int](Get-Field $document "minimum_distinct_game_types" 2)
$minimumPorts = [int](Get-Field $document "minimum_distinct_ports" 2)
$minimumInputCases = [int](Get-Field $document "minimum_exercised_input_cases" 2)
$minimumInMatchCases = [int](Get-Field $document "minimum_in_match_cases" 1)
$minimumDeployHoldCases = [int](Get-Field $document "minimum_deploy_hold_cases" 1)
if ($minimumMissions -lt 4 -or $minimumGameTypes -lt 4 -or
        $minimumPorts -ne 4) {
    Add-ParityFailure "MATRIX_POLICY_WEAK" "" "" `
        "The matrix must require four distinct missions, four game types, and all four retail automation ports."
}
if (@($cases | ForEach-Object { "$(Get-Field $_ 'mission' '')" } | Select-Object -Unique).Count -lt $minimumMissions) {
    Add-ParityFailure "MISSION_DIVERSITY" "" "" "Matrix does not meet minimum_distinct_missions=$minimumMissions."
}
$pinnedGameTypes = @($cases | Where-Object {
    Test-UInt32JsonField $_ 'game_type'
} | ForEach-Object { [uint32]$_.game_type } | Select-Object -Unique)
if ($pinnedGameTypes.Count -lt $minimumGameTypes) {
    Add-ParityFailure "GAME_TYPE_DIVERSITY" "" "" "Matrix does not meet minimum_distinct_game_types=$minimumGameTypes."
}
$pinnedPorts = @($cases | ForEach-Object { [int](Get-Field $_ 'port' 0) } |
    Where-Object { Test-RetailAutomationPort $_ } | Select-Object -Unique)
if ($pinnedPorts.Count -lt $minimumPorts) {
    Add-ParityFailure "PORT_DIVERSITY" "" "" `
        "Matrix does not meet minimum_distinct_ports=$minimumPorts within the retail automation range $($script:RetailAutomationPortMin)-$($script:RetailAutomationPortMax)."
}
$exercisedCases = @($cases | Where-Object {
    (Get-Field $_ 'exercise_input' $false) -is [bool] -and
        [bool](Get-Field $_ 'exercise_input' $false)
})
if ($minimumInputCases -lt 3 -or $exercisedCases.Count -lt $minimumInputCases) {
    Add-ParityFailure "INPUT_CASE_COVERAGE" "" "" `
        "Matrix has $($exercisedCases.Count) exercised-input cases; policy requires at least $minimumInputCases (minimum allowed 3)."
}
$inMatchCases = @($cases | Where-Object {
    "$(Get-Field $_ 'readiness_mode' '')" -eq 'in_match'
})
$deployHoldCases = @($cases | Where-Object {
    "$(Get-Field $_ 'readiness_mode' '')" -eq 'deploy_hold'
})
if ($minimumInMatchCases -lt 3 -or $minimumDeployHoldCases -lt 1 -or
        $inMatchCases.Count -lt $minimumInMatchCases -or
        $deployHoldCases.Count -lt $minimumDeployHoldCases) {
    Add-ParityFailure "READINESS_MODE_COVERAGE" "" "" `
        "Matrix has $($inMatchCases.Count) in_match and $($deployHoldCases.Count) deploy_hold cases; policy requires $minimumInMatchCases and $minimumDeployHoldCases."
}

$decodedCases = @{}
$seenCapturePaths = @{}
$seenCaptureHashes = @{}
$seenExternalCapturePaths = @{}
$seenExternalCaptureHashes = @{}
$seenRunIds = @{}
$seenMissionHashes = @{}
foreach ($case in $cases) {
    $caseId = "$(Get-Field $case 'id' '')"
    $caseMissionArtifact = Get-Field $case 'mission_artifact' ([pscustomobject]@{})
    $caseMissionPathValue = "$(Get-Field $caseMissionArtifact 'path' '')"
    $caseMissionHash = Test-MissionArtifactBinding `
        "$(Get-Field $case 'mission' '')" $caseMissionPathValue `
        "$(Get-Field $caseMissionArtifact 'sha256' '')" "" $caseId ""
    $caseMissionPath = Resolve-ManifestPath $manifestDir $caseMissionPathValue
    $caseMissionVerified = $false
    $caseMissionIdentity = $null
    if ([string]::IsNullOrWhiteSpace($caseMissionPath) -or
            -not (Test-Path -LiteralPath $caseMissionPath -PathType Leaf)) {
        Add-ParityFailure "MISSION_ARTIFACT_MISSING" $caseId "" `
            "Case mission BMS is missing: $caseMissionPath"
    } else {
        $actualCaseMissionHash = (Get-FileHash -LiteralPath $caseMissionPath -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($caseMissionHash -ne $actualCaseMissionHash) {
            Add-ParityFailure "MISSION_ARTIFACT_HASH" $caseId "" `
                "Case mission SHA-256 does not match the actual BMS bytes."
        } elseif ($caseMissionHash -match '^[0-9a-f]{64}$' -and
                $caseMissionHash -ne ('0' * 64)) {
            $caseMissionVerified = Register-UniqueMissionHash `
                $seenMissionHashes $caseMissionHash $caseId `
                "$(Get-Field $case 'mission' '')"
            try {
                $caseMissionIdentity = Get-BmsHeaderIdentity -Path $caseMissionPath
            } catch {
                Add-ParityFailure "MISSION_HEADER_INVALID" $caseId "" `
                    $_.Exception.Message
            }
        }
    }
    if ((Get-Field $case 'maintenance' $false) -isnot [bool]) {
        Add-ParityFailure "CASE_METADATA_TYPE" $caseId "" `
            "maintenance must be a JSON boolean."
    }
    if ((Get-Field $case 'exercise_input' $null) -isnot [bool] -or
            (Get-Field $case 'require_analog' $false) -isnot [bool]) {
        Add-ParityFailure "CASE_METADATA_TYPE" $caseId "" `
            "exercise_input and require_analog must be JSON booleans."
    }
    if (-not (Test-UInt32JsonField $case 'game_type')) {
        Add-ParityFailure "GAME_TYPE_UNPINNED" $caseId "" `
            "Case game_type must be an explicit unsigned 32-bit numeric value; Deathmatch zero is valid."
    }
    if ("$(Get-Field $case 'resolution' '')" -notmatch '^[1-9][0-9]*x[1-9][0-9]*$') {
        Add-ParityFailure "RESOLUTION_UNPINNED" $caseId "" "Case resolution must be WIDTHxHEIGHT."
    }
    $hostSessionName = "$(Get-Field $case 'host_session_name' '')"
    $serverName = "$(Get-Field $case 'server_name' '')"
    $hostCallsign = "$(Get-Field $case 'host_callsign' '')"
    $readinessMode = "$(Get-Field $case 'readiness_mode' '')"
    if ([string]::IsNullOrWhiteSpace($hostSessionName) -or
            $hostSessionName.Length -gt 31 -or $hostSessionName -notmatch '^[\x20-\x7e]+$') {
        Add-ParityFailure "SESSION_NAME_UNPINNED" $caseId "" `
            "host_session_name must be 1..31 printable ASCII bytes and may retain significant trailing spaces."
    } elseif (-not $hostSessionName.Equals($serverName, [StringComparison]::Ordinal)) {
        Add-ParityFailure "SERVER_SESSION_NAME_MISMATCH" $caseId "" `
            "host_session_name must exactly mirror the explicit server_name wire binding."
    }
    if (-not $serverName.Equals("Untitled ", [StringComparison]::Ordinal)) {
        Add-ParityFailure "SERVER_NAME_UNPINNED" $caseId "" `
            "server_name must be exactly 'Untitled ' including its significant trailing space."
    } elseif ($serverName.Equals($hostCallsign, [StringComparison]::OrdinalIgnoreCase)) {
        Add-ParityFailure "SESSION_CALLSIGN_CONFLATED" $caseId "" `
            "server_name must remain distinct from the host player callsign."
    }
    if ($readinessMode -notin @('in_match','deploy_hold')) {
        Add-ParityFailure "READINESS_MODE_INVALID" $caseId "" `
            "readiness_mode must be exactly in_match or deploy_hold."
    }
    if ($readinessMode -eq 'deploy_hold' -and
            ([bool](Get-Field $case 'exercise_input' $false) -or
                "$(Get-Field $case 'control_mode' 'observe')" -ne 'forbidden')) {
        Add-ParityFailure "DEPLOY_HOLD_POLICY_INVALID" $caseId "" `
            "deploy_hold must disable exercised input and forbid in-match control quartets."
    }
    $casePort = [int](Get-Field $case "port" 0)
    if ($casePort -lt 1 -or $casePort -gt 65535) {
        Add-ParityFailure "PORT_UNPINNED" $caseId "" "Case port must be from 1 through 65535."
    } elseif (-not (Test-RetailAutomationPort $casePort)) {
        Add-ParityFailure "PORT_OUTSIDE_RETAIL_AUTOMATION" $caseId "" `
            "Case port must be in the current onHook retail LAN automation range $($script:RetailAutomationPortMin)-$($script:RetailAutomationPortMax)."
    }
    $controlMode = "$(Get-Field $case 'control_mode' 'observe')"
    if (@('required','forbidden','observe') -notcontains $controlMode) {
        Add-ParityFailure "CONTROL_MODE_INVALID" $caseId "" `
            "control_mode must be required, forbidden, or observe."
    }
    if ($controlMode -eq 'required' -and -not [bool](Get-Field $case 'maintenance' $false)) {
        Add-ParityFailure "CONTROL_MODE_INVALID" $caseId "" `
            "control_mode=required needs maintenance=true."
    }
    if ([bool](Get-Field $case 'maintenance' $false) -and
            [int](Get-Field $case 'minimum_integrity_events' 4) -lt 4) {
        Add-ParityFailure "MAINTENANCE_POLICY_WEAK" $caseId "" `
            "Maintenance cases must require at least four alternating integrity events."
    }
    if ($controlMode -eq 'required' -and
            [int](Get-Field $case 'minimum_control_quartets' 4) -lt 4) {
        Add-ParityFailure "MAINTENANCE_POLICY_WEAK" $caseId "" `
            "Required-control cases must require at least four control quartets."
    }
    $runs = Get-Field $case "runs" $null
    $factsByTopology = @{}
    $steadyWindows = @()
    foreach ($topology in $requiredTopologies) {
        if ($null -eq $runs -or -not (Test-Field $runs $topology)) {
            Add-ParityFailure "TOPOLOGY_MISSING" $caseId $topology "Case lacks required topology $topology."
            continue
        }
        $run = Get-Field $runs $topology $null
        if ("$(Get-Field $run 'readiness_mode' '')" -cne $readinessMode) {
            Add-ParityFailure "READINESS_MODE_MISMATCH" $caseId $topology `
                "Run readiness_mode does not exactly match its enclosing case."
        }
        $runId = "$(Get-Field $run 'run_id' '')"
        if (-not [string]::IsNullOrWhiteSpace($runId)) {
            if ($seenRunIds.ContainsKey($runId)) {
                Add-ParityFailure 'RUN_ID_REUSED' $caseId $topology `
                    "run_id is already bound to $($seenRunIds[$runId])."
            } else {
                $seenRunIds[$runId] = "$caseId/$topology"
            }
        }
        $steadyWindows += [int](Get-Field $run "steady_seconds" 0)
        $declaredCapture = Resolve-ManifestPath $manifestDir "$(Get-Field $run 'capture' '')"
        $captureKey = $declaredCapture.ToLowerInvariant()
        if ($seenCapturePaths.ContainsKey($captureKey)) {
            Add-ParityFailure "CAPTURE_REUSED" $caseId $topology `
                "Every topology/case needs its own capture; this path is already bound to $($seenCapturePaths[$captureKey])."
        } else {
            $seenCapturePaths[$captureKey] = "$caseId/$topology"
        }
        $declaredExternalCapture = Resolve-ManifestPath $manifestDir `
            "$(Get-Field $run 'external_capture' '')"
        $externalCaptureKey = $declaredExternalCapture.ToLowerInvariant()
        if ($seenExternalCapturePaths.ContainsKey($externalCaptureKey)) {
            Add-ParityFailure 'EXTERNAL_CAPTURE_REUSED' $caseId $topology `
                "Every topology/case needs its own independent external pcapng; this path is already bound to $($seenExternalCapturePaths[$externalCaptureKey])."
        } else {
            $seenExternalCapturePaths[$externalCaptureKey] = "$caseId/$topology"
        }
        $facts = Get-RunFacts $run $case $corpus `
            $manifestDir $topology $nwpp $artifactPaths $suiteId $itemsPath
        if ($null -ne $facts) {
            $null = Register-UniqueCaptureHash $seenCaptureHashes $facts.sha256 $caseId $topology
            $null = Register-UniqueCaptureHash $seenExternalCaptureHashes `
                $facts.external_sha256 $caseId "$topology-external"
            $factsByTopology[$topology] = $facts
            if ($null -ne $caseMissionIdentity) {
                Test-WireIdentity $facts $case $topology $corpus $caseMissionIdentity
            }
        }
    }
    if (@($steadyWindows | Select-Object -Unique).Count -ne 1) {
        Add-ParityFailure "WINDOW_MISMATCH" $caseId "" `
            "All four topologies in a case must declare the same steady_seconds."
    }
    if ([bool](Get-Field $case 'exercise_input' $false)) {
        foreach ($topology in $requiredTopologies) {
            if ($factsByTopology.ContainsKey($topology)) {
                Test-ControllableInputWitness $factsByTopology[$topology] $case $topology
            }
        }
    }
    $decodedCases[$caseId] = [pscustomobject]@{
        case=$case
        facts=$factsByTopology
        mission_path=$caseMissionPath
        mission_sha256=$caseMissionHash
        mission_verified=$caseMissionVerified
    }
}
if ($seenMissionHashes.Count -lt $minimumMissions) {
    Add-ParityFailure "MISSION_BYTE_DIVERSITY" "" "" `
        "Matrix contains $($seenMissionHashes.Count) unique verified BMS byte images; require at least $minimumMissions."
}

# The RR captures form the only CRC oracle. This deliberately fails a mixed
# row that no RR capture witnessed instead of silently trusting implementation
# constants or a stale profile name.
$crcOracle = @{}
$entityCrcOracle = @{}
foreach ($caseId in $decodedCases.Keys) {
    $entry = $decodedCases[$caseId]
    if (-not $entry.facts.ContainsKey("RR")) { continue }
    foreach ($transaction in @(Get-IntegrityTransactions $entry.facts["RR"])) {
        if (-not $transaction.valid) { continue }
        $row = "{0:x2}" -f $transaction.row
        if ($crcOracle.ContainsKey($row) -and [uint32]$crcOracle[$row] -ne [uint32]$transaction.base_crc) {
            Add-ParityFailure "RR_ORACLE_CONFLICT" $caseId "RR" `
                "Retail-retail emitted conflicting CRCs for ammo row 0x$row."
        } else {
            $crcOracle[$row] = [uint32]$transaction.base_crc
        }
    }
    foreach ($transaction in @(Get-EntityIntegrityTransactions $entry.facts["RR"])) {
        if (-not $transaction.valid) { continue }
        $row = "{0:x2}" -f $transaction.row
        if ($entityCrcOracle.ContainsKey($row) -and
                [uint32]$entityCrcOracle[$row] -ne [uint32]$transaction.base_crc) {
            Add-ParityFailure "RR_ENTITY_ORACLE_CONFLICT" $caseId "RR" `
                "Retail-retail emitted conflicting checksums for ADM/entity row 0x$row."
        } else {
            $entityCrcOracle[$row] = [uint32]$transaction.base_crc
        }
    }
}
$minimumRows = [int](Get-Field $document "minimum_distinct_integrity_rows" 2)
if ($minimumRows -lt 2) {
    Add-ParityFailure "INTEGRITY_POLICY_WEAK" "" "" `
        "minimum_distinct_integrity_rows must be at least 2."
}
if ($crcOracle.Count -lt $minimumRows) {
    Add-ParityFailure "INTEGRITY_ROW_DIVERSITY" "" "" `
        "RR captures witnessed $($crcOracle.Count) ammo rows; require at least $minimumRows to guard against a fixed-row implementation."
}

$timingToleranceMs = [double](Get-Field $policy "timing_tolerance_ms" 1250)
foreach ($caseId in $decodedCases.Keys) {
    $entry = $decodedCases[$caseId]
    $case = $entry.case
    $charattrOracle = @()
    foreach ($topology in $requiredTopologies) {
        if (-not $entry.facts.ContainsKey($topology)) { continue }
        $facts = $entry.facts[$topology]
        $null = Test-OuterHandshakeContract $facts $case $topology
        Test-FramePhaseStream $facts $case $topology
        Test-AckStream $facts $case $topology
        $transactions = @(Test-IntegrityStream $facts $case $topology $crcOracle `
            $entityCrcOracle $timingToleranceMs)
        $charattrBases = @(Test-ControlStream $facts $case $topology `
            $timingToleranceMs $charattrOracle)
        if ($topology -eq 'RR') {
            $charattrOracle = @($charattrBases | Select-Object -Unique)
        }
        if ($topology -in @("OR", "OO") -and $entry.facts.ContainsKey("RR")) {
            $rrRows = @((Get-IntegrityTransactions $entry.facts["RR"]) |
                Where-Object { $_.valid } | ForEach-Object { $_.row } | Select-Object -Unique | Sort-Object)
            $orRows = @($transactions | Where-Object { $_.valid } |
                ForEach-Object { $_.row } | Select-Object -Unique | Sort-Object)
            if (($rrRows -join ',') -ne ($orRows -join ',')) {
                Add-ParityFailure "HOST_DYNAMIC_ROW_MISMATCH" $caseId $topology `
                    "OpenNova host challenge rows do not match the same case's RR oracle rows."
            }
            Compare-SameCaseEntityIntegrityRows $caseId $topology `
                @(Get-EntityIntegrityTransactions $entry.facts['RR']) `
                @(Get-EntityIntegrityTransactions $facts)
        }
    }
    foreach ($subjectTopology in @('RO','OR','OO')) {
        if (-not $entry.facts.ContainsKey("RR") -or
                -not $entry.facts.ContainsKey($subjectTopology)) { continue }
        foreach ($direction in @('C','S')) {
            Compare-ParityDirection $case $subjectTopology $direction `
                $entry.facts["RR"] $entry.facts[$subjectTopology] $policy
            Compare-InitialPacketGrouping $case $subjectTopology $direction `
                $entry.facts["RR"] $entry.facts[$subjectTopology]
            Compare-InitialMaterialBodies $case $subjectTopology $direction `
                $entry.facts["RR"] $entry.facts[$subjectTopology]
            Compare-SteadyGameplayDistributions $case $subjectTopology $direction `
                $entry.facts["RR"] $entry.facts[$subjectTopology] $policy
            Compare-SessionPacketEnvelope $case $subjectTopology $direction `
                $entry.facts["RR"] $entry.facts[$subjectTopology] $policy
            Compare-SemanticDirection $case $subjectTopology $direction `
                $entry.facts["RR"] $entry.facts[$subjectTopology] $policy
        }
        Compare-OuterHandshakeParity $case $subjectTopology `
            $entry.facts['RR'] $entry.facts[$subjectTopology]
    }

    $runReports = @()
    foreach ($topology in $requiredTopologies) {
        if (-not $entry.facts.ContainsKey($topology)) { continue }
        $facts = $entry.facts[$topology]
        $runMetadata = Get-Field (Get-Field $case 'runs' ([pscustomobject]@{})) `
            $topology ([pscustomobject]@{})
        $runReports += [pscustomobject]@{
            topology = $topology
            capture = $facts.capture
            sha256 = $facts.sha256
            external_capture = $facts.external_capture
            external_sha256 = $facts.external_sha256
            mission_path = "$(Get-Field $runMetadata 'mission_path' '')"
            mission_sha256 = Normalize-Sha256 "$(Get-Field $runMetadata 'mission_sha256' '')"
            capture_stats_available = [bool](Get-Field $runMetadata 'capture_stats_available' $false)
            readiness_mode = "$(Get-Field $runMetadata 'readiness_mode' '')"
            wire_ready_witness = Get-Field $runMetadata 'wire_ready_witness' $null
            steady_started_utc = "$(Get-Field $runMetadata 'steady_started_utc' '')"
            steady_completed_utc = "$(Get-Field $runMetadata 'steady_completed_utc' '')"
            duration_ms = [Math]::Round($facts.duration_ms, 3)
            comparison_duration_ms = [Math]::Round(
                [double](Get-Field $facts 'comparison_duration_ms' $facts.duration_ms), 3)
            capture_duration_ms = [Math]::Round(
                [double](Get-Field $facts 'capture_duration_ms' $facts.duration_ms), 3)
            messages = $facts.events.Count
            packets = $facts.packets.Count
            semantic_states = $facts.states.Count
            entities = $facts.entities.Count
        }
    }
    $script:CaseReports.Add([pscustomobject]@{
        id = $caseId
        mission = "$(Get-Field $case 'mission' '')"
        mission_path = $entry.mission_path
        mission_sha256 = $entry.mission_sha256
        mission_verified = [bool]$entry.mission_verified
        game_type = [uint32](Get-Field $case "game_type" 0)
        maintenance = [bool](Get-Field $case "maintenance" $false)
        control_mode = "$(Get-Field $case 'control_mode' 'observe')"
        readiness_mode = "$(Get-Field $case 'readiness_mode' '')"
        exercise_input = [bool](Get-Field $case 'exercise_input' $false)
        require_analog = [bool](Get-Field $case 'require_analog' $false)
        rr_charattr_crc_oracle = @($charattrOracle | ForEach-Object { '0x{0:x8}' -f [uint32]$_ })
        runs = $runReports
    })
}

$motionReport = [System.Collections.Generic.List[object]]::new()
foreach ($topology in $requiredTopologies) {
    $uplinkMoving = 0
    $playerMoving = 0
    $vehicleMoving = 0
    foreach ($caseId in $decodedCases.Keys) {
        $entry = $decodedCases[$caseId]
        if (-not $entry.facts.ContainsKey($topology)) { continue }
        $facts = $entry.facts[$topology]
        $steadyStates = @(Get-FactCollection $facts 'steady_states' 'states')
        $steadyEntities = @(Get-FactCollection $facts 'steady_entities' 'entities')
        $uplinkMoving += Get-MovingHandleCount @($steadyStates | Where-Object {
            $_.kind -eq 'uplink' -and $_.decode
        })
        $playerMoving += Get-MovingHandleCount @($steadyEntities | Where-Object { $_.class -eq 'P' })
        $vehicleMoving += Get-MovingHandleCount @($steadyEntities | Where-Object { $_.class -eq 'V' })
    }
    $motionReport.Add([pscustomobject]@{
        topology = $topology
        moving_uplink_players = $uplinkMoving
        moving_s2c_players = $playerMoving
        moving_s2c_vehicles = $vehicleMoving
    })
    if ($uplinkMoving -lt 1 -or $playerMoving -lt 1 -or $vehicleMoving -lt 1) {
        Add-ParityFailure "MOTION_COVERAGE" "" $topology `
            "The suite must witness changing C2S player position plus changing S2C player and vehicle compact positions; saw uplink=$uplinkMoving player=$playerMoving vehicle=$vehicleMoving."
    }
}

$retentionLimitation = if ($script:RetentionWitnesses -eq 0) {
    "No same-sequence NACK replay was present, so this passive matrix does not capture-prove reliable/transient reconstruction; run the structural protocol-message test or supply a loss-induced witness."
} else {
    "Observed and validated $($script:RetentionWitnesses) same-sequence NACK replay witness(es)."
}
$occupiedMountCoverage = Get-OccupiedMountCoverageReport $decodedCases $requiredTopologies
$limitations = @(
    "Capture timing checks 311/62Hz and 744/62Hz boundaries plus the no-earlier-than-call-1861 control gate within the manifest tolerance; exact 311/1861/744 counters and process-global phase persistence remain structural native-test evidence."
    "Healthy lossless captures prove one grouped C2S 0x1C/0x08[/0x3D] reply per quartet and the 1,1,2,2 time-sync cycle; malformed/timing-invalid 0x08 silence reset and exact 3-percent host/client delta acceptance remain structural native-test evidence."
    $retentionLimitation
    "RO, OR, and OO are compared bidirectionally to same-case RR across both client-to-server and server-to-client traffic."
    "Canonical input has no deterministic, map-independent occupied-seat acquisition action. Phase-8 on-foot/bound observations are reported, but occupied-mount clip/reserve parity remains structural-only evidence in netsim_two_peer_fanout."
)
$result = [pscustomobject]@{
    schema = 1
    status = if ($script:Failures.Count -eq 0) { "pass" } else { "fail" }
    suite_id = "$(Get-Field $document 'suite_id' '')"
    manifest = $Manifest
    manifest_sha256 = $manifestHash
    nw_pp = $nwpp
    generated_utc = [DateTime]::UtcNow.ToString("o")
    corpus_fingerprint = $derivedCorpusFingerprint
    repository_source_state = $currentRepositorySourceState
    corpus_artifacts = @($artifactReports.ToArray())
    rr_crc_oracle = [pscustomobject]$crcOracle
    rr_entity_crc_oracle = [pscustomobject]$entityCrcOracle
    retention_witnesses = $script:RetentionWitnesses
    motion_coverage = @($motionReport.ToArray())
    occupied_mount_coverage = $occupiedMountCoverage
    cases = @($script:CaseReports.ToArray())
    failures = @($script:Failures.ToArray())
    limitations = $limitations
}
$json = $result | ConvertTo-Json -Depth 30
Set-Content -LiteralPath $Output -Value $json -Encoding utf8
Write-Host "PARITY_STATUS=$($result.status.ToUpperInvariant())"
Write-Host "PARITY_FAILURES=$($script:Failures.Count)"
Write-Host "PARITY_REPORT=$Output"
if ($script:Failures.Count -gt 0) {
    foreach ($failure in $script:Failures) {
        Write-Host "PARITY_FAILURE code=$($failure.code) case=$($failure.case_id) topology=$($failure.topology) message=$($failure.message)"
    }
    exit 1
}
exit 0
