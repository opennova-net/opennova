using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Net.Sockets;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Threading;
using System.Threading.Tasks;
using OpenNova.Launcher.Models;

namespace OpenNova.Launcher.Services;

public sealed record ServerEndpoint(string IPv4, IReadOnlyList<string> RedirectHostnames);

public sealed record ServerInfo(string NovaworldIp, IReadOnlyList<string> RedirectHostnames);

public interface IServerInfoClient
{
    /// <summary>Fetches /server-info from the backend, or null when unavailable.</summary>
    Task<ServerInfo?> FetchAsync(CancellationToken cancellationToken);
}

public interface IDnsResolver
{
    /// <summary>Resolves the first IPv4 address for a hostname, or null when none.</summary>
    Task<string?> ResolveIPv4Async(string hostname, CancellationToken cancellationToken);
}

/// <summary>
/// Resolves the NovaWorld server IPv4 and the hostname set to redirect at it.
/// Dev mode targets 127.0.0.1. Production order: backend /server-info over the
/// real internet, then the cached IP from settings, then a DNS lookup of the
/// anchor hostname. Successful resolutions are persisted to settings.
/// </summary>
public sealed class ServerEndpointResolver
{
    public const string DefaultRedirectHostname = "gs.novaworld.net";
    public const string DefaultAnchorHostname = "nw.opennova.net";
    public const string AnchorHostEnvironmentVariable = "ONLAUNCHER_NW_ANCHOR_HOST";

    private readonly IServerInfoClient _serverInfoClient;
    private readonly IDnsResolver _dnsResolver;
    private readonly Func<LauncherSettings> _settingsAccessor;
    private readonly Func<Task> _saveSettingsAsync;

    public ServerEndpointResolver(
        IServerInfoClient serverInfoClient,
        IDnsResolver dnsResolver,
        Func<LauncherSettings> settingsAccessor,
        Func<Task> saveSettingsAsync)
    {
        _serverInfoClient = serverInfoClient ?? throw new ArgumentNullException(nameof(serverInfoClient));
        _dnsResolver = dnsResolver ?? throw new ArgumentNullException(nameof(dnsResolver));
        _settingsAccessor = settingsAccessor ?? throw new ArgumentNullException(nameof(settingsAccessor));
        _saveSettingsAsync = saveSettingsAsync ?? throw new ArgumentNullException(nameof(saveSettingsAsync));
    }

    public async Task<ServerEndpoint?> ResolveAsync(bool devMode, CancellationToken cancellationToken)
    {
        var settings = _settingsAccessor();
        var hostnames = CachedOrDefaultHostnames(settings);

        if (devMode)
        {
            return new ServerEndpoint("127.0.0.1", hostnames);
        }

        try
        {
            var info = await _serverInfoClient.FetchAsync(cancellationToken).ConfigureAwait(false);
            if (info != null && !string.IsNullOrWhiteSpace(info.NovaworldIp))
            {
                var resolvedHostnames = info.RedirectHostnames is { Count: > 0 }
                    ? info.RedirectHostnames.Where(h => !string.IsNullOrWhiteSpace(h)).Select(h => h.Trim()).ToArray()
                    : hostnames;
                if (resolvedHostnames.Count == 0)
                {
                    resolvedHostnames = hostnames;
                }

                await PersistAsync(settings, info.NovaworldIp.Trim(), resolvedHostnames).ConfigureAwait(false);
                return new ServerEndpoint(info.NovaworldIp.Trim(), resolvedHostnames);
            }
        }
        catch (Exception ex)
        {
            Debug.WriteLine($"[Resolver] server-info lookup failed: {ex.Message}");
        }

        if (!string.IsNullOrWhiteSpace(settings.CachedServerIp))
        {
            return new ServerEndpoint(settings.CachedServerIp.Trim(), hostnames);
        }

        try
        {
            var anchorHost = Environment.GetEnvironmentVariable(AnchorHostEnvironmentVariable);
            anchorHost = string.IsNullOrWhiteSpace(anchorHost) ? DefaultAnchorHostname : anchorHost.Trim();

            var ip = await _dnsResolver.ResolveIPv4Async(anchorHost, cancellationToken).ConfigureAwait(false);
            if (!string.IsNullOrWhiteSpace(ip))
            {
                await PersistAsync(settings, ip.Trim(), hostnames).ConfigureAwait(false);
                return new ServerEndpoint(ip.Trim(), hostnames);
            }
        }
        catch (Exception ex)
        {
            Debug.WriteLine($"[Resolver] DNS anchor lookup failed: {ex.Message}");
        }

        return null;
    }

