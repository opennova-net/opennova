using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;
using OpenNova.Launcher.Forms;
using OpenNova.Launcher.Hosts;
using OpenNova.Launcher.Models;
using OpenNova.Launcher.Services;

namespace OpenNova.Launcher;

internal sealed class TrayApplicationContext : ApplicationContext
{
    private const string RedirectionOffMessage = "NovaWorld redirection is off. Turn it on in Preferences to play.";

    private readonly NotifyIcon _notifyIcon;
    private readonly ContextMenuStrip _contextMenu;
    private readonly ToolStripMenuItem _launchMenuItem;
    private readonly ToolStripMenuItem _redirectionMenuItem;
    private readonly ToolStripMenuItem _devModeMenuItem;
    private readonly ToolStripSeparator _menuSeparator;
    private readonly ToolStripMenuItem _preferencesMenuItem;
    private readonly ToolStripMenuItem _exitMenuItem;
    private readonly LauncherSettingsService _settingsService = new();
    private readonly LauncherAutoUpdateService _launcherAutoUpdateService;
    private readonly ExpansionDeploymentService _expansionDeploymentService = new();
    private readonly BackendApiClient _backendApiClient;
    private readonly ExpansionCoordinator _expansionCoordinator;
    private readonly HostsFileService _hostsFileService = new(new HostsFileStore(), new DnsCacheFlusher());
    private readonly HttpServerInfoClient _serverInfoClient;
    private readonly ServerEndpointResolver _endpointResolver;
    private readonly GameLauncher _gameLauncher = new();
    private IReadOnlyList<GameDefinition> _supportedGames = GameCatalog.SupportedGames;
    private LauncherSettings _settings = new();
    private ServerEndpoint? _currentEndpoint;
    private HostsRedirectState _lastHostsState = HostsRedirectState.Disabled;
    private bool _disposed;
    private bool _deploymentInProgress;
    private readonly ToolStripMenuItem _expansionsMenuItem;

    public TrayApplicationContext()
    {
        _contextMenu = new ContextMenuStrip();

        _launchMenuItem = new ToolStripMenuItem("Launch")
        {
            Enabled = false
        };
        _expansionsMenuItem = new ToolStripMenuItem("Expansions...", null, OnExpansionsClicked);
        // Quick on/off for our hosts-file redirect, without opening Preferences.
        // Checkmark reflects whether the launcher is managing the redirect.
        _redirectionMenuItem = new ToolStripMenuItem("Redirect to OpenNova", null, OnToggleRedirectionClicked)
        {
            ToolTipText = "Toggle OpenNova's NovaWorld hosts-file redirect on or off."
        };
        _devModeMenuItem = new ToolStripMenuItem("Developer mode (127.0.0.1)", null, OnToggleDevModeClicked)
        {
            ToolTipText = "Point the redirect at a NovaWorld server on this machine instead of OpenNova."
        };
        _preferencesMenuItem = new ToolStripMenuItem("Preferences", null, OnPreferencesClicked);
        _exitMenuItem = new ToolStripMenuItem("Exit", null, OnExitClicked);

        _menuSeparator = new ToolStripSeparator();

        _contextMenu.Items.AddRange(new ToolStripItem[]
        {
            _launchMenuItem,
            _expansionsMenuItem,
            _redirectionMenuItem,
            _devModeMenuItem,
            _preferencesMenuItem,
            _menuSeparator,
            _exitMenuItem
        });

        _notifyIcon = new NotifyIcon
        {
            Icon = LoadNotifyIcon(),
            Text = "OpenNova Launcher",
            Visible = true,
            ContextMenuStrip = _contextMenu
        };

        var apiBaseUrl = ResolveApiBaseUrl();
        _backendApiClient = new BackendApiClient(apiBaseUrl);
        _serverInfoClient = new HttpServerInfoClient(apiBaseUrl);
        _endpointResolver = new ServerEndpointResolver(
            _serverInfoClient,
            new SystemDnsResolver(),
            () => _settings,
            SaveSettingsAsync);

        _launcherAutoUpdateService = new LauncherAutoUpdateService();
        _launcherAutoUpdateService.UpdateReady += OnLauncherUpdateReady;
        _launcherAutoUpdateService.NoUpdateAvailable += OnLauncherNoUpdateAvailable;
        _launcherAutoUpdateService.UpdateFailed += OnLauncherUpdateFailed;

        _expansionCoordinator = new ExpansionCoordinator(
            _backendApiClient,
            _expansionDeploymentService,
            () => _settings,
            SaveSettingsAsync,
            () => BuildInstallations());

        InitializeAsync();
    }

