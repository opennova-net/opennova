using OpenNova.Launcher.Hosts;

namespace OpenNova.Launcher.Services;

/// <summary>
/// Reads the mode from the hosts file and switches it. Each change reads the file
/// fresh, rewrites only the managed block (and, when the user agreed, the foreign
/// lines), writes only when the text changed and only over the text it read, then
/// flushes the DNS cache. IO failures (<see cref="IOException"/>, including
/// <see cref="HostsFileException"/>, and <see cref="UnauthorizedAccessException"/>)
/// reach the caller, which tells the user.
/// </summary>
public sealed class HostsSwitcher
{
    private const int MaxAttempts = 3;

    private readonly IHostsFileStore _store;
    private readonly IDnsCacheFlusher _dnsCacheFlusher;

    public HostsSwitcher(IHostsFileStore store, IDnsCacheFlusher dnsCacheFlusher, IReadOnlyList<string>? hostnames = null)
    {
        _store = store ?? throw new ArgumentNullException(nameof(store));
        _dnsCacheFlusher = dnsCacheFlusher ?? throw new ArgumentNullException(nameof(dnsCacheFlusher));
        Hostnames = hostnames ?? NovaWorldHosts.Names;
    }

    public IReadOnlyList<string> Hostnames { get; }

    public HostsState ReadState() => ModeDetector.Detect(Load(out _), Hostnames);

    /// <summary>
    /// Puts the hosts file in <paramref name="mode"/>: NovaLogic removes the block;
    /// OpenNova and Custom map every hostname to <paramref name="address"/>. Lines of
    /// the user's found inside the block are kept. <paramref name="removeForeign"/> also
    /// drops the lines outside the block that map the hostnames. Returns whether the
    /// file changed.
    /// <para>
    /// When another program rewrites the file between the read and the write, the switch
    /// starts over on the new text, unless it removes foreign lines: the user agreed to
    /// remove the lines they were shown, so that switch is refused instead
    /// (<see cref="HostsFileChangedException"/>).
    /// </para>
    /// </summary>
    public bool Switch(NovaWorldMode mode, string? address, bool removeForeign)
    {
        var redirects = mode is NovaWorldMode.OpenNova or NovaWorldMode.Custom ? Redirects(address) : null;
        for (var attempt = 1; ; attempt++)
        {
            try
            {
                var document = Load(out var before);
                if (removeForeign)
                {
                    document = document.WithoutForeignLines(Hostnames);
                }

                document = mode switch
                {
                    NovaWorldMode.NovaLogic => document.WithoutManagedBlock(),
                    NovaWorldMode.OpenNova => document.WithManagedBlock(ModeDetector.OpenNovaTag, redirects!),
                    NovaWorldMode.Custom => document.WithManagedBlock(ModeDetector.CustomTag, redirects!),
                    _ => throw new ArgumentOutOfRangeException(nameof(mode), mode, "Not a mode a switch writes."),
                };

                return Save(before, document);
            }
            catch (HostsFileChangedException) when (!removeForeign && attempt < MaxAttempts)
            {
            }
        }
    }

    /// <summary>
    /// Drops the lines outside the block that map the hostnames; the block stays. A
    /// rewrite by another program meanwhile refuses it (<see cref="HostsFileChangedException"/>).
    /// </summary>
    public bool RemoveForeignLines()
    {
        var document = Load(out var before);
        return Save(before, document.WithoutForeignLines(Hostnames));
    }

    private IReadOnlyList<HostsRedirect> Redirects(string? address)
    {
        if (!Ipv4.IsUsable(address))
        {
            throw new ArgumentException($"'{address}' is not an IPv4 address a hosts line can use.", nameof(address));
        }

        return Hostnames.Select(hostname => new HostsRedirect(hostname, address!)).ToList();
    }

    private HostsDocument Load(out string text)
    {
        text = _store.Exists ? _store.Read() : string.Empty;
        return HostsDocument.Parse(text, Hostnames);
    }

    private bool Save(string before, HostsDocument document)
    {
        var after = document.Render();
        if (string.Equals(before, after, StringComparison.Ordinal))
        {
            return false;
        }

        _store.Write(after, before);
        _dnsCacheFlusher.Flush();
        return true;
    }
}
