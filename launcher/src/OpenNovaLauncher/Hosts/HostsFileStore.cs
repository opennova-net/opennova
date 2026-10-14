using System.Text;

namespace OpenNova.Launcher.Hosts;

public interface IHostsFileStore
{
    bool Exists { get; }
    string Read();
    void Write(string contents);
}

/// <summary>
/// Reads and writes %SystemRoot%\System32\drivers\etc\hosts.
/// <list type="bullet">
/// <item>The text is read and written as Latin-1, which maps every byte to one
/// character and back, so lines the launcher does not own keep their exact bytes
/// whatever code page (or UTF-8) they were written in. The launcher's own lines
/// are ASCII.</item>
/// <item>A write is atomic: the new text lands in a temp file in the same
/// directory, which then replaces the hosts file in one step
/// (<see cref="File.Replace(string, string, string?, bool)"/>), keeping the file
/// it replaced as <see cref="BackupPath"/>.</item>
/// <item>A ReadOnly attribute is cleared for the write and put back after it, and
/// transient IO failures (another program holding the file) are retried.</item>
/// </list>
/// </summary>
public sealed class HostsFileStore : IHostsFileStore
{
    private const int MaxAttempts = 3;
    private const int RetryDelayMilliseconds = 250;

    public HostsFileStore()
        : this(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "drivers", "etc", "hosts"))
    {
    }

    public HostsFileStore(string hostsPath)
    {
        HostsPath = hostsPath ?? throw new ArgumentNullException(nameof(hostsPath));
    }

    public string HostsPath { get; }

    /// <summary>The hosts file as it was before the launcher's last write.</summary>
    public string BackupPath => HostsPath + ".opennova-launcher.bak";

    public bool Exists => File.Exists(HostsPath);

    public string Read()
    {
        return WithRetries(() => Encoding.Latin1.GetString(File.ReadAllBytes(HostsPath)));
    }

    public void Write(string contents)
    {
        ArgumentNullException.ThrowIfNull(contents);
        var bytes = Encoding.Latin1.GetBytes(contents);
        WithRetries(() =>
        {
            var directory = Path.GetDirectoryName(HostsPath) ?? throw new IOException("The hosts path has no directory.");
            var tempPath = Path.Combine(directory, $"hosts.{Guid.NewGuid():N}.tmp");

            var wasReadOnly = false;
            if (File.Exists(HostsPath))
            {
                var attributes = File.GetAttributes(HostsPath);
                wasReadOnly = (attributes & FileAttributes.ReadOnly) != 0;
                if (wasReadOnly)
                {
                    File.SetAttributes(HostsPath, attributes & ~FileAttributes.ReadOnly);
                }
            }

            try
            {
                File.WriteAllBytes(tempPath, bytes);
                if (File.Exists(HostsPath))
                {
                    File.Replace(tempPath, HostsPath, BackupPath, ignoreMetadataErrors: true);
                }
                else
                {
                    File.Move(tempPath, HostsPath);
                }
            }
            finally
            {
                TryDelete(tempPath);
                if (wasReadOnly && File.Exists(HostsPath))
                {
                    File.SetAttributes(HostsPath, File.GetAttributes(HostsPath) | FileAttributes.ReadOnly);
                }
            }

            return true;
        });
    }

    private static T WithRetries<T>(Func<T> operation)
    {
        for (var attempt = 1; ; attempt++)
        {
            try
            {
                return operation();
            }
            catch (IOException) when (attempt < MaxAttempts)
            {
                Thread.Sleep(RetryDelayMilliseconds);
            }
        }
    }

    private static void TryDelete(string path)
    {
        try
        {
            if (File.Exists(path))
            {
                File.Delete(path);
            }
        }
        catch (IOException)
        {
            // A temp file left behind is harmless; the next write makes a new one.
        }
        catch (UnauthorizedAccessException)
        {
        }
    }
}
