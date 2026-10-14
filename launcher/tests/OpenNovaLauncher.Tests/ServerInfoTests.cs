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
    public void EndpointFor_RefusesWhatIsNotAnHttpUrl(string? serviceUrl)
    {
        Assert.Null(ServerInfo.EndpointFor(serviceUrl));
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

        await source.ResolveAsync(CancellationToken.None);

        Assert.Equal("http://127.0.0.1:8080/api/server-info", client.LastEndpoint?.AbsoluteUri);
    }
}
