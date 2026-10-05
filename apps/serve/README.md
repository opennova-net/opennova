# opennova-serve

The headless game server (ADR 0051): the game's Serve Only host with no window and no
Godot, configured by a host file, retail's own dedicated-server format, over the
`game.cfg` in the directory it runs from. It mounts a Joint Operations install the way
`opennova.exe` mounts it, boots the starting mission through the engine's one host boot
(`engine/runtime/inmatch/host_boot.h`, the same two phases the game's hosts run), binds
`game.cfg`'s LAN port range, answers LAN browsers and admits joiners, retail clients
included. Pre-1.0 and experimental.

## Usage

```text
opennova-serve --resource-dir <game dir> /HOST <host file> [/exp <name>] [/d]
               [/game <code>] [--loose-root] [--lan-port <n>] [--log-debug]
```

| Option | Meaning |
|---|---|
| `--resource-dir <dir>` | The install to serve from (its `.pff` set), as `opennova.exe` takes it. Required. |
| `/HOST <file>` | The host file (below). Required. |
| `/exp <name>` | Mount that expansion (for example `revx02`). |
| `/d` | Prefer loose files over the archives, as the game's `/d`. |
| `/game <code>` | The data's game code (its SCR keying), as the game's `/game`. |
| `--loose-root` | Mount a directory that holds no game archives as loose files. |
| `--lan-port <n>` | The first port of the bind scan (default: `game.cfg`'s `mplanserverportmin`, 32768 in a stock file). The scan steps by `mplanserverportdelta` up to `mplanserverportmax` and wraps. |
| `--log-debug` | Print the engine's debug log lines. |

`--help` prints the usage. Ctrl+C stops the server: it sends every joiner the round
reset and the session's STOP goodbye before it closes the socket.

## The host file

`/HOST <file>` names a text file of `<Key> <value>` lines, the text form of the game's host
screen that retail's dedicated server reads (`ServerConfig_ApplyHostSetting`). Keys match
without regard to case; a value with spaces is quoted; `//` or `;` starts a comment. The
`Key = value` form of `game.cfg` does not work here. A key the server does not know changes
nothing and is logged. [`example.host`](example.host) documents every key with its default.

| Key | Host-screen control |
|---|---|
| `GameName` (up to 31 characters) | `GAME_NAME` |
| `MPHostGamePassword` (up to 16) | `SERVER_PASSWORD` |
| `ServerMessage` (up to 127) | `SERVER_MESSAGE` |
| `GameLocation` (3 characters) | `GAME_LOCATION` |
| `ConnectionSpeed` | `CONNECTIONSPEED` |
| `GameType` | none (`game.cfg` `mp_gametype`); the session plays the starting map's mode |
| `Replay`, `Delay`, `Respawn`, `Time_Limit` | `REPLAY`, `DELAY`, `RESPAWN`, `TIME` |
| `KillLimit`, `MaxScore` | `KILL_LIMIT`, `MAX_SCORE` |
| `MaxPlayers` (not held to the screen's 64; the session takes 1 to 65) | `MAX_PLAYERS` |
| `UseLineUpQueue`, `LineUpQueueSize` | none (`mpuselineupqueue`, `mplineupqueuesize`) |
| `MaxFFKills`, `TakeoverTime` | `MAX_FF_KILLS`, `TAKEOVER_TIME` |
| `TeamFF`, `FriendlyTag`, `FFWarning`, `TeamChoose`, `ClaymorePref`, `Tracers` (1 = on) | `TEAM_FF`, `FRIENDLY_TAG`, `FF_WARNING`, `TEAM_CHOOSE`, `CLAYMORE_PREF`, `TRACERS` |
| `MPHostSidePasswordA`, `MPHostSidePasswordB` (up to 16) | `BLUE_PW`, `RED_PW` |
| `Mission <file> <launch option>` | the `SELECTED_MISSIONS` table: one line per rotation entry |

`Mission` names a mission the install lists (a `.bms` in its archives, or a loose one in the
install folder); a name it does not list is skipped with a warning, and a file that names no
listed mission is refused. The last `Mission` line is the starting map. The server type is
always Serve Only (the server has no player of its own) and the published player cap gains
the server's own slot.

Each key writes the same setting `game.cfg` keeps, so a setting the host file leaves out
keeps its `game.cfg` value. The settings the host screen has no host-file key for come from
`game.cfg` alone: `mpnumspectatorsmax` and `mphostspectatorpassword` (spectators),
`mpnovaworldhostlanonly` (LAN only), `sv_punkbuster`, `enable_ai`, `mptodcontinuity`
(time-of-day continuity), and the rest of the session rules (`time_limit`, `koth_limit`,
`max_team_lives`, the vote, autobalance and ping settings, `lanmode`, ...).

## The map rotation

The server plays round after round, as a retail host does (D-NET-331). When a round's
post-round linger runs out (about 45 seconds), the server loads the next `Mission` line's
map inside the same session: every joiner stays connected and reloads into it. Before each
map change the server saves `game.cfg`, and the next map takes the session settings from the
`game.cfg` block again. After the last line the list starts over when `Replay` is 1; with
`Replay 0` the session ends there, every joiner receives the session's STOP goodbye, and the
server exits with 0. Because the last `Mission` line is the starting map, the second round
plays the first line's map.

A map whose launch option is set (the line's second value, `1`; the server clears it on a
co-op or non-team map) plays twice, as the two halves of Attack and Defend: the second half
swaps every player's team and the side passwords' teams, and the swap undoes itself at the
second half's end.

The server also reads, from the install as the game does: the loose `score.ini` (the
session's score table), `gametext.bin` (the default game name and the "Server" chat
lines) and `charattr.def` (the class attributes the medic heal and knife reach read).

## The working directory

Like retail's dedicated server, `opennova-serve` keeps its files in the directory it runs
from (the process's working directory; there is no option for it, so start the server from
the directory you want). It never writes into `--resource-dir` unless it runs from there.

1. It reads `game.cfg` there. A missing file means the game's defaults; a file whose
   `version` is not 29 is replaced by the defaults, as the game does. The install's
   `weapon.def` and `gametext.bin` are read with it (the `avail_wpn_*` lines and the default
   game name and message).
2. It applies the host file over those settings.
3. It writes `activesrvr.txt` ("This directory has a server running in it that did not exit
   cleanly or is running"), then saves `game.cfg` with `dedicated = 1` and the host file's
   settings in it.
4. It saves `game.cfg` again at every map change.
5. On a clean exit (Ctrl+C, or the session's end) it saves `game.cfg` again and deletes
   `activesrvr.txt`. A server that is killed or crashes leaves `activesrvr.txt` behind;
   nothing reads it.

A `game.cfg` with `mpreset = "1"` stops the server before it writes anything, with exit code
0, as the game exits at that read.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | `--help`, stopped by Ctrl+C, `game.cfg` sets `mpreset`, or the map rotation ran out (the end of the list with `Replay 0`). |
| 1 | The server did not start: the install did not mount, the host file did not open or named no listed mission, no port of the bind scan was free, or the mission did not boot. |
| 2 | A usage error. |
| 3 | The session ended otherwise: a later map that did not boot, or a mission exit that is not a round end. |
