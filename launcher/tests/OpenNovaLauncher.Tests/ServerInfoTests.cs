using OpenNova.Launcher.Hosts;
using OpenNova.Launcher.Services;
using Xunit;

namespace OpenNova.Launcher.Tests;

public class ServerInfoTests
{
    [Theory]
    [InlineData("{\"novaworld_ip\":\"54.162.79.147\"}", "54.162.79.147")]
    [InlineData("{ \"novaworld_ip\": \"127.0.0.1\", \"other\": [1, 2] }", "127.0.0.1")]
    public void ParseAddress_ReadsTheAddress(string json, string expected)
    {
        Assert.Equal(expected, ServerInfo.ParseAddress(json));
    }

    [Theory]
    [InlineData(null)]
    [InlineData("")]
    [InlineData("not json")]
    [InlineData("[\"54.162.79.147\"]")]
    [InlineData("{}")]
    [InlineData("{\"error\":\"no_public_ipv4\"}")]
    [InlineData("{\"novaworld_ip\":54}")]
    [InlineData("{\"novaworld_ip\":\"nw.example.com\"}")]
    [InlineData("{\"novaworld_ip\":\"0.0.0.0\"}")]
    [InlineData("{\"novaworld_ip\":\"239.1.2.3\"}")]
    [InlineData("{\"novaworld_ip\":\" 54.162.79.147\"}")]
    [InlineData("{\"novaworld_ip\":\"054.162.79.147\"}")]
    public void ParseAddress_RefusesAnythingElse(string? json)
    {
        Assert.Null(ServerInfo.ParseAddress(json));
    }

    [Theory]
    [InlineData("https://opennova.net", "https://opennova.net/api/server-info")]
    [InlineData("https://opennova.net/", "https://opennova.net/api/server-info")]
    [InlineData(" http://127.0.0.1:8080 ", "http://127.0.0.1:8080/api/server-info")]
    [InlineData("http://localhost:8080", "http://localhost:8080/api/server-info")]
    [InlineData("http://192.168.1.20:8080", "http://192.168.1.20:8080/api/server-info")]
    [InlineData("https://example.com/nw", "https://example.com/nw/api/server-info")]
    public void EndpointFor_AddsTheRoute(string serviceUrl, string expected)
    {
        Assert.Equal(expected, ServerInfo.EndpointFor(serviceUrl)?.AbsoluteUri);
    }

    [Theory]
    [InlineData(null)]
    [InlineData("")]
    [InlineData("opennova.net")]
    [InlineData("ftp://opennova.net")]
    [InlineData("https://opennova.net/?x=1")]
    [InlineData("http://opennova.net")]            // http only to this computer or a private network
    [InlineData("http://evil.example/sub/")]
    [InlineData("http://203.0.113.10:8080")]
    [InlineData("http://nas.lan:8080")]             // a name proves nothing about where it points
    [InlineData("https://user:pw@opennova.net")]    // no user name or password
    [InlineData("http://user@127.0.0.1:8080")]
    public void EndpointFor_RefusesWhatIsNotAServiceUrl(string? serviceUrl)
    {
        Assert.Null(ServerInfo.EndpointFor(serviceUrl));
    }

    [Theory]
    [InlineData("http://127.0.0.1:8080", true)]
    [InlineData("https://localhost", true)]
    [InlineData("http://10.1.2.3", true)]
    [InlineData("https://opennova.net", false)]
    [InlineData("https://203.0.113.10", false)]
    [InlineData("not a url", false)]
    public void IsLocalService(string serviceUrl, bool local)
    {
        Assert.Equal(local, ServerInfo.IsLocalService(serviceUrl));
    }

    [Theory]
    [InlineData("54.162.79.147", true)]
    [InlineData("127.0.0.1", false)]
    [InlineData("10.0.0.1", false)]
    [InlineData("172.16.0.1", false)]
    [InlineData("172.31.255.255", false)]
    [InlineData("172.32.0.1", true)]
    [InlineData("192.168.0.1", false)]
    [InlineData("169.254.1.1", false)]
    [InlineData("100.64.0.1", false)]
    [InlineData("100.128.0.1", true)]
    [InlineData("0.0.0.0", false)]
    [InlineData("224.0.0.1", false)]
    public void Ipv4_IsPublic(string text, bool isPublic)
    {
        Assert.Equal(isPublic, Ipv4.IsPublic(text));
    }

    [Theory]
    [InlineData("1.2.3.4", true)]
    [InlineData("127.0.0.1", true)]
    [InlineData("223.255.255.255", true)]
    [InlineData("0.1.2.3", false)]
    [InlineData("224.0.0.1", false)]
    [InlineData("255.255.255.255", false)]
    [InlineData("1.2.3", false)]
    [InlineData("1.2.3.4.5", false)]
    [InlineData("1.2.3.256", false)]
    [InlineData("1.2.3.04", false)]
    [InlineData("1.2.3.+4", false)]
    [InlineData("1.2.3.4 ", false)]
    [InlineData("::1", false)]
    public void Ipv4_IsUsable(string text, bool usable)
    {
        Assert.Equal(usable, Ipv4.IsUsable(text));
    }
}

