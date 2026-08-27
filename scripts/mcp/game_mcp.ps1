# The opennova-game MCP client for PowerShell 5.1 (docs/mcp.md). Dot-sourced
# by scripts/net/lib.ps1 and the render scripts; the Python twin is
# scripts/mcp/game_mcp.py. One JSON-RPC session per port, initialized on first
# use; every request is a single POST to http://127.0.0.1:<port>/mcp.
#
#   Invoke-GameTool -Port 8975 -Name game_state
#   Get-StructuredResult (Invoke-GameTool -Port 8975 -Name game_state)
#   Invoke-GameProbe -Port 8975 -Name perf_sample -Arguments @{ sample_ms = 5000 }
#   Stop-GameViaMcp -Port 8975 -Process $process

$script:GameMcpProtocolVersion = "2025-06-18"
$script:GameMcpDefaultPort = 8975
$script:GameMcpHostPort = 8975
$script:GameMcpJoinerPort = 8976
$script:GameMcpSessions = @{}
$script:GameMcpRequestId = 0

function Get-GameMcpUrl {
    param([Parameter(Mandatory = $true)] [int] $Port)
    return "http://127.0.0.1:$Port/mcp"
}

# One POST; returns the decoded JSON-RPC envelope ($null for an empty body).
function Send-GameMcpMessage {
    param(
        [Parameter(Mandatory = $true)] [int] $Port,
        [Parameter(Mandatory = $true)] [hashtable] $Message,
        [int] $TimeoutSeconds = 120
    )
    $json = $Message | ConvertTo-Json -Depth 32 -Compress
    $bytes = [System.Text.UTF8Encoding]::new($false).GetBytes($json)
    $headers = @{ Accept = "application/json" }
    if ($script:GameMcpSessions.ContainsKey($Port)) {
        $headers["Mcp-Session-Id"] = $script:GameMcpSessions[$Port]
    }
    $response = Invoke-WebRequest -UseBasicParsing -Method Post -Uri (Get-GameMcpUrl -Port $Port) `
        -ContentType "application/json" -Headers $headers -Body $bytes -TimeoutSec $TimeoutSeconds
    $session = $response.Headers["Mcp-Session-Id"]
    if ($session) { $script:GameMcpSessions[$Port] = [string] $session }
    if (-not $response.RawContentStream -or $response.RawContentStream.Length -eq 0) { return $null }
    $text = [System.Text.Encoding]::UTF8.GetString($response.RawContentStream.ToArray())
    if ([string]::IsNullOrWhiteSpace($text)) { return $null }
    return ($text | ConvertFrom-Json)
}

function Initialize-GameMcpSession {
    param(
        [Parameter(Mandatory = $true)] [int] $Port,
        [int] $TimeoutSeconds = 30
    )
    $script:GameMcpSessions.Remove($Port)
    $script:GameMcpRequestId++
    $envelope = Send-GameMcpMessage -Port $Port -TimeoutSeconds $TimeoutSeconds -Message @{
        jsonrpc = "2.0"
        id = $script:GameMcpRequestId
        method = "initialize"
        params = @{
            protocolVersion = $script:GameMcpProtocolVersion
            capabilities = @{}
            clientInfo = @{ name = "game_mcp.ps1"; version = "1" }
        }
    }
    if (-not $envelope -or ($envelope.PSObject.Properties.Name -contains "error")) {
        throw "initialize on port $Port failed: $($envelope.error | ConvertTo-Json -Compress)"
    }
    $null = Send-GameMcpMessage -Port $Port -TimeoutSeconds $TimeoutSeconds -Message @{
        jsonrpc = "2.0"
        method = "notifications/initialized"
    }
    return $envelope.result
}

# A JSON-RPC request; throws on a JSON-RPC error, returns `result` otherwise.
function Invoke-GameRpc {
    param(
        [Parameter(Mandatory = $true)] [int] $Port,
        [Parameter(Mandatory = $true)] [string] $Method,
        [hashtable] $Params = @{},
        [int] $TimeoutSeconds = 120
    )
    if ($Method -ne "initialize" -and -not $script:GameMcpSessions.ContainsKey($Port)) {
        $null = Initialize-GameMcpSession -Port $Port
    }
    $script:GameMcpRequestId++
    $envelope = Send-GameMcpMessage -Port $Port -TimeoutSeconds $TimeoutSeconds -Message @{
        jsonrpc = "2.0"
        id = $script:GameMcpRequestId
        method = $Method
        params = $Params
    }
    if (-not $envelope) { throw "$Method on port $Port returned no JSON-RPC response" }
    if ($envelope.PSObject.Properties.Name -contains "error") {
        throw "$Method on port $Port failed: $($envelope.error | ConvertTo-Json -Compress)"
    }
    return $envelope.result
}

# tools/call; throws when the tool reports isError unless -AllowError.
function Invoke-GameTool {
    param(
        [Parameter(Mandatory = $true)] [int] $Port,
        [Parameter(Mandatory = $true)] [string] $Name,
        [hashtable] $Arguments = @{},
        [int] $TimeoutSeconds = 120,
        [switch] $AllowError
    )
    $result = Invoke-GameRpc -Port $Port -Method "tools/call" -TimeoutSeconds $TimeoutSeconds -Params @{
        name = $Name
        arguments = $Arguments
    }
    if ($result.isError -and -not $AllowError) {
        $message = @($result.content | Where-Object { $_.type -eq "text" } |
            ForEach-Object { $_.text }) -join "`n"
        throw "$Name failed: $message"
    }
    return $result
}

