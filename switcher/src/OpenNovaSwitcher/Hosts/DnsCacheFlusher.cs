using System.Diagnostics;

namespace OpenNova.Switcher.Hosts;

public interface IDnsCacheFlusher
{
    void Flush();
}

/// <summary>
/// Flushes the Windows DNS resolver cache so a hosts-file change takes effect at
/// once. A failure is not fatal: the cached answer expires on its own.
/// </summary>
public sealed class DnsCacheFlusher : IDnsCacheFlusher
{
    /// <summary>
    /// ipconfig by its full path: the switcher runs elevated, and a bare name would be
    /// searched for in the switcher's own folder first, which the user may control.
    /// </summary>
    public static string IpconfigPath => Path.Combine(Environment.SystemDirectory, "ipconfig.exe");

    public void Flush()
    {
        try
        {
            var startInfo = new ProcessStartInfo(IpconfigPath, "/flushdns")
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                WindowStyle = ProcessWindowStyle.Hidden,
                WorkingDirectory = Environment.SystemDirectory,
            };

            using var process = Process.Start(startInfo);
            process?.WaitForExit(5000);
        }
        catch (Exception ex) when (ex is System.ComponentModel.Win32Exception or InvalidOperationException)
        {
            Debug.WriteLine($"[Hosts] DNS flush failed: {ex.Message}");
        }
    }
}
