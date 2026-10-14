using System.Diagnostics;
using System.Security;
using OpenNova.Launcher.Hosts;
using OpenNova.Launcher.Services;

namespace OpenNova.Launcher;

/// <summary>
/// The tray icon and its menu: the mode the hosts file is in, one item per mode
/// to switch to it, and the OpenNova service URL. The mode is read from the hosts
/// file every time the menu opens, so an edit made elsewhere shows.
/// </summary>
internal sealed class TrayApplicationContext : ApplicationContext
{
    private const string DefaultCustomAddress = "127.0.0.1";

    private readonly NotifyIcon _notifyIcon;
    private readonly ContextMenuStrip _menu;
    private readonly ToolStripMenuItem _statusItem;
    private readonly ToolStripMenuItem _foreignItem;
    private readonly ToolStripMenuItem _openNovaItem;
    private readonly ToolStripMenuItem _novaLogicItem;
    private readonly ToolStripMenuItem _customItem;
    private readonly ToolStripMenuItem _serviceUrlItem;

    private readonly HostsSwitcher _switcher = new(new HostsFileStore(), new DnsCacheFlusher());
    private readonly LauncherSettingsStore _settingsStore = new();
    private readonly LauncherSettings _settings;
    private readonly HttpServerInfoClient _serverInfoClient = new();
    private readonly OpenNovaAddressSource _addressSource;

    private HostsState? _state;
    private string? _stateError;
    private bool _busy;

    public TrayApplicationContext()
    {
        _settings = _settingsStore.Load();
        _addressSource = new OpenNovaAddressSource(_serverInfoClient, _settings, () => _settingsStore.Save(_settings));

        _statusItem = new ToolStripMenuItem { Enabled = false };
        _foreignItem = new ToolStripMenuItem(string.Empty, null, OnRemoveForeignClicked) { Visible = false };
        _openNovaItem = new ToolStripMenuItem("OpenNova NovaWorld", null, OnOpenNovaClicked)
        {
            ToolTipText = "Point retail's NovaWorld at OpenNova's NovaWorld service.",
        };
        _novaLogicItem = new ToolStripMenuItem("NovaLogic NovaWorld", null, OnNovaLogicClicked)
        {
            ToolTipText = "Remove the launcher's hosts entries so retail reaches NovaLogic's own NovaWorld.",
        };
        _customItem = new ToolStripMenuItem("Custom address...", null, OnCustomClicked)
        {
            ToolTipText = "Point retail's NovaWorld at an address you enter, such as a local service on 127.0.0.1.",
        };
        _serviceUrlItem = new ToolStripMenuItem("OpenNova service URL...", null, OnServiceUrlClicked);

        _menu = new ContextMenuStrip { ShowCheckMargin = true, ShowImageMargin = false };
        _menu.Items.AddRange(new ToolStripItem[]
        {
            _statusItem,
            _foreignItem,
            new ToolStripSeparator(),
            _openNovaItem,
            _novaLogicItem,
            _customItem,
            new ToolStripSeparator(),
            _serviceUrlItem,
            new ToolStripMenuItem("Exit", null, (_, _) => ExitThread()),
        });
        _menu.Opening += (_, _) => RefreshState();

        _notifyIcon = new NotifyIcon
        {
            Icon = LoadIcon(),
            Text = Program.ProductName,
            ContextMenuStrip = _menu,
            Visible = true,
        };

        RefreshState();
        _ = RefreshOpenNovaAddressAsync();
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            _notifyIcon.Visible = false;
            _notifyIcon.Dispose();
            _menu.Dispose();
            _serverInfoClient.Dispose();
        }

