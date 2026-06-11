using OpenNova.Launcher.Hosts;
using Xunit;

namespace OpenNova.Launcher.Tests;

public class HostsDocumentTests
{
    private static readonly HostsRedirect GsRedirect = new("gs.novaworld.net", "203.0.113.10");
    private static readonly HostsRedirect[] SingleRedirect = { GsRedirect };

    private static string Block(string ip = "203.0.113.10", string hostname = "gs.novaworld.net", string newline = "\r\n")
        => HostsDocument.BeginMarker + newline + $"{ip} {hostname}" + newline + HostsDocument.EndMarker + newline;

    [Fact]
    public void Parse_EmptyText_YieldsEmptyDocument()
    {
        var document = HostsDocument.Parse(string.Empty);

        Assert.Empty(document.ManagedRedirects);
        Assert.Equal(string.Empty, document.Render());
    }

    [Fact]
    public void WithManagedBlock_OnEmptyDocument_RendersJustTheBlock()
    {
        var rendered = HostsDocument.Parse(string.Empty)
            .WithManagedBlock(SingleRedirect)
            .Render();

        Assert.Equal(Block(), rendered);
    }

    [Fact]
    public void WithManagedBlock_OnExistingContent_AppendsWithSeparatingBlankLine()
    {
        const string existing = "# My hosts file\r\n127.0.0.1 localhost\r\n";

        var rendered = HostsDocument.Parse(existing)
            .WithManagedBlock(SingleRedirect)
            .Render();

        Assert.Equal(existing + "\r\n" + Block(), rendered);
    }

    [Fact]
    public void WithManagedBlock_ReplacesExistingBlockInPlace()
    {
        var original = "127.0.0.1 localhost\r\n" + Block(ip: "198.51.100.1") + "# trailing comment\r\n";

        var rendered = HostsDocument.Parse(original)
            .WithManagedBlock(SingleRedirect)
            .Render();

        Assert.Equal("127.0.0.1 localhost\r\n" + Block() + "# trailing comment\r\n", rendered);
    }

    [Fact]
    public void WithManagedBlock_IsIdempotent()
    {
        var first = HostsDocument.Parse("127.0.0.1 localhost\r\n")
            .WithManagedBlock(SingleRedirect)
            .Render();

        var second = HostsDocument.Parse(first)
            .WithManagedBlock(SingleRedirect)
            .Render();

        var third = HostsDocument.Parse(second)
            .WithManagedBlock(SingleRedirect)
            .Render();

        Assert.Equal(first, second);
        Assert.Equal(second, third);
    }

    [Fact]
    public void WithoutManagedBlock_RemovesTheBlockAndKeepsOutsideContent()
    {
        var original = "127.0.0.1 localhost\r\n" + Block() + "# after\r\n";

        var rendered = HostsDocument.Parse(original)
            .WithoutManagedBlock()
            .Render();

        Assert.Equal("127.0.0.1 localhost\r\n# after\r\n", rendered);
        Assert.DoesNotContain(HostsDocument.BeginMarker, rendered);
        Assert.DoesNotContain(HostsDocument.EndMarker, rendered);
    }

    [Fact]
    public void Render_BytePreservesUserContentOutsideMarkers()
    {
        var userContent =
            "# Copyright (c) 1993-2009 Microsoft Corp.\r\n" +
            "#\r\n" +
            "#\tweird   spacing\t preserved \r\n" +
            "\r\n" +
            "# Added by Docker Desktop\r\n" +
            "192.168.1.5 host.docker.internal\r\n" +
            "192.168.1.5 gateway.docker.internal\r\n" +
            "# End of section\r\n";

        var rendered = HostsDocument.Parse(userContent)
            .WithManagedBlock(SingleRedirect)
            .Render();

        Assert.StartsWith(userContent, rendered, StringComparison.Ordinal);

        var removed = HostsDocument.Parse(rendered).WithoutManagedBlock().Render();
        Assert.Equal(userContent, removed);
    }

    [Fact]
    public void Render_PreservesCrlfLineEndings()
    {
        var rendered = HostsDocument.Parse("127.0.0.1 localhost\r\n")
            .WithManagedBlock(SingleRedirect)
            .Render();

        Assert.Contains("\r\n", rendered, StringComparison.Ordinal);
        // No bare LF anywhere once CRLF pairs are stripped.
        Assert.DoesNotContain("\n", rendered.Replace("\r\n", string.Empty), StringComparison.Ordinal);
    }

