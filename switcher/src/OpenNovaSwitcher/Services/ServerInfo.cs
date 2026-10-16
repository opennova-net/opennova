using System.Net.Http.Headers;
using System.Text.Json;
using OpenNova.Switcher.Hosts;

namespace OpenNova.Switcher.Services;

/// <summary>
/// The OpenNova NovaWorld service's <c>GET /api/server-info</c>: <c>{"novaworld_ip": "&lt;IPv4&gt;"}</c>,
/// the public address its gate advertises (apps/novaworld_common/README.md). The site's
/// own domain sits behind a proxy, so its DNS answer is the proxy's, not the game
/// server's; the route is how the switcher learns the address.
/// </summary>
public static class ServerInfo
{
    public const string DefaultServiceUrl = "https://opennova.net";
    public const string RoutePath = "api/server-info";

    /// <summary>
    /// The route's URL under <paramref name="serviceUrl"/>, or null when that is not a
    /// service URL the switcher asks: an absolute https URL, or http only to this computer
    /// or a private network (<see cref="IsLocalService"/>: a local stack such as
    /// http://127.0.0.1:8080); never with a user name, password, query or fragment.
    /// </summary>
    public static Uri? EndpointFor(string? serviceUrl)
    {
        if (string.IsNullOrWhiteSpace(serviceUrl) ||
            !Uri.TryCreate(serviceUrl.Trim(), UriKind.Absolute, out var baseUri) ||
            (baseUri.Scheme != Uri.UriSchemeHttps && !(baseUri.Scheme == Uri.UriSchemeHttp && IsLocalService(baseUri))) ||
            !string.IsNullOrEmpty(baseUri.UserInfo) ||
            !string.IsNullOrEmpty(baseUri.Query) || !string.IsNullOrEmpty(baseUri.Fragment))
        {
            return null;
        }

        var root = baseUri.AbsoluteUri.EndsWith('/') ? baseUri : new Uri(baseUri.AbsoluteUri + "/");
        return new Uri(root, RoutePath);
    }

    /// <summary>
    /// Whether <paramref name="serviceUrl"/> names a service on this computer or a private
    /// network: <c>localhost</c> or an IPv4 literal in an <see cref="Ipv4.IsLocal"/> range.
    /// Only such a service may answer a non-public NovaWorld address.
    /// </summary>
    public static bool IsLocalService(string? serviceUrl)
    {
        return !string.IsNullOrWhiteSpace(serviceUrl) &&
               Uri.TryCreate(serviceUrl.Trim(), UriKind.Absolute, out var uri) &&
               IsLocalService(uri);
    }

    private static bool IsLocalService(Uri uri)
    {
        return string.Equals(uri.Host, "localhost", StringComparison.OrdinalIgnoreCase) ||
               (uri.HostNameType == UriHostNameType.IPv4 && Ipv4.IsLocal(uri.Host));
    }

    /// <summary>
    /// The address a reply names: its <c>novaworld_ip</c> when that is a usable dotted
    /// quad (<see cref="Ipv4.IsUsable"/>), else null. Other members are ignored.
    /// </summary>
    public static string? ParseAddress(string? json)
    {
        if (string.IsNullOrWhiteSpace(json))
        {
            return null;
        }

        try
        {
            using var document = JsonDocument.Parse(json);
            if (document.RootElement.ValueKind != JsonValueKind.Object ||
                !document.RootElement.TryGetProperty("novaworld_ip", out var ip) ||
                ip.ValueKind != JsonValueKind.String)
            {
                return null;
            }

            var address = ip.GetString();
            return Ipv4.IsUsable(address) ? address : null;
        }
        catch (JsonException)
        {
            return null;
        }
    }
}

public interface IServerInfoClient
{
    /// <summary>The address <paramref name="endpoint"/> answers, or null for a reply without one.</summary>
    Task<string?> FetchAddressAsync(Uri endpoint, CancellationToken cancellationToken);
}

public sealed class HttpServerInfoClient : IServerInfoClient, IDisposable
{
    /// <summary>The largest reply read; the route's is a few dozen bytes.</summary>
    public const int MaxReplyBytes = 64 * 1024;

    private readonly HttpClient _httpClient;

    public HttpServerInfoClient()
    {
        _httpClient = new HttpClient
        {
            Timeout = TimeSpan.FromSeconds(10),
            MaxResponseContentBufferSize = MaxReplyBytes,
        };
        var version = typeof(HttpServerInfoClient).Assembly.GetName().Version?.ToString(3) ?? "0";
        _httpClient.DefaultRequestHeaders.UserAgent.Add(new ProductInfoHeaderValue("OpenNova-Switcher", version));
        _httpClient.DefaultRequestHeaders.Accept.Add(new MediaTypeWithQualityHeaderValue("application/json"));
    }

    public async Task<string?> FetchAddressAsync(Uri endpoint, CancellationToken cancellationToken)
    {
        using var response = await _httpClient.GetAsync(endpoint, cancellationToken).ConfigureAwait(false);
        if (!response.IsSuccessStatusCode)
        {
            return null;
        }

        var body = await response.Content.ReadAsStringAsync(cancellationToken).ConfigureAwait(false);
        return ServerInfo.ParseAddress(body);
    }

    public void Dispose() => _httpClient.Dispose();
}
