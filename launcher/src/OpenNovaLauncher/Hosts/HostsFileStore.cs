using System;
using System.IO;
using System.Text;
using System.Threading;

namespace OpenNova.Launcher.Hosts;

public interface IHostsFileStore
{
    bool Exists { get; }
    string Read();
    void Write(string contents);
}

/// <summary>
/// Reads and writes %SystemRoot%\System32\drivers\etc\hosts. Writes are atomic:
/// the new contents land in a temp file in the same directory and replace the
/// original in one move. A ReadOnly attribute is cleared for the write and
/// restored afterwards, and transient IO failures are retried.
/// </summary>
public sealed class HostsFileStore : IHostsFileStore
{
    private const int MaxAttempts = 3;
    private const int RetryDelayMilliseconds = 250;
    private static readonly Encoding HostsEncoding = new UTF8Encoding(encoderShouldEmitUTF8Identifier: false);

    private readonly string _hostsPath;

    public HostsFileStore()
        : this(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "drivers", "etc", "hosts"))
    {
    }

    public HostsFileStore(string hostsPath)
    {
        _hostsPath = hostsPath ?? throw new ArgumentNullException(nameof(hostsPath));
    }

    public bool Exists => File.Exists(_hostsPath);

    public string Read()
    {
        return WithRetries(() => File.ReadAllText(_hostsPath, HostsEncoding));
    }

    public void Write(string contents)
    {
        WithRetries(() =>
        {
            var directory = Path.GetDirectoryName(_hostsPath) ?? throw new IOException("Hosts path has no directory.");
            var tempPath = Path.Combine(directory, $"hosts.{Guid.NewGuid():N}.tmp");

            var wasReadOnly = false;
            if (File.Exists(_hostsPath))
            {
                var attributes = File.GetAttributes(_hostsPath);
                wasReadOnly = (attributes & FileAttributes.ReadOnly) != 0;
                if (wasReadOnly)
                {
                    File.SetAttributes(_hostsPath, attributes & ~FileAttributes.ReadOnly);
                }
            }

            try
            {
                File.WriteAllText(tempPath, contents, HostsEncoding);
                if (File.Exists(_hostsPath))
                {
                    File.Replace(tempPath, _hostsPath, destinationBackupFileName: null, ignoreMetadataErrors: true);
                }
                else
                {
                    File.Move(tempPath, _hostsPath);
                }
            }
            finally
            {
                TryDelete(tempPath);
                if (wasReadOnly && File.Exists(_hostsPath))
                {
                    File.SetAttributes(_hostsPath, File.GetAttributes(_hostsPath) | FileAttributes.ReadOnly);
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
        catch
        {
            // Best-effort cleanup of the temp file.
        }
    }
}
