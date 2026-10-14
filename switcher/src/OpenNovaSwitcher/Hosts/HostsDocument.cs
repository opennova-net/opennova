using System.Text;

namespace OpenNova.Switcher.Hosts;

/// <summary>
/// Pure (no IO) model of a Windows hosts file with one switcher-managed block:
/// <code>
/// # >>> OpenNova Switcher managed block - do not edit between markers >>>
/// # mode: opennova
/// 203.0.113.10 gs.novaworld.net
/// # &lt;&lt;&lt; OpenNova Switcher managed block &lt;&lt;&lt;
/// </code>
/// The mode line names the choice the block was written for, so the tray can tell
/// OpenNova's address from a custom one without asking the network.
/// <para>
/// The block holds only the lines the switcher writes: the mode line, blank lines and
/// <c>&lt;IPv4&gt; &lt;managed hostname&gt;</c>, which it rewrites as a unit. Every other
/// line keeps its text and line ending: a line found inside the markers is kept,
/// after the block, and a block whose end marker is gone ends at its first line that
/// is not the switcher's, its first blank line, or a second line for a name it already
/// holds. Either marks the block not intact. A file that ends without
/// a newline still does after the block is added or removed, and adding then removing
/// the block gives back the original text.
/// </para>
/// </summary>
public sealed class HostsDocument
{
    public const string BeginMarker = "# >>> OpenNova Switcher managed block - do not edit between markers >>>";
    public const string EndMarker = "# <<< OpenNova Switcher managed block <<<";
    public const string ModePrefix = "# mode: ";

    private readonly IReadOnlyList<RawLine> _outsideLines;
    private readonly bool _hasBlock;
    private readonly int _blockIndex;        // insertion index into _outsideLines when _hasBlock
    private readonly bool _separator;        // one empty line before the block, the block's own
    private readonly bool _intact;           // the block parsed as one the switcher wrote
    private readonly string? _mode;
    private readonly IReadOnlyList<HostsRedirect> _managedRedirects;
    private readonly string _lineEnding;
    private readonly bool _noFinalNewline;

    private readonly record struct RawLine(string Text, string Ending);

    private HostsDocument(
        IReadOnlyList<RawLine> outsideLines,
        bool hasBlock,
        int blockIndex,
        bool separator,
        bool intact,
        string? mode,
        IReadOnlyList<HostsRedirect> managedRedirects,
        string lineEnding,
        bool noFinalNewline)
    {
        _outsideLines = outsideLines;
        _hasBlock = hasBlock;
        _blockIndex = blockIndex;
        _separator = separator;
        _intact = intact;
        _mode = mode;
        _managedRedirects = managedRedirects;
        _lineEnding = lineEnding;
        _noFinalNewline = noFinalNewline;
    }

    /// <summary>Whether the document holds a managed block (possibly an empty one).</summary>
    public bool HasManagedBlock => _hasBlock;

    /// <summary>
    /// False when the block was not as the switcher writes it: its end marker missing,
    /// a stray end marker, a second block, a second mode line, or a line of the user's
    /// inside it.
    /// </summary>
    public bool ManagedBlockIntact => !_hasBlock || _intact;

    /// <summary>The block's mode line, or null when it has none.</summary>
    public string? ManagedMode => _mode;

    /// <summary>The hostname to address lines inside the block, in file order.</summary>
    public IReadOnlyList<HostsRedirect> ManagedRedirects => _managedRedirects;

    /// <summary>Parses hosts-file text, with the switcher's hostnames as the managed set.</summary>
    public static HostsDocument Parse(string? text) => Parse(text, NovaWorldHosts.Names);

