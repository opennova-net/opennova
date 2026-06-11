using System;
using System.Collections.Generic;
using System.Linq;
using System.Windows.Forms;

namespace OpenNova.Launcher.Forms;

internal sealed partial class ExpansionLaunchDialog : Form
{
    public ExpansionLaunchDialog(
        string gameName,
        IReadOnlyList<ExpansionLaunchOption> options,
        string? initialSelectionSlug = null,
        bool initialWindowed = true,
        bool initialAllowManyInstances = true)
    {
        if (options is null || options.Count == 0)
        {
            throw new ArgumentException("At least one option is required", nameof(options));
        }

        InitializeComponent();

        Text = $"{gameName} – Select Expansion";
        headerLabel.Text = "Choose which expansion to launch (or launch without one).";

        windowedCheckBox.Checked = initialWindowed;
        allowManyInstancesCheckBox.Checked = initialAllowManyInstances;

        optionsList.SelectedIndexChanged += (_, _) => UpdateLaunchButtonState();
        optionsList.DoubleClick += (_, _) => TryAcceptSelection();

        foreach (var option in options)
        {
            var item = new ListViewItem(option.DisplayName)
            {
                Tag = option
            };
            item.SubItems.Add(option.VersionLabel);
            item.SubItems.Add(option.SummaryLabel);
            optionsList.Items.Add(item);
        }

        ListViewItem? itemToSelect = null;
        if (!string.IsNullOrWhiteSpace(initialSelectionSlug))
        {
            itemToSelect = optionsList.Items
                .Cast<ListViewItem>()
                .FirstOrDefault(item =>
                    item.Tag is ExpansionLaunchOption option &&
                    string.Equals(option.Slug, initialSelectionSlug, StringComparison.OrdinalIgnoreCase));
        }

        itemToSelect ??= optionsList.Items.Cast<ListViewItem>().FirstOrDefault();
        if (itemToSelect != null)
        {
            itemToSelect.Selected = true;
            itemToSelect.Focused = true;
        }

        optionsList.AutoResizeColumns(ColumnHeaderAutoResizeStyle.ColumnContent);
        for (var i = 0; i < optionsList.Columns.Count; i++)
        {
            var column = optionsList.Columns[i];
            var minimumWidth = i switch
            {
                0 => 260,
                1 => 100,
                _ => 180
            };
            if (column.Width < minimumWidth)
            {
                column.Width = minimumWidth;
            }
        }

        UpdateLaunchButtonState();
    }

    public string? SelectedExpansionSlug { get; private set; }
    public bool LaunchWindowed => windowedCheckBox.Checked;
    public bool AllowManyInstances => allowManyInstancesCheckBox.Checked;
    public bool UseRealNovaWorld => useRealNovaWorldCheckBox.Checked;

    private void LaunchButton_Click(object sender, EventArgs e)
    {
        if (!TryCommitSelection())
        {
            DialogResult = DialogResult.None;
        }
    }

    private void UpdateLaunchButtonState()
    {
        launchButton.Enabled = optionsList.SelectedItems.Count > 0;
    }

    private void TryAcceptSelection()
    {
        if (TryCommitSelection())
        {
            DialogResult = DialogResult.OK;
            Close();
        }
    }

    private bool TryCommitSelection()
    {
        if (optionsList.SelectedItems.Count == 0)
        {
            MessageBox.Show(
                this,
                "Select an expansion to continue.",
                "OpenNova Launcher",
                MessageBoxButtons.OK,
                MessageBoxIcon.Information);
            return false;
        }

        if (optionsList.SelectedItems[0].Tag is not ExpansionLaunchOption option)
        {
            return false;
        }

        SelectedExpansionSlug = option.Slug;
        return true;
    }
}

internal sealed record ExpansionLaunchOption(
    string? Slug,
    string DisplayName,
    string VersionLabel,
    string SummaryLabel)
{
    public static ExpansionLaunchOption BaseGame() => new(
        null,
        "Launch without expansion",
        "—",
        string.Empty);
}
