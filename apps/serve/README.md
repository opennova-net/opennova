# opennova-serve

The headless game server (ADR 0051): the game's Serve Only host with no window and no
Godot, configured by a host file, retail's own dedicated-server format, over the
`game.cfg` in the directory it runs from. It mounts a Joint Operations install the way
`opennova.exe` mounts it, boots the starting mission through the engine's one host boot
(`engine/runtime/inmatch/host_boot.h`, the same two phases the game's hosts run), binds
`game.cfg`'s LAN port range (or, given a NovaWorld gate, its NovaWorld one, listing itself there),
answers LAN browsers and admits joiners, retail clients included, and takes retail's remote
admin when `game.cfg` names its port. Pre-1.0 and experimental.

## Usage

```text
opennova-serve --resource-dir <game dir> /HOST <host file> [/exp <name>] [/d]
               [/game <code>] [--loose-root] [--lan-port <n>] [--log-debug]
               [--master-host <gate>] [--master-gate-port <n>]
               [--credentials <file>] [--allow-public]
               [/PROFILE <path>] [/PUNTLOG | /PUNT.TXT] [/CHEATLOG]
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
| `--master-host <gate>` | The NovaWorld gate to list on (`127.0.0.1` for an `opennova-novaworld-server` on this machine). Without it the server serves LAN only (below). |
| `--master-gate-port <n>` | The gate's UDP port (default 7597). |
| `--credentials <file>` | `NOVAWORLD_USER=` and `NOVAWORLD_PASS=` lines: the account the listing logs in with to fetch its HOSTKEY, as the game's NovaWorld menu does. Without it the server hosts with no HOSTKEY, which an `opennova-novaworld-server` accepts. |
| `--allow-public` | Allow NovaLogic's NovaWorld (`novaworld.net` and every host under it), a live shared service. Loopback and any other host, an OpenNova service among them, need no flag. |
| `/PROFILE <path>` | Record every mission to a `.sph` server log (below). |
| `/PUNTLOG`, `/PUNT.TXT` | Log the players the server punts to `_PUNT.TXT` (below). |
| `/CHEATLOG` | Start `_CHEAT.TXT` (below). |

`--help` prints the usage. Ctrl+C, SIGTERM or closing the console window (Windows) stops
the server: it sends every joiner the round reset and the session's STOP goodbye, and a
listed server deregisters (ClientStopHosting and the goodbye), before it closes the socket.

## NovaWorld

With `--master-host`, `game.cfg`'s `networkconnecttype` picks the network, as retail's
dedicated server reads it: `1`, the NovaWorld screen's and the default (so also with no
`game.cfg`), lists the server on that gate; `2`, the LAN screen's, serves LAN only. Without
`--master-host` the server serves LAN whatever the file says, and says so on the console:
retail's default would host on NovaLogic's `gs.novaworld.net`, which OpenNova never contacts
unasked (D-NET-358).

A listed server binds `game.cfg`'s NovaWorld port range (`mpnovaworldportmin` / `max` /
`delta`, 32768..65535 in a stock file) instead of the LAN one, and the NovaWorld session runs
on that same socket: the service learns the server's address from the session's own
datagrams, so joiners reach the game wherever the server's address is. Before the mission
boots the server probes the gate, verifies its session (logging in and fetching the HOSTKEY
when `--credentials` names an account) and hosts; a listing that does not host stops the
start (exit code 1), as retail's does. While it serves, the listing carries the session's
name, message and rules, the starting map, the time left and every joiner, and the service's
commands (`PuntPlayer`, `SetServerName`, ...) run on the match, a changed name or message
saved to `game.cfg` as retail saves it, so the next map keeps it. LAN browsers still find the
server on the same socket.

Loopback and any host outside NovaLogic's domain, an OpenNova NovaWorld service wherever it
is deployed, can be listed on without a flag. A host that is `novaworld.net` or under it
(`gs.novaworld.net`, NovaLogic's live master) is refused, unresolved, unless `--allow-public`
is given; a live master is a shared service, so use it sparingly. Account names, passwords and
session tags are masked in the log.

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

The remote admin's files sit there too (below): `admin_log.txt`, which every launch starts
empty, `admin.cfg`, read once at the start, and the ban lists `banned.txt` and
`banlist.txt`, read when the session starts.

| File | Read | Written |
|---|---|---|
| `game.cfg` | at the start (twice, as the game) | at the start, at every map change, at the exit, and by every admin `SET` |
| `activesrvr.txt` | never | at the start; deleted at a clean exit |
| `admin.cfg` | at the start | never |
| `admin_log.txt` | never | emptied at every launch, then one line per admin event |
| `banned.txt` | when the session starts | at the exit, when an in-game ban added an entry |
| `banlist.txt` | when a NovaWorld session starts (a missing one is created) | by every ban or unban on a NovaWorld session (`PLAYER BAN`, `CMD BAN`, `CMD UNBAN`) |

## Remote admin

A nonzero `remote_admin_port` in `game.cfg` (0, off, by default) opens retail's remote-admin
console on that TCP port, on every interface, for retail's admin client (`RAT.exe`, shipped
with the game) or `opennova-nw-lister --admin`. A port that cannot be opened is logged and
the server serves on without it. `admin.cfg` holds its users and its address whitelist, in
retail's format:

```text
// <user> <password> <hex rights>
boss secret 4F
ip_restrict = 192.168.*
```

The rights are hexadecimal bits: `1` GET, `2` SET, `4` MISSION, `8` PLAYER, `10` WEAPON,
`20` CMD, `40` GOTO, CHAT and the stub ADMINUSER and BANLIST verbs. With no `ip_restrict` line
every address may connect; with one or more, an address must match one (`*` matches the rest,
so `192.168.*` is the 192.168 network). The shipped game's `admin.cfg` has no users and admits
192.168 only, so a local test needs a user line and a pattern such as `127.0.0.1`.

Every verb behaves as retail's does, replies included. In short:

| Verb | What it does |
|---|---|
| `GET GAMESTATE`, `GET GAMESETTINGS` | The scene, and 29 session settings. |
| `SET <key> <value>` | A session rule (`StartDelay`, `KillLimit`, `FriendlyFire`, ...), the server name or a password, live at once, and `game.cfg` saved. `SET` with no key lists the keys. |
| `MISSION LIST` / `AVAILABLE` | The rotation with its marks (`(2x)` Attack and Defend, `(ONE_SHOT)`, `<CURRENT MISSION>`, `<NEXT MISSION>`), and the install's missions. |
| `MISSION ADD <file> [switch] [at] [ONESHOT]` | Sets the map's Attack-and-Defend switch (1 with no value), then appends it to the rotation, or inserts it at position `at`; a `ONESHOT` entry is removed after it plays. |
| `MISSION REMOVE <#>`, `CLEAR`, `SETNEXT <#>`, `CYCLE` | Remove an entry; clear the rotation (the session then ends at the round's end); play entry `#` next; end the round now (the next map loads after a 10-second linger). |
| `PLAYER LIST` / `PUNT` / `BAN` / `SWAPTEAM` / `KILL` / `ZEROSCORE` `<# \| ALL>` | The players, and per-player actions. A LAN server keeps no PCID ban list, so `PLAYER BAN` answers that the player has no PCID and does nothing. |
| `WEAPON LIST`, `WEAPON SET <# \| ALL> <ALWAYS \| NEVER \| ARMORY>` | The armory's weapon availability. |
| `CMD <line>` | A console line (`BAN`, `UNBAN`, `PUNT`, `BANDWIDTH`). |
| `CHAT SEND <words>`, `CHAT GET` | A server chat line to every player in the match, and the server's chat lines (the status page's). |
| `GOTO GAMESTATE` / `MENUSTATE` | `GAMESTATE` cycles the map. `MENUSTATE` ends the round and quits the session, as a retail dedicated server leaves its match for its main menu; with no menu to show, `opennova-serve` then exits (code 0). |
| `QUIT` | Close the connection (send it rather than dropping the connection). |

