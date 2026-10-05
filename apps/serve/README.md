# opennova-serve

The headless game server (ADR 0051): the game's Serve Only host with no window and no
Godot, configured by a host file, retail's own dedicated-server format. It mounts a Joint
Operations install the way `opennova.exe` mounts it, boots the starting mission through
the engine's one host boot (`engine/runtime/inmatch/host_boot.h`, the same two phases the
game's hosts run), binds the retail LAN port range, answers LAN browsers and admits
joiners, retail clients included. Pre-1.0 and experimental.

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
| `--lan-port <n>` | The first port of the bind scan (default: the head of the retail LAN server range, `game.cfg` `mplanserverportmin`). The scan steps by one through the range. |
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
| `MaxPlayers` (not held to the screen's 64) | `MAX_PLAYERS` |
| `UseLineUpQueue`, `LineUpQueueSize` | none (`mpuselineupqueue`, `mplineupqueuesize`) |
| `MaxFFKills`, `TakeoverTime` | `MAX_FF_KILLS`, `TAKEOVER_TIME` |
| `TeamFF`, `FriendlyTag`, `FFWarning`, `TeamChoose`, `ClaymorePref`, `Tracers` (1 = on) | `TEAM_FF`, `FRIENDLY_TAG`, `FF_WARNING`, `TEAM_CHOOSE`, `CLAYMORE_PREF`, `TRACERS` |
| `MPHostSidePasswordA`, `MPHostSidePasswordB` (up to 16) | `BLUE_PW`, `RED_PW` |
| `Mission <file> <launch option>` | the `SELECTED_MISSIONS` table: one line per rotation entry |

`Mission` names a mission the install lists (a `.bms` in its archives, or a loose one in the
install folder); a name it does not list is skipped with a warning, and a file that names no
listed mission is refused. The last `Mission` line is the starting map. The server type is
always Serve Only (the server has no player of its own) and the published player cap gains
the server's own slot. The rest of the session (PunkBuster, LAN only, spectators, AI,
time-of-day continuity) keeps the host screen's defaults for now; `game.cfg` support comes
with a later slice.

The server also reads, from the install as the game does: the loose `score.ini` (the
session's score table), `gametext.bin` (the default game name and the "Server" chat
lines) and `charattr.def` (the class attributes the medic heal and knife reach read).

## The working directory

Retail's dedicated server keeps `game.cfg` in its working directory and marks a running
server with `activesrvr.txt` there; `opennova-serve` will do the same in a later slice
(ADR 0051 decision 2) and never writes into `--resource-dir` unless it runs from there.
Today it writes nothing.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | `--help`, or stopped by Ctrl+C. |
| 1 | The server did not start: the install did not mount, the host file did not open or named no listed mission, no port of the bind scan was free, or the mission did not boot. |
| 2 | A usage error. |
| 3 | The session ended by itself. Until the map rotation's round-end advance lands (D-NET-331), a server plays one round and exits with 3. |
