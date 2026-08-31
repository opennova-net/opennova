class_name JoinTarget
extends RefCounted

const ROLE_PLAYER := 0
const ROLE_SPECTATOR := 1
const FLAG_ALLOW_SPECTATORS := 0x2000
const FLAG_SPECTATOR_PASSWORD := 0x4000

## Typed record for dialing a co-op host as a JOINER (ADR 0017): the LAN browser row the
## player activated, the NovaWorld panel's resolved join, or the --lan-join launch flag,
## carried shell -> GameWorld.load_mission_as_joiner. Retail LAN enumeration supplies an
## ENDPOINT, not a map: the normal path authenticates first and learns the mission from
## the S2C 0x7B session record. `mission` is only a browse/debug display hint; the
## join path never opens it locally (D-NET-194).

var host_ip := "127.0.0.1"
var port := HostSessionConfig.DEFAULT_LAN_PORT
var player_name := ""  ## the joiner's callsign; the shell fills its profile default when empty
var mission := ""      ## non-authoritative display hint; wire 0x7B/0x0B always owns the load
var dir := ""          ## resource-dir override (dev/tests); empty = the persisted directory
var integrity_profile := ""  ## explicit registered retail corpus; empty = safe CRC silence
## The game-session BT join token the client recovers from the NWJoin .joi CK; a
## NovaWorld host validates it in ClientAuth (reject code 9). "0" = the LAN default.
var join_token := "0"
# Browse-time DISPLAY HINTS for the loading screen only — never session state. The
# authoritative values arrive post-auth in the 0x7B record (join_session_identified).
var server_name := ""
var game_type := -1    ## numeric g_GameType hint; -1 = unknown before authentication
var server_flags := -1 ## ServerHello P2; -1 = not discovered yet
var join_role := ROLE_PLAYER
var spectator_password := ""
var role_explicit := false


func allows_spectators() -> bool:
	return server_flags >= 0 and (server_flags & FLAG_ALLOW_SPECTATORS) != 0


func spectator_password_required() -> bool:
	return server_flags >= 0 and (server_flags & FLAG_SPECTATOR_PASSWORD) != 0


## Decode a LanSession discovery row (a transport edge; keys from
## lan_session.cpp's row builder). Map identity is deliberately absent here:
## retail LAN enumeration has not joined the session yet, so the mission arrives
## in the normal post-auth 0x7B stream.
static func from_lan_row(row: Dictionary) -> JoinTarget:
	var target := JoinTarget.new()
	target.host_ip = String(row.get("host_ip", target.host_ip))
	target.port = int(row.get("port", target.port))
	target.server_name = String(row.get("server_name", row.get("name", "")))
	target.game_type = int(row.get("gametype", -1))
	target.server_flags = int(row.get("server_flags", -1))
	return target
