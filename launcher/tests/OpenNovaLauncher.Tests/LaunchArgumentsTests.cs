using OpenNova.Launcher.Services;
using Xunit;

namespace OpenNova.Launcher.Tests;

public class LaunchArgumentsTests
{
    [Fact]
    public void EmptyOptions_ProduceEmptyString()
    {
        Assert.Equal(string.Empty, GameLauncher.BuildArguments(new LaunchOptions()));
    }

    [Fact]
    public void Windowed_MapsToSlashW()
    {
        Assert.Equal("/w", GameLauncher.BuildArguments(new LaunchOptions(Windowed: true)));
    }

    [Fact]
    public void MultiInstance_MapsToSlashMany()
    {
        Assert.Equal("/many", GameLauncher.BuildArguments(new LaunchOptions(MultiInstance: true)));
    }

    [Fact]
    public void ExpansionSlug_MapsToSlashExp()
    {
        Assert.Equal("/exp jox01", GameLauncher.BuildArguments(new LaunchOptions(ExpansionSlug: "jox01")));
    }

    [Fact]
    public void BlankExpansionSlug_IsIgnored()
    {
        Assert.Equal(string.Empty, GameLauncher.BuildArguments(new LaunchOptions(ExpansionSlug: "   ")));
    }

    [Fact]
    public void AdvancedArgs_AreAppendedVerbatim()
    {
        Assert.Equal("/d /connectlog", GameLauncher.BuildArguments(new LaunchOptions(AdvancedArgs: "/d /connectlog")));
    }

    [Fact]
    public void AllOptionsCombined_AppearInOrder()
    {
        var options = new LaunchOptions(
            Windowed: true,
            MultiInstance: true,
            ExpansionSlug: "jox01",
            AdvancedArgs: "/d /connectlog");

        Assert.Equal("/w /many /exp jox01 /d /connectlog", GameLauncher.BuildArguments(options));
    }

    [Fact]
    public void WindowedAndExpansion_WithoutMany()
    {
        var options = new LaunchOptions(Windowed: true, ExpansionSlug: "jox01");

        Assert.Equal("/w /exp jox01", GameLauncher.BuildArguments(options));
    }
}
