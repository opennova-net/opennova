using System.Text;

namespace OpenNova.Launcher.Hosts;

/// <summary>
/// Pure (no IO) model of a Windows hosts file with one launcher-managed block.
/// Everything outside the block's markers is preserved character for character;
/// the block is the launcher's alone and is rewritten as a unit:
/// <code>
/// # >>> OpenNova Launcher managed block - do not edit between markers >>>
/// # mode: opennova
/// 203.0.113.10 gs.novaworld.net
/// # &lt;&lt;&lt; OpenNova Launcher managed block &lt;&lt;&lt;
/// </code>
/// The mode line names the choice the block was written for, so the tray can
/// tell OpenNova's address from a custom one without asking the network.
/// </summary>
public sealed class HostsDocument
{
    public const string BeginMarker = "# >>> OpenNova Launcher managed block - do not edit between markers >>>";
    public const string EndMarker = "# <<< OpenNova Launcher managed block <<<";
    public const string ModePrefix = "# mode: ";

    private readonly IReadOnlyList<RawLine> _outsideLines;
    private readonly bool _hasBlock;
    private readonly int _blockIndex;          // insertion index into _outsideLines when _hasBlock
    private readonly bool _appendSeparator;    // emit a blank line before the block
    private readonly string? _mode;
    private readonly IReadOnlyList<HostsRedirect> _managedRedirects;
    private readonly string _lineEnding;

    private readonly record struct RawLine(string Text, string Ending);

    private HostsDocument(
        IReadOnlyList<RawLine> outsideLines,
        bool hasBlock,
        int blockIndex,
        bool appendSeparator,
        string? mode,
        IReadOnlyList<HostsRedirect> managedRedirects,
        string lineEnding)
    {
        _outsideLines = outsideLines;
        _hasBlock = hasBlock;
        _blockIndex = blockIndex;
        _appendSeparator = appendSeparator;
        _mode = mode;
        _managedRedirects = managedRedirects;
        _lineEnding = lineEnding;
    }

    /// <summary>Whether the document holds a managed block (possibly an empty one).</summary>
    public bool HasManagedBlock => _hasBlock;

    /// <summary>The block's mode line, or null when it has none.</summary>
    public string? ManagedMode => _mode;

    /// <summary>The hostname to address lines inside the block, in file order.</summary>
    public IReadOnlyList<HostsRedirect> ManagedRedirects => _managedRedirects;

    /// <summary>
    /// Parses hosts-file text. An empty string yields an empty document (a hosts
    /// file that does not exist yet). Duplicated or broken markers are tolerated:
    /// every managed region collapses into one block at the first marker.
    /// </summary>
    public static HostsDocument Parse(string? text)
    {
        var lines = SplitLines(text ?? string.Empty);
        var lineEnding = DominantLineEnding(lines);

        var outside = new List<RawLine>();
        var managed = new List<HostsRedirect>();
        string? mode = null;
        int? blockIndex = null;
        var absorbedSeparator = false;
        var inBlock = false;

        // Anchors the block at the current position. One blank line directly
        // before the first marker is the block's own separator, so removing the
        // block gives back the file as it was before the block was added.
        void AnchorBlock()
        {
            if (blockIndex.HasValue)
            {
                return;
            }

            if (outside.Count > 0 && string.IsNullOrWhiteSpace(outside[^1].Text))
            {
                outside.RemoveAt(outside.Count - 1);
                absorbedSeparator = true;
            }

            blockIndex = outside.Count;
        }

        foreach (var line in lines)
        {
            var trimmed = line.Text.Trim();
            if (inBlock)
            {
                if (trimmed == EndMarker)
                {
                    inBlock = false;
                }
                else if (trimmed.StartsWith(ModePrefix, StringComparison.Ordinal))
                {
                    mode ??= trimmed[ModePrefix.Length..].Trim();
                }
                else if (trimmed != BeginMarker)
                {
                    managed.AddRange(ParseRedirects(trimmed));
                }

                continue;
            }

            if (trimmed == BeginMarker)
            {
                inBlock = true;
                AnchorBlock();
                continue;
            }

            if (trimmed == EndMarker)
            {
                // A stray end marker with no begin marker: managed debris.
                AnchorBlock();
                continue;
            }

            outside.Add(line);
        }

        return new HostsDocument(
            outside,
            hasBlock: blockIndex.HasValue,
            blockIndex: blockIndex ?? outside.Count,
            appendSeparator: absorbedSeparator,
            mode: mode,
            managedRedirects: managed,
            lineEnding: lineEnding);
    }

    /// <summary>
    /// The lines outside the managed block that map any of <paramref name="hostnames"/>
    /// (comments do not count): entries the user or another tool wrote, which the
    /// launcher never edits without asking.
    /// </summary>
    public IReadOnlyList<string> ForeignLinesFor(IEnumerable<string> hostnames)
    {
        var lookup = BuildHostnameLookup(hostnames);
        return _outsideLines
            .Where(line => LineMentionsHostname(line.Text, lookup))
            .Select(line => line.Text)
            .ToList();
    }