public class OpenNovaAddressSourceTests
{
    private sealed class StubClient : IServerInfoClient
    {
        public string? Result { get; set; }
        public Exception? Throws { get; set; }
        public Uri? LastEndpoint { get; private set; }

        public Task<string?> FetchAddressAsync(Uri endpoint, CancellationToken cancellationToken)
        {
            LastEndpoint = endpoint;
            return Throws is null ? Task.FromResult(Result) : Task.FromException<string?>(Throws);
        }
    }

    private static (OpenNovaAddressSource Source, StubClient Client, LauncherSettings Settings, Func<int> Saves) Make()
    {
        var client = new StubClient();
        var settings = new LauncherSettings();
        var saves = 0;
        return (new OpenNovaAddressSource(client, settings, () => saves++), client, settings, () => saves);
    }

    [Fact]
    public async Task AnAnswer_IsUsedAndCached()
    {
        var (source, client, settings, saves) = Make();
        client.Result = "54.162.79.147";

        var resolved = await source.ResolveAsync(CancellationToken.None);

        Assert.Equal(new ResolvedAddress("54.162.79.147", FromCache: false), resolved);
        Assert.Equal("https://opennova.net/api/server-info", client.LastEndpoint?.AbsoluteUri);
        Assert.Equal("54.162.79.147", settings.CachedOpenNovaAddress);
        Assert.Equal(1, saves());

        await source.ResolveAsync(CancellationToken.None);
        Assert.Equal(1, saves());
    }

    [Fact]
    public async Task NoAnswer_FallsBackToTheCache()
    {
        var (source, client, settings, saves) = Make();
        settings.CachedOpenNovaAddress = "198.51.100.7";
        client.Throws = new HttpRequestException("offline");

        Assert.Equal(new ResolvedAddress("198.51.100.7", FromCache: true), await source.ResolveAsync(CancellationToken.None));

        client.Throws = new TaskCanceledException("timeout");
        Assert.Equal(new ResolvedAddress("198.51.100.7", FromCache: true), await source.ResolveAsync(CancellationToken.None));

        client.Throws = null;
        client.Result = null;
        Assert.Equal(new ResolvedAddress("198.51.100.7", FromCache: true), await source.ResolveAsync(CancellationToken.None));
        Assert.Equal(0, saves());
    }

    [Fact]
    public async Task NoAnswerAndNoCache_IsNull()
    {
        var (source, client, _, _) = Make();
        client.Throws = new HttpRequestException("offline");

        Assert.Null(await source.ResolveAsync(CancellationToken.None));
    }

    [Fact]
    public async Task TheServiceUrlSettingNamesTheEndpoint()
    {
        var (source, client, settings, _) = Make();
        settings.ServiceUrl = "http://127.0.0.1:8080";
        client.Result = "127.0.0.1";

        var resolved = await source.ResolveAsync(CancellationToken.None);

        Assert.Equal("http://127.0.0.1:8080/api/server-info", client.LastEndpoint?.AbsoluteUri);
        Assert.Equal(new ResolvedAddress("127.0.0.1", FromCache: false), resolved);
    }

    [Theory]
    [InlineData("127.0.0.1")]
    [InlineData("10.0.0.1")]
    [InlineData("169.254.1.1")]
    [InlineData("192.168.0.1")]
    public async Task ARemoteServiceCannotPointNovaWorldAtThisMachineOrTheLan(string address)
    {
        var (source, client, settings, saves) = Make();
        client.Result = address;

        Assert.Null(await source.ResolveAsync(CancellationToken.None));
        Assert.Null(settings.CachedOpenNovaAddress);
        Assert.Equal(0, saves());

        // Nor through a cached address a local service gave earlier.
        settings.CachedOpenNovaAddress = address;
        Assert.Null(await source.ResolveAsync(CancellationToken.None));

        // A service on this computer or the LAN may.
        settings.ServiceUrl = "http://192.168.1.20:8080";
        Assert.Equal(new ResolvedAddress(address, FromCache: false), await source.ResolveAsync(CancellationToken.None));
    }

    [Fact]
    public async Task AReplyInACharsetDotNetCannotDecode_FallsBackToTheCache()
    {
        var (source, client, settings, _) = Make();
        settings.CachedOpenNovaAddress = "198.51.100.7";
        client.Throws = new InvalidOperationException("The character set provided in ContentType is invalid.");

        Assert.Equal(new ResolvedAddress("198.51.100.7", FromCache: true), await source.ResolveAsync(CancellationToken.None));
    }

    [Fact]
    public async Task AnUnusableServiceUrl_UsesTheCacheWithoutAsking()
    {
        var (source, client, settings, _) = Make();
        settings.ServiceUrl = "http://evil.example";
        settings.CachedOpenNovaAddress = "198.51.100.7";
        client.Result = "203.0.113.99";

        Assert.Equal(new ResolvedAddress("198.51.100.7", FromCache: true), await source.ResolveAsync(CancellationToken.None));
        Assert.Null(client.LastEndpoint);
    }

    [Fact]
    public void TheHttpClient_CapsTheReply()
    {
        Assert.Equal(64 * 1024, HttpServerInfoClient.MaxReplyBytes);
    }
}
