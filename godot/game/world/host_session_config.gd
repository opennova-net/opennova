class_name HostSessionConfig
extends RefCounted

## Typed record for a hosted co-op game-session request (ADR 0017): what the player chose
## on a host screen, carried menu -> shell -> GameWorld.load_mission_as_host -> the mission
## runtime. Every host producer builds one — the mp.mnu host screen (MpMenuCompanion), the
## NovaWorld panel, and the NW_LAN_HOST env hook. A HostSessionConfig always requests a
## SOCKETED LAN listen server; single-player passes none and keeps the in-process
## (socketless) listen server. At the FFI boundary the runtime encodes it back to a
## Dictionary via to_session_options() for Simulation.configure_host_session.

## The retail Co-op session gametype. Retail derives it from an ATTRIB_COOP mission
## [orig: AI_GetTaskTypeFromFlags @ 0x40DAE0 -> Game_StartMission @ 0x524360];
## 0x10010 was an ASH_I5A capture value, never a default.
const GAME_TYPE_COOP := 0x30020
## A mission without a multiplayer attrib (the retail training mission case)
## resolves to the non-objective Co-op family when launched by LAN automation.
const GAME_TYPE_TRAINING_COOP := 0x10020
## The rest of the witnessed g_GameType code words (the retail LTGT_* gametext
## keys — docs/interface/loading-screen-re.md). C++ twin: engine/net/npwire
## game_type.h (kCoop = TRAINING_COOP, kObjectiveCoop = COOP); the GUT pin test
## holds both sides to the same hex. Code words are opaque beyond the objective
## bit — never decompose them.
const GAME_TYPE_DEATHMATCH := 0x00000            # LTGT_DM
const GAME_TYPE_KING_OF_THE_HILL := 0x00001      # LTGT_KOTH
const GAME_TYPE_TEAM_DEATHMATCH := 0x10000       # LTGT_TDM
const GAME_TYPE_TEAM_KING_OF_THE_HILL := 0x10001 # LTGT_TKOTH
const GAME_TYPE_ATTACK_AND_DEFEND := 0x10002     # LTGT_AD
const GAME_TYPE_CAPTURE_THE_FLAG := 0x10004      # LTGT_CTF
const GAME_TYPE_FLAGBALL := 0x10008              # LTGT_FB
const GAME_TYPE_ADVANCE_AND_SECURE := 0x10010    # LTGT_AAS
const GAME_TYPE_SEARCH_AND_DESTROY := 0x90002    # LTGT_SD
const GAME_TYPE_CONQUER_AND_CONTROL := 0x50010   # LTGT_CAC
## The 0x0A sub-block-3 gate bit (npwire game_type::kObjectiveBit):
## GAME_TYPE_COOP == GAME_TYPE_TRAINING_COOP | GAME_TYPE_OBJECTIVE_BIT.
const GAME_TYPE_OBJECTIVE_BIT := 0x20000
## First port of the witnessed retail LAN host range
## [orig: game.cfg mplanserverportmin/max 32768-32787, JO_SERVER].
## C++ twin: engine/net/npwire net_ports.h kRetailLanPortMin/Max (the maturity lint
## hard-fails new bare literals of these values outside the canonical homes).
const DEFAULT_LAN_PORT := 32768
## The NovaWorld gate's UDP port, shared by the OpenNova and original targets
## (mirrors GATE_DEFAULT_PORT territory in engine/net/novaworld/gate_probe.h).
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
var integrity_profile := ""  ## explicit registered retail corpus; empty = no host-side CRC validation
var max_players := DEFAULT_MAX_PLAYERS  ## lobby-advertised player cap
var game_type := GAME_TYPE_COOP  ## the numeric session g_GameType [orig: @ 0x24D2128]
var class_allow_mask := 0x03FF  ## enabled Soldier Class ids [orig: g_hostClassAllowMask @ 0x24D59FC]
var game_type_auto := false  ## derive g_GameType from the loaded mission's attrib mode
var game_type_attr := ""  ## the GAME_TYPE spin's raw value attr (HG_COOP=2, ...), for later
var channel := CHANNEL_LAN
## Retail g_LanMode: authority LAN send divider 1..4 -> 12/6/4/3 ticks.
## Invalid values fall back to retail mode 2; the shipped/default mode is 1.
var lan_mode := 1
var bind_port := DEFAULT_LAN_PORT
var dedicated := false  ## dedicated host: the listen server runs with NO local player
var custom_text := "Put your message here."   ## retail host default [orig: g_sessionvar_custom_text @ 0x522123]
# NovaWorld gate registration (CHANNEL_NOVAWORLD): where GameWorld registers the browsable
# listen host. An empty gate host means pure LAN — nothing is registered.
var nw_gate_host := ""
var nw_gate_port := DEFAULT_GATE_PORT
var region := "us"
var advertise := ""     ## explicit advertised-IP override for the gate row

