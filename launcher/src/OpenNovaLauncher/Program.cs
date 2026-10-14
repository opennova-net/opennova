using System.Security.Principal;

namespace OpenNova.Launcher;

internal static class Program
{
    internal const string ProductName = "OpenNova Launcher";

    [STAThread]
    private static void Main()
    {
        ApplicationConfiguration.Initialize();

        // One tray icon per session: two would race on the hosts file.
        using var instanceMutex = new Mutex(initiallyOwned: true, @"Local\OpenNovaLauncher", out var createdNew);
        if (!createdNew)
        {
            MessageBox.Show(
                "OpenNova Launcher is already running. Its icon is in the notification area.",
                ProductName,
                MessageBoxButtons.OK,
                MessageBoxIcon.Information);
            return;
        }

        // app.manifest asks for elevation; this covers a start that bypassed it.
        if (!IsAdministrator())
        {
            MessageBox.Show(
                "OpenNova Launcher edits the Windows hosts file and must run as administrator.",
                ProductName,
                MessageBoxButtons.OK,
                MessageBoxIcon.Error);
            return;
        }

        Application.Run(new TrayApplicationContext());
    }

    private static bool IsAdministrator()
    {
        using var identity = WindowsIdentity.GetCurrent();
        return new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator);
    }
}
