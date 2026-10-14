using System.Diagnostics;

namespace OpenNova.Launcher.Hosts;

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
        catch (Exception ex) when (ex is System.ComponentModel.Win32Exception or InvalidOperationException)
        {
            Debug.WriteLine($"[Hosts] DNS flush failed: {ex.Message}");
        }
    }
}