    [Fact]
    public void Render_LfOnlyDocumentStaysLf()
    {
        var rendered = HostsDocument.Parse("127.0.0.1 localhost\n# comment\n")
            .WithManagedBlock(SingleRedirect)
            .Render();

        Assert.DoesNotContain("\r", rendered, StringComparison.Ordinal);
        Assert.Equal("127.0.0.1 localhost\n# comment\n\n" + Block(newline: "\n"), rendered);
    }

    [Fact]
    public void Parse_DuplicatedBlocks_CollapseIntoOne()
    {
        var corrupted =
            "127.0.0.1 localhost\r\n" +
            Block(ip: "198.51.100.1") +
            "# middle\r\n" +
            Block(ip: "198.51.100.2");

        var rendered = HostsDocument.Parse(corrupted)
            .WithManagedBlock(SingleRedirect)
            .Render();

        Assert.Equal("127.0.0.1 localhost\r\n" + Block() + "# middle\r\n", rendered);
        Assert.Equal(1, CountOccurrences(rendered, HostsDocument.BeginMarker));
        Assert.Equal(1, CountOccurrences(rendered, HostsDocument.EndMarker));
    }

    [Fact]
    public void Parse_BeginMarkerWithoutEnd_IsToleratedAndCollapsed()
    {
        var corrupted =
            "127.0.0.1 localhost\r\n" +
            HostsDocument.BeginMarker + "\r\n" +
            "198.51.100.1 gs.novaworld.net\r\n";

        var document = HostsDocument.Parse(corrupted);
        Assert.Single(document.ManagedRedirects);

        var rendered = document.WithManagedBlock(SingleRedirect).Render();
        Assert.Equal("127.0.0.1 localhost\r\n" + Block(), rendered);
    }

    [Fact]
    public void Parse_StrayEndMarker_IsToleratedAndCollapsed()
    {
        var corrupted =
            "127.0.0.1 localhost\r\n" +
            HostsDocument.EndMarker + "\r\n" +
            "# after\r\n";

        var rendered = HostsDocument.Parse(corrupted)
            .WithManagedBlock(SingleRedirect)
            .Render();

        Assert.Equal("127.0.0.1 localhost\r\n" + Block() + "# after\r\n", rendered);
        Assert.Equal(1, CountOccurrences(rendered, HostsDocument.EndMarker));
    }

    [Fact]
    public void ManagedRedirects_ParsesEntriesInsideTheBlock()
    {
        var text =
            Block() +
            string.Empty;

        var document = HostsDocument.Parse(text);

        var redirect = Assert.Single(document.ManagedRedirects);
        Assert.Equal("gs.novaworld.net", redirect.Hostname);
        Assert.Equal("203.0.113.10", redirect.IPv4);
    }

    [Fact]
    public void ForeignLinesFor_DetectsStrayEntryOutsideTheBlock()
    {
        var text =
            "1.2.3.4 gs.novaworld.net\r\n" +
            "5.6.7.8 example.com\r\n" +
            "# 9.9.9.9 gs.novaworld.net commented out\r\n" +
            Block();

        var document = HostsDocument.Parse(text);
        var foreign = document.ForeignLinesFor(new[] { "gs.novaworld.net" });

        var line = Assert.Single(foreign);
        Assert.Equal("1.2.3.4 gs.novaworld.net", line);
    }

    [Fact]
    public void ForeignLinesFor_IgnoresUnrelatedLines()
    {
        var text =
            "127.0.0.1 localhost\r\n" +
            "5.6.7.8 example.com\r\n";

        var document = HostsDocument.Parse(text);

        Assert.Empty(document.ForeignLinesFor(new[] { "gs.novaworld.net" }));
    }

    [Fact]
    public void WithoutForeignLines_RemovesOnlyMatchingLines()
    {
        var text =
            "1.2.3.4 gs.novaworld.net\r\n" +
            "5.6.7.8 example.com\r\n" +
            Block();

        var rendered = HostsDocument.Parse(text)
            .WithoutForeignLines(new[] { "gs.novaworld.net" })
            .Render();

        Assert.Equal("5.6.7.8 example.com\r\n" + Block(), rendered);
    }

    [Fact]
    public void Render_GuaranteesTrailingNewline()
    {
        var withoutTrailing = "127.0.0.1 localhost";

        var rendered = HostsDocument.Parse(withoutTrailing)
            .WithManagedBlock(SingleRedirect)
            .Render();

        Assert.EndsWith("\r\n", rendered, StringComparison.Ordinal);

        var plain = HostsDocument.Parse(withoutTrailing).Render();
        Assert.Equal("127.0.0.1 localhost\r\n", plain);
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
