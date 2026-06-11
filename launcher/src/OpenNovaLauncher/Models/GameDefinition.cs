namespace OpenNova.Launcher.Models;

public sealed record GameDefinition(string Slug, string DisplayName, string ExecutableName)
{
    public string ProcessName => System.IO.Path.GetFileNameWithoutExtension(ExecutableName);
}
