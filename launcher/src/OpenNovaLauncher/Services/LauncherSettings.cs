using System.Diagnostics;
using System.Security;
using Microsoft.Win32;
using OpenNova.Launcher.Hosts;

namespace OpenNova.Launcher.Services;

/// <summary>The launcher's few preferences.</summary>
public sealed class LauncherSettings
{
    /// <summary>The OpenNova service whose <c>/api/server-info</c> names its NovaWorld address.</summary>
    public string ServiceUrl { get; set; } = ServerInfo.DefaultServiceUrl;

    /// <summary>The last address that route gave, used when it cannot be reached.</summary>
    public string? CachedOpenNovaAddress { get; set; }

    /// <summary>The last custom address entered, offered again next time.</summary>
    public string? CustomAddress { get; set; }
}

/// <summary>
/// The settings as string values of one registry key, by default
/// HKLM\SOFTWARE\OpenNova\Launcher. The launcher runs elevated and these settings decide
/// what it writes into the hosts file, so they live where only administrators can write:
/// a standard user cannot create or change a key under HKLM\SOFTWARE, where a file under
/// %APPDATA% (or a folder a user may have made first under %ProgramData%) would let one
/// steer the elevated process.
/// </summary>
public sealed class LauncherSettingsStore
{
    public const string DefaultKeyPath = @"SOFTWARE\OpenNova\Launcher";

    private const string ServiceUrlValue = "ServiceUrl";
    private const string OpenNovaAddressValue = "OpenNovaAddress";
    private const string CustomAddressValue = "CustomAddress";

    private readonly RegistryKey _root;
    private readonly string _keyPath;

    public LauncherSettingsStore()
        : this(Registry.LocalMachine, DefaultKeyPath)
    {
    }

    public LauncherSettingsStore(RegistryKey root, string keyPath)
    {
        _root = root ?? throw new ArgumentNullException(nameof(root));
        _keyPath = string.IsNullOrWhiteSpace(keyPath) ? throw new ArgumentException("A key path is required.", nameof(keyPath)) : keyPath;
    }

    /// <summary>Where the settings live, for messages.</summary>
    public string Location => _root.Name + "\\" + _keyPath;

    /// <summary>The saved settings; the defaults when there are none or a value is not valid.</summary>
    public LauncherSettings Load()
    {
        try
        {
            using var key = _root.OpenSubKey(_keyPath, writable: false);
            if (key is null)
            {
                return new LauncherSettings();
            }

            return Normalize(new LauncherSettings
            {
                ServiceUrl = key.GetValue(ServiceUrlValue) as string ?? ServerInfo.DefaultServiceUrl,
                CachedOpenNovaAddress = key.GetValue(OpenNovaAddressValue) as string,
                CustomAddress = key.GetValue(CustomAddressValue) as string,
            });
        }
        catch (Exception ex) when (ex is SecurityException or UnauthorizedAccessException or IOException)
        {
            Debug.WriteLine($"[Settings] Load failed, using defaults: {ex.Message}");
            return new LauncherSettings();
        }
    }

    /// <summary>Writes the settings; failures (<see cref="SecurityException"/> and the like) reach the caller.</summary>
    public void Save(LauncherSettings settings)
    {
        ArgumentNullException.ThrowIfNull(settings);
        var normalized = Normalize(settings);
        using var key = _root.CreateSubKey(_keyPath, writable: true);
        key.SetValue(ServiceUrlValue, normalized.ServiceUrl, RegistryValueKind.String);
        SetOrDelete(key, OpenNovaAddressValue, normalized.CachedOpenNovaAddress);
        SetOrDelete(key, CustomAddressValue, normalized.CustomAddress);
    }

    private static void SetOrDelete(RegistryKey key, string name, string? value)
    {
        if (value is null)
        {
            key.DeleteValue(name, throwOnMissingValue: false);
        }
        else
        {
            key.SetValue(name, value, RegistryValueKind.String);
        }
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
