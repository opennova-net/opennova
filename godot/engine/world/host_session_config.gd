class_name HostSessionConfig
extends RefCounted

## Typed record for a hosted co-op game-session request (ADR 0017): what the player chose
## on a host screen, carried menu -> shell -> GameWorld.load_mission_as_host -> the mission
## runtime. Every host producer builds one — the mp.mnu host screen (MpMenuCompanion), the
## NovaWorld panel, and the NW_LAN_HOST env hook. A HostSessionConfig always requests a
## SOCKETED LAN listen server; single-player passes none and keeps the in-process
## (socketless) listen server. At the FFI boundary the runtime encodes it back to a
## Dictionary via to_session_options() for NovaSimulation.configure_host_session.

## The retail Co-op session gametype. Retail derives it from an ATTRIB_COOP mission
## [orig: AI_GetTaskTypeFromFlags @ 0x40DAE0 -> Game_StartMission @ 0x524360];
## 0x10010 was an ASH_I5A capture value, never a default.
const GAME_TYPE_COOP := 0x30020
## First port of the witnessed retail LAN host range
## [orig: game.cfg mplanserverportmin/max 32768-32787, JO_SERVER].
## C++ twin: libs/npwire net_ports.h kRetailLanPortMin/Max (the maturity lint
## hard-fails new bare literals of these values outside the canonical homes).
const DEFAULT_LAN_PORT := 32768
## The NovaWorld gate's UDP port, shared by the OpenNova and original targets
## (mirrors GATE_DEFAULT_PORT territory in libs/novaworld/gate_probe.h).
const DEFAULT_GATE_PORT := 7597
## Default lobby player cap when no host UI supplied one (the NovaWorld panel has no
## cap control). The mp.mnu host screen always sets its own read-back value; the
## host-side clamp to the witnessed 1..65 applies either way.
const DEFAULT_MAX_PLAYERS := 32
## Which browser/lobby the session was requested from. The LAN channel never reads or
## manufactures NovaWorld service configuration; the NovaWorld channel supplies the gate.
const CHANNEL_LAN := "LAN"
const CHANNEL_NOVAWORLD := "NovaWorld"

var mission := ""       ## the .bms to load (host screens put the rotation's first pick here)
var missions: Array[String] = []  ## the SELECTED_MISSIONS rotation (rotation advance is future work)
var dir := ""           ## resource-dir override (dev/tests); empty = the persisted directory
var server_name := "COOPGAME"
var player_name := "Player"  ## the host's own callsign (rides ClientAuth like any player's)
var expansion := ""     ## g_ExpansionName: what the process actually mounted; "" for base JO
var max_players := DEFAULT_MAX_PLAYERS  ## lobby-advertised player cap
var game_type := GAME_TYPE_COOP  ## the numeric session g_GameType [orig: @ 0x24D2128]
var game_type_attr := ""  ## the GAME_TYPE spin's raw value attr (HG_COOP=2, ...), for later
var channel := CHANNEL_LAN
var bind_port := DEFAULT_LAN_PORT
var dedicated := false  ## dedicated host: the listen server runs with NO local player
var custom_text := ""   ## loading-screen message body [orig: g_sessionvar_custom_text @ 0x522123]
# NovaWorld gate registration (CHANNEL_NOVAWORLD): where GameWorld registers the browsable
# listen host. An empty gate host means pure LAN — nothing is registered.
var nw_gate_host := ""
var nw_gate_port := DEFAULT_GATE_PORT
var region := "us"
var advertise := ""     ## explicit advertised-IP override for the gate row


## Encode the session slice for NovaSimulation.configure_host_session — the FFI boundary
## keeps a Dictionary (ADR 0017). The mission runtime stamps the mission-derived identity
## (mission_name / mission_file / spawn_names) on top before handing it to the sim.
func to_session_options() -> Dictionary:
	return {
		"server_name": server_name,
		"player_name": player_name,
		"expansion": expansion,
		"gametype": game_type,
		"bind_port": bind_port,
		"max_players": max_players,
		"serve_and_play": not dedicated,
	}