    protected override void Dispose(bool disposing)
    {
        if (_disposed)
        {
            base.Dispose(disposing);
            return;
        }

        if (disposing)
        {
            _notifyIcon.Visible = false;
            _notifyIcon.Dispose();
            _contextMenu.Dispose();
            _launcherAutoUpdateService.Dispose();
            _backendApiClient.Dispose();
            _serverInfoClient.Dispose();
        }

        _disposed = true;
        base.Dispose(disposing);
    }

    private async void InitializeAsync()
    {
        _settings = await _settingsService.LoadAsync();
        try
        {
            await _expansionCoordinator.RefreshCatalogAsync(CancellationToken.None);
            _supportedGames = GameCatalog.SupportedGames;
        }
        catch (Exception ex)
        {
            Debug.WriteLine($"[Catalog] Failed to refresh on startup: {ex.Message}");
        }
        UpdateMenuState();

        // Check for launcher updates before doing anything else.
        await _launcherAutoUpdateService.CheckForUpdatesAsync();

        // A pending launcher update blocks launching until it is installed.
        if (_launcherAutoUpdateService.IsUpdateAvailable)
        {
            UpdateMenuState();
            return;
        }

        // Re-resolve the server IP and repair the hosts block if it is stale.
        await SyncRedirectionAsync(silent: true);
        UpdateMenuState();
    }

    /// <summary>
    /// Re-resolves the NovaWorld server endpoint and reconciles the hosts-file
    /// managed block with it: applies a missing block, repairs a stale one, and
    /// (interactively, when not silent) offers to clean conflicting foreign lines.
    /// Returns true when the redirection is verified active.
    /// </summary>
    private async Task<bool> SyncRedirectionAsync(bool silent)
    {
        if (!_settings.RedirectionEnabled)
        {
            _lastHostsState = HostsRedirectState.Disabled;
            return false;
        }

        var endpoint = await _endpointResolver.ResolveAsync(_settings.DevMode, CancellationToken.None);
        if (endpoint == null)
        {
            _currentEndpoint = null;
            _lastHostsState = HostsRedirectState.Inaccessible;
            if (!silent)
            {
                ShowBalloon(
                    "Server address unavailable",
                    "Could not find the OpenNova server address. Check your internet connection and try again.");
            }

            return false;
        }

        _currentEndpoint = endpoint;
        var desired = BuildDesiredRedirects(endpoint);
        var state = _hostsFileService.GetState(desired);

        if (state == HostsRedirectState.ForeignConflict)
        {
            if (silent)
            {
                _lastHostsState = state;
                return false;
            }

            var hostnameList = string.Join(", ", desired.Select(redirect => redirect.Hostname));
            var choice = MessageBox.Show(
                $"Your Windows hosts file already has entries for {hostnameList} that were not added by this launcher. " +
                "Remove them so the launcher can manage these names?",
                "OpenNova Launcher",
                MessageBoxButtons.YesNo,
                MessageBoxIcon.Warning);
            if (choice != DialogResult.Yes)
            {
                _lastHostsState = state;
                return false;
            }

            if (!_hostsFileService.TryCleanForeignLines(desired.Select(redirect => redirect.Hostname)))
            {
                _lastHostsState = HostsRedirectState.Inaccessible;
                ShowBalloon("Hosts file locked", "Windows blocked changes to the hosts file. Close other security tools and try again.");
                return false;
            }

            state = _hostsFileService.GetState(desired);
        }

        if (state is HostsRedirectState.Disabled or HostsRedirectState.EnabledStaleIp)
        {
            _hostsFileService.Apply(desired);
            state = _hostsFileService.GetState(desired);
        }

        _lastHostsState = state;

        if (state != HostsRedirectState.Enabled && !silent)
        {
            ShowBalloon("Hosts file locked", "Windows blocked changes to the hosts file. Close other security tools and try again.");
        }

        return state == HostsRedirectState.Enabled;
    }

