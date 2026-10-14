using System.Text;

namespace OpenNova.Switcher.Hosts;

public interface IHostsFileStore
{
    bool Exists { get; }

    /// <summary>The file's text; a <see cref="HostsFileException"/> when the switcher must not edit it.</summary>
    string Read();

    /// <summary>
    /// Replaces the file's text with <paramref name="contents"/>, provided it still holds
    /// <paramref name="expected"/> (what <see cref="Read"/> returned; empty for no file);
    /// otherwise throws <see cref="HostsFileChangedException"/> and writes nothing.
    /// </summary>
    void Write(string contents, string expected);
}

/// <summary>A hosts file the switcher will not edit; the message says why, for the user.</summary>
public class HostsFileException : IOException
{
    public HostsFileException(string message)
        : base(message)
    {
    }
}

/// <summary>Another program changed the hosts file between the switcher's read and its write.</summary>
public sealed class HostsFileChangedException : HostsFileException
{
    public HostsFileChangedException()
        : base("Another program changed the hosts file while the switcher was writing it, so nothing was written. Try again.")
    {
    }
}

/// <summary>
/// Reads and writes the Windows hosts file, System32\drivers\etc\hosts
/// (<see cref="DefaultHostsPath"/>).
/// <list type="bullet">
/// <item>The text is read and written as Latin-1, which maps every byte to one
/// character and back, so lines the switcher does not own keep their exact bytes in
/// any single-byte code page or UTF-8. A UTF-16 file (a byte-order mark) or one holding
/// NUL bytes is refused: appending ASCII to it would corrupt it.</item>
/// <item>A write first checks the file still holds what the switcher read, so a
/// program that rewrote it meanwhile (Docker Desktop, say) is not overwritten.</item>
/// <item>The first write keeps the file as it was before the switcher ever wrote it as
/// <see cref="OriginalPath"/>, never replaced afterwards.</item>
/// <item>A write is atomic: the new text lands, flushed to disk, in a temp file in the
/// same directory, which then replaces the hosts file in one step
/// (<see cref="File.Replace(string, string, string?, bool)"/>), keeping the file it
/// replaced as <see cref="BackupPath"/>.</item>
/// <item>A ReadOnly attribute is cleared for the write and put back after it, and
/// transient IO failures (another program holding the file) are retried.</item>
/// </list>
/// </summary>
public sealed class HostsFileStore : IHostsFileStore
{
    private const int MaxAttempts = 3;
    private const int RetryDelayMilliseconds = 250;

    public HostsFileStore()
        : this(DefaultHostsPath())
    {
    }

    public HostsFileStore(string hostsPath)
    {
        HostsPath = hostsPath ?? throw new ArgumentNullException(nameof(hostsPath));
    }

    public string HostsPath { get; }

    /// <summary>The hosts file as it was before the switcher's last write.</summary>
    public string BackupPath => HostsPath + ".opennova-switcher.bak";

    /// <summary>The hosts file as it was before the switcher's first write.</summary>
    public string OriginalPath => HostsPath + ".opennova-switcher.orig";

    public bool Exists => File.Exists(HostsPath);

    /// <summary>
    /// System32\drivers\etc\hosts, with System32 from <see cref="Environment.SystemDirectory"/>
    /// (GetSystemDirectory), which reads no environment variable. The switcher runs
    /// elevated, so nothing that names this path may come from the user's environment:
    /// not %SystemRoot% (a user can set it in HKCU\Environment), and so not the Tcpip
    /// <c>DataBasePath</c> either, a REG_EXPAND_SZ of <c>%SystemRoot%\...</c> that would
    /// have to be expanded in this process. A machine that moved its hosts file through
    /// <c>DataBasePath</c> is not supported.
    /// </summary>
    public static string DefaultHostsPath()
    {
        return Path.Combine(Environment.SystemDirectory, "drivers", "etc", "hosts");
    }

    public string Read()
    {
        return WithRetries(() => Decode(File.ReadAllBytes(HostsPath)));
    }

    public void Write(string contents, string expected)
    {
        ArgumentNullException.ThrowIfNull(contents);
        ArgumentNullException.ThrowIfNull(expected);
        var bytes = Encoding.Latin1.GetBytes(contents);
        WithRetries(() =>
        {
            var exists = File.Exists(HostsPath);
            var current = exists ? Decode(File.ReadAllBytes(HostsPath)) : string.Empty;
            if (!string.Equals(current, expected, StringComparison.Ordinal))
            {
                throw new HostsFileChangedException();
            }

            var directory = Path.GetDirectoryName(HostsPath) ?? throw new IOException("The hosts path has no directory.");
            if (exists && !File.Exists(OriginalPath))
            {
                File.Copy(HostsPath, OriginalPath, overwrite: false);
            }

            var tempPath = Path.Combine(directory, $"hosts.{Guid.NewGuid():N}.tmp");
            var wasReadOnly = false;
            if (exists)
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
                using (var stream = new FileStream(tempPath, FileMode.CreateNew, FileAccess.Write, FileShare.None))
                {
                    stream.Write(bytes);
                    stream.Flush(flushToDisk: true);
                }

                if (exists)
                {
                    ReplaceKeepingAFile(tempPath);
                }
                else
                {
                    File.Move(tempPath, HostsPath);
                }
            }
            finally
            {
                // With no hosts file in place the temp file holds the only new copy: keep it.
                if (File.Exists(HostsPath))
                {
                    TryDelete(tempPath);
                    if (wasReadOnly)
                    {
                        File.SetAttributes(HostsPath, File.GetAttributes(HostsPath) | FileAttributes.ReadOnly);
                    }
                }
            }

            return true;
        });
    }

    // File.Replace can fail after moving the hosts file to the backup and before moving
    // the new file in (ERROR_UNABLE_TO_MOVE_REPLACEMENT_2), leaving no hosts file: then
    // finish the move, or put the backup back.
    private void ReplaceKeepingAFile(string tempPath)
    {
        try
        {
            File.Replace(tempPath, HostsPath, BackupPath, ignoreMetadataErrors: true);
        }
        catch (IOException) when (!File.Exists(HostsPath))
        {
            if (File.Exists(tempPath))
            {
                File.Move(tempPath, HostsPath);
            }
            else if (File.Exists(BackupPath))
            {
                File.Copy(BackupPath, HostsPath);
                throw;
            }
            else
            {
                throw;
            }
        }
    }

    private static string Decode(byte[] bytes)
    {
        if (bytes.Length >= 2 && ((bytes[0] == 0xFF && bytes[1] == 0xFE) || (bytes[0] == 0xFE && bytes[1] == 0xFF)))
        {
            throw new HostsFileException(
                "The hosts file is saved as UTF-16 (Unicode), which the switcher does not edit, so it was left unchanged. " +
                "Save it as ANSI or UTF-8 in an elevated editor and try again.");
        }

        if (Array.IndexOf(bytes, (byte)0) >= 0)
        {
            throw new HostsFileException(
                "The hosts file holds NUL bytes, so the switcher left it unchanged. Check it in an elevated editor.");
        }

        return Encoding.Latin1.GetString(bytes);
    }

    private static T WithRetries<T>(Func<T> operation)
    {
        for (var attempt = 1; ; attempt++)
        {
            try
            {
                return operation();
            }
            catch (IOException ex) when (ex is not HostsFileException && attempt < MaxAttempts)
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
