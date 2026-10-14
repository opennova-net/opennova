using System.Diagnostics;
using System.Text.Json;
using OpenNova.Launcher.Hosts;

namespace OpenNova.Launcher.Services;

/// <summary>The launcher's few preferences, in %APPDATA%\OpenNova Launcher\settings.json.</summary>
public sealed class LauncherSettings
{
    /// <summary>The OpenNova service whose <c>/api/server-info</c> names its NovaWorld address.</summary>
    public string ServiceUrl { get; set; } = ServerInfo.DefaultServiceUrl;

    /// <summary>The last address that route gave, used when it cannot be reached.</summary>
    public string? CachedOpenNovaAddress { get; set; }

    /// <summary>The last custom address entered, offered again next time.</summary>
    public string? CustomAddress { get; set; }
}

public sealed class LauncherSettingsStore
{
    private static readonly JsonSerializerOptions SerializerOptions = new(JsonSerializerDefaults.Web)
    {
        WriteIndented = true,
    };

    public LauncherSettingsStore()
        : this(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "OpenNova Launcher"))
    {
    }

    public LauncherSettingsStore(string directory)
    {
        if (string.IsNullOrWhiteSpace(directory))
        {
            throw new ArgumentException("A settings directory is required.", nameof(directory));
        }

        SettingsPath = Path.Combine(directory, "settings.json");
    }

    public string SettingsPath { get; }

    /// <summary>The saved settings; the defaults when there are none or they do not parse.</summary>
    public LauncherSettings Load()
    {
        try
        {
            if (!File.Exists(SettingsPath))
            {
                return new LauncherSettings();
            }

            var settings = JsonSerializer.Deserialize<LauncherSettings>(File.ReadAllText(SettingsPath), SerializerOptions);
            return Normalize(settings ?? new LauncherSettings());
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException)
        {
            Debug.WriteLine($"[Settings] Load failed, using defaults: {ex.Message}");
            return new LauncherSettings();
        }
    }

    /// <summary>Writes the settings through a temp file; IO failures reach the caller.</summary>
    public void Save(LauncherSettings settings)
    {
        ArgumentNullException.ThrowIfNull(settings);
        var directory = Path.GetDirectoryName(SettingsPath)!;
        Directory.CreateDirectory(directory);
        var tempPath = SettingsPath + ".tmp";
        File.WriteAllText(tempPath, JsonSerializer.Serialize(Normalize(settings), SerializerOptions));
        File.Move(tempPath, SettingsPath, overwrite: true);
    }

    private static LauncherSettings Normalize(LauncherSettings settings)
    {
        settings.ServiceUrl = ServerInfo.EndpointFor(settings.ServiceUrl) is null
            ? ServerInfo.DefaultServiceUrl
            : settings.ServiceUrl.Trim();
        settings.CachedOpenNovaAddress = Ipv4.IsUsable(settings.CachedOpenNovaAddress) ? settings.CachedOpenNovaAddress : null;
        settings.CustomAddress = Ipv4.IsUsable(settings.CustomAddress) ? settings.CustomAddress : null;
        return settings;
    }
}
