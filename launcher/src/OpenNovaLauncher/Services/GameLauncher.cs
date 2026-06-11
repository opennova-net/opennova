using System;
using System.Collections.Generic;
using System.Diagnostics;

namespace OpenNova.Launcher.Services;

public sealed record LaunchOptions(
    bool Windowed = false,
    bool MultiInstance = false,
    string? ExpansionSlug = null,
    string? AdvancedArgs = null);

/// <summary>
/// Starts the stock game executable directly. The launcher never patches or
/// replaces game files; it passes the game's own command-line flags:
/// /w (windowed), /many (multiple instances), /exp &lt;slug&gt; (expansion).
/// </summary>
public sealed class GameLauncher
{
    public static string BuildArguments(LaunchOptions options)
    {
        ArgumentNullException.ThrowIfNull(options);

        var parts = new List<string>();
        if (options.Windowed)
        {
            parts.Add("/w");
        }

        if (options.MultiInstance)
        {
            parts.Add("/many");
        }

        if (!string.IsNullOrWhiteSpace(options.ExpansionSlug))
        {
            parts.Add($"/exp {options.ExpansionSlug.Trim()}");
        }

        if (!string.IsNullOrWhiteSpace(options.AdvancedArgs))
        {
            parts.Add(options.AdvancedArgs.Trim());
        }

        return string.Join(' ', parts);
    }

    public void Launch(GameInstallation installation, LaunchOptions options)
    {
        ArgumentNullException.ThrowIfNull(installation);
        ArgumentNullException.ThrowIfNull(options);

        var startInfo = new ProcessStartInfo
        {
            FileName = installation.ExecutablePath,
            WorkingDirectory = installation.DirectoryPath,
            Arguments = BuildArguments(options),
            UseShellExecute = false,
        };

        Process.Start(startInfo);
    }
}
