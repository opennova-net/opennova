# nw-lister

Lists one server on a NovaWorld master without running the game. It speaks
the Joint Operations host leg itself: the gate probe, the NWU lobby session and
verify, the account login and NWHost HOSTKEY over HTTP, `ClientHostRequest`,
then a `ClientHostUpdate` every 1860 ticks (about 30 s). On a clean stop it
sends `ClientStopHosting` and `ClientGoodBye` so the master deregisters the
row rather than waiting for the session to time out. Protocol and crypto come
from `engine/net`. The app owns the sockets, the HTTP transport, timing and
logging.

What it lists comes from a JSON file that is re-read while it runs, so another
program can keep the map, player count and names current. With `--admin` the
lister keeps them current itself from the game server's remote-admin port. The row points at
the machine running the lister: it advertises a server, it does not route joins
to one.

## Usage

```
nw-lister --listing listing.json --credentials creds.txt \
          --master-host <gate host> --allow-public
```

- `--listing FILE` is the listing JSON (see `listing.example.json`).
- `--credentials FILE` holds `NOVAWORLD_USER=` and `NOVAWORLD_PASS=` lines.
  Without it the host leg runs without the account login and HOSTKEY.
- Every destination must be on `127.0.0.0/8` unless `--allow-public` is given,
  and name lookups are off until then. Run it against a local
  `opennova-novaworld` first. A live master is a shared service: use it
  sparingly.
- `--admin HOST[:PORT]` reads the game server's remote-admin port (TCP, 4000
  by default) every 15 s (`--admin-poll-seconds`): `PLAYER LIST`, `MISSION
  LIST` and `GET GAMESETTINGS`, nothing that changes the server. The names,
  current map and time left replace the file's; the admin port does not give
  the game mode, so `game_type` stays the file's. A server that stops answering
  is listed with 0 players. The login is `ADMIN_USER=` and `ADMIN_PASS=` in
  the credentials file.
- `--dry-run` builds and prints every statement and sends nothing.
- Stop it with Ctrl+C, by closing its console window (Windows), with SIGTERM,
  or with `--stop-file PATH`. All of them deregister before exiting.

`nw-lister` with no arguments prints every option.

## Listing JSON

| Key | Meaning |
| --- | --- |
| `server_name`, `msg` | ServerName and Msg |
| `mission`, `game_type` | MissionName and GameType as the browser shows them |
| `max_players`, `players` | MaxPlayers; `players` is a list of names, or objects with `name`, `slot`, `team`, `ip_and_port`, `pcid`, `type` |
| `player_count` | Players column without a name list |
| `exp` | expansion tag (`""`, `jox01`, `ic`); the master derives Expbits from it, and a tag it does not know lists as Expbits 0 |
| `password`, `locked`, `dedicated`, `tracers`, `skins` | booleans |
| `country`, `region`, `time_of_day`, `time_left_minutes` | Country code, Jungle/Desert/Snow, Dawn/Day/Dusk/Night, whole minutes |
| `lobby_name` | override the gate's LOBBYNAME |

Unknown keys are ignored. A file that fails to parse keeps the previous listing.
