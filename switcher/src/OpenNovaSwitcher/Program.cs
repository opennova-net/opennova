using System.Security.Principal;

namespace OpenNova.Switcher;

internal static class Program
{
    internal const string ProductName = "OpenNova Switcher";

    [STAThread]
    private static void Main()
    {
        ApplicationConfiguration.Initialize();

        // app.manifest asks for elevation; this refuses a start that bypassed it (a
        // RunAsInvoker compatibility layer, or UAC turned off for a standard user).
        // It comes before anything else: an unelevated process cannot open the mutex
        // an elevated switcher holds, and must not hold one an elevated start would see.
        if (!IsAdministrator())
        {
            MessageBox.Show(
                "OpenNova Switcher edits the Windows hosts file and must run as administrator. " +
                "Right-click opennova-switcher.exe and choose Run as administrator.",
                ProductName,
                MessageBoxButtons.OK,
                MessageBoxIcon.Error);
            return;
        }

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

        Application.Run(new TrayApplicationContext());
    }

    private static bool IsAdministrator()
    {
        using var identity = WindowsIdentity.GetCurrent();
        return new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator);
    }
}