    /// <summary>
    /// Parses hosts-file text. An empty string yields an empty document (a hosts file
    /// that does not exist yet). Only <c>&lt;IPv4&gt; &lt;name&gt;</c> lines naming one of
    /// <paramref name="managedHostnames"/> belong to the block; damaged markers are
    /// tolerated and collapse into one block at the first marker.
    /// </summary>
    public static HostsDocument Parse(string? text, IEnumerable<string> managedHostnames)
    {
        var lookup = BuildHostnameLookup(managedHostnames);
        var lines = SplitLines(text ?? string.Empty);
        var lineEnding = DominantLineEnding(lines);
        var noFinalNewline = lines.Count > 0 && lines[^1].Ending.Length == 0;

        var lastEnd = -1;
        for (var i = 0; i < lines.Count; i++)
        {
            if (lines[i].Text.Trim() == EndMarker)
            {
                lastEnd = i;
            }
        }

        var outside = new List<RawLine>();
        var managed = new List<HostsRedirect>();
        string? mode = null;
        int? blockIndex = null;
        var separator = false;
        var intact = true;
        var inBlock = false;

        // Anchors the block at the current position. One empty line directly before
        // the first marker is the block's own separator (the switcher writes it), so
        // removing the block gives back the file as it was before the block was added.
        void AnchorBlock()
        {
            if (outside.Count > 0 && outside[^1].Text.Length == 0)
            {
                outside.RemoveAt(outside.Count - 1);
                separator = true;
            }

            blockIndex = outside.Count;
        }

        var blockNames = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        for (var i = 0; i < lines.Count; i++)
        {
            var line = lines[i];
            var trimmed = line.Text.Trim();
            if (inBlock && i > lastEnd && EndsUnterminatedBlock(trimmed, mode is not null, blockNames, lookup))
            {
                // No end marker follows, so where the block stops is a guess; it stops
                // early rather than take a line the switcher would not have written there:
                // its first blank line, a second mode line, or a second line for a name it
                // already holds (a user's own mapping below a damaged block, which then
                // stays outside, a foreign line the user is asked about).
                intact = false;
                inBlock = false;
                outside.Add(line);
                continue;
            }

            if (inBlock)
            {
                if (trimmed == EndMarker)
                {
                    inBlock = false;
                }
                else if (trimmed == BeginMarker)
                {
                    intact = false;
                }
                else if (trimmed.Length == 0)
                {
                    // A blank line inside the block is the switcher's to drop.
                }
                else if (trimmed.StartsWith(ModePrefix, StringComparison.Ordinal))
                {
                    if (mode is null)
                    {
                        mode = trimmed[ModePrefix.Length..].Trim();
                    }
                    else
                    {
                        intact = false;
                    }
                }
                else if (TryParseOwnRedirect(trimmed, lookup, out var redirect))
                {
                    managed.Add(redirect);
                    blockNames.Add(redirect.Hostname);
                }
                else
                {
                    // A line the switcher never writes: the user's, kept after the block.
                    // With no end marker after it, the block ended on the line above.
                    intact = false;
                    outside.Add(line);
                    if (i > lastEnd)
                    {
                        inBlock = false;
                    }
                }

                continue;
            }

            if (trimmed == BeginMarker)
            {
                if (blockIndex.HasValue)
                {
                    intact = false;   // a second block: its lines join the first
                }
                else
                {
                    AnchorBlock();
                }

                inBlock = true;
                continue;
            }

            if (trimmed == EndMarker)
            {
                // A stray end marker with no begin marker: managed debris.
                intact = false;
                if (!blockIndex.HasValue)
                {
                    AnchorBlock();
                }

                continue;
            }

            outside.Add(line);
        }

        if (inBlock)
        {
            intact = false;   // the end marker is missing
        }

        return new HostsDocument(
            outside,
            hasBlock: blockIndex.HasValue,
            blockIndex: blockIndex ?? outside.Count,
            separator: separator,
            intact: intact,
            mode: mode,
            managedRedirects: managed,
            lineEnding: lineEnding,
            noFinalNewline: noFinalNewline);
    }

