using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;

namespace OpenNova.Launcher.Hosts;

/// <summary>
/// Pure (no IO) model of a Windows hosts file with one launcher-managed block.
/// Everything outside the managed-block markers is byte-preserved; the managed
/// block is fully owned by the launcher and rewritten as a unit.
/// </summary>
public sealed class HostsDocument
{
    public const string BeginMarker = "# >>> OpenNova Launcher managed block - do not edit between markers >>>";
    public const string EndMarker = "# <<< OpenNova Launcher managed block <<<";

    private readonly IReadOnlyList<RawLine> _outsideLines;
    private readonly bool _hasBlock;
    private readonly int _blockIndex;          // insertion index into _outsideLines when _hasBlock
    private readonly bool _appendSeparator;    // emit a blank line before a freshly appended block
    private readonly IReadOnlyList<HostsRedirect> _managedRedirects;
    private readonly string _lineEnding;

    private readonly record struct RawLine(string Text, string Ending);

    private HostsDocument(
        IReadOnlyList<RawLine> outsideLines,
        bool hasBlock,
        int blockIndex,
        bool appendSeparator,
        IReadOnlyList<HostsRedirect> managedRedirects,
        string lineEnding)
    {
        _outsideLines = outsideLines;
        _hasBlock = hasBlock;
        _blockIndex = blockIndex;
        _appendSeparator = appendSeparator;
        _managedRedirects = managedRedirects;
        _lineEnding = lineEnding;
    }

    public IReadOnlyList<HostsRedirect> ManagedRedirects => _managedRedirects;

    /// <summary>
    /// Parses hosts-file text. An empty string yields an empty document (used when
    /// the hosts file does not exist yet). Duplicated or corrupted managed-block
    /// markers are tolerated: every managed region collapses into a single block
    /// anchored at the first marker position.
    /// </summary>
    public static HostsDocument Parse(string text)
    {
        text ??= string.Empty;
        var lines = SplitLines(text);
        var lineEnding = DominantLineEnding(lines);

        var outside = new List<RawLine>();
        var managed = new List<HostsRedirect>();
        int? blockIndex = null;
        var absorbedSeparator = false;
        var inBlock = false;

        // Anchors the managed block at the current position. A single blank line
        // directly before the first marker is absorbed as the launcher's own
        // separator so removing the block restores the original file exactly.
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
                if (string.Equals(trimmed, EndMarker, StringComparison.Ordinal))
                {
                    inBlock = false;
                }
                else if (!string.Equals(trimmed, BeginMarker, StringComparison.Ordinal) &&
                         TryParseRedirect(trimmed, out var redirect))
                {
                    managed.Add(redirect);
                }

                continue;
            }

            if (string.Equals(trimmed, BeginMarker, StringComparison.Ordinal))
            {
                inBlock = true;
                AnchorBlock();
                continue;
            }

            if (string.Equals(trimmed, EndMarker, StringComparison.Ordinal))
            {
                // Stray end marker without a begin marker: swallow it as managed debris.
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
            managedRedirects: managed,
            lineEnding: lineEnding);
    }

    /// <summary>
    /// Non-comment lines outside the managed block that mention any of the given
    /// hostnames. Used to detect entries placed by the user or other tools that
    /// would conflict with the launcher-managed redirection.
    /// </summary>
    public IReadOnlyList<string> ForeignLinesFor(IEnumerable<string> hostnames)
    {
        var lookup = BuildHostnameLookup(hostnames);
        var result = new List<string>();
        foreach (var line in _outsideLines)
        {
            if (LineMentionsHostname(line.Text, lookup))
            {
                result.Add(line.Text);
            }
        }

        return result;
    }

    /// <summary>
    /// Returns a document whose managed block holds exactly <paramref name="redirects"/>.
    /// An existing block is replaced in place; otherwise the block is appended at the
    /// end with a separating blank line.
    /// </summary>
    public HostsDocument WithManagedBlock(IReadOnlyList<HostsRedirect> redirects)
    {
        ArgumentNullException.ThrowIfNull(redirects);

        if (_hasBlock)
        {
            return new HostsDocument(_outsideLines, true, _blockIndex, _appendSeparator, redirects.ToArray(), _lineEnding);
        }

        var appendSeparator = _outsideLines.Count > 0 &&
                              !string.IsNullOrWhiteSpace(_outsideLines[^1].Text);
        return new HostsDocument(_outsideLines, true, _outsideLines.Count, appendSeparator, redirects.ToArray(), _lineEnding);
    }

    /// <summary>Returns a document without any managed block.</summary>
    public HostsDocument WithoutManagedBlock()
    {
        return new HostsDocument(_outsideLines, false, _outsideLines.Count, false, Array.Empty<HostsRedirect>(), _lineEnding);
    }

    /// <summary>
    /// Returns a document with foreign (outside-the-block) lines mentioning any of the
    /// given hostnames removed. Supports HostsFileService.TryCleanForeignLines.
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

        return new HostsDocument(outside, _hasBlock, blockIndex, _appendSeparator, _managedRedirects, _lineEnding);
    }

    /// <summary>
    /// Renders the document. Content outside the markers is byte-preserved; managed
    /// block lines use the document's dominant line ending (CRLF when mixed or empty);
    /// the result always ends with a trailing newline.
    /// </summary>
    public string Render()
    {
        if (!_hasBlock && _outsideLines.Count == 0)
        {
            return string.Empty;
        }

        var builder = new StringBuilder();
        for (var i = 0; i < _outsideLines.Count; i++)
        {
            if (_hasBlock && i == _blockIndex)
            {
                AppendBlock(builder);
            }

            var line = _outsideLines[i];
            builder.Append(line.Text);
            builder.Append(line.Ending.Length > 0 ? line.Ending : _lineEnding);
        }

        if (_hasBlock && _blockIndex >= _outsideLines.Count)
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
        var crlf = 0;
        var lf = 0;
        foreach (var line in lines)
        {
            switch (line.Ending)
            {
                case "\r\n":
                    crlf++;
                    break;
                case "\n":
                    lf++;
                    break;
            }
        }

        return lf > crlf ? "\n" : "\r\n";
    }

    private static bool TryParseRedirect(string trimmedLine, out HostsRedirect redirect)
    {
        redirect = default;
        if (trimmedLine.Length == 0 || trimmedLine.StartsWith('#'))
        {
            return false;
        }

        var tokens = trimmedLine.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
        if (tokens.Length < 2)
        {
            return false;
        }

        redirect = new HostsRedirect(tokens[1], tokens[0]);
        return true;
    }

    private static HashSet<string> BuildHostnameLookup(IEnumerable<string> hostnames)
    {
        ArgumentNullException.ThrowIfNull(hostnames);
        return new HashSet<string>(
            hostnames.Where(h => !string.IsNullOrWhiteSpace(h)).Select(h => h.Trim()),
            StringComparer.OrdinalIgnoreCase);
    }

    private static bool LineMentionsHostname(string lineText, HashSet<string> hostnames)
    {
        if (hostnames.Count == 0)
        {
            return false;
        }

        var commentStart = lineText.IndexOf('#');
        var effective = commentStart >= 0 ? lineText[..commentStart] : lineText;
        if (string.IsNullOrWhiteSpace(effective))
        {
            return false;
        }

        var tokens = effective.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
        return tokens.Any(hostnames.Contains);
    }
}