Each connection and command is logged to `admin_log.txt`. The rotation edits follow retail's
list operations except where retail corrupts its own memory: a one-shot entry's removal keeps
every later entry, an insert position past the end appends, and the rotation restarts at its
first entry after `MISSION CLEAR` and `ADD` (D-NET-364..366).

## The console and the logs

A retail dedicated server shows its status page in place of the game: the player slots,
the server line, the team block, the round clock, the frame and login counts and the four
newest chat lines. `opennova-serve` prints that same page as text on its standard output
whenever its rows change (the slots, the server line, the team block or the chat lines;
the clock and the counts ride along). A line typed on its standard input goes into the
page's chat input as a retail operator types it on the server's window: the Global talk opens
(Ctrl+T), the line is typed, and Enter sends it to every player in the match. Global is the
one talk a server with no player of its own can send. The page's labels come from the
install's `gametext.bin`.

The console also prints the session's lines retail writes under its `/INOUT` switch: `HOST
STARTED` and `HOST STOPPED` with the server's address, and `SERVER PLAYER ADDED` and
`SERVER PLAYER REMOVED` per connection.

These logs go to the working directory, in retail's names and formats:

| File | Switch | What it holds |
|---|---|---|
| `<path>.sph` | `/PROFILE <path>` | The server log: per map of the rotation, a file of its own holding the map, the players, every eighth tick each player's position and heading, each deploy and disconnect. The extension replaces everything after the path's first dot; an existing file is not overwritten, the name counts up instead (`host.sph`, `host1.sph`, ... `host10.sph`, then `host1.sph` again). `opennova-wire` decodes it. |
| `_PUNT.TXT` | `/PUNTLOG` or `/PUNT.TXT` | A `START` line, then one line per punted player (the weapon and ammo table checks, more than nine suicides). The switch starts the file anew. |
| `_CHEAT.TXT` | `/CHEATLOG` | A `START` line (retail writes nothing else to it). |

## Exit codes

| Code | Meaning |
|---|---|
| 0 | `--help`, stopped by Ctrl+C, `game.cfg` sets `mpreset`, the map rotation ran out (the end of the list with `Replay 0`), or the remote admin's `GOTO MENUSTATE` quit the session. |
| 1 | The server did not start: the install did not mount, the host file did not open or named no listed mission, no port of the bind scan was free, the NovaWorld listing did not host (no gate answered, the gate was refused, the login or the host request failed; `networkconnecttype = 2` or no `--master-host` serves LAN only), or the mission did not boot. |
| 2 | A usage error, or the `--credentials` file did not open. |
| 3 | The session ended otherwise: a later map that did not boot, a mission exit that is not a round end, or the NovaWorld service ending the hosting. |
