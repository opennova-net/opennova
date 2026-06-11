using System.IO;
using System.Reflection;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;
using AutoUpdaterDotNET;
using Newtonsoft.Json.Linq;

namespace OpenNova.Launcher.Services;

public sealed class LauncherAutoUpdateService : IDisposable
{
    private const string DefaultManifestUrl = "https://downloads.opennova.net/launcher/version.json";
    private const string ManifestUrlEnvironmentVariable = "ONLAUNCHER_UPDATE_MANIFEST_URL";

    private readonly Uri _manifestUri;
    private readonly SynchronizationContext _syncContext;
    private readonly object _stateLock = new();
    private bool _notifyWhenUpToDate;
    private TaskCompletionSource<bool>? _updateCheckCompletion;

    public event Action? UpdateReady;
    public event Action? NoUpdateAvailable;
    public event Action<string>? UpdateFailed;

    public bool IsUpdateAvailable { get; private set; }

    public LauncherAutoUpdateService()
    {
        var overrideUrl = Environment.GetEnvironmentVariable(ManifestUrlEnvironmentVariable);
        _manifestUri = new Uri(string.IsNullOrWhiteSpace(overrideUrl) ? DefaultManifestUrl : overrideUrl.Trim());

        _syncContext = SynchronizationContext.Current ?? new WindowsFormsSynchronizationContext();
        AutoUpdater.AppTitle = "OpenNova Launcher";
        AutoUpdater.RunUpdateAsAdmin = true;
        AutoUpdater.Synchronous = false;
        AutoUpdater.ShowSkipButton = false;
        AutoUpdater.ShowRemindLaterButton = false;
        AutoUpdater.DownloadPath = Path.Combine(Path.GetTempPath(), "OpenNovaLauncherUpdates");
        AutoUpdater.InstallationPath = AppContext.BaseDirectory;
        AutoUpdater.HttpUserAgent = "OpenNovaLauncher/1.0";

        var version = Assembly.GetExecutingAssembly().GetName().Version;
        if (version != null)
        {
            AutoUpdater.InstalledVersion = version;
        }

        AutoUpdater.ParseUpdateInfoEvent += AutoUpdaterOnParseUpdateInfoEvent;
        AutoUpdater.CheckForUpdateEvent += AutoUpdaterOnCheckForUpdateEvent;
    }

    public Task<bool> CheckForUpdatesAsync(bool notifyWhenUpToDate = false)
    {
        lock (_stateLock)
        {
            _notifyWhenUpToDate = notifyWhenUpToDate;
            _updateCheckCompletion = new TaskCompletionSource<bool>();
        }

        Task.Run(() => AutoUpdater.Start(_manifestUri.AbsoluteUri));

        return _updateCheckCompletion.Task;
    }

    public void CheckForUpdates(bool notifyWhenUpToDate = false)
    {
        _ = CheckForUpdatesAsync(notifyWhenUpToDate);
    }

    private void AutoUpdaterOnParseUpdateInfoEvent(ParseUpdateInfoEventArgs args)
    {
        try
        {
            var remoteData = args.RemoteData;
            if (string.IsNullOrWhiteSpace(remoteData))
            {
                throw new InvalidOperationException("Update manifest is empty.");
            }

            var json = JObject.Parse(remoteData);
            var versionText = (string?)json["version"];
            var downloadUrl = (string?)json["url"];
            var mandatory = json["mandatory"]?.Value<bool>() ?? false;

            if (string.IsNullOrWhiteSpace(downloadUrl))
            {
                throw new InvalidOperationException("Update manifest is missing the download url.");
            }

            var updateInfo = new UpdateInfoEventArgs
            {
                DownloadURL = downloadUrl,
                Mandatory = new Mandatory
                {
                    Value = mandatory,
                    UpdateMode = mandatory ? Mode.Forced : Mode.Normal
                }
            };

            if (!string.IsNullOrWhiteSpace(versionText))
            {
                updateInfo.CurrentVersion = Version.TryParse(versionText, out var parsed)
                    ? parsed.ToString()
                    : versionText;
            }

            args.UpdateInfo = updateInfo;
        }
        catch (Exception ex)
        {
            args.UpdateInfo = new UpdateInfoEventArgs
            {
                Error = ex
            };
        }
    }

    private void AutoUpdaterOnCheckForUpdateEvent(UpdateInfoEventArgs args)
    {
        if (args == null)
        {
            RaiseFailure("Unexpected response from update server.");
            CompleteUpdateCheck(false);
            return;
        }

        if (args.Error != null)
        {
            RaiseFailure(args.Error.Message);
            CompleteUpdateCheck(false);
            return;
        }

        if (!args.IsUpdateAvailable)
        {
            bool shouldNotify;
            lock (_stateLock)
            {
                shouldNotify = _notifyWhenUpToDate;
                _notifyWhenUpToDate = false;
            }

            if (shouldNotify)
            {
                RaiseNoUpdate();
            }

            CompleteUpdateCheck(false);
            return;
        }

        IsUpdateAvailable = true;

        lock (_stateLock)
        {
            _notifyWhenUpToDate = false;
        }

        Post(() =>
        {
            AutoUpdater.ShowUpdateForm(args);
            UpdateReady?.Invoke();
        });

        CompleteUpdateCheck(true);
    }

    private void CompleteUpdateCheck(bool updateAvailable)
    {
        TaskCompletionSource<bool>? completion;
        lock (_stateLock)
        {
            completion = _updateCheckCompletion;
            _updateCheckCompletion = null;
        }

        completion?.TrySetResult(updateAvailable);
    }

    private void RaiseNoUpdate()
    {
        var handler = NoUpdateAvailable;
        if (handler == null)
        {
            return;
        }

        Post(handler);
    }

    private void RaiseFailure(string message)
    {
        if (UpdateFailed == null)
        {
            return;
        }

        Post(() => UpdateFailed?.Invoke(message));
    }

    private void Post(Action action)
    {
        _syncContext.Post(_ => action(), null);
    }

    public void Dispose()
    {
        AutoUpdater.ParseUpdateInfoEvent -= AutoUpdaterOnParseUpdateInfoEvent;
        AutoUpdater.CheckForUpdateEvent -= AutoUpdaterOnCheckForUpdateEvent;
    }
}
