using System;
using System.Collections.Generic;

namespace OpenNova.Launcher.Models;

public sealed record ExpansionDescriptor(
    string Slug,
    string DisplayName,
    string Version,
    string? Summary,
    string PackageType,
    string InstallTarget,
    IReadOnlyList<ExpansionFileDescriptor> Files);

public sealed record ExpansionFileDescriptor(
    string DownloadUrl,
    string Sha256,
    long? SizeBytes,
    string FileType);

public sealed record InstalledExpansion(
    string Slug,
    string Version,
    string InstallTarget,
    DateTime InstalledAtUtc,
    IReadOnlyList<string> MaterializedFiles);

public sealed record GameCatalogEntry(
    GameDefinition Game,
    IReadOnlyList<ExpansionDescriptor> Expansions);

public enum ExpansionState
{
    NotInstalled,
    Installed,
    UpdateAvailable,
    NeedsGameDirectory,
}

public sealed record ExpansionStatus(
    ExpansionDescriptor Descriptor,
    InstalledExpansion? Installed,
    ExpansionState State,
    string StatusMessage);

public sealed record GameExpansionStatus(
    GameDefinition Game,
    bool IsConfigured,
    IReadOnlyList<ExpansionStatus> Expansions);
