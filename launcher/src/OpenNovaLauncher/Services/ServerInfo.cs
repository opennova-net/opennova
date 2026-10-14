using System.Net.Http.Headers;
using System.Text.Json;
using OpenNova.Launcher.Hosts;

namespace OpenNova.Launcher.Services;

/// <summary>
/// The OpenNova NovaWorld service's <c>GET /api/server-info</c>: <c>{"novaworld_ip": "&lt;IPv4&gt;"}</c>,
/// the public address its gate advertises (apps/novaworld_server/README.md). The site's
/// own domain sits behind a proxy, so its DNS answer is the proxy's, not the game
/// server's; the route is how the launcher learns the address.
/// </summary>
public static class ServerInfo
{
    public const string DefaultServiceUrl = "https://opennova.net";
    public const string RoutePath = "api/server-info";

    /// <summary>
    /// The route's URL under <paramref name="serviceUrl"/> (an absolute http or https
    /// URL: the site, or a service's own port such as http://127.0.0.1:8080), or null
    /// when it is not one.
    /// </summary>
    public static Uri? EndpointFor(string? serviceUrl)
    {
        if (string.IsNullOrWhiteSpace(serviceUrl) ||
            !Uri.TryCreate(serviceUrl.Trim(), UriKind.Absolute, out var baseUri) ||
            (baseUri.Scheme != Uri.UriSchemeHttp && baseUri.Scheme != Uri.UriSchemeHttps) ||
            !string.IsNullOrEmpty(baseUri.Query) || !string.IsNullOrEmpty(baseUri.Fragment))
        {
            return null;
        }

        var root = baseUri.AbsoluteUri.EndsWith('/') ? baseUri : new Uri(baseUri.AbsoluteUri + "/");
        return new Uri(root, RoutePath);
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
    private readonly HttpClient _httpClient;

    public HttpServerInfoClient()
    {
        _httpClient = new HttpClient { Timeout = TimeSpan.FromSeconds(10) };
        var version = typeof(HttpServerInfoClient).Assembly.GetName().Version?.ToString(3) ?? "0";
        _httpClient.DefaultRequestHeaders.UserAgent.Add(new ProductInfoHeaderValue("OpenNova-Launcher", version));
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
