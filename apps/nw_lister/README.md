# opennova-nw-lister

Lists one server on a NovaWorld master without running the game. It drives the
engine's own NovaWorld session, the code the game's NovaWorld menu uses
(`engine/net/novaworld`): the gate probe, the NWU lobby session and verify, the
account login and the hosting page's HOSTKEY over HTTP, `ClientHostRequest`, then
the `ClientHostUpdate` refresh every 1860 ticks (about 30 s). Roster changes go
out at once as `ClientHostPlayerAdded` / `ClientHostPlayerRemoved`. On a stop it
sends `ClientStopHosting` and the goodbye, so the master drops the row at once.

The listing comes from a JSON file that is re-read when it changes, so another
program can keep the map and players current. With `--admin`, the lister reads
the players, the current map and the time left from the game server's
remote-admin port instead. The row points at the machine running the lister: it
advertises a server, it does not route joins to one.

## Usage

```
opennova-nw-lister --listing listing.json --credentials creds.txt \
          --master-host <gate host> --allow-public
```

- `--listing FILE` is the listing JSON (see `listing.example.json`).
- `--credentials FILE` holds `NOVAWORLD_USER=` and `NOVAWORLD_PASS=` lines. With
  them, the lister logs in and fetches the HOSTKEY before the host request, as the
  game's menu does. Without them, it hosts with no HOSTKEY; a local
  `opennova-novaworld-server` accepts that.
- Every destination must be on `127.0.0.0/8` unless `--allow-public` is given.
  Until then, name lookups are off as well. Run it against a local
  `opennova-novaworld-server` first. A live master is a shared service, so use it
  sparingly.
- `--admin HOST[:PORT]` reads the game server's remote-admin port every 15 s.
  The port is the server's `remote_admin_port` (game.cfg; 0, the default, turns
  the console off), and the default here, 32800, is the retail admin client's.
  It sends only the read-only commands `PLAYER LIST`, `MISSION LIST` and
  `GET GAMESETTINGS`, and ends its session with `QUIT`. The names, current map
  and time left replace the file's. The admin port does not report the game
  mode, so `game_type` stays the file's. A server that stops answering, or sits
  in its menus, is listed with no players.

  The login is the `ADMIN_USER=` and `ADMIN_PASS=` lines of the credentials
  file, at most 31 characters each. They must match a server `admin.cfg` line
  `<USER> <PASS> <hexRIGHTS>` with the GET, MISSION and PLAYER rights (`0D`).
  The file's `ip_restrict` lines must admit the lister's address: the shipped
  file allows only `192.168.*`.
- `--log FILE` appends the log to a file as well. `--verbose` adds the debug
  lines. Account names, passwords and session tags are masked in both.
- Stop it with Ctrl+C, by closing its console window (Windows), with SIGTERM, or
  with `--stop-file PATH`. All of them deregister before it exits.

`opennova-nw-lister` with no arguments prints every option.

Exit codes:

| Code | Meaning |
| --- | --- |
| 0 | stopped and deregistered |
| 1 | bad command line |
| 2 | the listing or credentials file did not load |
| 3 | a socket could not be opened |
| 4 | the session, the login, the HOSTKEY or the host request failed |
| 5 | the master stopped the hosting or ended the session |

## Listing JSON

| Key | Meaning |
| --- | --- |
| `server_name`, `msg` | ServerName and Msg |
| `mission`, `game_type` | MissionName and GameType as the browser shows them |
| `max_players` | MaxPlayers |
| `players` | a list of names, or objects with `name`, `slot`, `team`, `ip_and_port`, `pcid`, `type`. A player without a `slot` keeps the one it was given; a new name takes the lowest free slot. |
| `exp` | expansion tag (`""`, `jox01`, `ic`); the master derives Expbits from it, and a tag it does not know lists as Expbits 0 |
| `password`, `locked`, `dedicated`, `tracers`, `skins` | booleans |
| `country`, `region`, `time_of_day`, `time_left_minutes` | Country code, Jungle/Desert/Snow, Dawn/Day/Dusk/Night, whole minutes |

The Players column is the number of listed players, plus the host's own slot
for a listen host (`"dedicated": false`) that lists no slot 0. The CountryName,
Lang and TZB columns are the machine's own locale, sent when the gate's reply
sets METEXT, as retail does whatever the server type. Unknown keys are ignored.
A file that fails to parse keeps the previous listing.