        base.Dispose(disposing);
    }

    protected override void ExitThreadCore()
    {
        _notifyIcon.Visible = false;
        base.ExitThreadCore();
    }

    private void RefreshState()
    {
        try
        {
            _state = _switcher.ReadState();
            _stateError = null;
        }
        catch (Exception ex) when (IsIoFailure(ex))
        {
            _state = null;
            _stateError = ex.Message;
        }

        UpdateMenu();
    }

    private void UpdateMenu()
    {
        var mode = _state?.Mode;
        _statusItem.Text = _state is null ? "Cannot read the hosts file" : "Now: " + Describe(_state);
        _statusItem.ToolTipText = _stateError;

        _openNovaItem.Checked = mode == NovaWorldMode.OpenNova;
        _novaLogicItem.Checked = mode == NovaWorldMode.NovaLogic;
        _customItem.Checked = mode == NovaWorldMode.Custom;
        _customItem.Text = mode == NovaWorldMode.Custom ? $"Custom address ({_state!.Address})..." : "Custom address...";

        var names = string.Join(", ", _switcher.Hostnames);
        _foreignItem.Visible = _state?.HasForeignLines == true;
        _foreignItem.Text = $"Other {names} entries in the hosts file: remove them...";
        _foreignItem.ToolTipText = _state is null ? null : string.Join(Environment.NewLine, _state.ForeignLines);

        foreach (var item in new[] { _openNovaItem, _novaLogicItem, _customItem, _foreignItem })
        {
            item.Enabled = !_busy && _state is not null;
        }

        var tooltip = _state is null ? $"{Program.ProductName}: hosts file unreadable" : $"{Program.ProductName}: {ShortName(_state)}";
        _notifyIcon.Text = tooltip.Length <= 127 ? tooltip : tooltip[..127];
    }

    private static string Describe(HostsState state) => state.Mode switch
    {
        NovaWorldMode.OpenNova => $"OpenNova NovaWorld ({state.Address})",
        NovaWorldMode.NovaLogic => "NovaLogic NovaWorld",
        NovaWorldMode.Custom => $"custom address {state.Address}",
        _ => "a hand-edited launcher block (pick a mode to fix it)",
    };

    private static string ShortName(HostsState state) => state.Mode switch
    {
        NovaWorldMode.OpenNova => "OpenNova NovaWorld",
        NovaWorldMode.NovaLogic => "NovaLogic NovaWorld",
        NovaWorldMode.Custom => $"custom address {state.Address}",
        _ => "hand-edited hosts block",
    };

    private async void OnOpenNovaClicked(object? sender, EventArgs e)
    {
        if (_busy)
        {
            return;
        }

        SetBusy(true);
        try
        {
            var resolved = await _addressSource.ResolveAsync(CancellationToken.None);
            if (resolved is null)
            {
                ShowError(
                    $"Could not get OpenNova's NovaWorld address from {_settings.ServiceUrl}, and no earlier address is saved. " +
                    "Check your internet connection and try again.");
                return;
            }

            if (SwitchTo(NovaWorldMode.OpenNova, resolved.Address) && resolved.FromCache)
            {
                ShowBalloon(
                    "Using the saved address",
                    $"{_settings.ServiceUrl} did not answer, so the launcher used OpenNova's last known address, {resolved.Address}.");
            }
        }
        finally
        {
            SetBusy(false);
        }
    }

    private void OnNovaLogicClicked(object? sender, EventArgs e)
    {
        if (!_busy)
        {
            SwitchTo(NovaWorldMode.NovaLogic, null);
        }
    }

    private void OnCustomClicked(object? sender, EventArgs e)
    {
        if (_busy)
        {
            return;
        }

        var initial = _state?.Mode == NovaWorldMode.Custom ? _state.Address : _settings.CustomAddress;
        var address = InputDialog.Ask(
            Program.ProductName,
            $"The IPv4 address of the NovaWorld service to send {string.Join(", ", _switcher.Hostnames)} to, " +
            "for example 127.0.0.1 for a service on this computer:",
            initial ?? DefaultCustomAddress);
        if (address is null)
        {
            return;
        }

        if (!Ipv4.IsUsable(address))
        {
            ShowError($"'{address}' is not an IPv4 address such as 127.0.0.1 or 192.168.1.20.");
            return;
        }

        _settings.CustomAddress = address;
        SaveSettings();
        SwitchTo(NovaWorldMode.Custom, address);
    }

    private void OnServiceUrlClicked(object? sender, EventArgs e)
    {
        var url = InputDialog.Ask(
            Program.ProductName,
            "The OpenNova service the launcher asks for its NovaWorld address (/api/server-info is added). " +
            $"Leave it empty for {ServerInfo.DefaultServiceUrl}:",
            _settings.ServiceUrl);
        if (url is null)
        {
            return;
        }

        if (url.Length == 0)
        {
            url = ServerInfo.DefaultServiceUrl;
        }

        if (ServerInfo.EndpointFor(url) is null)
        {
            ShowError($"'{url}' is not an http or https URL such as {ServerInfo.DefaultServiceUrl}.");
            return;
        }

        _settings.ServiceUrl = url;
        SaveSettings();
    }

    private void OnRemoveForeignClicked(object? sender, EventArgs e)
    {
        RefreshState();
        if (_busy || _state is null || !_state.HasForeignLines || !ConfirmRemoveForeign(_state, switching: false))
        {
            return;
        }

        try
        {
            _switcher.RemoveForeignLines();
        }
        catch (Exception ex) when (IsIoFailure(ex))
        {
            ShowError(HostsWriteFailed(ex));
        }

        RefreshState();
    }

    /// <summary>
    /// Switches the hosts file to <paramref name="mode"/>, asking first when lines
    /// outside the block also map the hostnames. Returns whether it switched.
    /// </summary>
    private bool SwitchTo(NovaWorldMode mode, string? address)
    {
        RefreshState();
        if (_state is null)
        {
            ShowError($"The hosts file cannot be read: {_stateError}");
            return false;
        }

        var removeForeign = false;
        if (_state.HasForeignLines)
        {
            if (!ConfirmRemoveForeign(_state, switching: true))
            {
                return false;
            }

            removeForeign = true;
        }

        try
        {
            _switcher.Switch(mode, address, removeForeign);
        }
        catch (Exception ex) when (IsIoFailure(ex))
        {
            ShowError(HostsWriteFailed(ex));
            RefreshState();
            return false;
        }

        RefreshState();
        if (_state is not null)
        {
            ShowBalloon(
                "Switched to " + ShortName(_state),
                "Retail Joint Operations uses it the next time it connects to NovaWorld.");
        }

        return true;
    }

    private bool ConfirmRemoveForeign(HostsState state, bool switching)
    {
        var names = string.Join(", ", _switcher.Hostnames);
        var lines = string.Join(Environment.NewLine, state.ForeignLines.Select(line => "    " + line.Trim()));
        var question = switching
            ? "Windows uses the first entry it finds, so the switch would not take effect reliably. Remove these lines and switch?"
            : "Remove these lines?";
        var answer = MessageBox.Show(
            $"Your hosts file has entries for {names} that OpenNova Launcher did not write:{Environment.NewLine}{Environment.NewLine}" +
            $"{lines}{Environment.NewLine}{Environment.NewLine}{question}",
            Program.ProductName,
            MessageBoxButtons.YesNo,
            MessageBoxIcon.Warning,
            MessageBoxDefaultButton.Button2);
        return answer == DialogResult.Yes;
    }

    /// <summary>
    /// At start: when the hosts file is on OpenNova and the service now names another
    /// address (the server moved), rewrite the block with it.
    /// </summary>
    private async Task RefreshOpenNovaAddressAsync()
    {
        if (_state is not { Mode: NovaWorldMode.OpenNova, HasForeignLines: false })
        {
            return;
        }

        try
        {
            var resolved = await _addressSource.ResolveAsync(CancellationToken.None);
            if (_busy || resolved is null || resolved.FromCache)
            {
                return;
            }

            RefreshState();
            if (_state is not { Mode: NovaWorldMode.OpenNova, HasForeignLines: false } ||
                string.Equals(_state.Address, resolved.Address, StringComparison.Ordinal))
            {
                return;
            }

            _switcher.Switch(NovaWorldMode.OpenNova, resolved.Address, removeForeign: false);
            RefreshState();
            ShowBalloon("OpenNova NovaWorld moved", $"The hosts file now sends NovaWorld to OpenNova's new address, {resolved.Address}.");
        }
        catch (Exception ex) when (IsIoFailure(ex))
        {
            Debug.WriteLine($"[Hosts] Address refresh failed: {ex.Message}");
        }
    }

    private void SetBusy(bool busy)
    {
        _busy = busy;
        UpdateMenu();
    }

    private void SaveSettings()
    {
        try
        {
            _settingsStore.Save(_settings);
        }
        catch (Exception ex) when (IsIoFailure(ex))
        {
            ShowError($"Could not save the launcher's settings to {_settingsStore.SettingsPath}: {ex.Message}");
        }
    }

    private static string HostsWriteFailed(Exception ex) =>
        $"Windows did not let the launcher change the hosts file: {ex.Message}{Environment.NewLine}{Environment.NewLine}" +
        "Security software can lock it. Windows Defender may report the change as SettingsModifier:Win32/HostsFileHijack; " +
        "see the launcher's README.";

    private static bool IsIoFailure(Exception ex) => ex is IOException or UnauthorizedAccessException or SecurityException;

    private static void ShowError(string message)
    {
        MessageBox.Show(message, Program.ProductName, MessageBoxButtons.OK, MessageBoxIcon.Error);
    }

    private void ShowBalloon(string title, string message)
    {
        _notifyIcon.BalloonTipTitle = title;
        _notifyIcon.BalloonTipText = message;
        _notifyIcon.ShowBalloonTip(4000);
    }

    private static Icon LoadIcon()
    {
        using var stream = typeof(TrayApplicationContext).Assembly.GetManifestResourceStream("onlauncher.ico");
        return stream is null ? SystemIcons.Application : new Icon(stream, SystemInformation.SmallIconSize);
    }
}
