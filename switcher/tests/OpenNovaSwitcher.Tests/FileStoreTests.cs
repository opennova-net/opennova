using System.Security.AccessControl;
using System.Security.Principal;
using System.Text;
using Microsoft.Win32;
using OpenNova.Switcher.Hosts;
using OpenNova.Switcher.Services;
using Xunit;

namespace OpenNova.Switcher.Tests;

public sealed class FileStoreTests : IDisposable
{
    private readonly string _directory = Path.Combine(Path.GetTempPath(), "opennova-switcher-tests-" + Guid.NewGuid().ToString("N"));

    public FileStoreTests()
    {
        Directory.CreateDirectory(_directory);
    }

    public void Dispose()
    {
        foreach (var file in Directory.EnumerateFileSystemEntries(_directory))
        {
            File.SetAttributes(file, FileAttributes.Normal);
        }

        Directory.Delete(_directory, recursive: true);
    }

    private string Hosts(byte[] bytes)
    {
        var path = Path.Combine(_directory, "hosts");
        File.WriteAllBytes(path, bytes);
        return path;
    }

    [Fact]
    public void HostsWrite_KeepsEveryByteTheOriginalAndTheLastVersion()
    {
        // cp1252 and UTF-8 bytes in comments must survive a switcher write untouched.
        var original = Encoding.ASCII.GetBytes("# caf")
            .Concat(new byte[] { 0xE9, 0x20, 0xC3, 0xA9, 0x0D, 0x0A })
            .Concat(Encoding.ASCII.GetBytes("127.0.0.1 localhost\r\n"))
            .ToArray();
        var store = new HostsFileStore(Hosts(original));
        var switcher = new HostsSwitcher(store, new NoFlush());

        switcher.Switch(NovaWorldMode.OpenNova, "203.0.113.10", removeForeign: false);
        var opennova = File.ReadAllBytes(store.HostsPath);
        Assert.Equal(original, opennova.Take(original.Length).ToArray());
        Assert.Equal(original, File.ReadAllBytes(store.BackupPath));
        Assert.Equal(NovaWorldMode.OpenNova, switcher.ReadState().Mode);

        switcher.Switch(NovaWorldMode.Custom, "127.0.0.1", removeForeign: false);
        switcher.Switch(NovaWorldMode.NovaLogic, null, removeForeign: false);

        Assert.Equal(original, File.ReadAllBytes(store.HostsPath));
        Assert.NotEqual(opennova, File.ReadAllBytes(store.BackupPath));
        // The .orig is the file before the switcher's first write, and stays so.
        Assert.Equal(original, File.ReadAllBytes(store.OriginalPath));
        Assert.Equal(
            new[] { "hosts", "hosts.opennova-switcher.bak", "hosts.opennova-switcher.orig" },
            Directory.EnumerateFiles(_directory).Select(Path.GetFileName).Order());
    }

    [Fact]
    public void HostsWrite_KeepsTheReadOnlyAttribute()
    {
        var path = Hosts(Encoding.ASCII.GetBytes("127.0.0.1 localhost\r\n"));
        File.SetAttributes(path, FileAttributes.ReadOnly);

        new HostsFileStore(path).Write("127.0.0.1 localhost\r\n10.0.0.1 example\r\n", "127.0.0.1 localhost\r\n");

        Assert.True(File.GetAttributes(path).HasFlag(FileAttributes.ReadOnly));
        Assert.Equal("127.0.0.1 localhost\r\n10.0.0.1 example\r\n", File.ReadAllText(path));
    }

    [Fact]
    public void HostsWrite_CreatesAMissingFile()
    {
        var store = new HostsFileStore(Path.Combine(_directory, "hosts"));

        Assert.False(store.Exists);
        store.Write("10.0.0.1 example\r\n", string.Empty);

        Assert.Equal("10.0.0.1 example\r\n", store.Read());
        Assert.False(File.Exists(store.BackupPath));
        Assert.False(File.Exists(store.OriginalPath));
    }

    [Fact]
    public void HostsWrite_OverAFileThatChanged_WritesNothing()
    {
        var path = Hosts(Encoding.ASCII.GetBytes("127.0.0.1 localhost\r\n10.9.9.9 docker.internal\r\n"));

        Assert.Throws<HostsFileChangedException>(() => new HostsFileStore(path).Write("x\r\n", "127.0.0.1 localhost\r\n"));

        Assert.Equal("127.0.0.1 localhost\r\n10.9.9.9 docker.internal\r\n", File.ReadAllText(path));
        Assert.Single(Directory.EnumerateFiles(_directory));
    }

