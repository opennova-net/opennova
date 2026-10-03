# The opennova-game MCP client for PowerShell 5.1 (docs/mcp.md). Dot-sourced
# by scripts/net/lib.ps1 and the render scripts; the Python twin is
# scripts/mcp/game_mcp.py. One JSON-RPC session per port, initialized on first
# use; every request is a single POST to http://127.0.0.1:<port>/mcp.
#
#   Invoke-GameTool -Port 8975 -Name game_state
#   Get-StructuredResult (Invoke-GameTool -Port 8975 -Name game_state)
#   Invoke-GameProbe -Port 8975 -Name perf_sample -Arguments @{ sample_ms = 5000 }
#   Stop-GameViaMcp -Port 8975 -Process $process
#
# [OpenNova.Scripts.BehindLaunch] is the twin of game_mcp.py's BehindLaunch:
# Start-OpenNovaProcess (scripts/net/lib.ps1) starts every game through it, so
# the windows start behind every other window and never take the foreground.

$script:GameMcpProtocolVersion = "2025-06-18"
$script:GameMcpDefaultPort = 8975
$script:GameMcpHostPort = 8975
$script:GameMcpJoinerPort = 8976
$script:GameMcpSessions = @{}
$script:GameMcpRequestId = 0

# The launch behind every other window (game_mcp.py's BehindLaunch says why and
# how). Here the start runs on a background thread while the caller proves the
# process and waits for its endpoint: Prepare (the launcher's foreground rights,
# the lock), Start (the process, its first window shown without activation; a
# console wrapper's console gets no window, and starts the game with a start
# state of its own), Watch (each window of the process and of the processes it
# starts kept at the bottom without activation, its flashing stopped, the lock
# retaken), Settle (once the endpoint answers: the grace, the lock let go) and
# Dispose (the lock let go however the start ends).
if (-not ('OpenNova.Scripts.BehindLaunch' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

namespace OpenNova.Scripts {
    public sealed class BehindLaunch : IDisposable {
        public const double GraceSeconds = 2.0;         // tended at least this long after the endpoint answers
        public const double QuietSeconds = 1.0;         // a shown window's flashing stopped this long
        public const double SettleLimitSeconds = 10.0;  // never tended (nor the lock held) longer than this after the answer
        public const double LockWaitSeconds = 30.0;     // another launch's lock waited for, with foreground rights
        const int SW_SHOWNOACTIVATE = 4;
        const uint LSFW_LOCK = 1, LSFW_UNLOCK = 2;
        const uint SWP_NOSIZE = 0x1, SWP_NOMOVE = 0x2, SWP_NOACTIVATE = 0x10, SWP_ASYNCWINDOWPOS = 0x4000;
        const uint FLASHW_STOP = 0, STARTF_USESHOWWINDOW = 0x1, CREATE_NO_WINDOW = 0x08000000;
        const string GodotWindowClass = "Engine";  // DisplayServerWindows' window class

        public bool Active { get; private set; }
        public bool Rights { get; private set; }
        public bool Locked { get; private set; }

        readonly HashSet<int> tended = new HashSet<int>();  // the started process and those it starts
        readonly Dictionary<IntPtr, DateTime> shownAt = new Dictionary<IntPtr, DateTime>();
        DateTime started;
        bool waiting;
        Thread watcher;
        volatile bool stopping;
        bool done;  // the lock let go for good
        readonly object state = new object();   // the lock's state
        readonly object passes = new object();  // one pass over the windows at a time

        public BehindLaunch(bool active) { Active = active; }

        // Before the start: the launcher's foreground rights read (AllowSetForegroundWindow on
        // itself succeeds only with them) and the lock taken, with rights waited for while another
        // launch holds it.
        public void Prepare() {
            if (!Active) return;
            Rights = AllowSetForegroundWindow((uint)Process.GetCurrentProcess().Id);
            DateTime deadline = DateTime.UtcNow.AddSeconds(Rights ? LockWaitSeconds : 0);
            Lock();
            while (!Locked && DateTime.UtcNow < deadline) {
                Thread.Sleep(100);
                Lock();
            }
        }

        public Process Start(string exe, string arguments, string workingDirectory, bool consoleWrapper) {
            var info = new STARTUPINFO();
            info.cb = Marshal.SizeOf(typeof(STARTUPINFO));
            uint flags = 0;
            if (consoleWrapper) {
                flags = CREATE_NO_WINDOW;
            } else {
                info.dwFlags = STARTF_USESHOWWINDOW;
                info.wShowWindow = SW_SHOWNOACTIVATE;
            }
            PROCESS_INFORMATION created;
            var commandLine = new StringBuilder("\"" + exe + "\" " + arguments);
            if (!CreateProcessW(null, commandLine, IntPtr.Zero, IntPtr.Zero, false, flags, IntPtr.Zero,
                    workingDirectory, ref info, out created)) {
                throw new Win32Exception(Marshal.GetLastWin32Error(), "CreateProcess failed for " + exe);
            }
            try {
                Process process = Process.GetProcessById(created.dwProcessId);
                IntPtr handle = process.Handle;  // its exit code stays queryable
                GC.KeepAlive(handle);
                return process;
            } finally {
                CloseHandle(created.hThread);
                CloseHandle(created.hProcess);
            }
        }

        public void Watch(Process process) {
            if (!Active) return;
            tended.Add(process.Id);
            started = process.StartTime.ToUniversalTime().AddSeconds(-1);
            watcher = new Thread(() => {
                while (!stopping) {
                    try {
                        lock (passes) { if (!stopping) Tend(false); }
                    } catch (Exception) { }
                    Thread.Sleep(50);
                }
            });
            watcher.IsBackground = true;
            watcher.Start();
        }

        // The endpoint answered (or there is none): the windows tended through the grace and until
        // they have settled (Godot may show its window after the endpoint answers), within the
        // limit; then every window once more, and the lock let go.
        public void Settle() {
            if (!Active) return;
            StopWatching();
            try {
                DateTime start = DateTime.UtcNow;
                while (true) {
                    lock (passes) { Tend(false); }
                    double spent = (DateTime.UtcNow - start).TotalSeconds;
                    if (spent >= SettleLimitSeconds || (spent >= GraceSeconds && Settled())) break;
                    Thread.Sleep(50);
                }
                lock (passes) { Tend(true); }
            } finally {
                Release();
            }
        }

        public void Dispose() {
            StopWatching();
            Release();
        }

        void StopWatching() {
            stopping = true;
            if (watcher != null) {
                watcher.Join(5000);
                watcher = null;
            }
        }

        void Lock() {
            lock (state) {
                if (Active && !Locked && !done) Locked = LockSetForegroundWindow(LSFW_LOCK);
            }
        }

        // Taken again when the system let it go (it does as the user switches windows); refused,
        // harmlessly, while it is still held.
        void Relock() {
            lock (state) {
                if (Active && Locked && !done) LockSetForegroundWindow(LSFW_LOCK);
            }
        }

        void Release() {
            lock (state) {
                done = true;
                if (Active && Locked) {
                    LockSetForegroundWindow(LSFW_UNLOCK);
                    Locked = false;
                }
            }
        }

        // The processes the started one starts (the console wrapper's game), adopted as they appear.
        void Adopt() {
            IntPtr snapshot = CreateToolhelp32Snapshot(0x2, 0);
            if (snapshot == new IntPtr(-1)) return;
            try {
                var entry = new PROCESSENTRY32W();
                entry.dwSize = (uint)Marshal.SizeOf(typeof(PROCESSENTRY32W));
                var children = new List<int>();
                if (Process32FirstW(snapshot, ref entry)) {
                    do {
                        if (tended.Contains((int)entry.th32ParentProcessID) && !tended.Contains((int)entry.th32ProcessID)) {
                            children.Add((int)entry.th32ProcessID);
                        }
                    } while (Process32NextW(snapshot, ref entry));
                }
                foreach (int child in children) {
                    try {
                        using (Process process = Process.GetProcessById(child)) {
                            if (process.StartTime.ToUniversalTime() >= started) tended.Add(child);
                        }
                    } catch (Exception) { }
                }
            } finally {
                CloseHandle(snapshot);
            }
        }

        bool Chosen() {
            IntPtr foreground = GetForegroundWindow();
            if (foreground == IntPtr.Zero) return false;
            uint owner;
            GetWindowThreadProcessId(foreground, out owner);
            return tended.Contains((int)owner);
        }

        List<KeyValuePair<IntPtr, bool>> Windows() {
            var found = new List<KeyValuePair<IntPtr, bool>>();
            var name = new StringBuilder(32);
            EnumWindowsProc visit = (hwnd, unused) => {
                uint owner;
                GetWindowThreadProcessId(hwnd, out owner);
                if (!tended.Contains((int)owner)) return true;
                if (IsWindowVisible(hwnd)) {
                    found.Add(new KeyValuePair<IntPtr, bool>(hwnd, true));
                } else if (GetClassNameW(hwnd, name, name.Capacity) > 0 && name.ToString() == GodotWindowClass) {
                    found.Add(new KeyValuePair<IntPtr, bool>(hwnd, false));
                }
                return true;
            };
            EnumWindows(visit, IntPtr.Zero);
            GC.KeepAlive(visit);
            return found;
        }

        void Tend(bool again) {
            Relock();
            Adopt();
            if (Chosen()) return;
            DateTime now = DateTime.UtcNow;
            waiting = false;
            foreach (var window in Windows()) {
                if (window.Value) {
                    DateTime first;
                    if (!shownAt.TryGetValue(window.Key, out first)) {
                        first = now;
                        shownAt[window.Key] = now;
                    }
                    if (again || (now - first).TotalSeconds <= QuietSeconds) StopFlashing(window.Key);
                } else {
                    waiting = true;
                }
                SendBack(window.Key);
            }
        }

        bool Settled() {
            if (waiting) return false;
            DateTime now = DateTime.UtcNow;
            foreach (DateTime first in shownAt.Values) {
                if ((now - first).TotalSeconds <= QuietSeconds) return false;
            }
            return true;
        }

        // To the bottom of the z-order without activation: posted to the window's thread, which
        // does it as it next pumps.
        static void SendBack(IntPtr hwnd) {
            SetWindowPos(hwnd, new IntPtr(1), 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        }

        static void StopFlashing(IntPtr hwnd) {
            var flash = new FLASHWINFO();
            flash.cbSize = (uint)Marshal.SizeOf(typeof(FLASHWINFO));
            flash.hwnd = hwnd;
            flash.dwFlags = FLASHW_STOP;
            FlashWindowEx(ref flash);
        }

        delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr parameter);

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        struct STARTUPINFO {
            public int cb; public string lpReserved; public string lpDesktop; public string lpTitle;
            public uint dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
            public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
        }

        [StructLayout(LayoutKind.Sequential)]
        struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }

        [StructLayout(LayoutKind.Sequential)]
        struct FLASHWINFO { public uint cbSize; public IntPtr hwnd; public uint dwFlags, uCount, dwTimeout; }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        struct PROCESSENTRY32W {
            public uint dwSize, cntUsage, th32ProcessID; public IntPtr th32DefaultHeapID;
            public uint th32ModuleID, cntThreads, th32ParentProcessID; public int pcPriClassBase; public uint dwFlags;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)] public string szExeFile;
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern bool CreateProcessW(string application, StringBuilder commandLine, IntPtr processAttributes,
            IntPtr threadAttributes, bool inheritHandles, uint flags, IntPtr environment, string directory,
            ref STARTUPINFO info, out PROCESS_INFORMATION created);
        [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
        [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr CreateToolhelp32Snapshot(uint flags, uint pid);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern bool Process32FirstW(IntPtr snapshot, ref PROCESSENTRY32W entry);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern bool Process32NextW(IntPtr snapshot, ref PROCESSENTRY32W entry);
        [DllImport("user32.dll")] static extern bool AllowSetForegroundWindow(uint pid);
        [DllImport("user32.dll")] static extern bool LockSetForegroundWindow(uint code);
        [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
        [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
        [DllImport("user32.dll")] static extern bool EnumWindows(EnumWindowsProc visit, IntPtr parameter);
        [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr hwnd);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassNameW(IntPtr hwnd, StringBuilder name, int capacity);
        [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr hwnd, IntPtr after, int x, int y, int cx, int cy, uint flags);
        [DllImport("user32.dll")] static extern bool FlashWindowEx(ref FLASHWINFO info);
    }
}
'@
}

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

function Test-GameMcpTransportTimeout {
    param([Parameter(Mandatory = $true)] $ErrorRecord)
    $exception = $ErrorRecord.Exception
    while ($exception) {
        if ($exception -is [System.Net.WebException] -or
                $exception.Message -match "timed out|timeout") {
            return $true
        }
        $exception = $exception.InnerException
    }
    return $false
}

# game_probe run + status polling. Returns the final status object; -OnLine
# receives each new line ({seq, t_ms, text}) as it arrives. Throws only on
# transport failures or the timeout; the caller judges verdict.ok / state.
# The game answers its endpoint from the main thread, so a mission load (a
# joiner's join preload, a big map) can leave a request unanswered for
# minutes: a transport timeout is retried until the deadline while the
# process lives, adopting the run the game did start if the request landed.
function Invoke-GameProbe {
    param(
        [Parameter(Mandatory = $true)] [int] $Port,
        [Parameter(Mandatory = $true)] [string] $Name,
        [hashtable] $Arguments = @{},
        [int] $TimeoutSeconds = 900,
        [int] $PollMs = 2000,
        [scriptblock] $OnLine = $null,
        [System.Diagnostics.Process] $Process = $null
    )
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $assertAlive = {
        if ($Process) {
            $Process.Refresh()
            if ($Process.HasExited) {
                throw "the game (PID $($Process.Id)) exited with code $($Process.ExitCode) while probe $Name was requested"
            }
        }
    }
    $runId = ""
    while (-not $runId) {
        try {
            $started = Get-StructuredResult (Invoke-GameTool -Port $Port -Name "game_probe" -TimeoutSeconds 60 -Arguments @{
                op = "run"
                name = $Name
                args = $Arguments
            })
            $runId = [string] $started.run_id
        }
        catch {
            if (-not (Test-GameMcpTransportTimeout $_)) {
                # The request may have landed before the response was lost:
                # the game's active run is then this probe.
                if ($_.Exception.Message -match "another probe|already running|in flight") {
                    $active = Get-StructuredResult (Invoke-GameTool -Port $Port -Name "game_probe" -TimeoutSeconds 60 -Arguments @{ op = "status"; wait_ms = 0 })
                    if ([string] $active.name -eq $Name) { $runId = [string] $active.run_id; continue }
                }
                throw
            }
            & $assertAlive
            if ([DateTime]::UtcNow -ge $deadline) {
                throw "the game on port $Port did not accept probe $Name within $TimeoutSeconds s (endpoint unresponsive)"
            }
            try {
                $active = Get-StructuredResult (Invoke-GameTool -Port $Port -Name "game_probe" -TimeoutSeconds 60 -Arguments @{ op = "status"; wait_ms = 0 })
                if ([string] $active.name -eq $Name -and [string] $active.state -eq "running") {
                    $runId = [string] $active.run_id
                    continue
                }
            }
            catch { }
            Start-Sleep -Seconds 2
        }
    }
    $cursor = 0
    $waitMs = [Math]::Min($PollMs, 30000)
    while ($true) {
        try {
            $status = Get-StructuredResult (Invoke-GameTool -Port $Port -Name "game_probe" `
                -TimeoutSeconds ([int][Math]::Max(60, ($waitMs / 1000) + 30)) -Arguments @{
                    op = "status"
                    run_id = $runId
                    cursor = $cursor
                    wait_ms = $waitMs
                })
        }
        catch {
            if (-not (Test-GameMcpTransportTimeout $_)) { throw }
            & $assertAlive
            if ([DateTime]::UtcNow -ge $deadline) {
                throw "probe $Name ($runId): the game on port $Port stopped answering status before $TimeoutSeconds s"
            }
            Start-Sleep -Seconds 2
            continue
        }
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