    private static IReadOnlyList<HostsRedirect> BuildDesiredRedirects(ServerEndpoint endpoint)
    {
        return endpoint.RedirectHostnames
            .Select(hostname => new HostsRedirect(hostname, endpoint.IPv4))
            .ToList();
    }

    private bool HostsReadyForLaunch =>
        _settings.RedirectionEnabled &&
        _lastHostsState is HostsRedirectState.Enabled or HostsRedirectState.EnabledStaleIp;

    private async void OnPreferencesClicked(object? sender, EventArgs e)
    {
        using var form = new PreferencesForm(_supportedGames, _settings.GameDirectories);
        form.CheckForUpdatesRequested += OnCheckForUpdatesRequested;

        var dialogResult = form.ShowDialog();
        form.CheckForUpdatesRequested -= OnCheckForUpdatesRequested;

        if (dialogResult != DialogResult.OK)
        {
            return;
        }

        _settings.GameDirectories = new Dictionary<string, string>(form.SelectedDirectories, StringComparer.OrdinalIgnoreCase);
        await SaveSettingsAsync();
        UpdateMenuState();
    }

    // Tray quick-toggle: flip whether the launcher manages the NovaWorld redirect.
    // On -> resolve + write the hosts block; off -> remove it. Same effect as the
    // Preferences checkbox, one click from the tray.
    private async void OnToggleRedirectionClicked(object? sender, EventArgs e)
    {
        var enable = !_settings.RedirectionEnabled;
        _settings.RedirectionEnabled = enable;
        await SaveSettingsAsync();

        if (enable)
        {
            await SyncRedirectionAsync(silent: false);
        }
        else
        {
            _hostsFileService.Remove();
            _lastHostsState = HostsRedirectState.Disabled;
        }

        UpdateMenuState();
    }

    // Tray quick-toggle: developer mode points the redirect at a NovaWorld server
    // on this machine (127.0.0.1) instead of resolving the OpenNova server.
    private async void OnToggleDevModeClicked(object? sender, EventArgs e)
    {
        _settings.DevMode = !_settings.DevMode;
        await SaveSettingsAsync();

        // Re-resolve against the new target if the redirect is currently active.
        if (_settings.RedirectionEnabled)
        {
            await SyncRedirectionAsync(silent: false);
        }

        UpdateMenuState();
    }

    private void OnExitClicked(object? sender, EventArgs e)
    {
        ExitThread();
    }

    protected override void ExitThreadCore()
    {
        _notifyIcon.Visible = false;
        base.ExitThreadCore();
    }

    private void UpdateMenuState()
    {
        var installations = BuildInstallations();
        RebuildLaunchMenuItems(installations);

        var hasConfiguredGame = installations.Count > 0;
        var launcherUpdatePending = _launcherAutoUpdateService.IsUpdateAvailable;

        _launchMenuItem.Enabled = hasConfiguredGame && !_deploymentInProgress && !launcherUpdatePending;
        _expansionsMenuItem.Enabled = _supportedGames.Count > 0;
        _redirectionMenuItem.Checked = _settings.RedirectionEnabled;
        _devModeMenuItem.Checked = _settings.DevMode;

        var status = launcherUpdatePending
            ? "Launcher update required"
            : _deploymentInProgress
                ? "Deploying files"
                : !hasConfiguredGame
                    ? "No games configured"
                    : !HostsReadyForLaunch
                        ? "Redirection off"
                        : "Ready";

        var summary = BuildInstallationSummary();
        _notifyIcon.Text = TrimTooltip(summary.Length > 0
            ? $"OpenNova Launcher - {status} | {summary}"
            : $"OpenNova Launcher - {status}");
    }

