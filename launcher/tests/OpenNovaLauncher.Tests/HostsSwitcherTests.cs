using OpenNova.Launcher.Hosts;
using OpenNova.Launcher.Services;
using Xunit;

namespace OpenNova.Launcher.Tests;

public class HostsSwitcherTests
{
    private const string Stock = "# Copyright (c) 1993-2009 Microsoft Corp.\r\n127.0.0.1 localhost\r\n";

    private sealed class MemoryStore : IHostsFileStore
    {
        public string? Text { get; set; }
        public int Writes { get; private set; }
        public bool Exists => Text is not null;
        public string Read() => Text ?? throw new FileNotFoundException();

        public void Write(string contents)
        {
            Writes++;
            Text = contents;
        }
    }

    private sealed class CountingFlusher : IDnsCacheFlusher
    {
        public int Flushes { get; private set; }
        public void Flush() => Flushes++;
    }

    private static (HostsSwitcher Switcher, MemoryStore Store, CountingFlusher Flusher) Make(string? text = Stock)
    {
        var store = new MemoryStore { Text = text };
        var flusher = new CountingFlusher();
        return (new HostsSwitcher(store, flusher), store, flusher);
    }

    [Fact]
    public void SwitchToOpenNova_WritesTheBlockAndFlushes()
    {
        var (switcher, store, flusher) = Make();

        Assert.True(switcher.Switch(NovaWorldMode.OpenNova, "54.162.79.147", removeForeign: false));

        Assert.Equal(
            Stock + "\r\n" + HostsDocument.BeginMarker + "\r\n# mode: opennova\r\n54.162.79.147 gs.novaworld.net\r\n" +
            HostsDocument.EndMarker + "\r\n",
            store.Text);
        Assert.Equal(1, flusher.Flushes);
        var state = switcher.ReadState();
        Assert.Equal(NovaWorldMode.OpenNova, state.Mode);
        Assert.Equal("54.162.79.147", state.Address);
    }

    [Fact]
    public void SwitchingBetweenModes_RoundTripsToTheStockFile()
    {
        var (switcher, store, _) = Make();

        switcher.Switch(NovaWorldMode.OpenNova, "54.162.79.147", removeForeign: false);
        switcher.Switch(NovaWorldMode.Custom, "127.0.0.1", removeForeign: false);
        var custom = switcher.ReadState();
        switcher.Switch(NovaWorldMode.NovaLogic, null, removeForeign: false);

        Assert.Equal(NovaWorldMode.Custom, custom.Mode);
        Assert.Equal("127.0.0.1", custom.Address);
        Assert.Equal(Stock, store.Text);
        Assert.Equal(NovaWorldMode.NovaLogic, switcher.ReadState().Mode);
    }

    [Fact]
    public void ASwitchThatChangesNothing_DoesNotWrite()
    {
        var (switcher, store, flusher) = Make();

        Assert.False(switcher.Switch(NovaWorldMode.NovaLogic, null, removeForeign: false));
        switcher.Switch(NovaWorldMode.OpenNova, "203.0.113.10", removeForeign: false);
        Assert.False(switcher.Switch(NovaWorldMode.OpenNova, "203.0.113.10", removeForeign: false));

        Assert.Equal(1, store.Writes);
        Assert.Equal(1, flusher.Flushes);
    }

    [Fact]
    public void MissingHostsFile_IsNovaLogicAndIsCreatedOnSwitch()
    {
        var (switcher, store, _) = Make(text: null);

        Assert.Equal(NovaWorldMode.NovaLogic, switcher.ReadState().Mode);
        Assert.False(switcher.Switch(NovaWorldMode.NovaLogic, null, removeForeign: false));
        switcher.Switch(NovaWorldMode.Custom, "192.168.1.20", removeForeign: false);

        Assert.StartsWith(HostsDocument.BeginMarker, store.Text, StringComparison.Ordinal);
        Assert.Equal(NovaWorldMode.Custom, switcher.ReadState().Mode);
    }

    [Fact]
    public void ForeignLines_StayUnlessRemovalIsAsked()
    {
        var (switcher, store, _) = Make(Stock + "10.1.1.1 gs.novaworld.net\r\n");

        switcher.Switch(NovaWorldMode.OpenNova, "54.162.79.147", removeForeign: false);
        Assert.Contains("10.1.1.1 gs.novaworld.net", store.Text, StringComparison.Ordinal);
        Assert.True(switcher.ReadState().HasForeignLines);

        switcher.Switch(NovaWorldMode.OpenNova, "54.162.79.147", removeForeign: true);
        Assert.DoesNotContain("10.1.1.1", store.Text, StringComparison.Ordinal);
        Assert.False(switcher.ReadState().HasForeignLines);
        Assert.Equal(NovaWorldMode.OpenNova, switcher.ReadState().Mode);
    }

    [Fact]
    public void RemoveForeignLines_KeepsTheModeAndEveryOtherLine()
    {
        var (switcher, store, _) = Make(Stock + "10.1.1.1 gs.novaworld.net\r\n10.1.1.2 example.com\r\n");
        switcher.Switch(NovaWorldMode.Custom, "127.0.0.1", removeForeign: false);

        Assert.True(switcher.RemoveForeignLines());

        Assert.StartsWith(Stock + "10.1.1.2 example.com\r\n", store.Text, StringComparison.Ordinal);
        Assert.Equal(NovaWorldMode.Custom, switcher.ReadState().Mode);
        Assert.False(switcher.RemoveForeignLines());
    }

    [Theory]
    [InlineData(null)]
    [InlineData("")]
    [InlineData("0.0.0.0")]
    [InlineData("example.com")]
    public void AddressModes_RefuseAnUnusableAddress(string? address)
    {
        var (switcher, store, _) = Make();

        Assert.Throws<ArgumentException>(() => switcher.Switch(NovaWorldMode.Custom, address, removeForeign: false));
        Assert.Equal(Stock, store.Text);
    }
}