    /// <summary>
    /// A document whose managed block holds exactly <paramref name="redirects"/> under
    /// the mode line <paramref name="mode"/> (none when null). An existing block is
    /// replaced in place; otherwise the block is appended after a blank line.
    /// </summary>
    public HostsDocument WithManagedBlock(string? mode, IReadOnlyList<HostsRedirect> redirects)
    {
        ArgumentNullException.ThrowIfNull(redirects);

        if (_hasBlock)
        {
            return new HostsDocument(_outsideLines, true, _blockIndex, _appendSeparator, mode, redirects.ToArray(), _lineEnding);
        }

        var appendSeparator = _outsideLines.Count > 0 && !string.IsNullOrWhiteSpace(_outsideLines[^1].Text);
        return new HostsDocument(_outsideLines, true, _outsideLines.Count, appendSeparator, mode, redirects.ToArray(), _lineEnding);
    }

    /// <summary>A document with no managed block.</summary>
    public HostsDocument WithoutManagedBlock()
    {
        return new HostsDocument(_outsideLines, false, _outsideLines.Count, false, null, Array.Empty<HostsRedirect>(), _lineEnding);
    }

    /// <summary>
    /// A document without the outside lines that map any of <paramref name="hostnames"/>
    /// (<see cref="ForeignLinesFor"/>). Only after the user agreed to it.
    /// </summary>
    public HostsDocument WithoutForeignLines(IEnumerable<string> hostnames)
    {
        var lookup = BuildHostnameLookup(hostnames);
        var outside = new List<RawLine>();
        var blockIndex = _blockIndex;
        for (var i = 0; i < _outsideLines.Count; i++)
        {
            if (LineMentionsHostname(_outsideLines[i].Text, lookup))
            {
                if (i < _blockIndex)
                {
                    blockIndex--;
                }

                continue;
            }

            outside.Add(_outsideLines[i]);
        }

        return new HostsDocument(outside, _hasBlock, blockIndex, _appendSeparator, _mode, _managedRedirects, _lineEnding);
    }

    /// <summary>
    /// The document's text. Lines outside the markers keep their own text and line
    /// ending, so a document without a block renders as it was parsed. The block's
    /// lines use the document's dominant line ending (CRLF when mixed or empty); a
    /// last line with no line ending gets one when the block follows it.
    /// </summary>
    public string Render()
    {
        if (!_hasBlock && _outsideLines.Count == 0)
        {
            return string.Empty;
        }

        var blockLast = _hasBlock && _blockIndex >= _outsideLines.Count;
        var builder = new StringBuilder();
        for (var i = 0; i < _outsideLines.Count; i++)
        {
            if (_hasBlock && i == _blockIndex)
            {
                AppendBlock(builder);
            }

            var line = _outsideLines[i];
            builder.Append(line.Text);
            if (line.Ending.Length > 0)
            {
                builder.Append(line.Ending);
            }
            else if (i < _outsideLines.Count - 1 || blockLast)
            {
                builder.Append(_lineEnding);
            }
        }

        if (blockLast)
        {
            AppendBlock(builder);
        }

        return builder.ToString();
    }

    private void AppendBlock(StringBuilder builder)
    {
        if (_appendSeparator)
        {
            builder.Append(_lineEnding);
        }

        builder.Append(BeginMarker).Append(_lineEnding);
        if (!string.IsNullOrEmpty(_mode))
        {
            builder.Append(ModePrefix).Append(_mode).Append(_lineEnding);
        }

        foreach (var redirect in _managedRedirects)
        {
            builder.Append(redirect.IPv4).Append(' ').Append(redirect.Hostname).Append(_lineEnding);
        }

        builder.Append(EndMarker).Append(_lineEnding);
    }

    private static List<RawLine> SplitLines(string text)
    {
        var lines = new List<RawLine>();
        var start = 0;
        for (var i = 0; i < text.Length; i++)
        {
            if (text[i] != '\n')
            {
                continue;
            }

            var hasCarriageReturn = i > start && text[i - 1] == '\r';
            var contentEnd = hasCarriageReturn ? i - 1 : i;
            lines.Add(new RawLine(text[start..contentEnd], hasCarriageReturn ? "\r\n" : "\n"));
            start = i + 1;
        }

        if (start < text.Length)
        {
            lines.Add(new RawLine(text[start..], string.Empty));
        }

        return lines;
    }

    private static string DominantLineEnding(IReadOnlyList<RawLine> lines)
    {
        var crlf = lines.Count(line => line.Ending == "\r\n");
        var lf = lines.Count(line => line.Ending == "\n");
        return lf > crlf ? "\n" : "\r\n";
    }

    // "<address> <name> [<name> ...] [# comment]": one redirect per name.
    private static IEnumerable<HostsRedirect> ParseRedirects(string trimmedLine)
    {
        var tokens = EffectiveTokens(trimmedLine);
        for (var i = 1; i < tokens.Length; i++)
        {
            yield return new HostsRedirect(tokens[i], tokens[0]);
        }
    }

    private static string[] EffectiveTokens(string lineText)
    {
        var commentStart = lineText.IndexOf('#');
        var effective = commentStart >= 0 ? lineText[..commentStart] : lineText;
        return effective.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
    }

    private static HashSet<string> BuildHostnameLookup(IEnumerable<string> hostnames)
    {
        ArgumentNullException.ThrowIfNull(hostnames);
        return new HashSet<string>(
            hostnames.Where(h => !string.IsNullOrWhiteSpace(h)).Select(h => h.Trim()),
            StringComparer.OrdinalIgnoreCase);
    }

    // A hosts line maps the names after its first token; the address itself never counts.
    private static bool LineMentionsHostname(string lineText, HashSet<string> hostnames)
    {
        if (hostnames.Count == 0)
        {
            return false;
        }

        var tokens = EffectiveTokens(lineText);
        return tokens.Skip(1).Any(hostnames.Contains);
    }
}
