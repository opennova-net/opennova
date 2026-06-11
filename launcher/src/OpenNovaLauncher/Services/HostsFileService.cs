using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Security;
using OpenNova.Launcher.Hosts;

namespace OpenNova.Launcher.Services;

public enum HostsRedirectState
{
    /// <summary>No managed block in the hosts file.</summary>
    Disabled,

    /// <summary>The managed block matches the desired redirects exactly.</summary>
    Enabled,

    /// <summary>A managed block exists but its entries differ from the desired redirects.</summary>
    EnabledStaleIp,

    /// <summary>Lines outside the managed block mention our hostnames.</summary>
    ForeignConflict,

    /// <summary>The hosts file could not be read or written.</summary>
    Inaccessible,
}

/// <summary>
/// Owns the launcher-managed block in the Windows hosts file. All IO failures are
/// absorbed here (mapped to <see cref="HostsRedirectState.Inaccessible"/> or a
/// false return); nothing throws through to the UI.
/// </summary>
public sealed class HostsFileService
{
    private readonly IHostsFileStore _store;
    private readonly IDnsCacheFlusher _dnsCacheFlusher;

    public HostsFileService(IHostsFileStore store, IDnsCacheFlusher dnsCacheFlusher)
    {
        _store = store ?? throw new ArgumentNullException(nameof(store));
        _dnsCacheFlusher = dnsCacheFlusher ?? throw new ArgumentNullException(nameof(dnsCacheFlusher));
    }

    public HostsRedirectState GetState(IReadOnlyList<HostsRedirect> desired)
    {
        try
        {
            var document = ReadDocument();

            if (document.ForeignLinesFor(desired.Select(redirect => redirect.Hostname)).Count > 0)
            {
                return HostsRedirectState.ForeignConflict;
            }

            var managed = document.ManagedRedirects;
            if (managed.Count == 0)
            {
                return HostsRedirectState.Disabled;
            }

            var managedByHostname = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            foreach (var redirect in managed)
            {
                managedByHostname[redirect.Hostname] = redirect.IPv4;
            }

            var matches = managedByHostname.Count == desired.Count &&
                          desired.All(redirect =>
                              managedByHostname.TryGetValue(redirect.Hostname, out var ip) &&
                              string.Equals(ip, redirect.IPv4, StringComparison.Ordinal));

            return matches ? HostsRedirectState.Enabled : HostsRedirectState.EnabledStaleIp;
        }
        catch (Exception ex) when (IsIoFailure(ex))
        {
            return HostsRedirectState.Inaccessible;
        }
    }

    public void Apply(IReadOnlyList<HostsRedirect> desired)
    {
        try
        {
            var document = ReadDocument().WithManagedBlock(desired);
            _store.Write(document.Render());
            _dnsCacheFlusher.Flush();
        }
        catch (Exception ex) when (IsIoFailure(ex))
        {
            Debug.WriteLine($"[Hosts] Apply failed: {ex.Message}");
        }
    }

    public void Remove()
    {
        try
        {
            if (!_store.Exists)
            {
                return;
            }

            var document = HostsDocument.Parse(_store.Read());
            if (document.ManagedRedirects.Count == 0 && document.WithoutManagedBlock().Render() == document.Render())
            {
                return;
            }

            _store.Write(document.WithoutManagedBlock().Render());
            _dnsCacheFlusher.Flush();
        }
        catch (Exception ex) when (IsIoFailure(ex))
        {
            Debug.WriteLine($"[Hosts] Remove failed: {ex.Message}");
        }
    }

    /// <summary>
    /// Removes only foreign lines (outside the managed block) that mention our
    /// hostnames. The caller must confirm with the user before invoking this.
    /// </summary>
    public bool TryCleanForeignLines(IEnumerable<string> hostnames)
    {
        try
        {
            if (!_store.Exists)
            {
                return true;
            }

            var hostnameList = hostnames.ToList();
            var document = HostsDocument.Parse(_store.Read());
            if (document.ForeignLinesFor(hostnameList).Count == 0)
            {
                return true;
            }

            _store.Write(document.WithoutForeignLines(hostnameList).Render());
            _dnsCacheFlusher.Flush();
            return true;
        }
        catch (Exception ex) when (IsIoFailure(ex))
        {
            Debug.WriteLine($"[Hosts] Clean foreign lines failed: {ex.Message}");
            return false;
        }
    }

    private HostsDocument ReadDocument()
    {
        return HostsDocument.Parse(_store.Exists ? _store.Read() : string.Empty);
    }

    private static bool IsIoFailure(Exception ex)
    {
        return ex is IOException or UnauthorizedAccessException or SecurityException;
    }
}
