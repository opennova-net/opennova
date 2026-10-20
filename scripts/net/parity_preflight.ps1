# Read-only readiness check for the retail-LAN parity suite (ADR 0050 R4): run
# it before run_parity_suite.ps1. It launches nothing and writes nothing in the
# repository or the retail copies; it reads the inputs a suite binds and says
# which one is not what a verdict-bearing suite needs.
#
#   powershell -File scripts\net\parity_preflight.ps1 `
#       -CorpusRoot C:\retail\corpus `
#       -RetailServerRoot C:\retail\jox01\SERVER -RetailClientRoot C:\retail\jox01\CLIENT `
#       -OnHookMcpPath C:\retail\onhook\onhook-mcp.exe `
#       -GodotLauncher C:\Godot\Godot_v4.6.1-stable_win64_console.exe `
#       -DllRunId <ci.yml run id> `
#       [-ExpectOnHookMcpSha256 <sha>] [-ExpectOnHookProxySha256 <sha>] `
#       [-ExpectGodotLauncherSha256 <sha>]
#
# Checks (one PARITY_PREFLIGHT line each, PASS or FAIL with the detail):
#   retail_exe      both Jointops.exe are the pinned jox01 reference (ADR 0050 d2)
#   onhook_proxy    both copies' binkw32.dll are one build, the one beside
#                   onhook-mcp.exe (the hook DLL and its companion exe come from
#                   one commit), and the pin when given
#   onhook_mcp      onhook-mcp.exe's hash (the pin when given)
#   bink_original   both copies hold binkw32_.dll, the original the proxy forwards to
#   server_cfg      the SERVER game.cfg game_name is the case table's
#                   host_session_name byte for byte, and every case port lies in
#                   its mplanserverportmin..max
#   corpus          items.def and every case mission match the case table's pins
#   godot_launcher  the Godot 4.6.1 console wrapper and the runtime beside it
#                   (the pin when given)
#   capture         dumpcap and the Npcap loopback adapter (detect_capture.ps1)
#   judges          opennova-wire, opennova-lan-probe and opennova-parity are
#                   built and newer than every tracked source they compile from
#   repo_clean      no uncommitted tracked change (a run is evidence for its commit)
#   gdextension     both template_debug DLLs in godot\bin are the CI artifact of
#                   run -DllRunId, whose commit's engine/ and godot/ trees equal
#                   HEAD's (the artifact is fetched to %TEMP%, never into the repo)
#   mcp_ports       the OpenNova MCP ports are free
#   case_ports      no process holds a case's UDP port
#   no_retail       no Jointops.exe is running
#
# Prints PARITY_PREFLIGHT_STATUS=PASS|FAIL; exit 0 PASS, 1 FAIL.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $CorpusRoot,

    [Parameter(Mandatory = $true)]
    [string] $RetailServerRoot,

    [Parameter(Mandatory = $true)]
    [string] $RetailClientRoot,

    [Parameter(Mandatory = $true)]
    [string] $OnHookMcpPath,

    [Parameter(Mandatory = $true)]
    [string] $GodotLauncher,

    [string] $CasesPath = "",

    # The ci.yml run whose gdext_windows_template_debug artifact the OpenNova
    # cells load.
    [string] $DllRunId = "",

    [string] $GitHubRepo = "opennova-net/opennova",

    [ValidatePattern('^(?:|[0-9a-f]{64})$')]
    [string] $ExpectOnHookMcpSha256 = "",

    [ValidatePattern('^(?:|[0-9a-f]{64})$')]
    [string] $ExpectOnHookProxySha256 = "",

    [ValidatePattern('^(?:|[0-9a-f]{64})$')]
    [string] $ExpectGodotLauncherSha256 = "",

    [ValidateRange(1, 65535)]
    [int] $HostMcpPort = 8975,

    [ValidateRange(1, 65535)]
    [int] $JoinerMcpPort = 8976
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$Repo = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
if (-not $CasesPath) { $CasesPath = Join-Path $PSScriptRoot "parity-cases.json" }
# The jox01 reference executable (ADR 0050 decision 2; run_parity_topology.ps1).
$PinnedJox01ExeSha256 = "9a1035440a53af2057ce0995ac42dced840d3b9fd53c04dc86041a962b84fe57"
$GodotConsoleName = "Godot_v4.6.1-stable_win64_console.exe"
$GodotRuntimeName = "Godot_v4.6.1-stable_win64.exe"
$DllNames = @(
    "libopennova.windows.template_debug.x86_64.dll",
    "libopennova_editor.windows.template_debug.x86_64.dll"
)
# The trees a GDExtension build and a source run's scripts come from.
$DllTrees = @("engine", "godot/src", "godot/game", "godot/probes", "godot/bin/opennova.gdextension")

