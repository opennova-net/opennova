using System.Text;
using OpenNova.Launcher.Hosts;
using OpenNova.Launcher.Services;
using Xunit;

namespace OpenNova.Launcher.Tests;

/// <summary>
/// The PR #1123 review's edge cases: damaged blocks, user lines inside the markers,
/// separators and final newlines, the original kept, unsafe addresses.
/// </summary>
public sealed class EdgeCaseTests : IDisposable
{
    private static readonly HostsRedirect[] One = { new("gs.novaworld.net", "203.0.113.10") };
    private readonly string _directory = Path.Combine(Path.GetTempPath(), "opennova-launcher-edge-" + Guid.NewGuid().ToString("N"));

    public EdgeCaseTests()
    {
        Directory.CreateDirectory(_directory);
    }

    public void Dispose()
    {
        foreach (var entry in Directory.EnumerateFileSystemEntries(_directory))
        {
            File.SetAttributes(entry, FileAttributes.Normal);
        }

        Directory.Delete(_directory, recursive: true);
    }

    [Fact]
    public void BeginWithoutEnd_LinesAfterTheBlockSurviveASwitchToNovaLogic()
    {
        var text = "127.0.0.1 localhost\r\n" +
                   HostsDocument.BeginMarker + "\r\n" +
                   "# mode: opennova\r\n" +
                   "203.0.113.10 gs.novaworld.net\r\n" +
                   // The end marker deleted by hand, then more editing:
                   "10.0.0.5 nas.lan\r\n" +
                   "192.168.1.9 printer.lan\r\n";
        var store = new MemStore(text);

        new HostsSwitcher(store, new NoFlush()).Switch(NovaWorldMode.NovaLogic, null, false);

        Assert.Equal("127.0.0.1 localhost\r\n10.0.0.5 nas.lan\r\n192.168.1.9 printer.lan\r\n", store.Text);
    }

    [Fact]
    public void BeginWithoutEnd_LinesAfterTheBlockSurviveASwitchToOpenNova()
    {
        var store = new MemStore(HostsDocument.BeginMarker + "\r\n" + "10.0.0.5 nas.lan\r\n");

        new HostsSwitcher(store, new NoFlush()).Switch(NovaWorldMode.OpenNova, "203.0.113.10", false);

        Assert.Equal(
            HostsDocument.BeginMarker + "\r\n# mode: opennova\r\n203.0.113.10 gs.novaworld.net\r\n" + HostsDocument.EndMarker +
            "\r\n10.0.0.5 nas.lan\r\n",
            store.Text);
    }

    [Fact]
    public void BeginWithoutEnd_ReadsAsDamagedNotOpenNova()
    {
        var text = HostsDocument.BeginMarker + "\r\n# mode: opennova\r\n203.0.113.10 gs.novaworld.net\r\n10.0.0.5 nas.lan\r\n";

        var state = new HostsSwitcher(new MemStore(text), new NoFlush()).ReadState();

        Assert.Equal(NovaWorldMode.Unrecognized, state.Mode);
    }

    [Fact]
    public void BeginWithoutEnd_ALaterMappingOfTheNameIsForeign()
    {
        var text = HostsDocument.BeginMarker + "\r\n203.0.113.10 gs.novaworld.net\r\n10.0.0.5 nas.lan\r\n10.1.1.1 gs.novaworld.net\r\n";

        var state = new HostsSwitcher(new MemStore(text), new NoFlush()).ReadState();

        Assert.Equal(new[] { "10.1.1.1 gs.novaworld.net" }, state.ForeignLines);
    }

    [Fact]
    public void BeginWithoutEnd_AUsersOwnMappingBelowIt_IsForeignAndAsked()
    {
        // The review's U1: the user's own line for the name, right below a block that lost
        // its end marker, must not be taken into the block and dropped unasked.
        var text = "127.0.0.1 localhost\r\n" + HostsDocument.BeginMarker + "\r\n# mode: opennova\r\n" +
                   "203.0.113.10 gs.novaworld.net\r\n" +
                   "192.168.1.50 gs.novaworld.net\r\n";
        var store = new MemStore(text);
        var switcher = new HostsSwitcher(store, new NoFlush());

        var state = switcher.ReadState();
        Assert.Equal(NovaWorldMode.Unrecognized, state.Mode);
        Assert.Equal(new[] { "192.168.1.50 gs.novaworld.net" }, state.ForeignLines);

        // A switch without the user's yes keeps it; with it, the line goes.
        switcher.Switch(NovaWorldMode.NovaLogic, null, removeForeign: false);
        Assert.Equal("127.0.0.1 localhost\r\n192.168.1.50 gs.novaworld.net\r\n", store.Text);
        switcher.Switch(NovaWorldMode.NovaLogic, null, removeForeign: true);
        Assert.Equal("127.0.0.1 localhost\r\n", store.Text);
    }

