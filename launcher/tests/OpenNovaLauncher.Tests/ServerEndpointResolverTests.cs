using OpenNova.Launcher.Models;
using OpenNova.Launcher.Services;
using Xunit;

namespace OpenNova.Launcher.Tests;

public class ServerEndpointResolverTests
{
    private sealed class StubServerInfoClient : IServerInfoClient
    {
        public ServerInfo? Result { get; set; }
        public Exception? Throws { get; set; }
        public int CallCount { get; private set; }

        public Task<ServerInfo?> FetchAsync(CancellationToken cancellationToken)
        {
            CallCount++;
            if (Throws != null)
            {
                throw Throws;
            }

            return Task.FromResult(Result);
        }
    }

    private sealed class StubDnsResolver : IDnsResolver
    {
        public string? Result { get; set; }
        public Exception? Throws { get; set; }
        public string? LastHostname { get; private set; }
        public int CallCount { get; private set; }

        public Task<string?> ResolveIPv4Async(string hostname, CancellationToken cancellationToken)
        {
            CallCount++;
            LastHostname = hostname;
            if (Throws != null)
            {
                throw Throws;
            }

            return Task.FromResult(Result);
        }
    }

    private sealed class Harness
    {
        public LauncherSettings Settings { get; } = new();
        public StubServerInfoClient ServerInfo { get; } = new();
        public StubDnsResolver Dns { get; } = new();
        public int SaveCount { get; private set; }

        public ServerEndpointResolver CreateResolver() => new(
            ServerInfo,
            Dns,
            () => Settings,
            () =>
            {
                SaveCount++;
                return Task.CompletedTask;
            });
    }

    [Fact]
    public async Task DevMode_Returns127001WithDefaultHostnames()
    {
        var harness = new Harness();

        var endpoint = await harness.CreateResolver().ResolveAsync(devMode: true, CancellationToken.None);

        Assert.NotNull(endpoint);
        Assert.Equal("127.0.0.1", endpoint!.IPv4);
        Assert.Equal(new[] { "gs.novaworld.net" }, endpoint.RedirectHostnames);
        Assert.Equal(0, harness.ServerInfo.CallCount);
        Assert.Equal(0, harness.Dns.CallCount);
    }

    [Fact]
    public async Task DevMode_UsesCachedHostnamesWhenPresent()
    {
        var harness = new Harness();
        harness.Settings.CachedRedirectHostnames = new[] { "gs.novaworld.net", "extra.novaworld.net" };

        var endpoint = await harness.CreateResolver().ResolveAsync(devMode: true, CancellationToken.None);

        Assert.NotNull(endpoint);
        Assert.Equal("127.0.0.1", endpoint!.IPv4);
        Assert.Equal(new[] { "gs.novaworld.net", "extra.novaworld.net" }, endpoint.RedirectHostnames);
    }

    [Fact]
    public async Task ServerInfoSuccess_ParsesAndCaches()
    {
        var harness = new Harness();
        harness.ServerInfo.Result = new ServerInfo("203.0.113.10", new[] { "gs.novaworld.net" });

        var endpoint = await harness.CreateResolver().ResolveAsync(devMode: false, CancellationToken.None);

        Assert.NotNull(endpoint);
        Assert.Equal("203.0.113.10", endpoint!.IPv4);
        Assert.Equal(new[] { "gs.novaworld.net" }, endpoint.RedirectHostnames);

        Assert.Equal("203.0.113.10", harness.Settings.CachedServerIp);
        Assert.Equal(new[] { "gs.novaworld.net" }, harness.Settings.CachedRedirectHostnames);
        Assert.Equal(1, harness.SaveCount);
        Assert.Equal(0, harness.Dns.CallCount);
    }

    [Fact]
    public async Task ServerInfoFailure_FallsBackToCachedIp()
    {
        var harness = new Harness();
        harness.ServerInfo.Throws = new HttpRequestException("offline");
        harness.Settings.CachedServerIp = "198.51.100.7";
        harness.Settings.CachedRedirectHostnames = new[] { "gs.novaworld.net" };

        var endpoint = await harness.CreateResolver().ResolveAsync(devMode: false, CancellationToken.None);

        Assert.NotNull(endpoint);
        Assert.Equal("198.51.100.7", endpoint!.IPv4);
        Assert.Equal(new[] { "gs.novaworld.net" }, endpoint.RedirectHostnames);
        Assert.Equal(0, harness.Dns.CallCount);
    }

    [Fact]
    public async Task ServerInfoNull_FallsBackToCachedIp()
    {
        var harness = new Harness();
        harness.ServerInfo.Result = null;
        harness.Settings.CachedServerIp = "198.51.100.7";

        var endpoint = await harness.CreateResolver().ResolveAsync(devMode: false, CancellationToken.None);

        Assert.NotNull(endpoint);
        Assert.Equal("198.51.100.7", endpoint!.IPv4);
    }

    [Fact]
    public async Task CacheMiss_FallsBackToDnsAnchorLookup()
    {
        var harness = new Harness();
        harness.ServerInfo.Throws = new HttpRequestException("offline");
        harness.Dns.Result = "192.0.2.33";

        var endpoint = await harness.CreateResolver().ResolveAsync(devMode: false, CancellationToken.None);

        Assert.NotNull(endpoint);
        Assert.Equal("192.0.2.33", endpoint!.IPv4);
        Assert.Equal(new[] { "gs.novaworld.net" }, endpoint.RedirectHostnames);
        Assert.Equal("nw.opennova.net", harness.Dns.LastHostname);

        // The DNS result is cached for the next offline run.
        Assert.Equal("192.0.2.33", harness.Settings.CachedServerIp);
        Assert.Equal(1, harness.SaveCount);
    }

    [Fact]
    public async Task EverythingFails_ReturnsNull()
    {
        var harness = new Harness();
        harness.ServerInfo.Throws = new HttpRequestException("offline");
        harness.Dns.Throws = new InvalidOperationException("no dns");

        var endpoint = await harness.CreateResolver().ResolveAsync(devMode: false, CancellationToken.None);

        Assert.Null(endpoint);
        Assert.Null(harness.Settings.CachedServerIp);
        Assert.Equal(0, harness.SaveCount);
    }
}