$script:Failures = 0

function Write-Check {
    param([string] $Name, [bool] $Ok, [string] $Detail)
    if (-not $Ok) { $script:Failures++ }
    Write-Host ("PARITY_PREFLIGHT {0} {1} {2}" -f $Name, $(if ($Ok) { "PASS" } else { "FAIL" }), $Detail)
}

function Get-Sha256OrEmpty {
    param([string] $Path)
    if (-not $Path -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) { return "" }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Format-Hash {
    param([string] $Hash)
    if (-not $Hash) { return "absent" }
    return $Hash
}

# A native command's stdout and exit code; its stderr dropped (Windows
# PowerShell turns a redirected stderr line into a terminating error under Stop).
function Invoke-Native {
    param([string] $Exe, [string[]] $Arguments)
    $previous = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $output = @(& $Exe @Arguments 2>$null)
        $code = $LASTEXITCODE
    }
    finally { $ErrorActionPreference = $previous }
    return [pscustomobject]@{ Exit = $code; Output = @($output | ForEach-Object { "$_" }) }
}

$table = Get-Content -LiteralPath $CasesPath -Raw | ConvertFrom-Json

# --- the retail copies and the hook -------------------------------------------
$serverExe = Get-Sha256OrEmpty (Join-Path $RetailServerRoot "Jointops.exe")
$clientExe = Get-Sha256OrEmpty (Join-Path $RetailClientRoot "Jointops.exe")
Write-Check "retail_exe" ($serverExe -ceq $PinnedJox01ExeSha256 -and $clientExe -ceq $PinnedJox01ExeSha256) `
    "server=$(Format-Hash $serverExe) client=$(Format-Hash $clientExe) pinned=$PinnedJox01ExeSha256"

$serverProxy = Get-Sha256OrEmpty (Join-Path $RetailServerRoot "binkw32.dll")
$clientProxy = Get-Sha256OrEmpty (Join-Path $RetailClientRoot "binkw32.dll")
$pairProxy = Get-Sha256OrEmpty (Join-Path (Split-Path -Parent $OnHookMcpPath) "binkw32.dll")
$proxyOk = $serverProxy -and $serverProxy -ceq $clientProxy -and
    ($pairProxy -or $ExpectOnHookProxySha256) -and
    (-not $pairProxy -or $serverProxy -ceq $pairProxy) -and
    (-not $ExpectOnHookProxySha256 -or $serverProxy -ceq $ExpectOnHookProxySha256)
Write-Check "onhook_proxy" ([bool] $proxyOk) `
    ("server=$(Format-Hash $serverProxy) client=$(Format-Hash $clientProxy)" +
    " beside_mcp=$(Format-Hash $pairProxy) pinned=$(if ($ExpectOnHookProxySha256) { $ExpectOnHookProxySha256 } else { '-' })")

$mcp = Get-Sha256OrEmpty $OnHookMcpPath
Write-Check "onhook_mcp" ([bool] ($mcp -and (-not $ExpectOnHookMcpSha256 -or $mcp -ceq $ExpectOnHookMcpSha256))) `
    "sha256=$(Format-Hash $mcp) pinned=$(if ($ExpectOnHookMcpSha256) { $ExpectOnHookMcpSha256 } else { '-' })"

$serverBink = Get-Sha256OrEmpty (Join-Path $RetailServerRoot "binkw32_.dll")
$clientBink = Get-Sha256OrEmpty (Join-Path $RetailClientRoot "binkw32_.dll")
Write-Check "bink_original" ([bool] ($serverBink -and $serverBink -ceq $clientBink)) `
    "server=$(Format-Hash $serverBink) client=$(Format-Hash $clientBink)"

