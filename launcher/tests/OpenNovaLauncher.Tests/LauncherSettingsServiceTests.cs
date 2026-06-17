using System.Text.Json;
using OpenNova.Launcher.Models;
using OpenNova.Launcher.Services;
using Xunit;

namespace OpenNova.Launcher.Tests;

public class LauncherSettingsServiceTests : IDisposable
{
    private readonly string _tempRoot;

    public LauncherSettingsServiceTests()
    {
        _tempRoot = Path.Combine(Path.GetTempPath(), $"onlauncher-tests-{Guid.NewGuid():N}");
        Directory.CreateDirectory(_tempRoot);
    }

    public void Dispose()
    {
        try
        {
            Directory.Delete(_tempRoot, recursive: true);
        }
        catch
        {
            // best effort
        }
    }

    private string NewSettingsDir() => Path.Combine(_tempRoot, Guid.NewGuid().ToString("N"));

    [Fact]
    public async Task Load_WithoutFile_ReturnsDefaults()
    {
        var service = new LauncherSettingsService(NewSettingsDir());

        var settings = await service.LoadAsync();

        Assert.False(settings.RedirectionEnabled);
        Assert.False(settings.DevMode);
        Assert.Null(settings.CachedServerIp);
        Assert.Null(settings.CachedRedirectHostnames);
        Assert.Null(settings.AdvancedArgs);
        Assert.Empty(settings.GameDirectories);
        Assert.Empty(settings.InstalledExpansions);
    }

    [Fact]
    public async Task SaveAndLoad_RoundTripsAllFields()
    {
        var dir = NewSettingsDir();
        var gameDir = _tempRoot;

        var original = new LauncherSettings
        {
            RedirectionEnabled = true,
            DevMode = true,
            CachedServerIp = "203.0.113.10",
            CachedRedirectHostnames = new[] { "gs.novaworld.net" },
            AdvancedArgs = new Dictionary<string, string> { ["jop_2_consumer"] = "/d /connectlog" },
            GameDirectories = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
            {
                ["jop_2_consumer"] = gameDir,
            },
        };
        original.InstalledExpansions["jop_2_consumer"] = new Dictionary<string, InstalledExpansion>(StringComparer.OrdinalIgnoreCase)
        {
            ["jox01"] = new InstalledExpansion("jox01", "1.0.0", "expansion", DateTime.UtcNow, new[] { "a.pff" }),
        };

        await new LauncherSettingsService(dir).SaveAsync(original);
        var loaded = await new LauncherSettingsService(dir).LoadAsync();

        Assert.True(loaded.RedirectionEnabled);
        Assert.True(loaded.DevMode);
        Assert.Equal("203.0.113.10", loaded.CachedServerIp);
        Assert.Equal(new[] { "gs.novaworld.net" }, loaded.CachedRedirectHostnames);
        Assert.NotNull(loaded.AdvancedArgs);
        Assert.Equal("/d /connectlog", loaded.AdvancedArgs!["jop_2_consumer"]);
        Assert.Equal(gameDir, loaded.GameDirectories["jop_2_consumer"]);
        Assert.True(loaded.InstalledExpansions.ContainsKey("jop_2_consumer"));
        Assert.Equal("1.0.0", loaded.InstalledExpansions["jop_2_consumer"]["jox01"].Version);
    }

    [Fact]
    public async Task Load_MigratesLegacyOnLauncherSettingsOnce()
    {
        var dir = NewSettingsDir();
        var legacyDir = Path.Combine(_tempRoot, "OnLauncher");
        Directory.CreateDirectory(legacyDir);
        var legacyPath = Path.Combine(legacyDir, "settings.json");

        var legacySettings = new LauncherSettings();
        legacySettings.GameDirectories["jop_2_consumer"] = _tempRoot;
        await File.WriteAllTextAsync(legacyPath, JsonSerializer.Serialize(legacySettings, new JsonSerializerOptions(JsonSerializerDefaults.Web)));

        var service = new LauncherSettingsService(dir, legacyPath);
        var migrated = await service.LoadAsync();

        Assert.Equal(_tempRoot, migrated.GameDirectories["jop_2_consumer"]);
        Assert.True(File.Exists(Path.Combine(dir, "settings.json")));

        // Once migrated, the legacy file is no longer consulted.
        var updated = new LauncherSettings { RedirectionEnabled = true };
        await service.SaveAsync(updated);
        await File.WriteAllTextAsync(legacyPath, "{\"redirectionEnabled\":false,\"gameDirectories\":{\"dfx2_consumer\":\"C:/changed\"}}");

        var reloaded = await new LauncherSettingsService(dir, legacyPath).LoadAsync();
        Assert.True(reloaded.RedirectionEnabled);
        Assert.False(reloaded.GameDirectories.ContainsKey("dfx2_consumer"));
    }

    [Fact]
    public async Task Load_CorruptJson_FallsBackToDefaultsWithoutThrowing()
    {
        var dir = NewSettingsDir();
        Directory.CreateDirectory(dir);
        await File.WriteAllTextAsync(Path.Combine(dir, "settings.json"), "{ this is not json !!");

        var settings = await new LauncherSettingsService(dir).LoadAsync();

        Assert.NotNull(settings);
        Assert.False(settings.RedirectionEnabled);
        Assert.Empty(settings.GameDirectories);
    }

    [Fact]
    public async Task Normalize_DropsBlankAdvancedArgsAndHostnames()
    {
        var dir = NewSettingsDir();
        var settings = new LauncherSettings
        {
            CachedRedirectHostnames = new[] { "  ", "gs.novaworld.net  " },
            AdvancedArgs = new Dictionary<string, string>
            {
                ["jop_2_consumer"] = "   ",
                ["dfx2_consumer"] = " /w ",
            },
        };

        await new LauncherSettingsService(dir).SaveAsync(settings);
        var loaded = await new LauncherSettingsService(dir).LoadAsync();

        Assert.Equal(new[] { "gs.novaworld.net" }, loaded.CachedRedirectHostnames);
        Assert.NotNull(loaded.AdvancedArgs);
        Assert.False(loaded.AdvancedArgs!.ContainsKey("jop_2_consumer"));
        Assert.Equal("/w", loaded.AdvancedArgs["dfx2_consumer"]);
    }
}