    [Theory]
    [InlineData(new byte[] { 0xFF, 0xFE, (byte)'1', 0, (byte)'\r', 0, (byte)'\n', 0 })]  // UTF-16 LE
    [InlineData(new byte[] { 0xFE, 0xFF, 0, (byte)'1', 0, (byte)'\r', 0, (byte)'\n' })]  // UTF-16 BE
    [InlineData(new byte[] { (byte)'1', 0, (byte)'2', (byte)'\n' })]                     // a NUL byte
    public void HostsInAnEncodingTheSwitcherDoesNotEdit_IsRefusedAndLeftAlone(byte[] bytes)
    {
        var path = Hosts(bytes);
        var switcher = new HostsSwitcher(new HostsFileStore(path), new NoFlush());

        Assert.Throws<HostsFileException>(() => switcher.ReadState());
        Assert.Throws<HostsFileException>(() => switcher.Switch(NovaWorldMode.OpenNova, "203.0.113.10", removeForeign: false));

        Assert.Equal(bytes, File.ReadAllBytes(path));
    }

    [Fact]
    public void HostsWrite_ThatFails_LeavesTheFileAndNoTempFile()
    {
        var path = Hosts(Encoding.ASCII.GetBytes("127.0.0.1 localhost\r\n"));
        var store = new HostsFileStore(path);
        // File.Replace fails when the backup path is a directory.
        Directory.CreateDirectory(store.BackupPath);

        Assert.NotNull(Record.Exception(() => store.Write("x\r\n", "127.0.0.1 localhost\r\n")));

        Assert.Equal("127.0.0.1 localhost\r\n", File.ReadAllText(path));
        Assert.Empty(Directory.EnumerateFiles(_directory, "hosts.*.tmp"));
    }

    [Fact]
    public void HostsWrite_WhileLocked_FailsCleanly_AndASharedReaderDoesNotBlockIt()
    {
        var path = Hosts(Encoding.ASCII.GetBytes("127.0.0.1 localhost\r\n"));
        var store = new HostsFileStore(path);

        using (new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.None))
        {
            Assert.NotNull(Record.Exception(() => store.Write("x\r\n", "127.0.0.1 localhost\r\n")));
        }

        Assert.Equal("127.0.0.1 localhost\r\n", File.ReadAllText(path));
        Assert.Empty(Directory.EnumerateFiles(_directory, "hosts.*.tmp"));

        // The DNS client and editors hold it open for reading with sharing.
        using (new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
        {
            store.Write("x\r\n", "127.0.0.1 localhost\r\n");
        }

        Assert.Equal("x\r\n", File.ReadAllText(path));
    }

    [Fact]
    public void DnsFlush_RunsIpconfigFromSystem32ByItsFullPath()
    {
        Assert.Equal(Path.Combine(Environment.SystemDirectory, "ipconfig.exe"), DnsCacheFlusher.IpconfigPath);
        Assert.True(Path.IsPathFullyQualified(DnsCacheFlusher.IpconfigPath));
    }

    [Fact]
    public void DefaultHostsPath_IsSystem32s_AndNoEnvironmentVariableMovesIt()
    {
        var expected = Path.Combine(Environment.SystemDirectory, "drivers", "etc", "hosts");
        Assert.Equal(expected, HostsFileStore.DefaultHostsPath());

        // A user can set SystemRoot (and windir) in HKCU\Environment; the elevated
        // switcher's hosts path must not follow it.
        var systemRoot = Environment.GetEnvironmentVariable("SystemRoot");
        var windir = Environment.GetEnvironmentVariable("windir");
        try
        {
            Environment.SetEnvironmentVariable("SystemRoot", _directory);
            Environment.SetEnvironmentVariable("windir", _directory);

            var path = HostsFileStore.DefaultHostsPath();

            Assert.Equal(expected, path);
            Assert.False(path.StartsWith(_directory, StringComparison.OrdinalIgnoreCase));
        }
        finally
        {
            Environment.SetEnvironmentVariable("SystemRoot", systemRoot);
            Environment.SetEnvironmentVariable("windir", windir);
        }
    }

    private sealed class NoFlush : IDnsCacheFlusher
    {
        public void Flush()
        {
        }
    }
}