    /// <summary>
    /// The lines outside the managed block that map any of <paramref name="hostnames"/>
    /// (comments do not count): entries the user or another tool wrote, which the
    /// switcher never edits without asking.
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
    /// replaced where it is; otherwise the block is appended after an empty line.
    /// </summary>
    public HostsDocument WithManagedBlock(string? mode, IReadOnlyList<HostsRedirect> redirects)
    {
        ArgumentNullException.ThrowIfNull(redirects);

        if (_hasBlock)
        {
            return new HostsDocument(_outsideLines, true, _blockIndex, _separator, true, mode, redirects.ToArray(), _lineEnding, _noFinalNewline);
        }

        return new HostsDocument(_outsideLines, true, _outsideLines.Count, _outsideLines.Count > 0, true, mode, redirects.ToArray(), _lineEnding, _noFinalNewline);
    }

    /// <summary>A document with no managed block; lines kept from inside it stay.</summary>
    public HostsDocument WithoutManagedBlock()
    {
        return new HostsDocument(_outsideLines, false, _outsideLines.Count, false, true, null, Array.Empty<HostsRedirect>(), _lineEnding, _noFinalNewline);
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

        return new HostsDocument(outside, _hasBlock, blockIndex, _separator, _intact, _mode, _managedRedirects, _lineEnding, _noFinalNewline);
    }

    /// <summary>
    /// The document's text. Lines outside the markers keep their text and line ending;
    /// the block's lines use the document's dominant line ending (CRLF when mixed or
    /// empty). When the parsed text ended without a newline, so does this one.
    /// </summary>
    public string Render()
    {
        var parts = new List<RawLine>();
        for (var i = 0; i < _outsideLines.Count; i++)
        {
            if (_hasBlock && i == _blockIndex)
            {
                AddBlock(parts);
            }

            var line = _outsideLines[i];
            parts.Add(line.Ending.Length > 0 ? line : line with { Ending = _lineEnding });
        }

        if (_hasBlock && _blockIndex >= _outsideLines.Count)
        {
            AddBlock(parts);
        }

        var builder = new StringBuilder();
        for (var i = 0; i < parts.Count; i++)
        {
            builder.Append(parts[i].Text);
            if (!(_noFinalNewline && i == parts.Count - 1))
            {
                builder.Append(parts[i].Ending);
            }
        }

        return builder.ToString();
    }

    private void AddBlock(List<RawLine> parts)
    {
        if (_separator)
        {
            parts.Add(new RawLine(string.Empty, _lineEnding));
        }

        parts.Add(new RawLine(BeginMarker, _lineEnding));
        if (!string.IsNullOrEmpty(_mode))
        {
            parts.Add(new RawLine(ModePrefix + _mode, _lineEnding));
        }

        foreach (var redirect in _managedRedirects)
        {
            parts.Add(new RawLine(redirect.IPv4 + " " + redirect.Hostname, _lineEnding));
        }

        parts.Add(new RawLine(EndMarker, _lineEnding));
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

    // Inside a block with no end marker after it: whether this line ends the block before it.
    private static bool EndsUnterminatedBlock(string trimmedLine, bool hasMode, HashSet<string> blockNames, HashSet<string> managedHostnames)
    {
        if (trimmedLine.Length == 0)
        {
            return true;
        }

        if (trimmedLine.StartsWith(ModePrefix, StringComparison.Ordinal))
        {
            return hasMode;
        }

        return TryParseOwnRedirect(trimmedLine, managedHostnames, out var redirect) && blockNames.Contains(redirect.Hostname);
    }

    // Exactly what the switcher writes: "<dotted quad> <managed name>", nothing else.
    private static bool TryParseOwnRedirect(string trimmedLine, HashSet<string> managedHostnames, out HostsRedirect redirect)
    {
        redirect = default;
        if (trimmedLine.Contains('#'))
        {
            return false;
        }

        var tokens = trimmedLine.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
        if (tokens.Length != 2 || !Ipv4.TryParse(tokens[0], out _) || !managedHostnames.Contains(tokens[1]))
        {
            return false;
        }

        redirect = new HostsRedirect(tokens[1], tokens[0]);
        return true;
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
