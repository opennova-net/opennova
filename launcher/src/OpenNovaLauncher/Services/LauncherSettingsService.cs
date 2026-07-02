using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text.Json;
using OpenNova.Launcher.Models;

namespace OpenNova.Launcher.Services;

public sealed class LauncherSettingsService
{
    private readonly string _settingsPath;
    private readonly string? _legacySettingsPath;
    private static readonly JsonSerializerOptions SerializerOptions = new(JsonSerializerDefaults.Web)
    {
        WriteIndented = true
    };

    public LauncherSettingsService()
        : this(
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "OpenNovaLauncher"),
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "OnLauncher", "settings.json"))
    {
    }

    /// <summary>Test-friendly constructor with an injectable settings directory and optional legacy file to migrate.</summary>
    public LauncherSettingsService(string settingsDirectory, string? legacySettingsPath = null)
    {
        if (string.IsNullOrWhiteSpace(settingsDirectory))
        {
            throw new ArgumentException("Settings directory is required", nameof(settingsDirectory));
        }

        Directory.CreateDirectory(settingsDirectory);
        _settingsPath = Path.Combine(settingsDirectory, "settings.json");
        _legacySettingsPath = legacySettingsPath;
    }

    public async Task<LauncherSettings> LoadAsync()
    {
        MigrateLegacySettingsIfNeeded();

        if (!File.Exists(_settingsPath))
        {
            return new LauncherSettings();
        }

        try
        {
            await using var stream = File.OpenRead(_settingsPath);
            var settings = await JsonSerializer.DeserializeAsync<LauncherSettings>(stream, SerializerOptions);
            return Normalize(settings ?? new LauncherSettings());
        }
        catch
        {
            return new LauncherSettings();
        }
    }

    public async Task SaveAsync(LauncherSettings settings)
    {
        var normalized = Normalize(settings);
        normalized.LegacyGameDirectory = null;

        await using var stream = File.Open(_settingsPath, FileMode.Create, FileAccess.Write, FileShare.None);
        await JsonSerializer.SerializeAsync(stream, normalized, SerializerOptions);
    }

    private void MigrateLegacySettingsIfNeeded()
    {
        if (File.Exists(_settingsPath) ||
            string.IsNullOrWhiteSpace(_legacySettingsPath) ||
            !File.Exists(_legacySettingsPath))
        {
            return;
        }

        try
        {
            File.Copy(_legacySettingsPath, _settingsPath);
        }
        catch (Exception ex)
        {
            Debug.WriteLine($"[Settings] Legacy settings migration failed: {ex.Message}");
        }
    }

    private static LauncherSettings Normalize(LauncherSettings settings)
    {
        if (settings.GameDirectories == null)
        {
            settings.GameDirectories = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        }
        else if (settings.GameDirectories.Comparer != StringComparer.OrdinalIgnoreCase)
        {
            settings.GameDirectories = new Dictionary<string, string>(settings.GameDirectories, StringComparer.OrdinalIgnoreCase);
        }

        foreach (var key in settings.GameDirectories.Keys.ToList())
        {
            var value = settings.GameDirectories[key];
            if (string.IsNullOrWhiteSpace(value))
            {
                settings.GameDirectories.Remove(key);
            }
            else
            {
                settings.GameDirectories[key] = value.Trim();
            }
        }

        if (!string.IsNullOrWhiteSpace(settings.LegacyGameDirectory) &&
            !settings.GameDirectories.ContainsKey("jop_2_consumer"))
        {
            settings.GameDirectories["jop_2_consumer"] = settings.LegacyGameDirectory.Trim();
        }

        if (settings.InstalledExpansions == null)
        {
            settings.InstalledExpansions = new Dictionary<string, Dictionary<string, InstalledExpansion>>(StringComparer.OrdinalIgnoreCase);
        }
        else if (settings.InstalledExpansions.Comparer != StringComparer.OrdinalIgnoreCase)
        {
            settings.InstalledExpansions = new Dictionary<string, Dictionary<string, InstalledExpansion>>(settings.InstalledExpansions, StringComparer.OrdinalIgnoreCase);
        }

        foreach (var gameSlug in settings.InstalledExpansions.Keys.ToList())
        {
            var expansions = settings.InstalledExpansions[gameSlug];
            if (expansions == null)
            {
                settings.InstalledExpansions.Remove(gameSlug);
                continue;
            }

            if (expansions.Comparer != StringComparer.OrdinalIgnoreCase)
            {
                settings.InstalledExpansions[gameSlug] = new Dictionary<string, InstalledExpansion>(expansions, StringComparer.OrdinalIgnoreCase);
                expansions = settings.InstalledExpansions[gameSlug];
            }

            foreach (var key in expansions.Keys.ToList())
            {
                if (expansions[key] == null)
                {
                    expansions.Remove(key);
                }
            }
        }

        if (!string.IsNullOrWhiteSpace(settings.CachedServerIp))
        {
            settings.CachedServerIp = settings.CachedServerIp.Trim();
        }
        else
        {
            settings.CachedServerIp = null;
        }

        if (settings.CachedRedirectHostnames != null)
        {
            var hostnames = settings.CachedRedirectHostnames
                .Where(h => !string.IsNullOrWhiteSpace(h))
                .Select(h => h.Trim())
                .ToArray();
            settings.CachedRedirectHostnames = hostnames.Length > 0 ? hostnames : null;
        }

        if (settings.AdvancedArgs != null)
        {
            var advancedArgs = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            foreach (var pair in settings.AdvancedArgs)
            {
                if (!string.IsNullOrWhiteSpace(pair.Key) && !string.IsNullOrWhiteSpace(pair.Value))
                {
                    advancedArgs[pair.Key.Trim()] = pair.Value.Trim();
                }
            }

            settings.AdvancedArgs = advancedArgs.Count > 0 ? advancedArgs : null;
        }

        return settings;
    }
}
