using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;
using OpenNova.Launcher.Models;

namespace OpenNova.Launcher.Forms;

internal partial class ExpansionManagerForm : Form
{
    private readonly TrayApplicationContext _host;
    private IReadOnlyList<GameExpansionStatus> _gameStatuses = Array.Empty<GameExpansionStatus>();
    private bool _isBusy;

    public ExpansionManagerForm(TrayApplicationContext host)
    {
        _host = host;
        InitializeComponent();
    }

    protected override async void OnShown(EventArgs e)
    {
        base.OnShown(e);
        await ReloadAsync();
    }

    private async Task ReloadAsync()
    {
        await RunOperationAsync(async progress =>
        {
            progress.Report("Loading expansion catalog...");
            await _host.RefreshExpansionCatalogAsync(CancellationToken.None);
        }, suppressCompletionMessage: true);

        _gameStatuses = _host.GetExpansionStatuses();
        PopulateGames();
        PopulateExpansions();
        labelStatus.Text = "Ready.";
    }

    private void PopulateGames()
    {
        var previousSlug = GetSelectedGame()?.Game.Slug;
        comboGames.BeginUpdate();
        comboGames.Items.Clear();

        foreach (var gameStatus in _gameStatuses)
        {
            comboGames.Items.Add(new GameListItem(gameStatus));
        }

        comboGames.EndUpdate();

        if (comboGames.Items.Count == 0)
        {
            listExpansions.Items.Clear();
            UpdateButtons();
            return;
        }

        if (previousSlug != null)
        {
            for (var i = 0; i < comboGames.Items.Count; i++)
            {
                if (comboGames.Items[i] is GameListItem item &&
                    string.Equals(item.Status.Game.Slug, previousSlug, StringComparison.OrdinalIgnoreCase))
                {
                    comboGames.SelectedIndex = i;
                    return;
                }
            }
        }

        comboGames.SelectedIndex = 0;
    }

    private void PopulateExpansions()
    {
        var selectedGame = GetSelectedGame();
        listExpansions.BeginUpdate();
        listExpansions.Items.Clear();

        if (selectedGame != null)
        {
            foreach (var status in selectedGame.Expansions)
            {
                var item = new ListViewItem(status.Descriptor.DisplayName)
                {
                    Tag = status
                };
                item.SubItems.Add(status.Descriptor.Version);
                item.SubItems.Add(RenderState(status));
                item.SubItems.Add(RenderDetails(status));
                item.SubItems.Add(status.Descriptor.Summary ?? string.Empty);
                listExpansions.Items.Add(item);
            }
        }

        listExpansions.EndUpdate();
        UpdateButtons();
    }

    private static string RenderState(ExpansionStatus status)
    {
        // An expansion with no downloadable files exists in the catalogue but has
        // no release published yet — surface that instead of "Not installed", which
        // would imply an Install that only errors out.
        if (status.Descriptor.Files.Count == 0)
        {
            return "Not published yet";
        }

        return status.State switch
        {
            ExpansionState.Installed => "Installed",
            ExpansionState.UpdateAvailable => "Update available",
            ExpansionState.NeedsGameDirectory => "Needs setup",
            _ => "Not installed",
        };
    }

    private static string RenderDetails(ExpansionStatus status)
        => status.StatusMessage;

    private GameExpansionStatus? GetSelectedGame()
    {
        if (comboGames.SelectedItem is GameListItem item)
        {
            return item.Status;
        }

        return null;
    }

    private ExpansionStatus? GetSelectedExpansion()
    {
        if (listExpansions.SelectedItems.Count == 0)
        {
            return null;
        }

        return listExpansions.SelectedItems[0].Tag as ExpansionStatus;
    }

