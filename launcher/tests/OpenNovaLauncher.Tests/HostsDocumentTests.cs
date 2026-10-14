using OpenNova.Launcher.Hosts;
using Xunit;

namespace OpenNova.Launcher.Tests;

public class HostsDocumentTests
{
    private const string Mode = ModeDetector.OpenNovaTag;
    private static readonly HostsRedirect[] SingleRedirect = { new("gs.novaworld.net", "203.0.113.10") };

    private static string Block(string ip = "203.0.113.10", string hostname = "gs.novaworld.net", string newline = "\r\n", string? mode = Mode)
        => HostsDocument.BeginMarker + newline +
           (mode is null ? string.Empty : HostsDocument.ModePrefix + mode + newline) +
           $"{ip} {hostname}" + newline +
           HostsDocument.EndMarker + newline;

    [Fact]
    public void Parse_EmptyText_YieldsEmptyDocument()
    {
        var document = HostsDocument.Parse(string.Empty);

        Assert.False(document.HasManagedBlock);
        Assert.Empty(document.ManagedRedirects);
        Assert.Null(document.ManagedMode);
        Assert.Equal(string.Empty, document.Render());
    }

    [Theory]
    [InlineData("127.0.0.1 localhost\r\n")]
    [InlineData("127.0.0.1 localhost")]
    [InlineData("# comment\n127.0.0.1 localhost\n\n")]
    [InlineData("mixed\r\nendings\nhere")]
    public void Render_WithoutBlock_GivesBackTheParsedText(string text)
    {
        Assert.Equal(text, HostsDocument.Parse(text).Render());
    }

    [Fact]
    public void Parse_ReadsTheBlocksModeAndRedirects()
    {
        var document = HostsDocument.Parse("127.0.0.1 localhost\r\n" + Block());

        Assert.True(document.HasManagedBlock);
        Assert.Equal(Mode, document.ManagedMode);
        var redirect = Assert.Single(document.ManagedRedirects);
        Assert.Equal("gs.novaworld.net", redirect.Hostname);
        Assert.Equal("203.0.113.10", redirect.IPv4);
    }

    [Theory]
    [InlineData("198.51.100.1 gs.novaworld.net novaworld.net")]
    [InlineData("198.51.100.1 gs.novaworld.net # note")]
    [InlineData("10.0.0.5 nas.lan")]
    [InlineData("# a comment of the user's")]
    public void Parse_LinesTheLauncherNeverWrites_AreKeptAfterTheBlock(string line)
    {
        var text = HostsDocument.BeginMarker + "\r\n# mode: opennova\r\n203.0.113.10 gs.novaworld.net\r\n" +
                   line + "\r\n" + HostsDocument.EndMarker + "\r\n";

        var document = HostsDocument.Parse(text);

        Assert.False(document.ManagedBlockIntact);
        Assert.Equal(new[] { new HostsRedirect("gs.novaworld.net", "203.0.113.10") }, document.ManagedRedirects);
        Assert.Equal(Block() + line + "\r\n", document.WithManagedBlock(Mode, SingleRedirect).Render());
        Assert.Equal(line + "\r\n", document.WithoutManagedBlock().Render());
    }

    [Fact]
    public void Parse_IntactBlock_IsIntact()
    {
        Assert.True(HostsDocument.Parse("127.0.0.1 localhost\r\n\r\n" + Block()).ManagedBlockIntact);
        Assert.True(HostsDocument.Parse("127.0.0.1 localhost\r\n").ManagedBlockIntact);
    }

    [Fact]
    public void WithManagedBlock_OnEmptyDocument_RendersJustTheBlock()
    {
        var rendered = HostsDocument.Parse(string.Empty).WithManagedBlock(Mode, SingleRedirect).Render();

        Assert.Equal(Block(), rendered);
    }

