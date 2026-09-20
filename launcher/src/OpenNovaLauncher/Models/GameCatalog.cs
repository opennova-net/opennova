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

    public static IReadOnlyList<GameDefinition> SupportedGames => _supportedGames;

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
    }
}