    private async Task RunOperationAsync(Func<IProgress<string>, Task> operation, bool suppressCompletionMessage = false)
    {
        if (_isBusy)
        {
            return;
        }

        try
        {
            _isBusy = true;
            SetBusyState(true);
            var progress = new Progress<string>(message => labelStatus.Text = message);
            await operation(progress);
            if (!suppressCompletionMessage)
            {
                labelStatus.Text = "Completed.";
            }
        }
        catch (Exception ex)
        {
            labelStatus.Text = "Failed.";
            MessageBox.Show(this, ex.Message, "Expansion Manager", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
        finally
        {
            _isBusy = false;
            SetBusyState(false);
            UpdateButtons();
        }
    }

    private void SetBusyState(bool busy)
    {
        progressBar.Visible = busy;
        progressBar.Style = busy ? ProgressBarStyle.Marquee : ProgressBarStyle.Blocks;
        comboGames.Enabled = !busy;
        listExpansions.Enabled = !busy;
        buttonRefresh.Enabled = !busy;
        buttonInstall.Enabled = !busy && buttonInstall.Enabled;
        buttonRemove.Enabled = !busy && buttonRemove.Enabled;
        buttonClose.Enabled = !busy;
    }

    private void UpdateButtons()
    {
        if (_isBusy)
        {
            buttonInstall.Enabled = false;
            buttonRemove.Enabled = false;
            return;
        }

        var selected = GetSelectedExpansion();
        var gameStatus = GetSelectedGame();

        if (selected == null || gameStatus == null)
        {
            buttonInstall.Enabled = false;
            buttonRemove.Enabled = false;
            buttonInstall.Text = "Install";
            return;
        }

        buttonInstall.Text = selected.State == ExpansionState.UpdateAvailable ? "Update" : "Install";
        buttonInstall.Enabled = selected.State is ExpansionState.NotInstalled or ExpansionState.UpdateAvailable;
        buttonInstall.Enabled &= gameStatus.IsConfigured;
        // Nothing to download until a release is published — keep Install disabled so
        // the user never triggers the "no files to download" error dialog.
        buttonInstall.Enabled &= selected.Descriptor.Files.Count > 0;

        buttonRemove.Enabled = selected.State is ExpansionState.Installed or ExpansionState.UpdateAvailable;
    }

    private void comboGames_SelectedIndexChanged(object? sender, EventArgs e)
    {
        PopulateExpansions();
    }

    private void listExpansions_SelectedIndexChanged(object? sender, EventArgs e)
    {
        UpdateButtons();
    }

    private async void buttonRefresh_Click(object? sender, EventArgs e)
    {
        await ReloadAsync();
    }

    private async void buttonInstall_Click(object? sender, EventArgs e)
    {
        var gameStatus = GetSelectedGame();
        var expansion = GetSelectedExpansion();
        if (gameStatus == null || expansion == null)
        {
            return;
        }

        await RunOperationAsync(async progress =>
        {
            await _host.InstallExpansionAsync(
                    gameStatus.Game.Slug,
                    expansion.Descriptor.Slug,
                    progress,
                    CancellationToken.None);
        });

        await RefreshStatusesAsync();
    }

    private async void buttonRemove_Click(object? sender, EventArgs e)
    {
        var gameStatus = GetSelectedGame();
        var expansion = GetSelectedExpansion();
        if (gameStatus == null || expansion?.Installed == null)
        {
            return;
        }

        await RunOperationAsync(async progress =>
        {
            await _host.RemoveExpansionAsync(
                    gameStatus.Game.Slug,
                    expansion.Descriptor.Slug,
                    progress,
                    CancellationToken.None);
        });

        await RefreshStatusesAsync();
    }

    private Task RefreshStatusesAsync()
    {
        var selectedGameSlug = GetSelectedGame()?.Game.Slug;
        var selectedExpansionSlug = GetSelectedExpansion()?.Descriptor.Slug;

        _gameStatuses = _host.GetExpansionStatuses();
        PopulateGames();

        if (!string.IsNullOrEmpty(selectedGameSlug))
        {
            for (var i = 0; i < comboGames.Items.Count; i++)
            {
                if (comboGames.Items[i] is GameListItem item &&
                    string.Equals(item.Status.Game.Slug, selectedGameSlug, StringComparison.OrdinalIgnoreCase))
                {
                    comboGames.SelectedIndex = i;
                    break;
                }
            }
        }

        if (!string.IsNullOrEmpty(selectedExpansionSlug))
        {
            foreach (ListViewItem item in listExpansions.Items)
            {
                if (item.Tag is ExpansionStatus status &&
                    string.Equals(status.Descriptor.Slug, selectedExpansionSlug, StringComparison.OrdinalIgnoreCase))
                {
                    item.Selected = true;
                    item.Focused = true;
                    item.EnsureVisible();
                    break;
                }
            }
        }

        labelStatus.Text = "Ready.";

        return Task.CompletedTask;
    }

    private void buttonClose_Click(object? sender, EventArgs e)
    {
        Close();
    }

    private sealed class GameListItem
    {
        public GameListItem(GameExpansionStatus status)
        {
            Status = status;
        }

        public GameExpansionStatus Status { get; }

        public override string ToString() => Status.Game.DisplayName;
    }
}