# Retail Config_SetDefaults values, also witnessed in 00TRg's S2C 0x08 block.
# These ride the typed request so every host producer shares one rule baseline.
var respawn_time := 30
var time_limit_minutes := 10
var replay_enabled := 1
var max_team_lives := 100
var score_limit := 50
var respawn_timeout := 5
var start_delay := 0
var destroy_buildings := 0
var death_messages := 1


## Retail's mission-attrib -> g_GameType table. The zero-mode case is important:
## 00TRg has no authored multiplayer mode, yet direct LAN hosting selects 0x10020.
static func game_type_for_mission_mode(mode: int) -> int:
	match mode:
		MissionData.ATTRIB_DEATHMATCH:
			return GAME_TYPE_DEATHMATCH
		MissionData.ATTRIB_TEAM_DEATHMATCH:
			return GAME_TYPE_TEAM_DEATHMATCH
		MissionData.ATTRIB_COOP:
			return GAME_TYPE_COOP
		MissionData.ATTRIB_KING_OF_THE_HILL:
			return GAME_TYPE_KING_OF_THE_HILL
		MissionData.ATTRIB_TEAM_KING_OF_THE_HILL:
			return GAME_TYPE_TEAM_KING_OF_THE_HILL
		MissionData.ATTRIB_SEARCH_AND_DESTROY:
			return GAME_TYPE_SEARCH_AND_DESTROY
		MissionData.ATTRIB_ATTACK_AND_DEFEND:
			return GAME_TYPE_ATTACK_AND_DEFEND
		MissionData.ATTRIB_CAPTURE_THE_FLAG:
			return GAME_TYPE_CAPTURE_THE_FLAG
		MissionData.ATTRIB_FLAGBALL:
			return GAME_TYPE_FLAGBALL
		MissionData.ATTRIB_ADVANCE_AND_SECURE:
			return GAME_TYPE_ADVANCE_AND_SECURE
		MissionData.ATTRIB_CONQUER_AND_CONTROL:
			return GAME_TYPE_CONQUER_AND_CONTROL
		_:
			return GAME_TYPE_TRAINING_COOP


## Encode the session slice for Simulation.configure_host_session — the FFI boundary
## keeps a Dictionary (ADR 0017). The mission runtime stamps the mission-derived identity
## (mission_name / mission_file / spawn_names) on top before handing it to the sim.
func to_session_options() -> Dictionary:
	return {
		"server_name": server_name,
		"player_name": player_name,
		"custom_text": custom_text,
		"expansion": expansion,
		"integrity_profile": integrity_profile,
		"gametype": game_type,
		"class_allow_mask": class_allow_mask,
		"channel": channel,
		"lan_mode": lan_mode,
		"bind_port": bind_port,
		"max_players": max_players,
		"serve_and_play": not dedicated,
		"respawn_time": respawn_time,
		"time_limit_minutes": time_limit_minutes,
		"replay_enabled": replay_enabled,
		"max_team_lives": max_team_lives,
		"score_limit": score_limit,
		"respawn_timeout": respawn_timeout,
		"start_delay": start_delay,
		"destroy_buildings": destroy_buildings,
		"death_messages": death_messages,
	}
