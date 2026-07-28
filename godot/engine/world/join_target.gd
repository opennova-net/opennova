class_name JoinTarget
extends RefCounted

## Typed record for dialing a co-op host as a JOINER (ADR 0017): the LAN browser row the
## player activated, the NovaWorld panel's resolved join, or the NW_LAN_JOIN env hook,
## carried shell -> GameWorld.load_mission_as_joiner. Retail LAN enumeration supplies an
## ENDPOINT, not a map: the normal path authenticates first and learns the mission from
## the S2C 0x7B session record; `mission` stays an explicit debug/online-row override.

var host_ip := "127.0.0.1"
var port := HostSessionConfig.DEFAULT_LAN_PORT
var player_name := ""  ## the joiner's callsign; the shell fills its profile default when empty
var mission := ""      ## explicit override; empty = learn map_file from S2C 0x7B post-auth
var dir := ""          ## resource-dir override (dev/tests); empty = the persisted directory
# Browse-time DISPLAY HINTS for the loading screen only — never session state. The
# authoritative values arrive post-auth in the 0x7B record (join_session_identified).
var server_name := ""
var game_type := -1    ## numeric g_GameType hint; -1 = unknown before authentication


## Decode a NovaLanSession discovery row (a transport edge; keys from
## nova_lan_session.cpp's row builder). Map identity is deliberately absent here:
## retail LAN enumeration has not joined the session yet, so the mission arrives
## in the normal post-auth 0x7B stream.
static func from_lan_row(row: Dictionary) -> JoinTarget:
	var target := JoinTarget.new()
	target.host_ip = String(row.get("host_ip", target.host_ip))
	target.port = int(row.get("port", target.port))
	target.server_name = String(row.get("server_name", row.get("name", "")))
	target.game_type = int(row.get("gametype", -1))
	return target
