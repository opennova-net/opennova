using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Windows.Forms;
using OpenNova.Launcher.Models;

namespace OpenNova.Launcher.Forms;

public sealed partial class PreferencesForm : Form
{
    private readonly Dictionary<string, TextBox> _directoryInputs = new(StringComparer.OrdinalIgnoreCase);
    private readonly IReadOnlyList<GameDefinition> _supportedGames;
    private readonly Func<string>? _redirectionStatusProvider;
    private readonly Action? _removeHostsEntries;

    public PreferencesForm(
        IReadOnlyList<GameDefinition> supportedGames,
        IReadOnlyDictionary<string, string> currentDirectories,
        bool redirectionEnabled,
        bool devModeEnabled,
        Func<string>? redirectionStatusProvider = null,
        Action? removeHostsEntries = null)
    {
        InitializeComponent();

        _supportedGames = supportedGames ?? GameCatalog.SupportedGames;
        _redirectionStatusProvider = redirectionStatusProvider;
        _removeHostsEntries = removeHostsEntries;

        toolTip.ShowAlways = true;
        PopulateGameRows(currentDirectories ?? new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase));

        redirectionCheckBox.Checked = redirectionEnabled;
        devModeCheckBox.Checked = devModeEnabled;
        removeHostsButton.Click += RemoveHostsButtonOnClick;
        RefreshRedirectionStatus();

        versionLabel.Text = $"Version {GetAssemblyVersion()}";
    }

    public Dictionary<string, string> SelectedDirectories
    {
        get
        {
            var result = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            foreach (var pair in _directoryInputs)
            {
                var value = pair.Value.Text.Trim();
                if (!string.IsNullOrWhiteSpace(value))
                {
                    result[pair.Key] = value;
                }
            }

            return result;
        }
    }

    public bool RedirectionEnabled => redirectionCheckBox.Checked;

    public bool DevModeEnabled => devModeCheckBox.Checked;

    public event EventHandler? CheckForUpdatesRequested
    {
        add => checkUpdatesButton.Click += value;
        remove => checkUpdatesButton.Click -= value;
    }

    private void RefreshRedirectionStatus()
    {
        redirectionStatusLabel.Text = _redirectionStatusProvider?.Invoke()
            ?? "Redirection status unavailable.";
    }

    private void RemoveHostsButtonOnClick(object? sender, EventArgs e)
    {
        _removeHostsEntries?.Invoke();
        RefreshRedirectionStatus();
    }

    private void PopulateGameRows(IReadOnlyDictionary<string, string> currentDirectories)
    {
        tableLayoutPanelGames.SuspendLayout();
        _directoryInputs.Clear();

        foreach (var game in _supportedGames)
        {
            if (!TryGetControlsForSlug(game.Slug, out var textBox, out var browseButton))
            {
                continue;
            }

            RegisterGameInput(game, textBox, browseButton, currentDirectories);
        }

        tableLayoutPanelGames.ResumeLayout();
    }

    private bool TryGetControlsForSlug(string slug, out TextBox textBox, out Button browseButton)
    {
        switch (slug.ToLowerInvariant())
        {
            case "jop_2_consumer":
                textBox = textBoxJointOps;
                browseButton = buttonBrowseJointOps;
                return true;
            case "dfx2_consumer":
                textBox = textBoxDfx2;
                browseButton = buttonBrowseDfx2;
                return true;
            default:
                textBox = textBoxJointOps;
                browseButton = buttonBrowseJointOps;
                return false;
        }
    }

    private void RegisterGameInput(GameDefinition game, TextBox textBox, Button browseButton, IReadOnlyDictionary<string, string> currentDirectories)
    {
        textBox.PlaceholderText = "Not set";
        textBox.ReadOnly = true;
        textBox.BorderStyle = BorderStyle.FixedSingle;
        browseButton.Tag = game.Slug;
        browseButton.Click -= BrowseButtonOnClick;
        browseButton.Click += BrowseButtonOnClick;

        if (currentDirectories.TryGetValue(game.Slug, out var existing) && !string.IsNullOrWhiteSpace(existing))
        {
            textBox.Text = existing.Trim();
            toolTip.SetToolTip(textBox, textBox.Text);
        }
        else
        {
            textBox.Clear();
            toolTip.SetToolTip(textBox, "Not set");
        }

        _directoryInputs[game.Slug] = textBox;
    }

    private void BrowseButtonOnClick(object? sender, EventArgs e)
    {
        if (sender is not Button button || button.Tag is not string slug)
        {
            return;
        }

        var game = _supportedGames.FirstOrDefault(g => string.Equals(g.Slug, slug, StringComparison.OrdinalIgnoreCase));

        if (game == null)
        {
            return;
        }

        if (!_directoryInputs.TryGetValue(game.Slug, out var textBox))
        {
            return;
        }

        using var dialog = new FolderBrowserDialog
        {
            Description = $"Select the install directory for {game.DisplayName}",
            ShowNewFolderButton = false
        };

        if (!string.IsNullOrWhiteSpace(textBox.Text) && Directory.Exists(textBox.Text))
        {
            dialog.SelectedPath = textBox.Text;
        }

        if (dialog.ShowDialog() != DialogResult.OK)
        {
            return;
        }

        var selectedPath = dialog.SelectedPath.Trim();
        textBox.Text = selectedPath;
        toolTip.SetToolTip(textBox, selectedPath);
    }

    private static string GetAssemblyVersion()
    {
        var version = Assembly.GetExecutingAssembly().GetName().Version;
        return version?.ToString() ?? "Unknown";
    }
}
