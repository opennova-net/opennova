using OpenNova.Launcher.Services;
using Xunit;

namespace OpenNova.Launcher.Tests;

public class BackendCatalogParsingTests
{
    // Mirrors the /api/games envelope shape the reference launcher consumes.
    private const string SampleCatalogJson = """
        {
          "games": [
            {
              "slug": "jop_2_consumer",
              "displayName": "Joint Operations: Typhoon Rising",
              "executableName": "Jointops.exe",
              "expansions": [
                {
                  "slug": "jox01",
                  "displayName": "Escalation",
                  "summary": "Adds the Kendari theatre.",
                  "version": "1.0.3",
                  "packageType": "zip",
                  "install": { "target": "expansion" },
                  "files": [
                    {
                      "downloadUrl": "https://downloads.opennova.net/expansions/jox01.zip",
                      "sha256": "abc123",
                      "sizeBytes": 1048576,
                      "fileType": "archive"
                    }
                  ]
                }
              ]
            },
            {
              "slug": "dfx2_consumer",
              "displayName": "Delta Force Xtreme 2",
              "executableName": "dfx2.exe",
              "expansions": []
            }
          ]
        }
        """;

    [Fact]
    public void ParseCatalog_ParsesGamesAndExpansions()
    {
        var entries = BackendApiClient.ParseCatalog(SampleCatalogJson);

        Assert.Equal(2, entries.Count);

        var jointOps = entries[0];
        Assert.Equal("jop_2_consumer", jointOps.Game.Slug);
        Assert.Equal("Joint Operations: Typhoon Rising", jointOps.Game.DisplayName);
        Assert.Equal("Jointops.exe", jointOps.Game.ExecutableName);

        var expansion = Assert.Single(jointOps.Expansions);
        Assert.Equal("jox01", expansion.Slug);
        Assert.Equal("Escalation", expansion.DisplayName);
        Assert.Equal("1.0.3", expansion.Version);
        Assert.Equal("zip", expansion.PackageType);
        Assert.Equal("expansion", expansion.InstallTarget);

        var file = Assert.Single(expansion.Files);
        Assert.Equal("https://downloads.opennova.net/expansions/jox01.zip", file.DownloadUrl);
        Assert.Equal("abc123", file.Sha256);
        Assert.Equal(1048576, file.SizeBytes);
        Assert.Equal("archive", file.FileType);

        var dfx2 = entries[1];
        Assert.Equal("dfx2_consumer", dfx2.Game.Slug);
        Assert.Empty(dfx2.Expansions);
    }

    [Fact]
    public void ParseCatalog_ToleratesUnknownFields()
    {
        const string jsonWithUnknowns = """
            {
              "schemaVersion": 2,
              "games": [
                {
                  "slug": "jop_2_consumer",
                  "displayName": "Joint Operations: Typhoon Rising",
                  "executableName": "Jointops.exe",
                  "minimumLauncherVersion": "0.2.0",
                  "icons": { "large": "x.png" },
                  "expansions": [
                    {
                      "slug": "jox01",
                      "displayName": "Escalation",
                      "version": "1.0.3",
                      "releaseNotesUrl": "https://opennova.net/news/jox01",
                      "install": { "target": "expansion", "mode": "merge" },
                      "files": []
                    }
                  ]
                }
              ],
              "generatedAt": "2026-06-10T00:00:00Z"
            }
            """;

        var entries = BackendApiClient.ParseCatalog(jsonWithUnknowns);

        var game = Assert.Single(entries);
        Assert.Equal("jop_2_consumer", game.Game.Slug);
        var expansion = Assert.Single(game.Expansions);
        Assert.Equal("jox01", expansion.Slug);
        Assert.Empty(expansion.Files);
    }

    [Fact]
    public void ParseCatalog_SkipsIncompleteEntries()
    {
        const string jsonWithGaps = """
            {
              "games": [
                { "slug": "missing_fields" },
                {
                  "slug": "jop_2_consumer",
                  "displayName": "Joint Operations: Typhoon Rising",
                  "executableName": "Jointops.exe",
                  "expansions": [
                    { "slug": "no_install_target", "displayName": "Broken", "version": "1.0" },
                    {
                      "slug": "jox01",
                      "displayName": "Escalation",
                      "version": "1.0.3",
                      "install": { "target": "expansion" },
                      "files": [
                        { "downloadUrl": "https://example.com/a.zip" },
                        { "downloadUrl": "https://example.com/b.zip", "sha256": "def456" }
                      ]
                    }
                  ]
                }
              ]
            }
            """;

        var entries = BackendApiClient.ParseCatalog(jsonWithGaps);

        var game = Assert.Single(entries);
        var expansion = Assert.Single(game.Expansions);
        Assert.Equal("jox01", expansion.Slug);

        // The file without a sha256 is skipped.
        var file = Assert.Single(expansion.Files);
        Assert.Equal("def456", file.Sha256);
    }

    [Fact]
    public void ParseCatalog_EmptyGames_ReturnsEmptyList()
    {
        Assert.Empty(BackendApiClient.ParseCatalog("""{ "games": [] }"""));
        Assert.Empty(BackendApiClient.ParseCatalog("{}"));
    }
}
