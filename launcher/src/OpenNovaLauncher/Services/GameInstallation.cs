using OpenNova.Launcher.Models;

namespace OpenNova.Launcher.Services;

public sealed record GameInstallation(
    GameDefinition Definition,
    string DirectoryPath,
    string ExecutablePath);
