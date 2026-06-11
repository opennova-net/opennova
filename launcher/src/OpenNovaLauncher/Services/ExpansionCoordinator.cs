using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using OpenNova.Launcher.Models;

namespace OpenNova.Launcher.Services;

public sealed class ExpansionCoordinator
{
    private readonly BackendApiClient _backendClient;
    private readonly ExpansionDeploymentService _deploymentService;
    private readonly Func<LauncherSettings> _settingsAccessor;
    private readonly Func<Task> _saveSettingsAsync;
    private readonly Func<List<GameInstallation>> _buildInstallations;

    private IReadOnlyList<GameDefinition> _games = Array.Empty<GameDefinition>();
    private Dictionary<string, IReadOnlyList<ExpansionDescriptor>> _catalog =
        new(StringComparer.OrdinalIgnoreCase);

    public ExpansionCoordinator(
        BackendApiClient backendClient,
        ExpansionDeploymentService deploymentService,
        Func<LauncherSettings> settingsAccessor,
        Func<Task> saveSettingsAsync,
        Func<List<GameInstallation>> buildInstallations)
    {
        _backendClient = backendClient ?? throw new ArgumentNullException(nameof(backendClient));
        _deploymentService = deploymentService ?? throw new ArgumentNullException(nameof(deploymentService));
        _settingsAccessor = settingsAccessor ?? throw new ArgumentNullException(nameof(settingsAccessor));
        _saveSettingsAsync = saveSettingsAsync ?? throw new ArgumentNullException(nameof(saveSettingsAsync));
        _buildInstallations = buildInstallations ?? throw new ArgumentNullException(nameof(buildInstallations));
    }

    public IReadOnlyList<GameDefinition> Games => _games;

    public IReadOnlyList<ExpansionDescriptor> GetAvailableExpansions(string gameSlug)
    {
        if (string.IsNullOrWhiteSpace(gameSlug))
        {
            return Array.Empty<ExpansionDescriptor>();
        }

        return _catalog.TryGetValue(gameSlug, out var descriptors)
            ? descriptors
            : Array.Empty<ExpansionDescriptor>();
    }

    public async Task RefreshCatalogAsync(CancellationToken cancellationToken)
    {
        var catalogEntries = await _backendClient.FetchCatalogAsync(cancellationToken).ConfigureAwait(false);
        if (catalogEntries.Count == 0)
        {
            return;
        }

        _games = catalogEntries.Select(entry => entry.Game).ToList();
        GameCatalog.ReplaceGames(_games);

        _catalog = catalogEntries.ToDictionary(
            entry => entry.Game.Slug,
            entry => entry.Expansions,
            StringComparer.OrdinalIgnoreCase);

        var settings = _settingsAccessor();
        foreach (var game in _games)
        {
            if (!settings.InstalledExpansions.ContainsKey(game.Slug))
            {
                settings.InstalledExpansions[game.Slug] =
                    new Dictionary<string, InstalledExpansion>(StringComparer.OrdinalIgnoreCase);
            }
        }
    }

    public IReadOnlyList<GameExpansionStatus> BuildViewModel()
    {
        var settings = _settingsAccessor();
        var installations = _buildInstallations();

        var statuses = new List<GameExpansionStatus>();
        foreach (var game in _games)
        {
            var isConfigured = installations.Any(installation =>
                string.Equals(installation.Definition.Slug, game.Slug, StringComparison.OrdinalIgnoreCase));

            settings.InstalledExpansions.TryGetValue(game.Slug, out var installedMap);
            installedMap ??= new Dictionary<string, InstalledExpansion>(StringComparer.OrdinalIgnoreCase);

            _catalog.TryGetValue(game.Slug, out var descriptors);
            descriptors ??= Array.Empty<ExpansionDescriptor>();

            var expansionStates = new List<ExpansionStatus>();
            foreach (var descriptor in descriptors)
            {
                installedMap.TryGetValue(descriptor.Slug, out var installed);

                ExpansionState state;
                string message;
                if (!isConfigured)
                {
                    state = ExpansionState.NeedsGameDirectory;
                    message = "Set game directory to manage this expansion.";
                }
                else if (installed == null)
                {
                    state = ExpansionState.NotInstalled;
                    message = "Not installed.";
                }
                else if (!string.Equals(installed.Version, descriptor.Version, StringComparison.OrdinalIgnoreCase))
                {
                    state = ExpansionState.UpdateAvailable;
                    message = $"Installed {installed.Version}, latest {descriptor.Version}.";
                }
                else
                {
                    state = ExpansionState.Installed;
                    message = $"Installed {installed.Version} on {installed.InstalledAtUtc:yyyy-MM-dd}.";
                }

                expansionStates.Add(new ExpansionStatus(descriptor, installed, state, message));
            }

            statuses.Add(new GameExpansionStatus(game, isConfigured, expansionStates));
        }

        return statuses;
    }

    public async Task InstallAsync(
        string gameSlug,
        string expansionSlug,
        IProgress<string>? progress,
        CancellationToken cancellationToken)
    {
        var descriptor = ResolveExpansionDescriptor(gameSlug, expansionSlug);
        var installation = ResolveInstallation(gameSlug);

        progress?.Report($"Preparing to install {descriptor.DisplayName}...");

        var installed = await _deploymentService.InstallAsync(
            installation,
            descriptor,
            progress,
            cancellationToken).ConfigureAwait(false);

        var settings = _settingsAccessor();
        if (!settings.InstalledExpansions.TryGetValue(gameSlug, out var installedMap))
        {
            installedMap = new Dictionary<string, InstalledExpansion>(StringComparer.OrdinalIgnoreCase);
            settings.InstalledExpansions[gameSlug] = installedMap;
        }

        installedMap[installed.Slug] = installed;
        await _saveSettingsAsync().ConfigureAwait(false);
    }

    public async Task RemoveAsync(
        string gameSlug,
        string expansionSlug,
        IProgress<string>? progress,
        CancellationToken cancellationToken)
    {
        var settings = _settingsAccessor();
        if (!settings.InstalledExpansions.TryGetValue(gameSlug, out var installedMap) ||
            !installedMap.TryGetValue(expansionSlug, out var installed))
        {
            throw new InvalidOperationException("Expansion is not installed.");
        }

        var installation = ResolveInstallation(gameSlug);
        await _deploymentService.RemoveAsync(installation, installed, progress, cancellationToken)
            .ConfigureAwait(false);

        installedMap.Remove(expansionSlug);
        if (installedMap.Count == 0)
        {
            settings.InstalledExpansions.Remove(gameSlug);
        }

        await _saveSettingsAsync().ConfigureAwait(false);
    }

    private ExpansionDescriptor ResolveExpansionDescriptor(string gameSlug, string expansionSlug)
    {
        _catalog.TryGetValue(gameSlug, out var descriptors);
        if (descriptors == null)
        {
            throw new InvalidOperationException("Unknown game or no expansions available.");
        }

        var descriptor = descriptors.FirstOrDefault(d =>
            string.Equals(d.Slug, expansionSlug, StringComparison.OrdinalIgnoreCase));
        if (descriptor == null)
        {
            throw new InvalidOperationException("Unknown expansion slug.");
        }

        return descriptor;
    }

    private GameInstallation ResolveInstallation(string gameSlug)
    {
        var installations = _buildInstallations();
        var installation = installations.FirstOrDefault(i =>
            string.Equals(i.Definition.Slug, gameSlug, StringComparison.OrdinalIgnoreCase));
        if (installation == null)
        {
            throw new InvalidOperationException("Configure the game directory before managing expansions.");
        }

        return installation;
    }
}
