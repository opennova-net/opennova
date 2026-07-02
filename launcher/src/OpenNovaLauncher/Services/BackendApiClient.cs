using System;
using System.Collections.Generic;
using System.Linq;
using System.Net.Http;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Threading;
using System.Threading.Tasks;
using OpenNova.Launcher.Models;

namespace OpenNova.Launcher.Services;

public sealed class BackendApiClient : IDisposable
{
    private static readonly JsonSerializerOptions JsonOptions = new(JsonSerializerDefaults.Web);

    private readonly HttpClient _httpClient;
    private bool _disposed;

    public BackendApiClient(string baseAddress)
    {
        if (string.IsNullOrWhiteSpace(baseAddress))
        {
            throw new ArgumentException("Base address is required", nameof(baseAddress));
        }

        if (!baseAddress.EndsWith("/", StringComparison.Ordinal))
        {
            baseAddress += "/";
        }

        _httpClient = new HttpClient
        {
            BaseAddress = new Uri(baseAddress, UriKind.Absolute),
            Timeout = TimeSpan.FromSeconds(15)
        };
    }

    public async Task<IReadOnlyList<GameCatalogEntry>> FetchCatalogAsync(CancellationToken cancellationToken)
    {
        var payload = await _httpClient.GetStringAsync("games", cancellationToken);
        return ParseCatalog(payload);
    }

    /// <summary>Parses the /api/games envelope. Unknown fields are tolerated; incomplete entries are skipped.</summary>
    public static IReadOnlyList<GameCatalogEntry> ParseCatalog(string json)
    {
        var envelope = JsonSerializer.Deserialize<GameCatalogEnvelope>(json, JsonOptions);
        if (envelope?.Games == null || envelope.Games.Count == 0)
        {
            return Array.Empty<GameCatalogEntry>();
        }

        var entries = new List<GameCatalogEntry>();

        foreach (var gameDto in envelope.Games)
        {
            if (string.IsNullOrWhiteSpace(gameDto.Slug) ||
                string.IsNullOrWhiteSpace(gameDto.DisplayName) ||
                string.IsNullOrWhiteSpace(gameDto.ExecutableName))
            {
                continue;
            }

            var gameDefinition = new GameDefinition(gameDto.Slug, gameDto.DisplayName, gameDto.ExecutableName);

            var expansions = new List<ExpansionDescriptor>();
            if (gameDto.Expansions != null)
            {
                foreach (var expansionDto in gameDto.Expansions)
                {
                    if (string.IsNullOrWhiteSpace(expansionDto?.Slug) ||
                        string.IsNullOrWhiteSpace(expansionDto.DisplayName) ||
                        string.IsNullOrWhiteSpace(expansionDto.Version) ||
                        string.IsNullOrWhiteSpace(expansionDto.Install?.Target))
                    {
                        continue;
                    }

                    var files = new List<ExpansionFileDescriptor>();
                    if (expansionDto.Files != null)
                    {
                        foreach (var fileDto in expansionDto.Files)
                        {
                            if (fileDto == null || string.IsNullOrWhiteSpace(fileDto.DownloadUrl) || string.IsNullOrWhiteSpace(fileDto.Sha256))
                            {
                                continue;
                            }

                            // An all-zero sha256 is the seed/placeholder marker (no real
                            // release published). Skip it so the expansion reads as
                            // "Not published yet" rather than offering a download that 404s.
                            if (fileDto.Sha256.All(c => c == '0'))
                            {
                                continue;
                            }

                            files.Add(new ExpansionFileDescriptor(
                                fileDto.DownloadUrl,
                                fileDto.Sha256,
                                fileDto.SizeBytes,
                                fileDto.FileType ?? "archive"));
                        }
                    }

                    expansions.Add(new ExpansionDescriptor(
                        expansionDto.Slug,
                        expansionDto.DisplayName,
                        expansionDto.Version,
                        expansionDto.Summary,
                        expansionDto.PackageType ?? "zip",
                        expansionDto.Install.Target,
                        files));
                }
            }

            entries.Add(new GameCatalogEntry(gameDefinition, expansions));
        }

        return entries;
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _httpClient.Dispose();
        _disposed = true;
    }

    private sealed class GameCatalogEnvelope
    {
        [JsonPropertyName("games")]
        public List<GameDto>? Games { get; init; }
    }

    private sealed class GameDto
    {
        [JsonPropertyName("slug")]
        public string? Slug { get; init; }

        [JsonPropertyName("displayName")]
        public string? DisplayName { get; init; }

        [JsonPropertyName("executableName")]
        public string? ExecutableName { get; init; }

        [JsonPropertyName("expansions")]
        public List<ExpansionDto>? Expansions { get; init; }
    }

    private sealed class ExpansionDto
    {
        [JsonPropertyName("slug")]
        public string? Slug { get; init; }

        [JsonPropertyName("displayName")]
        public string? DisplayName { get; init; }

        [JsonPropertyName("summary")]
        public string? Summary { get; init; }

        [JsonPropertyName("version")]
        public string? Version { get; init; }

        [JsonPropertyName("packageType")]
        public string? PackageType { get; init; }

        [JsonPropertyName("install")]
        public InstallDto? Install { get; init; }

        [JsonPropertyName("files")]
        public List<ExpansionFileDto>? Files { get; init; }
    }

    private sealed class InstallDto
    {
        [JsonPropertyName("target")]
        public string? Target { get; init; }
    }

    private sealed class ExpansionFileDto
    {
        [JsonPropertyName("downloadUrl")]
        public string? DownloadUrl { get; init; }

        [JsonPropertyName("sha256")]
        public string? Sha256 { get; init; }

        [JsonPropertyName("sizeBytes")]
        public long? SizeBytes { get; init; }

        [JsonPropertyName("fileType")]
        public string? FileType { get; init; }
    }
}
