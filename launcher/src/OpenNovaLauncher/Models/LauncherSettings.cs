using System;
using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace OpenNova.Launcher.Models;

public sealed class LauncherSettings
{
    public Dictionary<string, string> GameDirectories { get; set; } = new(StringComparer.OrdinalIgnoreCase);

    [JsonPropertyName("gameDirectory")]
    public string? LegacyGameDirectory { get; set; }

    public Dictionary<string, Dictionary<string, InstalledExpansion>> InstalledExpansions { get; set; } =
        new(StringComparer.OrdinalIgnoreCase);

    /// <summary>Whether the launcher manages the NovaWorld hosts-file redirection.</summary>
    public bool RedirectionEnabled { get; set; }

    /// <summary>Developer mode: redirect the NovaWorld hostnames to 127.0.0.1.</summary>
    public bool DevMode { get; set; }

    /// <summary>Last successfully resolved OpenNova NovaWorld server IPv4.</summary>
    public string? CachedServerIp { get; set; }

    /// <summary>Last known set of hostnames to redirect.</summary>
    public string[]? CachedRedirectHostnames { get; set; }

    /// <summary>Free-form extra command-line arguments per game slug, appended verbatim at launch.</summary>
    public Dictionary<string, string>? AdvancedArgs { get; set; }
}