    [Fact]
    public void WithManagedBlock_WithoutMode_WritesNoModeLine()
    {
        var rendered = HostsDocument.Parse(string.Empty).WithManagedBlock(null, SingleRedirect).Render();

        Assert.Equal(Block(mode: null), rendered);
    }

    [Fact]
    public void WithManagedBlock_OnExistingContent_AppendsAfterABlankLine()
    {
        const string existing = "# My hosts file\r\n127.0.0.1 localhost\r\n";

        var rendered = HostsDocument.Parse(existing).WithManagedBlock(Mode, SingleRedirect).Render();

        Assert.Equal(existing + "\r\n" + Block(), rendered);
    }

    [Fact]
    public void WithManagedBlock_ReplacesTheBlockInPlace()
    {
        var original = "127.0.0.1 localhost\r\n" + Block(ip: "198.51.100.1", mode: ModeDetector.CustomTag) + "# trailing comment\r\n";

        var rendered = HostsDocument.Parse(original).WithManagedBlock(Mode, SingleRedirect).Render();

        Assert.Equal("127.0.0.1 localhost\r\n" + Block() + "# trailing comment\r\n", rendered);
    }

    [Fact]
    public void WithManagedBlock_IsIdempotent()
    {
        var first = HostsDocument.Parse("127.0.0.1 localhost\r\n").WithManagedBlock(Mode, SingleRedirect).Render();
        var second = HostsDocument.Parse(first).WithManagedBlock(Mode, SingleRedirect).Render();
        var third = HostsDocument.Parse(second).WithManagedBlock(Mode, SingleRedirect).Render();

        Assert.Equal(first, second);
        Assert.Equal(second, third);
    }

    [Fact]
    public void WithoutManagedBlock_RemovesTheBlockAndKeepsOutsideContent()
    {
        var original = "127.0.0.1 localhost\r\n" + Block() + "# after\r\n";

        var rendered = HostsDocument.Parse(original).WithoutManagedBlock().Render();

        Assert.Equal("127.0.0.1 localhost\r\n# after\r\n", rendered);
        Assert.DoesNotContain(HostsDocument.BeginMarker, rendered);
        Assert.DoesNotContain(HostsDocument.EndMarker, rendered);
    }

    [Fact]
    public void AddThenRemove_GivesBackTheOriginalBytes()
    {
        var userContent =
            "# Copyright (c) 1993-2009 Microsoft Corp.\r\n" +
            "#\r\n" +
            "#\tweird   spacing\t preserved \r\n" +
            "\r\n" +
            "# Added by Docker Desktop\r\n" +
            "192.168.1.5 host.docker.internal\r\n" +
            "192.168.1.5 gateway.docker.internal\r\n" +
            "# Café ÿ end of section\r\n";

        var added = HostsDocument.Parse(userContent).WithManagedBlock(Mode, SingleRedirect).Render();
        Assert.StartsWith(userContent, added, StringComparison.Ordinal);

        var removed = HostsDocument.Parse(added).WithoutManagedBlock().Render();
        Assert.Equal(userContent, removed);
    }

    [Fact]
    public void Render_CrlfDocumentStaysCrlf()
    {
        var rendered = HostsDocument.Parse("127.0.0.1 localhost\r\n").WithManagedBlock(Mode, SingleRedirect).Render();

        Assert.DoesNotContain("\n", rendered.Replace("\r\n", string.Empty), StringComparison.Ordinal);
    }

    [Fact]
    public void Render_LfDocumentStaysLf()
    {
        var rendered = HostsDocument.Parse("127.0.0.1 localhost\n# comment\n").WithManagedBlock(Mode, SingleRedirect).Render();

        Assert.Equal("127.0.0.1 localhost\n# comment\n\n" + Block(newline: "\n"), rendered);
    }

    [Fact]
    public void Render_FileWithoutFinalNewline_StillEndsWithoutOne()
    {
        var added = HostsDocument.Parse("127.0.0.1 localhost").WithManagedBlock(Mode, SingleRedirect).Render();

        Assert.Equal("127.0.0.1 localhost\r\n\r\n" + Block()[..^2], added);
        Assert.Equal("127.0.0.1 localhost", HostsDocument.Parse(added).WithoutManagedBlock().Render());
    }

