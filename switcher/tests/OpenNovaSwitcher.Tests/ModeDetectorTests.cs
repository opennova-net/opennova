using OpenNova.Switcher.Hosts;
using Xunit;

namespace OpenNova.Switcher.Tests;

public class ModeDetectorTests
{
    private static readonly string[] Names = { "gs.novaworld.net" };

    private static HostsState Detect(string text, IReadOnlyList<string>? names = null)
        => ModeDetector.Detect(HostsDocument.Parse(text, names ?? Names), names ?? Names);

    private static string Block(string? mode, params string[] lines)
        => HostsDocument.BeginMarker + "\r\n" +
           (mode is null ? string.Empty : HostsDocument.ModePrefix + mode + "\r\n") +
           string.Concat(lines.Select(line => line + "\r\n")) +
           HostsDocument.EndMarker + "\r\n";

    [Fact]
    public void TheNameSetIsTheGateHostAlone()
    {
        Assert.Equal(new[] { "gs.novaworld.net" }, NovaWorldHosts.Names);
    }

    [Theory]
    [InlineData("")]
    [InlineData("127.0.0.1 localhost\r\n")]
    public void NoBlock_IsNovaLogic(string text)
    {
        var state = Detect(text);

        Assert.Equal(NovaWorldMode.NovaLogic, state.Mode);
        Assert.Null(state.Address);
        Assert.False(state.HasForeignLines);
    }

    [Fact]
    public void EmptyBlock_IsNovaLogic()
    {
        Assert.Equal(NovaWorldMode.NovaLogic, Detect(Block(ModeDetector.OpenNovaTag)).Mode);
    }

    [Fact]
    public void OpenNovaTaggedBlock_IsOpenNova()
    {
        var state = Detect("127.0.0.1 localhost\r\n" + Block(ModeDetector.OpenNovaTag, "54.162.79.147 gs.novaworld.net"));

        Assert.Equal(NovaWorldMode.OpenNova, state.Mode);
        Assert.Equal("54.162.79.147", state.Address);
    }

    [Fact]
    public void CustomTaggedBlock_IsCustom()
    {
        var state = Detect(Block(ModeDetector.CustomTag, "127.0.0.1 gs.novaworld.net"));

        Assert.Equal(NovaWorldMode.Custom, state.Mode);
        Assert.Equal("127.0.0.1", state.Address);
    }

    [Fact]
    public void BlockWithNoModeLine_IsUnrecognized()
    {
        var state = Detect("127.0.0.1 localhost\r\n\r\n" + Block(null, "54.162.79.147 gs.novaworld.net"));

        Assert.Equal(NovaWorldMode.Unrecognized, state.Mode);
        Assert.Null(state.Address);
    }

    [Fact]
    public void UnknownModeLine_IsUnrecognized()
    {
        var state = Detect(Block("something-else", "127.0.0.1 gs.novaworld.net"));

        Assert.Equal(NovaWorldMode.Unrecognized, state.Mode);
        Assert.Null(state.Address);
    }

    [Theory]
    [InlineData("# mode: opennova\r\n54.162.79.147 gs.novaworld.net\r\n")]                       // no end marker
    [InlineData("# mode: opennova\r\n54.162.79.147 gs.novaworld.net\r\n10.0.0.5 nas.lan\r\n")] // and a user line
    public void DamagedBlocks_AreUnrecognized(string body)
    {
        Assert.Equal(NovaWorldMode.Unrecognized, Detect(HostsDocument.BeginMarker + "\r\n" + body).Mode);
    }

    [Fact]
    public void AUserLineInsideAnIntactPairOfMarkers_IsUnrecognizedUntilASwitch()
    {
        var state = Detect(Block(ModeDetector.OpenNovaTag, "54.162.79.147 gs.novaworld.net", "10.0.0.5 nas.lan"));

        Assert.Equal(NovaWorldMode.Unrecognized, state.Mode);
        Assert.False(state.HasForeignLines);
    }

    [Fact]
    public void HostnamesMatchWithoutCase()
    {
        Assert.Equal(NovaWorldMode.OpenNova, Detect(Block(ModeDetector.OpenNovaTag, "203.0.113.5 GS.NovaWorld.NET")).Mode);
    }

    [Theory]
    [InlineData("203.0.113.5 example.com")]                                  // not our name
    [InlineData("203.0.113.5 gs.novaworld.net", "203.0.113.6 gs.novaworld.net")] // repeated name
    [InlineData("203.0.113.5 gs.novaworld.net example.com")]                // an extra name
    [InlineData("0.0.0.0 gs.novaworld.net")]                                // black-holed
    [InlineData("not-an-ip gs.novaworld.net")]
    public void HandEditedBlocks_AreUnrecognized(params string[] lines)
    {
        var state = Detect(Block(ModeDetector.OpenNovaTag, lines));

        Assert.Equal(NovaWorldMode.Unrecognized, state.Mode);
        Assert.Null(state.Address);
    }

    [Fact]
    public void EveryNameMustShareOneAddress()
    {
        string[] names = { "gs.novaworld.net", "other.example" };

        var same = Detect(Block(ModeDetector.CustomTag, "10.0.0.2 gs.novaworld.net", "10.0.0.2 other.example"), names);
        var split = Detect(Block(ModeDetector.CustomTag, "10.0.0.2 gs.novaworld.net", "10.0.0.3 other.example"), names);
        var missing = Detect(Block(ModeDetector.CustomTag, "10.0.0.2 gs.novaworld.net"), names);

        Assert.Equal(NovaWorldMode.Custom, same.Mode);
        Assert.Equal(NovaWorldMode.Unrecognized, split.Mode);
        Assert.Equal(NovaWorldMode.Unrecognized, missing.Mode);
    }

    [Fact]
    public void ForeignLines_AreReportedBesideTheMode()
    {
        var withBlock = Detect("1.2.3.4 gs.novaworld.net\r\n" + Block(ModeDetector.OpenNovaTag, "54.162.79.147 gs.novaworld.net"));
        var withoutBlock = Detect("127.0.0.1 gs.novaworld.net\r\n");

        Assert.Equal(NovaWorldMode.OpenNova, withBlock.Mode);
        Assert.Equal(new[] { "1.2.3.4 gs.novaworld.net" }, withBlock.ForeignLines);
        Assert.Equal(NovaWorldMode.NovaLogic, withoutBlock.Mode);
        Assert.True(withoutBlock.HasForeignLines);
    }
}