    private List<GameInstallation> BuildInstallations()
    {
        var installations = new List<GameInstallation>();

        foreach (var game in _supportedGames)
        {
            if (!_settings.GameDirectories.TryGetValue(game.Slug, out var candidate) || string.IsNullOrWhiteSpace(candidate))
            {
                continue;
            }

            var directory = candidate.Trim();
            if (!Directory.Exists(directory))
            {
                continue;
            }

            var exePath = Path.Combine(directory, game.ExecutableName);
            if (!File.Exists(exePath))
            {
                continue;
            }

            installations.Add(new GameInstallation(game, directory, exePath));
        }

        return installations;
    }

    private string BuildInstallationSummary()
    {
        var parts = new List<string>();

        foreach (var game in _supportedGames)
        {
            var shortName = GetShortGameName(game.DisplayName);

            if (_settings.GameDirectories.TryGetValue(game.Slug, out var candidate) && !string.IsNullOrWhiteSpace(candidate))
            {
                var directory = candidate.Trim();
                var exePath = Path.Combine(directory, game.ExecutableName);
                var folderName = SafeGetFolderName(directory);
                var label = File.Exists(exePath) ? "✓" : "✗";
                parts.Add(folderName.Length > 0
                    ? $"{shortName} {folderName} {label}"
                    : $"{shortName} {label}");
            }
            else
            {
                parts.Add($"{shortName} ✗");
            }
        }

        return string.Join(", ", parts);
    }

    private static string GetShortGameName(string displayName)
    {
        return string.IsNullOrWhiteSpace(displayName)
            ? "Game"
            : displayName.Trim();
    }

    private static string SafeGetFolderName(string directory)
    {
        try
        {
            if (string.IsNullOrWhiteSpace(directory))
            {
                return string.Empty;
            }

            var trimmed = directory.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            if (string.IsNullOrEmpty(trimmed))
            {
                return directory;
            }

            return Path.GetFileName(trimmed) ?? directory;
        }
        catch
        {
            return string.Empty;
        }
    }

    private void RebuildLaunchMenuItems(IReadOnlyList<GameInstallation> installations)
    {
        _launchMenuItem.DropDownItems.Clear();

        if (installations.Count == 0)
        {
            _launchMenuItem.Enabled = false;
            _launchMenuItem.DropDownItems.Add(new ToolStripMenuItem("No games configured")
            {
                Enabled = false
            });
            return;
        }

        foreach (var installation in installations)
        {
            var shortName = GetShortGameName(installation.Definition.DisplayName);

            var menuItem = new ToolStripMenuItem(shortName, null, OnLaunchGameClicked)
            {
                Tag = installation
            };

            var canLaunch = !_deploymentInProgress && !_launcherAutoUpdateService.IsUpdateAvailable;
            menuItem.Enabled = canLaunch;

            if (_deploymentInProgress)
            {
                menuItem.ToolTipText = "Deployment in progress. Please wait.";
            }
            else if (!HostsReadyForLaunch)
            {
                menuItem.ToolTipText = "Redirect is off — you'll pick OpenNova or the original NovaWorld at launch.";
            }

            _launchMenuItem.DropDownItems.Add(menuItem);
        }

        _launchMenuItem.Enabled = true;
    }

    private async void OnLaunchGameClicked(object? sender, EventArgs e)
    {
        if (sender is not ToolStripMenuItem item || item.Tag is not GameInstallation installation)
        {
            return;
        }

        if (_launcherAutoUpdateService.IsUpdateAvailable)
        {
            ShowBalloon("Launcher update required", "Update OpenNova Launcher before launching games.");
            return;
        }

        if (_deploymentInProgress)
        {
            ShowBalloon("Deployment in progress", "Please wait for the current file deployment to finish before launching.");
            return;
        }

        // The launch dialog (in TryLaunchGameAsync) lets the player pick OpenNova
        // (default, applies the redirect) or the original NovaWorld (skips it), so
        // launching is no longer gated on the redirect being active up front.
        await TryLaunchGameAsync(installation);
    }