    [Fact]
    public void Parse_DuplicatedBlocks_CollapseIntoOne()
    {
        var corrupted = "127.0.0.1 localhost\r\n" + Block(ip: "198.51.100.1") + "# middle\r\n" + Block(ip: "198.51.100.2");

        var rendered = HostsDocument.Parse(corrupted).WithManagedBlock(Mode, SingleRedirect).Render();

        Assert.Equal("127.0.0.1 localhost\r\n" + Block() + "# middle\r\n", rendered);
        Assert.Equal(1, CountOccurrences(rendered, HostsDocument.BeginMarker));
        Assert.Equal(1, CountOccurrences(rendered, HostsDocument.EndMarker));
    }

    [Fact]
    public void Parse_BeginMarkerWithoutEnd_IsToleratedAndCollapsed()
    {
        var corrupted = "127.0.0.1 localhost\r\n" + HostsDocument.BeginMarker + "\r\n" + "198.51.100.1 gs.novaworld.net\r\n";

        var document = HostsDocument.Parse(corrupted);
        Assert.Single(document.ManagedRedirects);
        Assert.False(document.ManagedBlockIntact);

        Assert.Equal("127.0.0.1 localhost\r\n" + Block(), document.WithManagedBlock(Mode, SingleRedirect).Render());
    }

    [Fact]
    public void Parse_StrayEndMarker_IsToleratedAndCollapsed()
    {
        var corrupted = "127.0.0.1 localhost\r\n" + HostsDocument.EndMarker + "\r\n" + "# after\r\n";

        var rendered = HostsDocument.Parse(corrupted).WithManagedBlock(Mode, SingleRedirect).Render();

        Assert.Equal("127.0.0.1 localhost\r\n" + Block() + "# after\r\n", rendered);
        Assert.Equal(1, CountOccurrences(rendered, HostsDocument.EndMarker));
    }

    [Fact]
    public void ForeignLinesFor_FindsMappingsOutsideTheBlockOnly()
    {
        var text =
            "1.2.3.4 gs.novaworld.net\r\n" +
            "5.6.7.8 example.com GS.NovaWorld.net\r\n" +
            "9.9.9.9 example.org # gs.novaworld.net in a comment\r\n" +
            "# 9.9.9.9 gs.novaworld.net commented out\r\n" +
            "gs.novaworld.net\r\n" +
            Block();

        var foreign = HostsDocument.Parse(text).ForeignLinesFor(new[] { "gs.novaworld.net" });

        Assert.Equal(new[] { "1.2.3.4 gs.novaworld.net", "5.6.7.8 example.com GS.NovaWorld.net" }, foreign);
    }

    [Fact]
    public void ForeignLinesFor_IgnoresUnrelatedLines()
    {
        var document = HostsDocument.Parse("127.0.0.1 localhost\r\n5.6.7.8 example.com\r\n");

        Assert.Empty(document.ForeignLinesFor(new[] { "gs.novaworld.net" }));
    }

    [Fact]
    public void WithoutForeignLines_RemovesOnlyThoseLinesAndKeepsTheBlock()
    {
        var text = "1.2.3.4 gs.novaworld.net\r\n5.6.7.8 example.com\r\n" + Block() + "4.3.2.1 gs.novaworld.net\r\n";

        var rendered = HostsDocument.Parse(text).WithoutForeignLines(new[] { "gs.novaworld.net" }).Render();

        Assert.Equal("5.6.7.8 example.com\r\n" + Block(), rendered);
    }

    private static int CountOccurrences(string text, string token)
    {
        var count = 0;
        var index = 0;
        while ((index = text.IndexOf(token, index, StringComparison.Ordinal)) >= 0)
        {
            count++;
            index += token.Length;
        }

        return count;
    }
}
