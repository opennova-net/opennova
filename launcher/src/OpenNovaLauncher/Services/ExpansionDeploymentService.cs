using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Net.Http;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using OpenNova.Launcher.Models;

namespace OpenNova.Launcher.Services;

public sealed class ExpansionDeploymentService
{
    private static readonly HttpClient HttpClient = new();

    static ExpansionDeploymentService()
    {
        HttpClient.Timeout = TimeSpan.FromSeconds(30);
    }

    public async Task<InstalledExpansion> InstallAsync(
        GameInstallation installation,
        ExpansionDescriptor expansion,
        IProgress<string>? progress,
        CancellationToken cancellationToken)
    {
        if (!string.Equals(expansion.PackageType, "zip", StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidOperationException($"Unsupported expansion package type '{expansion.PackageType}'.");
        }

        if (expansion.Files == null || expansion.Files.Count == 0)
        {
            throw new InvalidOperationException("Expansion does not define any files to download.");
        }

        var downloadPaths = new List<string>();
        var tempRoot = Path.Combine(Path.GetTempPath(), $"onlauncher-exp-{Guid.NewGuid():N}");
        Directory.CreateDirectory(tempRoot);

        try
        {
            var fileIndex = 0;
            foreach (var file in expansion.Files)
            {
                cancellationToken.ThrowIfCancellationRequested();
                fileIndex++;
                progress?.Report($"Downloading {expansion.DisplayName} ({fileIndex}/{expansion.Files.Count})...");

                var downloaded = await DownloadFileAsync(file, cancellationToken);
                downloadPaths.Add(downloaded);
            }

            var extractRoot = Path.Combine(tempRoot, "extracted");
            Directory.CreateDirectory(extractRoot);

            var archiveIndex = 0;
            foreach (var archive in downloadPaths)
            {
                cancellationToken.ThrowIfCancellationRequested();
                archiveIndex++;
                progress?.Report($"Extracting archive {archiveIndex}/{downloadPaths.Count}...");

                ZipFile.ExtractToDirectory(archive, extractRoot, overwriteFiles: true);
            }

            var targetRoot = ResolveInstallRoot(installation.DirectoryPath, expansion.InstallTarget);
            if (Directory.Exists(targetRoot))
            {
                progress?.Report("Removing previous expansion content...");
                Directory.Delete(targetRoot, recursive: true);
            }

            Directory.CreateDirectory(Path.GetDirectoryName(targetRoot) ?? installation.DirectoryPath);

            progress?.Report("Finalizing expansion deployment...");

            var extractRootFull = Path.GetFullPath(extractRoot);
            var targetRootFull = Path.GetFullPath(targetRoot);
            var extractVolume = Path.GetPathRoot(extractRootFull);
            var targetVolume = Path.GetPathRoot(targetRootFull);

            if (string.Equals(extractVolume, targetVolume, StringComparison.OrdinalIgnoreCase))
            {
                Directory.Move(extractRootFull, targetRootFull);
            }
            else
            {
                CopyDirectory(extractRootFull, targetRootFull);
                TryDelete(extractRootFull, recursive: true);
            }

            var materializedFiles = Directory.Exists(targetRoot)
                ? Directory.EnumerateFiles(targetRoot, "*", SearchOption.AllDirectories).Select(Path.GetFullPath).ToList()
                : new List<string>();

            return new InstalledExpansion(
                expansion.Slug,
                expansion.Version,
                expansion.InstallTarget,
                DateTime.UtcNow,
                materializedFiles);
        }
        finally
        {
            foreach (var download in downloadPaths)
            {
                TryDelete(download);
            }

            TryDelete(tempRoot, recursive: true);
        }
    }

    public Task RemoveAsync(
        GameInstallation installation,
        InstalledExpansion expansion,
        IProgress<string>? progress,
        CancellationToken cancellationToken)
    {
        progress?.Report($"Removing {expansion.Slug}...");

        var targetRoot = ResolveInstallRoot(installation.DirectoryPath, expansion.InstallTarget);

        foreach (var file in expansion.MaterializedFiles)
        {
            cancellationToken.ThrowIfCancellationRequested();
            TryDelete(file);
        }

        if (Directory.Exists(targetRoot))
        {
            TryDelete(targetRoot, recursive: true);
        }

        return Task.CompletedTask;
    }

    private static async Task<string> DownloadFileAsync(ExpansionFileDescriptor descriptor, CancellationToken cancellationToken)
    {
        var uri = new Uri(descriptor.DownloadUrl, UriKind.Absolute);
        var targetPath = Path.Combine(Path.GetTempPath(), $"onlauncher-exp-{Guid.NewGuid():N}-{Path.GetFileName(uri.LocalPath)}");

        using (var response = await HttpClient.GetAsync(uri, HttpCompletionOption.ResponseHeadersRead, cancellationToken))
        {
            response.EnsureSuccessStatusCode();
            await using var source = await response.Content.ReadAsStreamAsync(cancellationToken);
            await using var destination = File.Create(targetPath);
            await source.CopyToAsync(destination, cancellationToken);
        }

        await VerifySha256Async(targetPath, descriptor.Sha256, cancellationToken);
        return targetPath;
    }

    private static async Task VerifySha256Async(string filePath, string expectedHash, CancellationToken cancellationToken)
    {
        await using var stream = File.OpenRead(filePath);
        using var sha = SHA256.Create();
        var hash = await sha.ComputeHashAsync(stream, cancellationToken);
        var builder = new StringBuilder(hash.Length * 2);
        foreach (var b in hash)
        {
            builder.AppendFormat("{0:x2}", b);
        }

        var computed = builder.ToString();
        if (!computed.Equals(expectedHash, StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidOperationException($"Checksum mismatch for {Path.GetFileName(filePath)}");
        }
    }

    private static string ResolveInstallRoot(string gameDirectory, string installTarget)
    {
        var relative = installTarget?.Replace('/', Path.DirectorySeparatorChar) ?? string.Empty;
        relative = relative.TrimStart(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        return Path.Combine(gameDirectory, relative);
    }

    private static void CopyDirectory(string source, string destination)
    {
        var stack = new Stack<(string Source, string Destination)>();
        stack.Push((source, destination));

        while (stack.Count > 0)
        {
            var current = stack.Pop();

            Directory.CreateDirectory(current.Destination);

            foreach (var file in Directory.GetFiles(current.Source))
            {
                var targetFile = Path.Combine(current.Destination, Path.GetFileName(file));
                File.Copy(file, targetFile, overwrite: true);
            }

            foreach (var directory in Directory.GetDirectories(current.Source))
            {
                var targetDirectory = Path.Combine(current.Destination, Path.GetFileName(directory));
                stack.Push((directory, targetDirectory));
            }
        }
    }

    private static void TryDelete(string? path, bool recursive = false)
    {
        if (string.IsNullOrWhiteSpace(path))
        {
            return;
        }

        try
        {
            if (Directory.Exists(path))
            {
                Directory.Delete(path, recursive);
            }
            else if (File.Exists(path))
            {
                File.Delete(path);
            }
        }
        catch
        {
            // best-effort cleanup
        }
    }
}