    private async Task TryLaunchGameAsync(GameInstallation installation)
    {
        if (!File.Exists(installation.ExecutablePath))
        {
            ShowBalloon("Game missing", $"Could not find {installation.Definition.ExecutableName} in the configured directory.");
            UpdateMenuState();
            return;
        }

        if (!TryPromptForLaunchOptions(
                installation,
                out var selectedExpansionSlug,
                out var launchWindowed,
                out var allowManyInstances,
                out var useRealNovaWorld))
        {
            return;
        }

        // Point the game at the right NovaWorld before it starts.
        if (useRealNovaWorld)
        {
            // Original NovaWorld: clear our managed hosts block so the real
            // hostnames resolve normally. Re-applied next time you launch OpenNova.
            _hostsFileService.Remove();
            _lastHostsState = HostsRedirectState.Disabled;
            UpdateMenuState();
        }
        else
        {
            // OpenNova: make sure the redirect is on and pointing at the current IP.
            if (!_settings.RedirectionEnabled)
            {
                _settings.RedirectionEnabled = true;
                await SaveSettingsAsync();
            }

            var ready = await SyncRedirectionAsync(silent: false);
            UpdateMenuState();
            if (!ready)
            {
                return;
            }
        }

        string? advancedArgs = null;
        _settings.AdvancedArgs?.TryGetValue(installation.Definition.Slug, out advancedArgs);

        var options = new LaunchOptions(
            Windowed: launchWindowed,
            MultiInstance: allowManyInstances,
            ExpansionSlug: selectedExpansionSlug,
            AdvancedArgs: advancedArgs);

        try
        {
            _gameLauncher.Launch(installation, options);
        }
        catch (Exception ex)
        {
            ShowBalloon("Launch failed", $"Could not launch {installation.Definition.DisplayName}: {ex.Message}");
        }
    }

    private bool TryPromptForLaunchOptions(
        GameInstallation installation,
        out string? expansionSlug,
        out bool launchWindowed,
        out bool allowManyInstances,
        out bool useRealNovaWorld)
    {
        expansionSlug = null;
        launchWindowed = true;
        allowManyInstances = true;
        useRealNovaWorld = false;

        _settings.InstalledExpansions.TryGetValue(installation.Definition.Slug, out var installedMap);

        var descriptors = _expansionCoordinator.GetAvailableExpansions(installation.Definition.Slug);
        var descriptorLookup = descriptors.ToDictionary(d => d.Slug, d => d, StringComparer.OrdinalIgnoreCase);

        var options = new List<ExpansionLaunchOption>
        {
            ExpansionLaunchOption.BaseGame()
        };

        IEnumerable<InstalledExpansion> installedValues = installedMap != null
            ? installedMap.Values
            : Enumerable.Empty<InstalledExpansion>();

        var installedOptions = installedValues
            .Select(installed =>
            {
                descriptorLookup.TryGetValue(installed.Slug, out var descriptor);
                var displayName = descriptor?.DisplayName ?? installed.Slug;
                var summary = descriptor?.Summary;
                summary = string.IsNullOrWhiteSpace(summary) ? string.Empty : summary;
                var versionLabel = string.IsNullOrWhiteSpace(installed.Version) ? "—" : installed.Version;
                return new ExpansionLaunchOption(installed.Slug, displayName, versionLabel, summary);
            })
            .OrderBy(option => option.DisplayName, StringComparer.OrdinalIgnoreCase)
            .ToList();

        options.AddRange(installedOptions);

        string? initialSelection = installedMap != null && installedMap.Count == 1
            ? installedMap.Values.First().Slug
            : null;

        using var dialog = new ExpansionLaunchDialog(
            installation.Definition.DisplayName,
            options,
            initialSelection,
            initialWindowed: true,
            initialAllowManyInstances: true);
        var result = dialog.ShowDialog();
        if (result != DialogResult.OK)
        {
            return false;
        }

        expansionSlug = dialog.SelectedExpansionSlug;
        launchWindowed = dialog.LaunchWindowed;
        allowManyInstances = dialog.AllowManyInstances;
        useRealNovaWorld = dialog.UseRealNovaWorld;
        return true;
    }

