namespace OpenNova.Launcher.Hosts;

/// <summary>Where the hosts file sends a stock client's NovaWorld lookups.</summary>
public enum NovaWorldMode
{
    /// <summary>No managed block: the names resolve through DNS to NovaLogic's NovaWorld.</summary>
    NovaLogic,

    /// <summary>The block maps the names to OpenNova's NovaWorld service.</summary>
    OpenNova,

    /// <summary>The block maps the names to an address the user entered.</summary>
    Custom,

    /// <summary>
    /// A well-formed block with no mode line: the one the retired launcher (before
    /// ADR 0054) wrote. Any switch rewrites it with its mode.
    /// </summary>
    OldLauncher,

    /// <summary>
    /// A block no switch writes: damaged markers, a line of the user's inside it, a name
    /// missing or repeated, two addresses, an unusable address, an unknown mode line.
    /// A switch rewrites it, keeping the user's lines.
    /// </summary>
    Unrecognized,
}

/// <summary>
/// What the hosts file says: the mode, the address the names go to (null for
/// <see cref="NovaWorldMode.NovaLogic"/> and <see cref="NovaWorldMode.Unrecognized"/>),
/// and any lines outside the block that also map the names.
/// </summary>
public sealed record HostsState(NovaWorldMode Mode, string? Address, IReadOnlyList<string> ForeignLines)
{
    public bool HasForeignLines => ForeignLines.Count > 0;
}

public static class ModeDetector
{
    /// <summary>The block's mode line for <see cref="NovaWorldMode.OpenNova"/>.</summary>
    public const string OpenNovaTag = "opennova";

    /// <summary>The block's mode line for <see cref="NovaWorldMode.Custom"/>.</summary>
    public const string CustomTag = "custom";

    /// <summary>
    /// The mode <paramref name="document"/> is in for <paramref name="hostnames"/>: no
    /// block, or one mapping nothing, is NovaLogic; an intact block mapping each name
    /// once, all to one usable address, is OpenNova or Custom by its mode line and
    /// OldLauncher with none; anything else is Unrecognized.
    /// </summary>
    public static HostsState Detect(HostsDocument document, IReadOnlyList<string> hostnames)
    {
        ArgumentNullException.ThrowIfNull(document);
        ArgumentNullException.ThrowIfNull(hostnames);

        var foreign = document.ForeignLinesFor(hostnames);
        var unrecognized = new HostsState(NovaWorldMode.Unrecognized, null, foreign);
        if (!document.ManagedBlockIntact)
        {
            return unrecognized;
        }

        var managed = document.ManagedRedirects;
        if (managed.Count == 0)
        {
            return new HostsState(NovaWorldMode.NovaLogic, null, foreign);
        }

        var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        string? address = null;
        foreach (var redirect in managed)
        {
            if (!names.Add(redirect.Hostname))
            {
                return unrecognized;
            }

            if (address is null)
            {
                address = redirect.IPv4;
            }
            else if (!string.Equals(address, redirect.IPv4, StringComparison.Ordinal))
            {
                return unrecognized;
            }
        }

        if (!names.SetEquals(hostnames) || !Ipv4.IsUsable(address))
        {
            return unrecognized;
        }

        var mode = document.ManagedMode switch
        {
            null => NovaWorldMode.OldLauncher,
            OpenNovaTag => NovaWorldMode.OpenNova,
            CustomTag => NovaWorldMode.Custom,
            _ => NovaWorldMode.Unrecognized,
        };
        return new HostsState(mode, mode == NovaWorldMode.Unrecognized ? null : address, foreign);
    }
}
