using System.Diagnostics;
using OpenNova.Launcher.Hosts;

namespace OpenNova.Launcher.Services;

/// <summary>An OpenNova NovaWorld address, and whether it came from the cache.</summary>
public sealed record ResolvedAddress(string Address, bool FromCache);

/// <summary>
/// OpenNova's NovaWorld address: the service's <c>/api/server-info</c> when it
/// answers (the answer is cached), else the last address it gave, else none. An
/// address from the network must be public (<see cref="Ipv4.IsPublic"/>) unless the
/// service URL itself names this computer or a private network: a remote reply never
/// points a player's NovaWorld at their own machine or LAN.
/// </summary>
public sealed class OpenNovaAddressSource
{
    private readonly IServerInfoClient _client;
    private readonly LauncherSettings _settings;
    private readonly Action _saveSettings;

    public OpenNovaAddressSource(IServerInfoClient client, LauncherSettings settings, Action saveSettings)
    {
        _client = client ?? throw new ArgumentNullException(nameof(client));
        _settings = settings ?? throw new ArgumentNullException(nameof(settings));
        _saveSettings = saveSettings ?? throw new ArgumentNullException(nameof(saveSettings));
    }

    public async Task<ResolvedAddress?> ResolveAsync(CancellationToken cancellationToken)
    {
        var endpoint = ServerInfo.EndpointFor(_settings.ServiceUrl);
        if (endpoint is not null)
        {
            try
            {
                var address = await _client.FetchAddressAsync(endpoint, cancellationToken).ConfigureAwait(true);
                if (Acceptable(address))
                {
                    Remember(address!);
                    return new ResolvedAddress(address!, FromCache: false);
                }
            }
            catch (Exception ex) when (ex is HttpRequestException or TaskCanceledException or OperationCanceledException
                                           or InvalidOperationException)
            {
                // InvalidOperationException: a reply naming a charset .NET cannot decode.
                Debug.WriteLine($"[ServerInfo] {endpoint} failed: {ex.Message}");
            }
        }

        return Acceptable(_settings.CachedOpenNovaAddress)
            ? new ResolvedAddress(_settings.CachedOpenNovaAddress!, FromCache: true)
            : null;
    }

    private bool Acceptable(string? address)
    {
        return Ipv4.IsUsable(address) && (Ipv4.IsPublic(address) || ServerInfo.IsLocalService(_settings.ServiceUrl));
    }

    private void Remember(string address)
    {
        if (string.Equals(_settings.CachedOpenNovaAddress, address, StringComparison.Ordinal))
        {
            return;
        }

        _settings.CachedOpenNovaAddress = address;
        try
        {
            _saveSettings();
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or System.Security.SecurityException)
        {
            // The address is still good for this run; only the offline fallback misses it.
            Debug.WriteLine($"[Settings] Save failed: {ex.Message}");
        }
    }
}
