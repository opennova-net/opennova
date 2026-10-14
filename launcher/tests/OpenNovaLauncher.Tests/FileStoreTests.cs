using System.Text;
using OpenNova.Launcher.Hosts;
using OpenNova.Launcher.Services;
using Xunit;

namespace OpenNova.Launcher.Tests;

public sealed class FileStoreTests : IDisposable
{
    private readonly string _directory = Path.Combine(Path.GetTempPath(), "opennova-launcher-tests-" + Guid.NewGuid().ToString("N"));

    public FileStoreTests()
    {
        Directory.CreateDirectory(_directory);
    }

    public void Dispose()
    {
        foreach (var file in Directory.EnumerateFiles(_directory))
        {
            File.SetAttributes(file, FileAttributes.Normal);
        }

        Directory.Delete(_directory, recursive: true);
    }

    [Fact]
    public void HostsWrite_KeepsEveryByteAndBacksUpTheReplacedFile()
    {
        var path = Path.Combine(_directory, "hosts");
        // cp1252 and UTF-8 bytes in comments must survive a launcher write untouched.
        var original = Encoding.ASCII.GetBytes("# caf")
            .Concat(new byte[] { 0xE9, 0x20, 0xC3, 0xA9, 0x0D, 0x0A })
            .Concat(Encoding.ASCII.GetBytes("127.0.0.1 localhost\r\n"))
            .ToArray();
        File.WriteAllBytes(path, original);
        var store = new HostsFileStore(path);

        var switcher = new HostsSwitcher(store, new NoFlush());
        switcher.Switch(NovaWorldMode.OpenNova, "203.0.113.10", removeForeign: false);

        var written = File.ReadAllBytes(path);
        Assert.Equal(original, written.Take(original.Length).ToArray());
        Assert.Equal(original, File.ReadAllBytes(store.BackupPath));
        Assert.Equal(NovaWorldMode.OpenNova, switcher.ReadState().Mode);

        switcher.Switch(NovaWorldMode.NovaLogic, null, removeForeign: false);
        Assert.Equal(original, File.ReadAllBytes(path));
        Assert.Equal(written, File.ReadAllBytes(store.BackupPath));
        Assert.Equal(new[] { "hosts", "hosts.opennova-launcher.bak" }, Directory.EnumerateFiles(_directory).Select(Path.GetFileName).Order());
    }

    [Fact]
    public void HostsWrite_KeepsTheReadOnlyAttribute()
    {
        var path = Path.Combine(_directory, "hosts");
        File.WriteAllText(path, "127.0.0.1 localhost\r\n");
        File.SetAttributes(path, FileAttributes.ReadOnly);

        new HostsFileStore(path).Write("127.0.0.1 localhost\r\n10.0.0.1 example\r\n");

        Assert.True(File.GetAttributes(path).HasFlag(FileAttributes.ReadOnly));
        Assert.Equal("127.0.0.1 localhost\r\n10.0.0.1 example\r\n", File.ReadAllText(path));
    }

    [Fact]
    public void HostsWrite_CreatesAMissingFile()
    {
        var store = new HostsFileStore(Path.Combine(_directory, "hosts"));

        Assert.False(store.Exists);
        store.Write("10.0.0.1 example\r\n");

        Assert.Equal("10.0.0.1 example\r\n", store.Read());
        Assert.False(File.Exists(store.BackupPath));
    }

    [Fact]
    public void Settings_RoundTrip()
    {
        var store = new LauncherSettingsStore(Path.Combine(_directory, "settings"));
        store.Save(new LauncherSettings
        {
            ServiceUrl = "http://127.0.0.1:8080",
            CachedOpenNovaAddress = "54.162.79.147",
            CustomAddress = "192.168.1.20",
        });

        var loaded = store.Load();

        Assert.Equal("http://127.0.0.1:8080", loaded.ServiceUrl);
        Assert.Equal("54.162.79.147", loaded.CachedOpenNovaAddress);
        Assert.Equal("192.168.1.20", loaded.CustomAddress);
    }

    [Fact]
    public void Settings_MissingOrBrokenOrInvalid_GiveTheDefaults()
    {
        var store = new LauncherSettingsStore(_directory);
        Assert.Equal(ServerInfo.DefaultServiceUrl, store.Load().ServiceUrl);

        File.WriteAllText(store.SettingsPath, "{ not json");
        Assert.Equal(ServerInfo.DefaultServiceUrl, store.Load().ServiceUrl);

        File.WriteAllText(store.SettingsPath, "{\"serviceUrl\":\"opennova\",\"cachedOpenNovaAddress\":\"0.0.0.0\",\"customAddress\":\"x\"}");
        var loaded = store.Load();
        Assert.Equal(ServerInfo.DefaultServiceUrl, loaded.ServiceUrl);
        Assert.Null(loaded.CachedOpenNovaAddress);
        Assert.Null(loaded.CustomAddress);
    }

    private sealed class NoFlush : IDnsCacheFlusher
    {
        public void Flush()
        {
        }
    }
}
