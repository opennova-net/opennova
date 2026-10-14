using System.Security.Principal;

namespace OpenNova.Switcher;

internal static class Program
{
    internal const string ProductName = "OpenNova Switcher";

    [STAThread]
    private static void Main()
    {
        ApplicationConfiguration.Initialize();

        // One switcher per computer, across sessions: two would race on the hosts file.
        using var instanceMutex = new Mutex(initiallyOwned: true, @"Global\OpenNovaSwitcher", out var createdNew);
        if (!createdNew)
        {
            MessageBox.Show(
                "OpenNova Switcher is already running on this computer. Its icon is in the notification area " +
                "(or in another signed-in user's).",
                ProductName,
                MessageBoxButtons.OK,
                MessageBoxIcon.Information);
            return;
        }

        // app.manifest asks for elevation; this covers a start that bypassed it.
        if (!IsAdministrator())
        {
            MessageBox.Show(
                "OpenNova Switcher edits the Windows hosts file and must run as administrator.",
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
