using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;

namespace OpenNova.Launcher.Models;

internal static class GameCatalog
{
    private static readonly GameDefinition[] DefaultGames =
    {
        new("jop_2_consumer", "Joint Operations: Typhoon Rising", "Jointops.exe"),
        new("dfx2_consumer", "Delta Force Xtreme 2", "dfx2.exe"),
    };

    private static IReadOnlyList<GameDefinition> _supportedGames = Array.AsReadOnly(DefaultGames);
    private static Dictionary<string, GameDefinition> _gamesBySlug = DefaultGames.ToDictionary(g => g.Slug, StringComparer.OrdinalIgnoreCase);
    private static Dictionary<string, GameDefinition> _gamesByProcessName = DefaultGames.ToDictionary(g => g.ProcessName, StringComparer.OrdinalIgnoreCase);

    public static IReadOnlyList<GameDefinition> SupportedGames => _supportedGames;

    public static bool TryGetBySlug(string slug, out GameDefinition? definition) => _gamesBySlug.TryGetValue(slug, out definition);

    public static bool TryGetByProcessName(string processName, out GameDefinition? definition) => _gamesByProcessName.TryGetValue(processName, out definition);

    public static void ReplaceGames(IEnumerable<GameDefinition> games)
    {
        if (games == null)
        {
            return;
        }

        var list = games.ToList();
        if (list.Count == 0)
        {
            return;
        }

        _supportedGames = new ReadOnlyCollection<GameDefinition>(list);
        _gamesBySlug = list.ToDictionary(g => g.Slug, StringComparer.OrdinalIgnoreCase);
        _gamesByProcessName = list.ToDictionary(g => g.ProcessName, StringComparer.OrdinalIgnoreCase);
    }
}
