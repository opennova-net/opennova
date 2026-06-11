using System.Drawing;
using System.Security.Principal;
using System.Threading;
using System.Windows.Forms;

namespace OpenNova.Launcher;

internal static class Program
{
    /// <summary>
    ///  The main entry point for the application.
    /// </summary>
    [STAThread]
    static void Main()
    {
        using var instanceMutex = new Mutex(initiallyOwned: true, "OpenNovaLauncher", out var createdNew);
        if (!createdNew)
        {
            return;
        }

        // app.manifest requests requireAdministrator; this is a belt-and-braces
        // check for hosts where the manifest was bypassed.
        if (!IsAdministrator())
        {
            MessageBox.Show(
                "OpenNova Launcher manages Windows hosts entries and must run as administrator.",
                "OpenNova Launcher",
                MessageBoxButtons.OK,
                MessageBoxIcon.Error);
            return;
        }

        Application.SetDefaultFont(new Font("Segoe UI", 9F));
        ApplicationConfiguration.Initialize();
        Application.Run(new TrayApplicationContext());
    }

    private static bool IsAdministrator()
    {
        using var identity = WindowsIdentity.GetCurrent();
        return new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator);
    }
}
