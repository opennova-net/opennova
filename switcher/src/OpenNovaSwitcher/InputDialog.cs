namespace OpenNova.Switcher;

/// <summary>A one-line text prompt: a label, a text box, OK and Cancel.</summary>
internal sealed class InputDialog : Form
{
    private readonly TextBox _textBox;

    private InputDialog(string title, string prompt, string initialValue)
    {
        Text = title;
        FormBorderStyle = FormBorderStyle.FixedDialog;
        StartPosition = FormStartPosition.CenterScreen;
        MaximizeBox = false;
        MinimizeBox = false;
        ShowInTaskbar = true;
        TopMost = true;
        AutoSize = true;
        AutoSizeMode = AutoSizeMode.GrowAndShrink;
        Padding = new Padding(12);

        var layout = new TableLayoutPanel
        {
            ColumnCount = 1,
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            Dock = DockStyle.Fill,
        };

        var label = new Label { Text = prompt, AutoSize = true, MaximumSize = new Size(420, 0), Margin = new Padding(0, 0, 0, 8) };
        _textBox = new TextBox { Text = initialValue, Width = 420, Margin = new Padding(0, 0, 0, 12) };

        var ok = new Button { Text = "OK", DialogResult = DialogResult.OK, AutoSize = true };
        var cancel = new Button { Text = "Cancel", DialogResult = DialogResult.Cancel, AutoSize = true };
        var buttons = new FlowLayoutPanel
        {
            FlowDirection = FlowDirection.RightToLeft,
            AutoSize = true,
            Dock = DockStyle.Fill,
            Margin = Padding.Empty,
        };
        buttons.Controls.Add(cancel);
        buttons.Controls.Add(ok);

        layout.Controls.Add(label);
        layout.Controls.Add(_textBox);
        layout.Controls.Add(buttons);
        Controls.Add(layout);

        AcceptButton = ok;
        CancelButton = cancel;
    }

    /// <summary>The text entered, trimmed, or null when the dialog was cancelled.</summary>
    public static string? Ask(string title, string prompt, string initialValue)
    {
        using var dialog = new InputDialog(title, prompt, initialValue);
        dialog.Shown += (_, _) =>
        {
            dialog.Activate();
            dialog._textBox.SelectAll();
            dialog._textBox.Focus();
        };
        return dialog.ShowDialog() == DialogResult.OK ? dialog._textBox.Text.Trim() : null;
    }
}
