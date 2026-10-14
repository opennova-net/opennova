using System.Diagnostics;
using System.Security;
using System.Security.AccessControl;
using System.Security.Principal;
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
/// steer the elevated process. The key does not rely on what it inherits either: every
/// save gives it <see cref="ProtectedKeySecurity"/> (under the non-default "object
/// creator" owner policy an inherited CREATOR OWNER entry would hand the user's own
/// account write access).
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
        : this(Registry.LocalMachine, DefaultKeyPath, protectKey: true)
    {
    }

    /// <summary>
    /// A store over <paramref name="root"/>\<paramref name="keyPath"/>; <paramref name="protectKey"/>
    /// gives the key <see cref="ProtectedKeySecurity"/> on every save, which needs an
    /// elevated process (the default store's case).
    /// </summary>
    public LauncherSettingsStore(RegistryKey root, string keyPath, bool protectKey = false)
    {
        _root = root ?? throw new ArgumentNullException(nameof(root));
        _keyPath = string.IsNullOrWhiteSpace(keyPath) ? throw new ArgumentException("A key path is required.", nameof(keyPath)) : keyPath;
        ProtectsKey = protectKey;
    }

    /// <summary>Where the settings live, for messages.</summary>
    public string Location => _root.Name + "\\" + _keyPath;

    /// <summary>Whether a save gives the key <see cref="ProtectedKeySecurity"/>.</summary>
    public bool ProtectsKey { get; }

    /// <summary>
    /// The settings key's ACL: owned by Administrators; SYSTEM and Administrators full
    /// control, Users read; protected, so nothing is inherited (no CREATOR OWNER).
    /// </summary>
    public static RegistrySecurity ProtectedKeySecurity()
    {
        var administrators = new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null);
        var security = new RegistrySecurity();
        security.SetAccessRuleProtection(isProtected: true, preserveInheritance: false);
        security.SetOwner(administrators);
        foreach (var (sid, rights) in new[]
                 {
                     (new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null), RegistryRights.FullControl),
                     (administrators, RegistryRights.FullControl),
                     (new SecurityIdentifier(WellKnownSidType.BuiltinUsersSid, null), RegistryRights.ReadKey),
                 })
        {
            security.AddAccessRule(new RegistryAccessRule(
                sid, rights, InheritanceFlags.ContainerInherit, PropagationFlags.None, AccessControlType.Allow));
        }

        return security;
    }

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
        if (ProtectsKey)
        {
            // A new key is created with the ACL; one that already existed gets it again
            // before anything is written to it.
            var security = ProtectedKeySecurity();
            using (_root.CreateSubKey(_keyPath, RegistryKeyPermissionCheck.ReadWriteSubTree, security))
            {
            }

            using var guard = _root.OpenSubKey(
                _keyPath,
                RegistryKeyPermissionCheck.ReadWriteSubTree,
                RegistryRights.ReadPermissions | RegistryRights.ChangePermissions | RegistryRights.TakeOwnership)
                ?? throw new IOException($"{Location} could not be opened to set its permissions.");
            guard.SetAccessControl(security);
        }

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
