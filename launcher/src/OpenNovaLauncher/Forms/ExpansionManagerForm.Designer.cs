namespace OpenNova.Launcher.Forms;

internal partial class ExpansionManagerForm
{
    /// <summary>
    ///  Required designer variable.
    /// </summary>
    private System.ComponentModel.IContainer components = null;

    /// <summary>
    ///  Clean up any resources being used.
    /// </summary>
    /// <param name="disposing">true if managed resources should be disposed; otherwise, false.</param>
    protected override void Dispose(bool disposing)
    {
        if (disposing && (components != null))
        {
            components.Dispose();
        }
        base.Dispose(disposing);
    }

    #region Windows Form Designer generated code

    private void InitializeComponent()
    {
        this.comboGames = new System.Windows.Forms.ComboBox();
        this.buttonRefresh = new System.Windows.Forms.Button();
        this.listExpansions = new System.Windows.Forms.ListView();
        this.columnName = new System.Windows.Forms.ColumnHeader();
        this.columnVersion = new System.Windows.Forms.ColumnHeader();
        this.columnState = new System.Windows.Forms.ColumnHeader();
        this.columnDetails = new System.Windows.Forms.ColumnHeader();
        this.columnSummary = new System.Windows.Forms.ColumnHeader();
        this.buttonInstall = new System.Windows.Forms.Button();
        this.buttonRemove = new System.Windows.Forms.Button();
        this.buttonClose = new System.Windows.Forms.Button();
        this.progressBar = new System.Windows.Forms.ProgressBar();
        this.labelStatus = new System.Windows.Forms.Label();
        this.SuspendLayout();
        //
        // comboGames
        //
        this.comboGames.DropDownStyle = System.Windows.Forms.ComboBoxStyle.DropDownList;
        this.comboGames.FormattingEnabled = true;
        this.comboGames.Location = new System.Drawing.Point(12, 12);
        this.comboGames.Name = "comboGames";
        this.comboGames.Size = new System.Drawing.Size(320, 23);
        this.comboGames.TabIndex = 0;
        this.comboGames.SelectedIndexChanged += new System.EventHandler(this.comboGames_SelectedIndexChanged);
        //
        // buttonRefresh
        //
        this.buttonRefresh.Anchor = ((System.Windows.Forms.AnchorStyles)((System.Windows.Forms.AnchorStyles.Top | System.Windows.Forms.AnchorStyles.Right)));
        this.buttonRefresh.Location = new System.Drawing.Point(612, 11);
        this.buttonRefresh.Name = "buttonRefresh";
        this.buttonRefresh.Size = new System.Drawing.Size(96, 25);
        this.buttonRefresh.TabIndex = 1;
        this.buttonRefresh.Text = "Refresh";
        this.buttonRefresh.UseVisualStyleBackColor = true;
        this.buttonRefresh.Click += new System.EventHandler(this.buttonRefresh_Click);
        //
        // listExpansions
        //
        this.listExpansions.Anchor = ((System.Windows.Forms.AnchorStyles)((((System.Windows.Forms.AnchorStyles.Top | System.Windows.Forms.AnchorStyles.Bottom)
                    | System.Windows.Forms.AnchorStyles.Left)
                    | System.Windows.Forms.AnchorStyles.Right)));
        this.listExpansions.Columns.AddRange(new System.Windows.Forms.ColumnHeader[] {
            this.columnName,
            this.columnVersion,
            this.columnState,
            this.columnDetails,
            this.columnSummary});
        this.listExpansions.FullRowSelect = true;
        this.listExpansions.HideSelection = false;
        this.listExpansions.Location = new System.Drawing.Point(12, 48);
        this.listExpansions.MultiSelect = false;
        this.listExpansions.Name = "listExpansions";
        this.listExpansions.Size = new System.Drawing.Size(696, 270);
        this.listExpansions.TabIndex = 2;
        this.listExpansions.UseCompatibleStateImageBehavior = false;
        this.listExpansions.View = System.Windows.Forms.View.Details;
        this.listExpansions.SelectedIndexChanged += new System.EventHandler(this.listExpansions_SelectedIndexChanged);
        //
        // columnName
        //
        this.columnName.Text = "Expansion";
        this.columnName.Width = 180;
        //
        // columnVersion
        //
        this.columnVersion.Text = "Version";
        this.columnVersion.Width = 100;
        //
        // columnState
        //
        this.columnState.Text = "State";
        this.columnState.Width = 120;
        //
        // columnDetails
        //
        this.columnDetails.Text = "Details";
        this.columnDetails.Width = 220;
        //
        // columnSummary
        //
        this.columnSummary.Text = "Summary";
        this.columnSummary.Width = 200;
        //
        // buttonInstall
        //
        this.buttonInstall.Anchor = ((System.Windows.Forms.AnchorStyles)((System.Windows.Forms.AnchorStyles.Bottom | System.Windows.Forms.AnchorStyles.Left)));
        this.buttonInstall.Location = new System.Drawing.Point(12, 382);
        this.buttonInstall.Name = "buttonInstall";
        this.buttonInstall.Size = new System.Drawing.Size(96, 27);
        this.buttonInstall.TabIndex = 4;
        this.buttonInstall.Text = "Install";
        this.buttonInstall.UseVisualStyleBackColor = true;
        this.buttonInstall.Click += new System.EventHandler(this.buttonInstall_Click);
        //
        // buttonRemove
        //
        this.buttonRemove.Anchor = ((System.Windows.Forms.AnchorStyles)((System.Windows.Forms.AnchorStyles.Bottom | System.Windows.Forms.AnchorStyles.Left)));
        this.buttonRemove.Location = new System.Drawing.Point(114, 382);
        this.buttonRemove.Name = "buttonRemove";
        this.buttonRemove.Size = new System.Drawing.Size(96, 27);
        this.buttonRemove.TabIndex = 5;
        this.buttonRemove.Text = "Remove";
        this.buttonRemove.UseVisualStyleBackColor = true;
        this.buttonRemove.Click += new System.EventHandler(this.buttonRemove_Click);
        //
        // buttonClose
        //
        this.buttonClose.Anchor = ((System.Windows.Forms.AnchorStyles)((System.Windows.Forms.AnchorStyles.Bottom | System.Windows.Forms.AnchorStyles.Right)));
        this.buttonClose.DialogResult = System.Windows.Forms.DialogResult.OK;
        this.buttonClose.Location = new System.Drawing.Point(612, 382);
        this.buttonClose.Name = "buttonClose";
        this.buttonClose.Size = new System.Drawing.Size(96, 27);
        this.buttonClose.TabIndex = 6;
        this.buttonClose.Text = "Close";
        this.buttonClose.UseVisualStyleBackColor = true;
        this.buttonClose.Click += new System.EventHandler(this.buttonClose_Click);
        //
        // progressBar
        //
        this.progressBar.Anchor = ((System.Windows.Forms.AnchorStyles)(((System.Windows.Forms.AnchorStyles.Bottom | System.Windows.Forms.AnchorStyles.Left)
                    | System.Windows.Forms.AnchorStyles.Right)));
        this.progressBar.Location = new System.Drawing.Point(12, 330);
        this.progressBar.Name = "progressBar";
        this.progressBar.Size = new System.Drawing.Size(696, 8);
        this.progressBar.Style = System.Windows.Forms.ProgressBarStyle.Marquee;
        this.progressBar.TabIndex = 7;
        this.progressBar.Visible = false;
        //
        // labelStatus
        //
        this.labelStatus.Anchor = ((System.Windows.Forms.AnchorStyles)(((System.Windows.Forms.AnchorStyles.Bottom | System.Windows.Forms.AnchorStyles.Left)
                    | System.Windows.Forms.AnchorStyles.Right)));
        this.labelStatus.Location = new System.Drawing.Point(12, 344);
        this.labelStatus.Name = "labelStatus";
        this.labelStatus.Size = new System.Drawing.Size(696, 32);
        this.labelStatus.TabIndex = 3;
        this.labelStatus.Text = "Ready.";
        this.labelStatus.TextAlign = System.Drawing.ContentAlignment.MiddleLeft;
        //
        // ExpansionManagerForm
        //
        this.AcceptButton = this.buttonClose;
        this.AutoScaleDimensions = new System.Drawing.SizeF(7F, 15F);
        this.AutoScaleMode = System.Windows.Forms.AutoScaleMode.Font;
        this.ClientSize = new System.Drawing.Size(720, 421);
        this.Controls.Add(this.labelStatus);
        this.Controls.Add(this.progressBar);
        this.Controls.Add(this.buttonClose);
        this.Controls.Add(this.buttonRemove);
        this.Controls.Add(this.buttonInstall);
        this.Controls.Add(this.listExpansions);
        this.Controls.Add(this.buttonRefresh);
        this.Controls.Add(this.comboGames);
        this.MinimumSize = new System.Drawing.Size(600, 400);
        this.Name = "ExpansionManagerForm";
        this.StartPosition = System.Windows.Forms.FormStartPosition.CenterScreen;
        this.Text = "Expansion Manager";
        this.ResumeLayout(false);

    }

    #endregion

    private System.Windows.Forms.ComboBox comboGames;
    private System.Windows.Forms.Button buttonRefresh;
    private System.Windows.Forms.ListView listExpansions;
    private System.Windows.Forms.ColumnHeader columnName;
    private System.Windows.Forms.ColumnHeader columnVersion;
    private System.Windows.Forms.ColumnHeader columnState;
    private System.Windows.Forms.ColumnHeader columnDetails;
    private System.Windows.Forms.ColumnHeader columnSummary;
    private System.Windows.Forms.Button buttonInstall;
    private System.Windows.Forms.Button buttonRemove;
    private System.Windows.Forms.Button buttonClose;
    private System.Windows.Forms.ProgressBar progressBar;
    private System.Windows.Forms.Label labelStatus;
}
