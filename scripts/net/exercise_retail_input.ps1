[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateRange(1, 2147483647)]
    [int] $ProcessId,

    [Parameter(Mandatory = $true)]
    [ValidateSet('RR', 'OR')]
    [string] $Topology,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9](?:[A-Za-z0-9._-]{0,54}[A-Za-z0-9])?$')]
    [string] $RunId,

    [Parameter(Mandatory = $true)]
    [string] $Output,

    [ValidateRange(250, 10000)]
    [int] $ForwardMilliseconds = 1800,

    [ValidateRange(250, 10000)]
    [int] $StrafeMilliseconds = 1200,

    [ValidateRange(250, 10000)]
    [int] $ReturnMilliseconds = 900,

    [ValidateRange(16, 2000)]
    [int] $TurnPixels = 320
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

if (Test-Path -LiteralPath $Output) {
    throw "Retail input witness output must be create-new: $Output"
}

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Threading;

public static class OpenNovaRetailInputExercise {
    [DllImport("user32.dll", SetLastError = true)] static extern bool IsWindow(IntPtr hWnd);
    [DllImport("user32.dll", SetLastError = true)] static extern bool ShowWindowAsync(IntPtr hWnd, int nCmdShow);
    [DllImport("user32.dll", SetLastError = true)] static extern bool BringWindowToTop(IntPtr hWnd);
    [DllImport("user32.dll", SetLastError = true)] static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] static extern IntPtr SetActiveWindow(IntPtr hWnd);
    [DllImport("user32.dll")] static extern IntPtr SetFocus(IntPtr hWnd);
    [DllImport("user32.dll", SetLastError = true)] static extern bool AttachThreadInput(uint attach, uint attachTo, bool enable);
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
    [DllImport("user32.dll", SetLastError = true)] static extern uint SendInput(uint count, INPUT[] inputs, int size);
    [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();

    [StructLayout(LayoutKind.Sequential)]
    struct INPUT {
        public uint type;
        public INPUTUNION data;
    }

    [StructLayout(LayoutKind.Explicit)]
    struct INPUTUNION {
        [FieldOffset(0)] public MOUSEINPUT mouse;
        [FieldOffset(0)] public KEYBDINPUT keyboard;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct MOUSEINPUT {
        public int dx;
        public int dy;
        public uint mouseData;
        public uint flags;
        public uint time;
        public UIntPtr extra;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct KEYBDINPUT {
        public ushort virtualKey;
        public ushort scanCode;
        public uint flags;
        public uint time;
        public UIntPtr extra;
    }

    const uint KEYEVENTF_KEYUP = 0x0002;
    const uint KEYEVENTF_SCANCODE = 0x0008;
    const uint MOUSEEVENTF_MOVE = 0x0001;
    const uint INPUT_MOUSE = 0;
    const uint INPUT_KEYBOARD = 1;
    const int SW_RESTORE = 9;

    public static int FocusAttempts { get; private set; }
    public static int ForegroundChecks { get; private set; }
    public static int InsertedEvents { get; private set; }
    public static string FailureStage { get; private set; }
    public static long LastForegroundWindow { get; private set; }
    public static uint LastForegroundPid { get; private set; }
    public static uint LastTargetPid { get; private set; }
    public static bool LastShowResult { get; private set; }
    public static bool LastBringResult { get; private set; }
    public static bool LastSetForegroundResult { get; private set; }
    public static bool LastAttachForegroundResult { get; private set; }
    public static bool LastAttachTargetResult { get; private set; }
    public static int LastWin32Error { get; private set; }

    static bool InsertKey(byte scan, bool down) {
        INPUT input = new INPUT();
        input.type = INPUT_KEYBOARD;
        input.data.keyboard.scanCode = scan;
        input.data.keyboard.flags = KEYEVENTF_SCANCODE | (down ? 0u : KEYEVENTF_KEYUP);
        uint inserted = SendInput(1, new INPUT[] { input }, Marshal.SizeOf(typeof(INPUT)));
        if (inserted != 1) {
            LastWin32Error = Marshal.GetLastWin32Error();
            return false;
        }
        InsertedEvents++;
        return true;
    }

    static bool InsertMouse(int dx) {
        INPUT input = new INPUT();
        input.type = INPUT_MOUSE;
        input.data.mouse.dx = dx;
        input.data.mouse.flags = MOUSEEVENTF_MOVE;
        uint inserted = SendInput(1, new INPUT[] { input }, Marshal.SizeOf(typeof(INPUT)));
        if (inserted != 1) {
            LastWin32Error = Marshal.GetLastWin32Error();
            return false;
        }
        InsertedEvents++;
        return true;
    }

    static bool IsExactForeground(IntPtr hwnd, uint expectedPid) {
        ForegroundChecks++;
        if (!IsWindow(hwnd)) {
            LastForegroundWindow = 0;
            LastForegroundPid = 0;
            LastTargetPid = 0;
            return false;
        }
        uint targetPid;
        GetWindowThreadProcessId(hwnd, out targetPid);
        LastTargetPid = targetPid;
        IntPtr foreground = GetForegroundWindow();
        uint foregroundPid;
        GetWindowThreadProcessId(foreground, out foregroundPid);
        LastForegroundWindow = foreground.ToInt64();
        LastForegroundPid = foregroundPid;
        if (targetPid != expectedPid) return false;
        return foreground == hwnd && foregroundPid == expectedPid;
    }

    static bool FocusExact(IntPtr hwnd, uint expectedPid) {
        if (!IsWindow(hwnd)) return false;
        for (int attempt = 0; attempt < 10; ++attempt) {
            FocusAttempts++;
            if (!IsWindow(hwnd)) {
                FailureStage = "focus-stale-window";
                return false;
            }
            uint pid;
            uint targetThread = GetWindowThreadProcessId(hwnd, out pid);
            LastTargetPid = pid;
            if (pid != expectedPid) {
                FailureStage = "focus-owner-mismatch";
                return false;
            }
            IntPtr foreground = GetForegroundWindow();
            uint foregroundPid;
            uint foregroundThread = GetWindowThreadProcessId(foreground, out foregroundPid);
            uint currentThread = GetCurrentThreadId();
            bool attachedForeground = false;
            bool attachedTarget = false;
            try {
                LastAttachForegroundResult = false;
                LastAttachTargetResult = false;
                if (foregroundThread != 0 && foregroundThread != currentThread) {
                    attachedForeground = AttachThreadInput(currentThread, foregroundThread, true);
                    LastAttachForegroundResult = attachedForeground;
                }
                if (targetThread != 0 && targetThread != currentThread &&
                        targetThread != foregroundThread) {
                    attachedTarget = AttachThreadInput(currentThread, targetThread, true);
                    LastAttachTargetResult = attachedTarget;
                }
                LastShowResult = ShowWindowAsync(hwnd, SW_RESTORE);
                LastBringResult = BringWindowToTop(hwnd);
                LastSetForegroundResult = SetForegroundWindow(hwnd);
                SetActiveWindow(hwnd);
                SetFocus(hwnd);
            } finally {
                if (attachedTarget) AttachThreadInput(currentThread, targetThread, false);
                if (attachedForeground) AttachThreadInput(currentThread, foregroundThread, false);
            }
            if (IsExactForeground(hwnd, expectedPid)) {
                Thread.Sleep(250);
                if (IsExactForeground(hwnd, expectedPid)) return true;
            } else {
                Thread.Sleep(250);
            }
        }
        FailureStage = "focus-acquire";
        return false;
    }

    static bool MoveAndTurn(IntPtr hwnd, uint expectedPid, string phase,
                            byte scan, int durationMs, int pixels) {
        const int steps = 20;
        int perStep = pixels / steps;
        int remainder = pixels - perStep * steps;
        FailureStage = phase + "-before-key-down";
        if (!IsExactForeground(hwnd, expectedPid)) return false;
        FailureStage = phase + "-key-down";
        if (!InsertKey(scan, true)) return false;
        bool success = true;
        try {
            FailureStage = phase + "-after-key-down";
            if (!IsExactForeground(hwnd, expectedPid)) return false;
            for (int i = 0; i < steps; ++i) {
                FailureStage = phase + "-sample-" + i + "-before";
                if (!IsExactForeground(hwnd, expectedPid)) return false;
                int dx = perStep + (i == steps - 1 ? remainder : 0);
                FailureStage = phase + "-sample-" + i + "-insert";
                if (!InsertMouse(dx)) return false;
                FailureStage = phase + "-sample-" + i + "-after-insert";
                if (!IsExactForeground(hwnd, expectedPid)) return false;
                Thread.Sleep(Math.Max(1, durationMs / steps));
                FailureStage = phase + "-sample-" + i + "-after-wait";
                if (!IsExactForeground(hwnd, expectedPid)) return false;
            }
        } finally {
            FailureStage = phase + "-key-up";
            if (!InsertKey(scan, false)) success = false;
        }
        FailureStage = phase + "-complete";
        if (!IsExactForeground(hwnd, expectedPid)) success = false;
        return success;
    }

    public static int Exercise(IntPtr hwnd, uint expectedPid, int forwardMs, int strafeMs,
                               int returnMs, int turnPixels) {
        FocusAttempts = 0;
        ForegroundChecks = 0;
        InsertedEvents = 0;
        FailureStage = "initial-focus";
        LastWin32Error = 0;
        if (!FocusExact(hwnd, expectedPid)) return 0;
        // DirectInput consumes scan-code key state and relative mouse deltas.
        // Exercise three independent trajectories and release every key in a
        // finally-protected helper so no input remains held after the run.
        if (!MoveAndTurn(hwnd, expectedPid, "forward", 0x11, forwardMs, turnPixels)) return 0; // W + right turn
        FailureStage = "strafe-focus";
        if (!FocusExact(hwnd, expectedPid)) return 0;
        if (!MoveAndTurn(hwnd, expectedPid, "strafe", 0x20, strafeMs, -turnPixels)) return 0; // D + left turn
        FailureStage = "return-focus";
        if (!FocusExact(hwnd, expectedPid)) return 0;
        if (!MoveAndTurn(hwnd, expectedPid, "return", 0x1E, returnMs, turnPixels / 2)) return 0; // A + partial right turn
        FailureStage = "complete";
        return InsertedEvents; // 3 key-down + 3 key-up + 60 nonzero relative mouse samples
    }
}
'@

$process = Get-Process -Id $ProcessId -ErrorAction Stop
$deadline = (Get-Date).AddSeconds(20)
$window = [IntPtr]::Zero
do {
    $process.Refresh()
    if ($process.HasExited) { throw "Retail PID $ProcessId exited before input exercise" }
    $window = $process.MainWindowHandle
    if ($window -ne [IntPtr]::Zero) { break }
    Start-Sleep -Milliseconds 250
} while ((Get-Date) -lt $deadline)
if ($window -eq [IntPtr]::Zero) {
    throw "Retail PID $ProcessId exposed no main window for exact input targeting"
}

$started = [DateTime]::UtcNow
$events = [OpenNovaRetailInputExercise]::Exercise(
    $window, [uint32]$ProcessId, $ForwardMilliseconds, $StrafeMilliseconds,
    $ReturnMilliseconds, $TurnPixels)
if ($events -le 0) {
    $currentProcess = Get-Process -Id $PID
    $failureFormat = ("Could not focus and exercise exact retail PID {0} window={1}; " +
        "stage={2} focus_attempts={3} foreground_checks={4} inserted={5} " +
        "foreground_window={6} foreground_pid={7} target_pid={8} " +
        "show={9} bring={10} set_foreground={11} attach_foreground={12} " +
        "attach_target={13} win32_error={14} caller_session={15} target_session={16}")
    throw ($failureFormat -f
        $ProcessId, $window,
        [OpenNovaRetailInputExercise]::FailureStage,
        [OpenNovaRetailInputExercise]::FocusAttempts,
        [OpenNovaRetailInputExercise]::ForegroundChecks,
        [OpenNovaRetailInputExercise]::InsertedEvents,
        [OpenNovaRetailInputExercise]::LastForegroundWindow,
        [OpenNovaRetailInputExercise]::LastForegroundPid,
        [OpenNovaRetailInputExercise]::LastTargetPid,
        [OpenNovaRetailInputExercise]::LastShowResult,
        [OpenNovaRetailInputExercise]::LastBringResult,
        [OpenNovaRetailInputExercise]::LastSetForegroundResult,
        [OpenNovaRetailInputExercise]::LastAttachForegroundResult,
        [OpenNovaRetailInputExercise]::LastAttachTargetResult,
        [OpenNovaRetailInputExercise]::LastWin32Error,
        $currentProcess.SessionId, $process.SessionId)
}
$process.Refresh()
if ($process.HasExited) { throw "Retail PID $ProcessId exited during input exercise" }

$parent = Split-Path -Parent $Output
if ($parent) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
$witness = [pscustomobject]@{
    schema = 'opennova.retail-input.v1'
    complete = $true
    run_id = $RunId
    topology = $Topology
    pid = $ProcessId
    process_start_utc = $process.StartTime.ToUniversalTime().ToString("o")
    window_handle = $window.ToInt64()
    started_utc = $started.ToString("o")
    completed_utc = [DateTime]::UtcNow.ToString("o")
    forward_ms = $ForwardMilliseconds
    strafe_ms = $StrafeMilliseconds
    return_ms = $ReturnMilliseconds
    turn_pixels = $TurnPixels
    injected_events = $events
    focus_attempts = [OpenNovaRetailInputExercise]::FocusAttempts
    foreground_checks = [OpenNovaRetailInputExercise]::ForegroundChecks
}
$json = $witness | ConvertTo-Json -Depth 5
$stream = [IO.File]::Open($Output, [IO.FileMode]::CreateNew,
    [IO.FileAccess]::Write, [IO.FileShare]::None)
try {
    $writer = [IO.StreamWriter]::new($stream, [Text.UTF8Encoding]::new($false))
    try { $writer.WriteLine($json) }
    finally { $writer.Dispose() }
} finally {
    $stream.Dispose()
}
Write-Host "RETAIL_INPUT_WITNESS=$Output"
Write-Host "RETAIL_INPUT_COMPLETE=True"