# --- the SERVER copy's game.cfg -------------------------------------------------
$cfgPath = Join-Path $RetailServerRoot "game.cfg"
$cfgOk = $false
$cfgDetail = "no game.cfg at the SERVER copy"
if (Test-Path -LiteralPath $cfgPath -PathType Leaf) {
    $nameMatch = Select-String -LiteralPath $cfgPath -Pattern '^\s*game_name\s*=\s*"([^"]*)"' |
        Select-Object -First 1
    $bounds = @{}
    foreach ($key in @("mplanserverportmin", "mplanserverportmax")) {
        $m = Select-String -LiteralPath $cfgPath -Pattern ('^\s*' + $key + '\s*=\s*"?(\d+)"?') |
            Select-Object -First 1
        if ($m) { $bounds[$key] = [int] $m.Matches[0].Groups[1].Value }
    }
    $gameName = if ($nameMatch) { $nameMatch.Matches[0].Groups[1].Value } else { $null }
    $outside = @()
    if ($bounds.Count -eq 2) {
        $outside = @($table.cases | Where-Object {
            [int] $_.port -lt $bounds["mplanserverportmin"] -or [int] $_.port -gt $bounds["mplanserverportmax"]
        } | ForEach-Object { "$($_.id):$($_.port)" })
    }
    $cfgOk = $null -ne $gameName -and
        [string]::Equals($gameName, [string] $table.host_session_name, [StringComparison]::Ordinal) -and
        $bounds.Count -eq 2 -and $outside.Count -eq 0
    $cfgDetail = ("game_name=" + $(if ($null -ne $gameName) { "'$gameName'" } else { "absent" }) +
        " expected='$($table.host_session_name)'" +
        " ports=$(if ($bounds.Count -eq 2) { "$($bounds['mplanserverportmin'])..$($bounds['mplanserverportmax'])" } else { 'absent' })" +
        " outside=$(if ($outside.Count) { $outside -join ',' } else { '-' })")
}
Write-Check "server_cfg" ([bool] $cfgOk) $cfgDetail