    [Theory]
    [InlineData("\r\n192.168.1.50 gs.novaworld.net\r\n", "\r\n192.168.1.50 gs.novaworld.net\r\n")]   // a blank line ends it
    [InlineData("# mode: custom\r\n192.168.1.50 gs.novaworld.net\r\n", "# mode: custom\r\n192.168.1.50 gs.novaworld.net\r\n")]
    public void BeginWithoutEnd_EndsAtABlankLineOrASecondModeLine(string after, string kept)
    {
        var text = HostsDocument.BeginMarker + "\r\n# mode: opennova\r\n203.0.113.10 gs.novaworld.net\r\n" + after;
        var document = HostsDocument.Parse(text);

        Assert.False(document.ManagedBlockIntact);
        Assert.Equal(new[] { new HostsRedirect("gs.novaworld.net", "203.0.113.10") }, document.ManagedRedirects);
        Assert.Equal(kept, document.WithoutManagedBlock().Render());
        Assert.Equal(new[] { "192.168.1.50 gs.novaworld.net" }, document.ForeignLinesFor(new[] { "gs.novaworld.net" }));
    }

    [Fact]
    public void AUserLineInsideTheBlock_IsKeptBySwitches()
    {
        var text = HostsDocument.BeginMarker + "\r\n# mode: opennova\r\n203.0.113.10 gs.novaworld.net\r\n10.0.0.5 nas.lan\r\n" +
                   HostsDocument.EndMarker + "\r\n";
        var store = new MemStore(text);
        var switcher = new HostsSwitcher(store, new NoFlush());

        switcher.Switch(NovaWorldMode.OpenNova, "203.0.113.10", false);
        Assert.Contains("10.0.0.5 nas.lan", store.Text, StringComparison.Ordinal);
        Assert.Equal(NovaWorldMode.OpenNova, switcher.ReadState().Mode);

        switcher.Switch(NovaWorldMode.NovaLogic, null, false);
        Assert.Equal("10.0.0.5 nas.lan\r\n", store.Text);
    }

    [Fact]
    public void ATrailingBlankLine_SurvivesAddThenRemove()
    {
        const string user = "127.0.0.1 localhost\r\n\r\n";

        var added = HostsDocument.Parse(user).WithManagedBlock("opennova", One).Render();

        Assert.Equal(user, HostsDocument.Parse(added).WithoutManagedBlock().Render());
    }

    [Fact]
    public void AMissingFinalNewline_SurvivesAddThenRemove()
    {
        const string user = "127.0.0.1 localhost";

        var added = HostsDocument.Parse(user).WithManagedBlock("opennova", One).Render();

        Assert.Equal(user, HostsDocument.Parse(added).WithoutManagedBlock().Render());
    }

    [Fact]
    public void AWhitespaceOnlyLineBeforeTheBlock_IsTheUsersAndStays()
    {
        var text = "a\r\n \t \r\n" + HostsDocument.BeginMarker + "\r\n" + HostsDocument.EndMarker + "\r\n";

        Assert.Equal("a\r\n \t \r\n", HostsDocument.Parse(text).WithoutManagedBlock().Render());
    }

    [Fact]
    public void TheFileBeforeTheFirstWrite_IsKeptThroughLaterWrites()
    {
        var path = Path.Combine(_directory, "hosts");
        const string pristine = "127.0.0.1 localhost\r\n";
        File.WriteAllText(path, pristine);
        var store = new HostsFileStore(path);
        var switcher = new HostsSwitcher(store, new NoFlush());

        switcher.Switch(NovaWorldMode.OpenNova, "203.0.113.10", false);
        switcher.Switch(NovaWorldMode.Custom, "127.0.0.1", false);

        Assert.Equal(pristine, File.ReadAllText(store.OriginalPath));
        Assert.NotEqual(pristine, File.ReadAllText(store.BackupPath));
    }

    [Fact]
    public void AUtf16HostsFile_IsNotTurnedIntoMixedEncoding()
    {
        var path = Path.Combine(_directory, "hosts");
        var original = Encoding.Unicode.GetPreamble().Concat(Encoding.Unicode.GetBytes("127.0.0.1 localhost\r\n")).ToArray();
        File.WriteAllBytes(path, original);

        Assert.ThrowsAny<IOException>(() =>
            new HostsSwitcher(new HostsFileStore(path), new NoFlush()).Switch(NovaWorldMode.OpenNova, "203.0.113.10", false));

        Assert.Equal(original, File.ReadAllBytes(path));
    }

    [Theory]
    [InlineData("1.2.3.4\n5.6.7.8 evil")]
    [InlineData("1.2.3.4 evil.com")]
    [InlineData("01.2.3.4")]
    [InlineData("1.2.3.4 ")]
    [InlineData(" 1.2.3.4")]
    [InlineData("１.2.3.4")]
    [InlineData("0.0.0.0")]
    [InlineData("224.0.0.1")]
    [InlineData("255.255.255.255")]
    public void UnsafeAddresses_AreRejected(string address)
    {
        Assert.False(Ipv4.IsUsable(address));
        Assert.Null(ServerInfo.ParseAddress("{\"novaworld_ip\":\"" + address.Replace("\n", "\\n") + "\"}"));
    }

    private sealed class MemStore : IHostsFileStore
    {
        public MemStore(string text) => Text = text;

        public string Text { get; private set; }

        public bool Exists => true;

        public string Read() => Text;

        public void Write(string contents, string expected)
        {
            if (!string.Equals(Text, expected, StringComparison.Ordinal))
            {
                throw new HostsFileChangedException();
            }

            Text = contents;
        }
    }

    private sealed class NoFlush : IDnsCacheFlusher
    {
        public void Flush()
        {
        }
    }
}