public sealed class SettingsStoreTests : IDisposable
{
    private const string ParentPath = @"Software\OpenNova-SwitcherTests";
    private readonly string _keyPath = ParentPath + @"\" + Guid.NewGuid().ToString("N");

    public void Dispose()
    {
        Registry.CurrentUser.DeleteSubKeyTree(_keyPath, throwOnMissingSubKey: false);
        try
        {
            // The parent goes with the last test's key; DeleteSubKey refuses one that still has children.
            Registry.CurrentUser.DeleteSubKey(ParentPath, throwOnMissingSubKey: false);
        }
        catch (InvalidOperationException)
        {
            // Another test's key is still there.
        }
    }

    [Fact]
    public void TheDefaultStore_IsAnAdminOnlyMachineKey()
    {
        var store = new SwitcherSettingsStore();

        Assert.Equal(@"HKEY_LOCAL_MACHINE\SOFTWARE\OpenNova\Switcher", store.Location);
        Assert.True(store.ProtectsKey);
        Assert.False(new SwitcherSettingsStore(Registry.CurrentUser, _keyPath).ProtectsKey);
    }

    [Fact]
    public void TheKeysAcl_IsProtected_AdminOwned_AndOnlyReadableByUsers()
    {
        var security = SwitcherSettingsStore.ProtectedKeySecurity();
        var administrators = new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null);
        var system = new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null);
        var users = new SecurityIdentifier(WellKnownSidType.BuiltinUsersSid, null);

        Assert.True(security.AreAccessRulesProtected);
        Assert.Equal(administrators, security.GetOwner(typeof(SecurityIdentifier)));
        var rules = security.GetAccessRules(includeExplicit: true, includeInherited: true, typeof(SecurityIdentifier))
            .Cast<RegistryAccessRule>()
            .ToDictionary(rule => (SecurityIdentifier)rule.IdentityReference, rule => rule);

        Assert.Equal(new[] { administrators, system, users }.OrderBy(s => s.Value), rules.Keys.OrderBy(s => s.Value));
        Assert.All(rules.Values, rule => Assert.Equal(AccessControlType.Allow, rule.AccessControlType));
        Assert.All(rules.Values, rule => Assert.False(rule.IsInherited));
        Assert.Equal(RegistryRights.FullControl, rules[system].RegistryRights);
        Assert.Equal(RegistryRights.FullControl, rules[administrators].RegistryRights);
        Assert.Equal(RegistryRights.ReadKey, rules[users].RegistryRights);
        Assert.Equal(0, (int)(rules[users].RegistryRights & (RegistryRights.SetValue | RegistryRights.CreateSubKey |
                                                              RegistryRights.ChangePermissions | RegistryRights.TakeOwnership |
                                                              RegistryRights.Delete)));
    }

    [Fact]
    public void Settings_RoundTrip()
    {
        var store = new SwitcherSettingsStore(Registry.CurrentUser, _keyPath);
        store.Save(new SwitcherSettings
        {
            ServiceUrl = "http://127.0.0.1:8080",
            CachedOpenNovaAddress = "54.162.79.147",
            CustomAddress = "192.168.1.20",
        });

        var loaded = store.Load();

        Assert.Equal("http://127.0.0.1:8080", loaded.ServiceUrl);
        Assert.Equal("54.162.79.147", loaded.CachedOpenNovaAddress);
        Assert.Equal("192.168.1.20", loaded.CustomAddress);

        loaded.CustomAddress = null;
        store.Save(loaded);
        Assert.Null(store.Load().CustomAddress);
    }

    [Fact]
    public void Settings_MissingOrInvalid_GiveTheDefaults()
    {
        var store = new SwitcherSettingsStore(Registry.CurrentUser, _keyPath);
        Assert.Equal(ServerInfo.DefaultServiceUrl, store.Load().ServiceUrl);

        using (var key = Registry.CurrentUser.CreateSubKey(_keyPath))
        {
            key.SetValue("ServiceUrl", "http://evil.example");
            key.SetValue("OpenNovaAddress", "0.0.0.0");
            key.SetValue("CustomAddress", 42, RegistryValueKind.DWord);
        }

        var loaded = store.Load();
        Assert.Equal(ServerInfo.DefaultServiceUrl, loaded.ServiceUrl);
        Assert.Null(loaded.CachedOpenNovaAddress);
        Assert.Null(loaded.CustomAddress);
    }
}
