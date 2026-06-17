namespace OpenNova.Launcher.Forms
{
    partial class ExpansionLaunchDialog
    {
        /// <summary>
        /// Required designer variable.
        /// </summary>
        private System.ComponentModel.IContainer components = null;

        /// <summary>
        /// Clean up any resources being used.
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

        /// <summary>
        /// Required method for Designer support - do not modify
        /// the contents of this method with the code editor.
        /// </summary>
        private void InitializeComponent()
        {
            components = new System.ComponentModel.Container();
            headerLabel = new System.Windows.Forms.Label();
            rootLayout = new System.Windows.Forms.TableLayoutPanel();
            optionsList = new System.Windows.Forms.ListView();
            columnExpansion = new System.Windows.Forms.ColumnHeader();
            columnVersion = new System.Windows.Forms.ColumnHeader();
            columnSummary = new System.Windows.Forms.ColumnHeader();
            footerLayout = new System.Windows.Forms.TableLayoutPanel();
            optionsFlow = new System.Windows.Forms.FlowLayoutPanel();
            windowedCheckBox = new System.Windows.Forms.CheckBox();
            allowManyInstancesCheckBox = new System.Windows.Forms.CheckBox();
            useRealNovaWorldCheckBox = new System.Windows.Forms.CheckBox();
            buttonsFlow = new System.Windows.Forms.FlowLayoutPanel();
            cancelButton = new System.Windows.Forms.Button();
            launchButton = new System.Windows.Forms.Button();
            rootLayout.SuspendLayout();
            footerLayout.SuspendLayout();
            optionsFlow.SuspendLayout();
            buttonsFlow.SuspendLayout();
            SuspendLayout();
            //
            // headerLabel
            //
            headerLabel.AutoSize = true;
            headerLabel.Dock = System.Windows.Forms.DockStyle.Fill;
            headerLabel.Font = new System.Drawing.Font("Segoe UI", 9F, System.Drawing.FontStyle.Regular, System.Drawing.GraphicsUnit.Point);
            headerLabel.Margin = new System.Windows.Forms.Padding(12, 12, 12, 8);
            headerLabel.Name = "headerLabel";
            headerLabel.Size = new System.Drawing.Size(576, 32);
            headerLabel.TabIndex = 0;
            headerLabel.Text = "Choose which expansion to launch (or launch without one).";
            //
            // rootLayout
            //
            rootLayout.ColumnCount = 1;
            rootLayout.ColumnStyles.Add(new System.Windows.Forms.ColumnStyle(System.Windows.Forms.SizeType.Percent, 100F));
            rootLayout.Controls.Add(headerLabel, 0, 0);
            rootLayout.Controls.Add(optionsList, 0, 1);
            rootLayout.Controls.Add(footerLayout, 0, 2);
            rootLayout.Dock = System.Windows.Forms.DockStyle.Fill;
            rootLayout.Location = new System.Drawing.Point(0, 0);
            rootLayout.Name = "rootLayout";
            rootLayout.RowCount = 3;
            rootLayout.RowStyles.Add(new System.Windows.Forms.RowStyle());
            rootLayout.RowStyles.Add(new System.Windows.Forms.RowStyle(System.Windows.Forms.SizeType.Percent, 100F));
            rootLayout.RowStyles.Add(new System.Windows.Forms.RowStyle());
            rootLayout.Size = new System.Drawing.Size(600, 360);
            rootLayout.TabIndex = 0;
            //
            // optionsList
            //
            optionsList.Columns.AddRange(new System.Windows.Forms.ColumnHeader[] { columnExpansion, columnVersion, columnSummary });
            optionsList.Dock = System.Windows.Forms.DockStyle.Fill;
            optionsList.FullRowSelect = true;
            optionsList.HeaderStyle = System.Windows.Forms.ColumnHeaderStyle.Nonclickable;
            optionsList.HideSelection = false;
            optionsList.Location = new System.Drawing.Point(12, 52);
            optionsList.Margin = new System.Windows.Forms.Padding(12, 0, 12, 0);
            optionsList.MultiSelect = false;
            optionsList.Name = "optionsList";
            optionsList.Size = new System.Drawing.Size(576, 229);
            optionsList.TabIndex = 1;
            optionsList.UseCompatibleStateImageBehavior = false;
            optionsList.View = System.Windows.Forms.View.Details;
            //
            // columnExpansion
            //
            columnExpansion.Text = "Expansion";
            columnExpansion.Width = 260;
            //
            // columnVersion
            //
            columnVersion.Text = "Version";
            columnVersion.Width = 100;
            //
            // columnSummary
            //
            columnSummary.Text = "Summary";
            columnSummary.Width = 180;
            //
            // footerLayout
            //
            footerLayout.AutoSize = true;
            footerLayout.AutoSizeMode = System.Windows.Forms.AutoSizeMode.GrowAndShrink;
            footerLayout.ColumnCount = 2;
            footerLayout.ColumnStyles.Add(new System.Windows.Forms.ColumnStyle(System.Windows.Forms.SizeType.Percent, 100F));
            footerLayout.ColumnStyles.Add(new System.Windows.Forms.ColumnStyle(System.Windows.Forms.SizeType.AutoSize));
            footerLayout.Controls.Add(optionsFlow, 0, 0);
            footerLayout.Controls.Add(buttonsFlow, 1, 0);
            footerLayout.Dock = System.Windows.Forms.DockStyle.Fill;
            footerLayout.Location = new System.Drawing.Point(12, 281);
            footerLayout.Margin = new System.Windows.Forms.Padding(12, 0, 12, 16);
            footerLayout.Name = "footerLayout";
            footerLayout.RowCount = 1;
            footerLayout.RowStyles.Add(new System.Windows.Forms.RowStyle());
            footerLayout.Size = new System.Drawing.Size(576, 63);
            footerLayout.TabIndex = 2;
            //
            // optionsFlow
            //
            optionsFlow.AutoSize = true;
            optionsFlow.AutoSizeMode = System.Windows.Forms.AutoSizeMode.GrowAndShrink;
            optionsFlow.Controls.Add(windowedCheckBox);
            optionsFlow.Controls.Add(allowManyInstancesCheckBox);
            optionsFlow.Controls.Add(useRealNovaWorldCheckBox);
            optionsFlow.Dock = System.Windows.Forms.DockStyle.Fill;
            optionsFlow.FlowDirection = System.Windows.Forms.FlowDirection.LeftToRight;
            optionsFlow.Location = new System.Drawing.Point(0, 0);
            optionsFlow.Margin = new System.Windows.Forms.Padding(0);
            optionsFlow.Name = "optionsFlow";
            optionsFlow.Padding = new System.Windows.Forms.Padding(0, 4, 0, 0);
            optionsFlow.Size = new System.Drawing.Size(430, 63);
            optionsFlow.TabIndex = 0;
            optionsFlow.WrapContents = true;
            //
            // windowedCheckBox
            //
            windowedCheckBox.AutoSize = true;
            windowedCheckBox.Location = new System.Drawing.Point(3, 7);
            windowedCheckBox.Margin = new System.Windows.Forms.Padding(3, 3, 12, 3);
            windowedCheckBox.Name = "windowedCheckBox";
            windowedCheckBox.Size = new System.Drawing.Size(156, 19);
            windowedCheckBox.TabIndex = 0;
            windowedCheckBox.Text = "Launch in windowed mode";
            windowedCheckBox.UseVisualStyleBackColor = true;
            //
            // allowManyInstancesCheckBox
            //
            allowManyInstancesCheckBox.AutoSize = true;
            allowManyInstancesCheckBox.Location = new System.Drawing.Point(174, 7);
            allowManyInstancesCheckBox.Margin = new System.Windows.Forms.Padding(3, 3, 12, 3);
            allowManyInstancesCheckBox.Name = "allowManyInstancesCheckBox";
            allowManyInstancesCheckBox.Size = new System.Drawing.Size(149, 19);
            allowManyInstancesCheckBox.TabIndex = 1;
            allowManyInstancesCheckBox.Text = "Allow many instances";
            allowManyInstancesCheckBox.UseVisualStyleBackColor = true;
            //
            // useRealNovaWorldCheckBox
            //
            useRealNovaWorldCheckBox.AutoSize = true;
            useRealNovaWorldCheckBox.Margin = new System.Windows.Forms.Padding(3, 3, 12, 3);
            useRealNovaWorldCheckBox.Name = "useRealNovaWorldCheckBox";
            useRealNovaWorldCheckBox.TabIndex = 2;
            useRealNovaWorldCheckBox.Text = "Use the original NovaWorld (skip OpenNova redirect)";
            useRealNovaWorldCheckBox.UseVisualStyleBackColor = true;
            //
            // buttonsFlow
            //
            buttonsFlow.AutoSize = true;
            buttonsFlow.AutoSizeMode = System.Windows.Forms.AutoSizeMode.GrowAndShrink;
            buttonsFlow.Controls.Add(cancelButton);
            buttonsFlow.Controls.Add(launchButton);
            buttonsFlow.FlowDirection = System.Windows.Forms.FlowDirection.LeftToRight;
            buttonsFlow.Location = new System.Drawing.Point(430, 0);
            buttonsFlow.Margin = new System.Windows.Forms.Padding(0);
            buttonsFlow.Name = "buttonsFlow";
            buttonsFlow.Size = new System.Drawing.Size(146, 63);
            buttonsFlow.TabIndex = 1;
            buttonsFlow.WrapContents = false;
            //
            // cancelButton
            //
            cancelButton.DialogResult = System.Windows.Forms.DialogResult.Cancel;
            cancelButton.Location = new System.Drawing.Point(3, 15);
            cancelButton.Margin = new System.Windows.Forms.Padding(3, 15, 8, 0);
            cancelButton.Name = "cancelButton";
            cancelButton.Size = new System.Drawing.Size(90, 30);
            cancelButton.TabIndex = 0;
            cancelButton.Text = "Cancel";
            cancelButton.UseVisualStyleBackColor = true;
            //
            // launchButton
            //
            launchButton.DialogResult = System.Windows.Forms.DialogResult.OK;
            launchButton.Location = new System.Drawing.Point(101, 15);
            launchButton.Margin = new System.Windows.Forms.Padding(0, 15, 0, 0);
            launchButton.Name = "launchButton";
            launchButton.Size = new System.Drawing.Size(90, 30);
            launchButton.TabIndex = 1;
            launchButton.Text = "Launch";
            launchButton.UseVisualStyleBackColor = true;
            launchButton.Click += LaunchButton_Click;
            //
            // ExpansionLaunchDialog
            //
            AutoScaleDimensions = new System.Drawing.SizeF(7F, 15F);
            AutoScaleMode = System.Windows.Forms.AutoScaleMode.Font;
            CancelButton = cancelButton;
            ClientSize = new System.Drawing.Size(600, 360);
            Controls.Add(rootLayout);
            FormBorderStyle = System.Windows.Forms.FormBorderStyle.FixedDialog;
            MaximizeBox = false;
            MinimizeBox = false;
            Name = "ExpansionLaunchDialog";
            ShowInTaskbar = false;
            StartPosition = System.Windows.Forms.FormStartPosition.CenterScreen;
            AcceptButton = launchButton;
            rootLayout.ResumeLayout(false);
            rootLayout.PerformLayout();
            footerLayout.ResumeLayout(false);
            footerLayout.PerformLayout();
            optionsFlow.ResumeLayout(false);
            optionsFlow.PerformLayout();
            buttonsFlow.ResumeLayout(false);
            ResumeLayout(false);
        }

        #endregion

        private System.Windows.Forms.TableLayoutPanel rootLayout;
        private System.Windows.Forms.Label headerLabel;
        private System.Windows.Forms.ListView optionsList;
        private System.Windows.Forms.ColumnHeader columnExpansion;
        private System.Windows.Forms.ColumnHeader columnVersion;
        private System.Windows.Forms.ColumnHeader columnSummary;
        private System.Windows.Forms.TableLayoutPanel footerLayout;
        private System.Windows.Forms.FlowLayoutPanel optionsFlow;
        private System.Windows.Forms.CheckBox windowedCheckBox;
        private System.Windows.Forms.CheckBox allowManyInstancesCheckBox;
        private System.Windows.Forms.CheckBox useRealNovaWorldCheckBox;
        private System.Windows.Forms.FlowLayoutPanel buttonsFlow;
        private System.Windows.Forms.Button cancelButton;
        private System.Windows.Forms.Button launchButton;
    }
}