    private static IReadOnlyList<string> CachedOrDefaultHostnames(LauncherSettings settings)
    {
        var cached = settings.CachedRedirectHostnames?
            .Where(h => !string.IsNullOrWhiteSpace(h))
            .Select(h => h.Trim())
            .ToArray();

        return cached is { Length: > 0 } ? cached : new[] { DefaultRedirectHostname };
    }

    private async Task PersistAsync(LauncherSettings settings, string ip, IReadOnlyList<string> hostnames)
    {
        try
        {
            settings.CachedServerIp = ip;
            settings.CachedRedirectHostnames = hostnames.ToArray();
            await _saveSettingsAsync().ConfigureAwait(false);
        }
        catch (Exception ex)
        {
            Debug.WriteLine($"[Resolver] Failed to persist cached endpoint: {ex.Message}");
        }
    }
}

/// <summary>HTTP implementation of <see cref="IServerInfoClient"/> over the backend API.</summary>
public sealed class HttpServerInfoClient : IServerInfoClient, IDisposable
{
    private readonly HttpClient _httpClient;
    private bool _disposed;

    public HttpServerInfoClient(string apiBaseUrl)
    {
        if (string.IsNullOrWhiteSpace(apiBaseUrl))
        {
            throw new ArgumentException("API base URL is required", nameof(apiBaseUrl));
        }

        if (!apiBaseUrl.EndsWith("/", StringComparison.Ordinal))
        {
            apiBaseUrl += "/";
        }

        _httpClient = new HttpClient
        {
            BaseAddress = new Uri(apiBaseUrl, UriKind.Absolute),
            Timeout = TimeSpan.FromSeconds(15),
        };
    }

    public async Task<ServerInfo?> FetchAsync(CancellationToken cancellationToken)
    {
        using var response = await _httpClient.GetAsync("server-info", cancellationToken).ConfigureAwait(false);
        if (!response.IsSuccessStatusCode)
        {
            // Tolerate the route not existing yet.
            return null;
        }

        var payload = await response.Content.ReadAsStringAsync(cancellationToken).ConfigureAwait(false);
        var dto = JsonSerializer.Deserialize<ServerInfoDto>(payload);
        if (dto == null || string.IsNullOrWhiteSpace(dto.NovaworldIp))
        {
            return null;
        }

        return new ServerInfo(dto.NovaworldIp, dto.RedirectHostnames ?? Array.Empty<string>());
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _httpClient.Dispose();
        _disposed = true;
    }

    private sealed class ServerInfoDto
    {
        [JsonPropertyName("novaworld_ip")]
        public string? NovaworldIp { get; init; }

        [JsonPropertyName("redirect_hostnames")]
        public string[]? RedirectHostnames { get; init; }
    }
}

/// <summary>System DNS implementation of <see cref="IDnsResolver"/>.</summary>
public sealed class SystemDnsResolver : IDnsResolver
{
    public async Task<string?> ResolveIPv4Async(string hostname, CancellationToken cancellationToken)
    {
        var addresses = await Dns.GetHostAddressesAsync(hostname, cancellationToken).ConfigureAwait(false);
        return addresses.FirstOrDefault(a => a.AddressFamily == AddressFamily.InterNetwork)?.ToString();
    }
}