function Get-StructuredResult {
    param([Parameter(Mandatory = $true)] $ToolResult)
    if (-not ($ToolResult.PSObject.Properties.Name -contains "structuredContent")) {
        throw "Tool response has no structuredContent"
    }
    return $ToolResult.structuredContent
}

function Get-GameToolText {
    param([Parameter(Mandatory = $true)] $ToolResult)
    return (@($ToolResult.content | Where-Object { $_.type -eq "text" } |
        ForEach-Object { $_.text }) -join "`n")
}

function Test-GameMcpPortOpen {
    param([Parameter(Mandatory = $true)] [int] $Port)
    $client = [System.Net.Sockets.TcpClient]::new()
    try {
        $async = $client.BeginConnect("127.0.0.1", $Port, $null, $null)
        if (-not $async.AsyncWaitHandle.WaitOne(250)) { return $false }
        $client.EndConnect($async)
        return $true
    }
    catch { return $false }
    finally { $client.Close() }
}

# Poll until the endpoint answers initialize. -Process (the game's Process)
# turns an early exit into an immediate throw instead of a timeout.
function Wait-GameMcpReady {
    param(
        [Parameter(Mandatory = $true)] [int] $Port,
        [int] $TimeoutSeconds = 240,
        [System.Diagnostics.Process] $Process = $null
    )
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($Process) {
            $Process.Refresh()
            if ($Process.HasExited) {
                throw "the game (PID $($Process.Id)) exited with code $($Process.ExitCode) before its MCP endpoint on port $Port answered"
            }
        }
        if (Test-GameMcpPortOpen -Port $Port) {
            try {
                $null = Initialize-GameMcpSession -Port $Port -TimeoutSeconds 5
                return $true
            }
            catch { }
        }
        Start-Sleep -Milliseconds 250
    }
    throw "no MCP endpoint answered on port $Port within $TimeoutSeconds s"
}

# game_probe run + status polling. Returns the final status object; -OnLine
# receives each new line ({seq, t_ms, text}) as it arrives. Throws only on
# transport failures or the timeout; the caller judges verdict.ok / state.
function Invoke-GameProbe {
    param(
        [Parameter(Mandatory = $true)] [int] $Port,
        [Parameter(Mandatory = $true)] [string] $Name,
        [hashtable] $Arguments = @{},
        [int] $TimeoutSeconds = 900,
        [int] $PollMs = 2000,
        [scriptblock] $OnLine = $null
    )
    $started = Get-StructuredResult (Invoke-GameTool -Port $Port -Name "game_probe" -Arguments @{
        op = "run"
        name = $Name
        args = $Arguments
    })
    $runId = [string] $started.run_id
    $cursor = 0
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $waitMs = [Math]::Min($PollMs, 30000)
    while ($true) {
        $status = Get-StructuredResult (Invoke-GameTool -Port $Port -Name "game_probe" `
            -TimeoutSeconds ([int][Math]::Max(60, ($waitMs / 1000) + 30)) -Arguments @{
                op = "status"
                run_id = $runId
                cursor = $cursor
                wait_ms = $waitMs
            })
        foreach ($line in @($status.lines)) {
            if ($OnLine) { & $OnLine $line }
        }
        $cursor = [int] $status.next_cursor
        if ([string] $status.state -ne "running") { return $status }
        if ([DateTime]::UtcNow -ge $deadline) {
            throw "probe $Name ($runId) still running after $TimeoutSeconds s; cancel it with game_probe op=cancel"
        }
    }
}

# Cooperative stop: cancel any probe, ask for game_control quit, then wait for
# the process (or the port) to go away. Returns $true when it did. Never kills.
function Stop-GameViaMcp {
    param(
        [Parameter(Mandatory = $true)] [int] $Port,
        [System.Diagnostics.Process] $Process = $null,
        [int] $TimeoutSeconds = 20
    )
    if (Test-GameMcpPortOpen -Port $Port) {
        try { $null = Invoke-GameTool -Port $Port -Name "game_probe" -Arguments @{ op = "cancel" } -AllowError -TimeoutSeconds 10 }
        catch { }
        try { $null = Invoke-GameTool -Port $Port -Name "game_control" -Arguments @{ action = "quit" } -AllowError -TimeoutSeconds 10 }
        catch { }
    }
    $script:GameMcpSessions.Remove($Port)
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($Process) {
            $Process.Refresh()
            if ($Process.HasExited) { return $true }
        }
        elseif (-not (Test-GameMcpPortOpen -Port $Port)) {
            return $true
        }
        Start-Sleep -Milliseconds 250
    }
    return $false
}