    private void OnCheckForUpdatesRequested(object? sender, EventArgs e)
    {
        _launcherAutoUpdateService.CheckForUpdates(notifyWhenUpToDate: true);
    }

    private void OnLauncherUpdateReady()
    {
        // AutoUpdater.NET will display its own UI when an update is available.
    }

    private void OnLauncherNoUpdateAvailable()
    {
        // Suppress informational balloons; errors are handled separately.
    }

    private void OnLauncherUpdateFailed(string message)
    {
        const int maxLength = 200;
        var detail = string.IsNullOrWhiteSpace(message)
            ? "Unable to contact the update server."
            : message.Length > maxLength
                ? message[..maxLength] + "..."
                : message;
        ShowBalloon("Update check failed", detail);
    }

    private static string TrimTooltip(string text)
    {
        const int maxLength = 63; // WinForms notify icon limit
        return text.Length <= maxLength ? text : text[..maxLength];
    }

    private Task SaveSettingsAsync() => _settingsService.SaveAsync(_settings);

    private static Icon LoadNotifyIcon()
    {
        var assetPath = Path.Combine(AppContext.BaseDirectory, "Assets", "onlauncher.ico");
        if (File.Exists(assetPath))
        {
            try
            {
                return new Icon(assetPath);
            }
            catch
            {
                // ignore and fall back to executable icon
            }
        }

        return Icon.ExtractAssociatedIcon(Application.ExecutablePath) ?? SystemIcons.Application;
    }

    private void ShowBalloon(string title, string message)
    {
        _notifyIcon.BalloonTipTitle = title;
        _notifyIcon.BalloonTipText = message;
        _notifyIcon.ShowBalloonTip(3000);
    }

    private static string ResolveApiBaseUrl()
    {
        var overrideUrl = Environment.GetEnvironmentVariable("ONLAUNCHER_API_BASE_URL");
        if (!string.IsNullOrWhiteSpace(overrideUrl))
        {
            return overrideUrl.TrimEnd('/');
        }

        return "https://opennova.net/api";
    }

    private async void OnExpansionsClicked(object? sender, EventArgs e)
    {
        using var form = new ExpansionManagerForm(this);
        form.ShowDialog();
    }

    internal async Task RefreshExpansionCatalogAsync(CancellationToken cancellationToken)
    {
        await _expansionCoordinator.RefreshCatalogAsync(cancellationToken);
        _supportedGames = GameCatalog.SupportedGames;
        UpdateMenuState();
    }

    internal IReadOnlyList<GameExpansionStatus> GetExpansionStatuses()
    {
        return _expansionCoordinator.BuildViewModel();
    }

    internal async Task InstallExpansionAsync(
        string gameSlug,
        string expansionSlug,
        IProgress<string>? progress,
        CancellationToken cancellationToken)
    {
        if (_deploymentInProgress)
        {
            throw new InvalidOperationException("Another deployment is already in progress.");
        }

        _deploymentInProgress = true;
        UpdateMenuState();

        try
        {
            await _expansionCoordinator.InstallAsync(gameSlug, expansionSlug, progress, cancellationToken);
        }
        finally
        {
            _deploymentInProgress = false;
            UpdateMenuState();
        }
    }

    internal async Task RemoveExpansionAsync(
        string gameSlug,
        string expansionSlug,
        IProgress<string>? progress,
        CancellationToken cancellationToken)
    {
        if (_deploymentInProgress)
        {
            throw new InvalidOperationException("Another deployment is already in progress.");
        }

        _deploymentInProgress = true;
        UpdateMenuState();

        try
        {
            await _expansionCoordinator.RemoveAsync(gameSlug, expansionSlug, progress, cancellationToken);
        }
        finally
        {
            _deploymentInProgress = false;
            UpdateMenuState();
        }
    }
}