# --- the corpus -----------------------------------------------------------------
$corpusProblems = [System.Collections.Generic.List[string]]::new()
$items = Get-Sha256OrEmpty (Join-Path $CorpusRoot "items.def")
if ($items -cne [string] $table.items_sha256) { $corpusProblems.Add("items.def=$(Format-Hash $items)") }
foreach ($case in @($table.cases)) {
    $mission = Get-Sha256OrEmpty (Join-Path $CorpusRoot ("missions\" + [string] $case.mission))
    if ($mission -cne [string] $case.mission_sha256) {
        $corpusProblems.Add("$($case.mission)=$(Format-Hash $mission)")
    }
}
Write-Check "corpus" ($corpusProblems.Count -eq 0) `
    $(if ($corpusProblems.Count) { "off the pins: " + ($corpusProblems -join " ") } else {
        "items.def and $(@($table.cases).Count) missions match the pins" })

# --- Godot ------------------------------------------------------------------------
$launcher = Get-Sha256OrEmpty $GodotLauncher
$runtime = Get-Sha256OrEmpty (Join-Path (Split-Path -Parent $GodotLauncher) $GodotRuntimeName)
$launcherOk = $launcher -and $runtime -and
    [IO.Path]::GetFileName($GodotLauncher) -ceq $GodotConsoleName -and
    (-not $ExpectGodotLauncherSha256 -or $launcher -ceq $ExpectGodotLauncherSha256)
Write-Check "godot_launcher" ([bool] $launcherOk) `
    ("console=$(Format-Hash $launcher) runtime=$(Format-Hash $runtime)" +
    " pinned=$(if ($ExpectGodotLauncherSha256) { $ExpectGodotLauncherSha256 } else { '-' })")

# --- capture ----------------------------------------------------------------------
$detectRun = Invoke-Native "powershell.exe" @("-NoProfile", "-ExecutionPolicy", "Bypass",
    "-File", (Join-Path $PSScriptRoot "detect_capture.ps1"))
$detect = $detectRun.Output
$detectExit = $detectRun.Exit
$ready = @($detect | Where-Object { "$_" -like "CAPTURE_READY=*" } | Select-Object -Last 1)
$npcap = @($detect | Where-Object { "$_" -like "NPCAP_RUNNING=*" } | Select-Object -Last 1)
Write-Check "capture" ($detectExit -eq 0 -and $ready.Count -eq 1 -and "$($ready[0])" -ceq "CAPTURE_READY=loopback") `
    "detect_capture exit=$detectExit $(if ($ready.Count) { $ready[0] } else { 'CAPTURE_READY=?' }) $(if ($npcap.Count) { $npcap[0] } else { '' })"

# --- the native judges ------------------------------------------------------------
# A judge is current when it is newer than every tracked file of the trees it
# compiles from (its app, the app libraries it links, the engine groups).
$judges = @(
    [pscustomobject]@{ Name = "opennova-wire"; Dir = "wire"
        Trees = @("apps/wire", "engine/base", "engine/formats", "engine/net", "engine/runtime") },
    [pscustomobject]@{ Name = "opennova-lan-probe"; Dir = "lan_probe"
        Trees = @("apps/lan_probe", "apps/common", "engine/base", "engine/formats", "engine/net") },
    [pscustomobject]@{ Name = "opennova-parity"; Dir = "parity"
        Trees = @("apps/parity", "apps/wire", "engine/base", "engine/formats", "engine/net") }
)
$judgeProblems = [System.Collections.Generic.List[string]]::new()
foreach ($judge in $judges) {
    $exe = Join-Path $Repo "build\apps\$($judge.Dir)\Release\$($judge.Name).exe"
    if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
        $judgeProblems.Add("$($judge.Name) not built")
        continue
    }
    $built = [IO.File]::GetLastWriteTimeUtc($exe)
    $newest = $null
    foreach ($file in (Invoke-Native "git" (@("-C", $Repo, "ls-files", "--") + $judge.Trees)).Output) {
        $path = Join-Path $Repo $file
        if (-not [IO.File]::Exists($path)) { continue }
        $written = [IO.File]::GetLastWriteTimeUtc($path)
        if (-not $newest -or $written -gt $newest.Time) {
            $newest = [pscustomobject]@{ Time = $written; Path = $file }
        }
    }
    if ($newest -and $newest.Time -gt $built) {
        $judgeProblems.Add("$($judge.Name) older than $($newest.Path)")
    }
}
Write-Check "judges" ($judgeProblems.Count -eq 0) `
    $(if ($judgeProblems.Count) { ($judgeProblems -join "; ") + " (cmake --build build --config Release --target opennova_wire opennova_lan_probe opennova_parity)" } else {
        "opennova-wire, opennova-lan-probe, opennova-parity current" })

# --- the checkout -----------------------------------------------------------------
$head = [string] (Invoke-Native "git" @("-C", $Repo, "rev-parse", "HEAD")).Output[0]
$dirty = @((Invoke-Native "git" @("-C", $Repo, "status", "--porcelain", "--untracked-files=no")).Output)
Write-Check "repo_clean" ($dirty.Count -eq 0) "head=$head changed=$($dirty.Count)"

# --- the GDExtension --------------------------------------------------------------
$local = @{}
foreach ($name in $DllNames) { $local[$name] = Get-Sha256OrEmpty (Join-Path $Repo "godot\bin\$name") }
$dllDetail = "runtime=$(Format-Hash $local[$DllNames[0]]) editor=$(Format-Hash $local[$DllNames[1]])"
$dllOk = $false
if (-not $DllRunId) {
    $dllDetail += " (no -DllRunId: the artifact is not compared)"
}
elseif (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
    $dllDetail += " (gh is not installed)"
}
else {
    $view = Invoke-Native "gh" @("run", "view", $DllRunId, "-R", $GitHubRepo, "--json", "headSha")
    if ($view.Exit -ne 0 -or -not ($view.Output -join "")) {
        $dllDetail += " (run $DllRunId not found in $GitHubRepo)"
    }
    else {
        $runSha = [string] (($view.Output -join "") | ConvertFrom-Json).headSha
        $dllDetail += " run=$DllRunId commit=$runSha"
        if ((Invoke-Native "git" @("-C", $Repo, "cat-file", "-e", "$runSha^{commit}")).Exit -ne 0) {
            $dllDetail += " (the run's commit is not in this clone; git fetch it)"
        }
        else {
            $changed = @((Invoke-Native "git" (@("-C", $Repo, "diff", "--name-only", $runSha, "HEAD", "--") + $DllTrees)).Output)
            if ($changed.Count -gt 0) {
                $dllDetail += " (not equivalent: $($changed.Count) files under $($DllTrees -join ',') differ from HEAD, e.g. $($changed[0]))"
            }
            else {
                $cache = Join-Path ([IO.Path]::GetTempPath()) "opennova-preflight-gdext-$DllRunId"
                $cached = @($DllNames | Where-Object { Test-Path -LiteralPath (Join-Path $cache $_) -PathType Leaf })
                if ($cached.Count -ne $DllNames.Count) {
                    New-Item -ItemType Directory -Force -Path $cache | Out-Null
                    $null = Invoke-Native "gh" @("run", "download", $DllRunId, "-R", $GitHubRepo,
                        "-n", "gdext_windows_template_debug", "-D", $cache)
                }
                $mismatch = @()
                foreach ($name in $DllNames) {
                    # The artifact keeps the godot/bin layout or a flat one.
                    $artifact = @(Get-ChildItem -LiteralPath $cache -Recurse -File -Filter $name |
                        Select-Object -First 1)
                    $artifactSha = if ($artifact.Count) { Get-Sha256OrEmpty $artifact[0].FullName } else { "" }
                    if (-not $artifactSha -or $artifactSha -cne $local[$name]) {
                        $mismatch += "$name artifact=$(Format-Hash $artifactSha)"
                    }
                }
                if ($mismatch.Count) { $dllDetail += " (godot\bin differs: $($mismatch -join '; '))" }
                else {
                    $dllOk = $true
                    $dllDetail += " equivalent, godot\bin == artifact"
                }
            }
        }
    }
}
Write-Check "gdextension" $dllOk $dllDetail

# --- ports and processes ------------------------------------------------------------
$listening = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue |
    Where-Object { [int] $_.LocalPort -in @($HostMcpPort, $JoinerMcpPort) } |
    ForEach-Object { "$($_.LocalPort)/pid$($_.OwningProcess)" })
Write-Check "mcp_ports" ($listening.Count -eq 0) `
    "$HostMcpPort,$JoinerMcpPort $(if ($listening.Count) { 'held: ' + ($listening -join ',') } else { 'free' })"

$casePorts = @($table.cases | ForEach-Object { [int] $_.port } | Sort-Object -Unique)
$bound = @(Get-NetUDPEndpoint -ErrorAction SilentlyContinue |
    Where-Object { [int] $_.LocalPort -in $casePorts } |
    ForEach-Object { "$($_.LocalPort)/pid$($_.OwningProcess)" })
Write-Check "case_ports" ($bound.Count -eq 0) `
    "$($casePorts -join ',') $(if ($bound.Count) { 'held: ' + ($bound -join ',') } else { 'free' })"

$retail = @(Get-Process -Name "Jointops" -ErrorAction SilentlyContinue)
Write-Check "no_retail" ($retail.Count -eq 0) `
    $(if ($retail.Count) { "running: pid " + (@($retail | ForEach-Object { $_.Id }) -join ",") } else { "no Jointops.exe running" })

Write-Host "PARITY_PREFLIGHT_STATUS=$(if ($script:Failures -eq 0) { 'PASS' } else { 'FAIL' })"
exit $(if ($script:Failures -eq 0) { 0 } else { 1 })
