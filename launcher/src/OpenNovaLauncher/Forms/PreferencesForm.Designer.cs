namespace OpenNova.Launcher.Forms;

partial class PreferencesForm
{
    private System.ComponentModel.IContainer components = null;

    /// <summary>
    ///  Clean up any resources being used.
    /// </summary>
    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            components?.Dispose();
        }
        base.Dispose(disposing);
    }

    private void InitializeComponent()
    {
        components = new System.ComponentModel.Container();
        mainLayout = new TableLayoutPanel();
        headerLabel = new Label();
        directoriesPanel = new Panel();
        tableLayoutPanelGames = new TableLayoutPanel();
        labelJointOps = new Label();
        textBoxJointOps = new TextBox();
        buttonBrowseJointOps = new Button();
        labelDfx2 = new Label();
        textBoxDfx2 = new TextBox();
        buttonBrowseDfx2 = new Button();
        statusPanel = new FlowLayoutPanel();
        checkUpdatesButton = new Button();
        versionLabel = new Label();
        buttonsPanel = new FlowLayoutPanel();
        cancelButton = new Button();
        okButton = new Button();
        toolTip = new ToolTip(components);
        mainLayout.SuspendLayout();
        directoriesPanel.SuspendLayout();
        tableLayoutPanelGames.SuspendLayout();
        statusPanel.SuspendLayout();
        buttonsPanel.SuspendLayout();
        SuspendLayout();
        //
        // mainLayout
        //
        mainLayout.ColumnCount = 1;
        mainLayout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100F));
        mainLayout.Controls.Add(headerLabel, 0, 0);
        mainLayout.Controls.Add(directoriesPanel, 0, 1);
        mainLayout.Controls.Add(statusPanel, 0, 2);
        mainLayout.Controls.Add(buttonsPanel, 0, 3);
        mainLayout.Dock = DockStyle.Fill;
        mainLayout.Location = new Point(0, 0);
        mainLayout.Margin = new Padding(0);
        mainLayout.Name = "mainLayout";
        mainLayout.Padding = new Padding(16, 16, 16, 12);
        mainLayout.RowCount = 4;
        mainLayout.RowStyles.Add(new RowStyle());
        mainLayout.RowStyles.Add(new RowStyle(SizeType.Percent, 100F));
        mainLayout.RowStyles.Add(new RowStyle());
        mainLayout.RowStyles.Add(new RowStyle());
        mainLayout.Size = new Size(580, 460);
        mainLayout.TabIndex = 0;
        //
        // headerLabel
        //
        headerLabel.Anchor = AnchorStyles.Left;
        headerLabel.AutoSize = true;
        headerLabel.Margin = new Padding(0, 0, 0, 12);
        headerLabel.Name = "headerLabel";
        headerLabel.TabIndex = 0;
        headerLabel.Text = "Registered game directories:";
        //
        // directoriesPanel
        //
        directoriesPanel.AutoScroll = true;
        directoriesPanel.AutoScrollMargin = new Size(0, 8);
        directoriesPanel.BorderStyle = BorderStyle.FixedSingle;
        directoriesPanel.Controls.Add(tableLayoutPanelGames);
        directoriesPanel.Dock = DockStyle.Fill;
        directoriesPanel.Margin = new Padding(0);
        directoriesPanel.MinimumSize = new Size(0, 100);
        directoriesPanel.Name = "directoriesPanel";
        directoriesPanel.TabIndex = 1;
        //
        // tableLayoutPanelGames
        //
        tableLayoutPanelGames.AutoSize = true;
        tableLayoutPanelGames.AutoSizeMode = AutoSizeMode.GrowAndShrink;
        tableLayoutPanelGames.ColumnCount = 3;
        tableLayoutPanelGames.ColumnStyles.Add(new ColumnStyle());
        tableLayoutPanelGames.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100F));
        tableLayoutPanelGames.ColumnStyles.Add(new ColumnStyle());
        tableLayoutPanelGames.Controls.Add(labelJointOps, 0, 0);
        tableLayoutPanelGames.Controls.Add(textBoxJointOps, 1, 0);
        tableLayoutPanelGames.Controls.Add(buttonBrowseJointOps, 2, 0);
        tableLayoutPanelGames.Controls.Add(labelDfx2, 0, 1);
        tableLayoutPanelGames.Controls.Add(textBoxDfx2, 1, 1);
        tableLayoutPanelGames.Controls.Add(buttonBrowseDfx2, 2, 1);
        tableLayoutPanelGames.Dock = DockStyle.Top;
        tableLayoutPanelGames.Margin = new Padding(0);
        tableLayoutPanelGames.Name = "tableLayoutPanelGames";
        tableLayoutPanelGames.RowCount = 2;
        tableLayoutPanelGames.RowStyles.Add(new RowStyle());
        tableLayoutPanelGames.RowStyles.Add(new RowStyle());
        tableLayoutPanelGames.TabIndex = 0;
        //
        // labelJointOps
        //
        labelJointOps.AutoSize = true;
        labelJointOps.Margin = new Padding(0, 6, 6, 6);
        labelJointOps.Name = "labelJointOps";
        labelJointOps.TabIndex = 0;
        labelJointOps.Text = "Joint Operations: Typhoon Rising:";
        //
        // textBoxJointOps
        //
        textBoxJointOps.Dock = DockStyle.Fill;
        textBoxJointOps.Margin = new Padding(0, 3, 6, 3);
        textBoxJointOps.Name = "textBoxJointOps";
        textBoxJointOps.PlaceholderText = "Not set";
        textBoxJointOps.ReadOnly = true;
        textBoxJointOps.TabIndex = 1;
        //
        // buttonBrowseJointOps
        //
        buttonBrowseJointOps.AutoSize = true;
        buttonBrowseJointOps.AutoSizeMode = AutoSizeMode.GrowAndShrink;
        buttonBrowseJointOps.Margin = new Padding(0, 3, 0, 3);
        buttonBrowseJointOps.Name = "buttonBrowseJointOps";
        buttonBrowseJointOps.TabIndex = 2;
        buttonBrowseJointOps.Text = "Browse…";
        buttonBrowseJointOps.UseVisualStyleBackColor = true;
        //
        // labelDfx2
        //
        labelDfx2.AutoSize = true;
        labelDfx2.Margin = new Padding(0, 6, 6, 6);
        labelDfx2.Name = "labelDfx2";
        labelDfx2.TabIndex = 3;
        labelDfx2.Text = "Delta Force Xtreme 2:";
        //
        // textBoxDfx2
        //
        textBoxDfx2.Dock = DockStyle.Fill;
        textBoxDfx2.Margin = new Padding(0, 3, 6, 3);
        textBoxDfx2.Name = "textBoxDfx2";
        textBoxDfx2.PlaceholderText = "Not set";
        textBoxDfx2.ReadOnly = true;
        textBoxDfx2.TabIndex = 4;
        //
        // buttonBrowseDfx2
        //
        buttonBrowseDfx2.AutoSize = true;
        buttonBrowseDfx2.AutoSizeMode = AutoSizeMode.GrowAndShrink;
        buttonBrowseDfx2.Margin = new Padding(0, 3, 0, 3);
        buttonBrowseDfx2.Name = "buttonBrowseDfx2";
        buttonBrowseDfx2.TabIndex = 5;
        buttonBrowseDfx2.Text = "Browse…";
        buttonBrowseDfx2.UseVisualStyleBackColor = true;
        //
        // statusPanel
        //
        statusPanel.AutoSize = true;
        statusPanel.AutoSizeMode = AutoSizeMode.GrowAndShrink;
        statusPanel.Controls.Add(checkUpdatesButton);
        statusPanel.Controls.Add(versionLabel);
        statusPanel.Dock = DockStyle.Fill;
        statusPanel.FlowDirection = FlowDirection.LeftToRight;
        statusPanel.Margin = new Padding(0, 12, 0, 0);
        statusPanel.Name = "statusPanel";
        statusPanel.Padding = new Padding(0);
        statusPanel.TabIndex = 3;
        statusPanel.WrapContents = false;
        //
        // checkUpdatesButton
        //
        checkUpdatesButton.AutoSize = true;
        checkUpdatesButton.Margin = new Padding(0, 0, 16, 0);
        checkUpdatesButton.Name = "checkUpdatesButton";
        checkUpdatesButton.TabIndex = 0;
        checkUpdatesButton.Text = "Check for updates";
        checkUpdatesButton.UseVisualStyleBackColor = true;
        //
        // versionLabel
        //
        versionLabel.AutoSize = true;
        versionLabel.ForeColor = Color.DimGray;
        versionLabel.Margin = new Padding(0, 6, 16, 0);
        versionLabel.Name = "versionLabel";
        versionLabel.TabIndex = 1;
        versionLabel.Text = "Version 0.0.0";
        //
        // buttonsPanel
        //
        buttonsPanel.AutoSize = true;
        buttonsPanel.AutoSizeMode = AutoSizeMode.GrowAndShrink;
        buttonsPanel.Anchor = AnchorStyles.Right;
        buttonsPanel.Controls.Add(cancelButton);
        buttonsPanel.Controls.Add(okButton);
        buttonsPanel.FlowDirection = FlowDirection.RightToLeft;
        buttonsPanel.Margin = new Padding(0, 12, 0, 0);
        buttonsPanel.Name = "buttonsPanel";
        buttonsPanel.Padding = new Padding(0);
        buttonsPanel.TabIndex = 4;
        buttonsPanel.WrapContents = false;
        //
        // cancelButton
        //
        cancelButton.DialogResult = DialogResult.Cancel;
        cancelButton.Margin = new Padding(0, 6, 0, 0);
        cancelButton.Name = "cancelButton";
        cancelButton.Size = new Size(100, 27);
        cancelButton.TabIndex = 1;
        cancelButton.Text = "Cancel";
        cancelButton.UseVisualStyleBackColor = true;
        //
        // okButton
        //
        okButton.DialogResult = DialogResult.OK;
        okButton.Margin = new Padding(8, 6, 0, 0);
        okButton.Name = "okButton";
        okButton.Size = new Size(96, 27);
        okButton.TabIndex = 0;
        okButton.Text = "OK";
        okButton.UseVisualStyleBackColor = true;
        //
        // PreferencesForm
        //
        AcceptButton = okButton;
        AutoScaleDimensions = new SizeF(7F, 15F);
        AutoScaleMode = AutoScaleMode.Font;
        CancelButton = cancelButton;
        ClientSize = new Size(580, 460);
        Controls.Add(mainLayout);
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;
        MinimizeBox = false;
        Name = "PreferencesForm";
        StartPosition = FormStartPosition.CenterScreen;
        Text = "Preferences";
        mainLayout.ResumeLayout(false);
        mainLayout.PerformLayout();
        directoriesPanel.ResumeLayout(false);
        directoriesPanel.PerformLayout();
        tableLayoutPanelGames.ResumeLayout(false);
        tableLayoutPanelGames.PerformLayout();
        statusPanel.ResumeLayout(false);
        statusPanel.PerformLayout();
        buttonsPanel.ResumeLayout(false);
        ResumeLayout(false);
    }

    private System.Windows.Forms.TableLayoutPanel mainLayout;
    private System.Windows.Forms.Label headerLabel;
    private System.Windows.Forms.Panel directoriesPanel;
    private System.Windows.Forms.TableLayoutPanel tableLayoutPanelGames;
    private System.Windows.Forms.Label labelJointOps;
    private System.Windows.Forms.TextBox textBoxJointOps;
    private System.Windows.Forms.Button buttonBrowseJointOps;
    private System.Windows.Forms.Label labelDfx2;
    private System.Windows.Forms.TextBox textBoxDfx2;
    private System.Windows.Forms.Button buttonBrowseDfx2;
    private System.Windows.Forms.FlowLayoutPanel statusPanel;
    private System.Windows.Forms.Button checkUpdatesButton;
    private System.Windows.Forms.Label versionLabel;
    private System.Windows.Forms.FlowLayoutPanel buttonsPanel;
    private System.Windows.Forms.Button cancelButton;
    private System.Windows.Forms.Button okButton;
    private System.Windows.Forms.ToolTip toolTip;
}
