using System.Diagnostics;

namespace OpenNova.Launcher.Hosts;

public interface IDnsCacheFlusher
{
    void Flush();
}

/// <summary>
/// Flushes the Windows DNS resolver cache so hosts-file changes take effect
/// immediately. Failures are non-fatal; the cache also expires on its own.
/// </summary>
public sealed class DnsCacheFlusher : IDnsCacheFlusher
{
    public void Flush()
    {
        try
        {
            var startInfo = new ProcessStartInfo("ipconfig", "/flushdns")
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                WindowStyle = ProcessWindowStyle.Hidden,
            };

            using var process = Process.Start(startInfo);
            process?.WaitForExit(5000);
        }
        catch
        {
            // Non-fatal: stale cache entries expire on their own.
        }
    }
}
