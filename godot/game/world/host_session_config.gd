class_name HostSessionConfig
extends HostSessionOptions

## Typed record for a hosted co-op game-session request (ADR 0017): what the player chose
## on a host screen, carried menu -> shell -> GameWorld.load_mission_as_host -> the mission
## runtime. Every host producer builds one — the mp.mnu host screen (MpMenuCompanion), the
## NovaWorld panel, and the --lan-host launch flag. A HostSessionConfig always requests a
## SOCKETED LAN listen server; single-player passes none and keeps the in-process
## (socketless) listen server. The runtime projects it to the sim's HostSessionOptions
## record via to_session_options() for Simulation.configure_host_session.

## The g_GameType code words host producers name, re-exported from the
## NetProtocol binding (every other mode is spelled NetProtocol.GAME_TYPE_*).
## The canonical home (and every witness, including
## [orig: AI_GetTaskTypeFromFlags @ 0x40DAE0 -> Game_StartMission @ 0x524360]
## for the Co-op derivation) lives at engine/base/gameprofile/game_type.h; the
## GUT pin test holds the re-export chain to the witnessed hex. Code words are opaque beyond the objective bit — never
## decompose them. GAME_TYPE_TRAINING_COOP is what a mission without a
## multiplayer attrib (the retail training mission case) resolves to when
## launched by LAN automation.
const GAME_TYPE_COOP := NetProtocol.GAME_TYPE_COOP                           # LTGT_COOP (objective)
const GAME_TYPE_TRAINING_COOP := NetProtocol.GAME_TYPE_TRAINING_COOP         # LTGT_COOP (stock)
const GAME_TYPE_FLAG_ME := NetProtocol.GAME_TYPE_FLAG_ME                     # LTGT_FM
## The 0x0A sub-block-3 gate bit (gameprofile game_type::kObjectiveBit):
## GAME_TYPE_COOP == GAME_TYPE_TRAINING_COOP | GAME_TYPE_OBJECTIVE_BIT.
const GAME_TYPE_OBJECTIVE_BIT := NetProtocol.GAME_TYPE_OBJECTIVE_BIT
## First port of the retail LAN host range — the witness
## [orig: game.cfg mplanserverportmin/max, JO_SERVER] lives at the engine home,
## engine/net/npwire net_ports.h kRetailLanPortMin/Max (the maturity lint
## hard-fails new bare literals of these values outside the canonical homes).
const DEFAULT_LAN_PORT := NetProtocol.DEFAULT_LAN_PORT
## The NovaWorld gate's UDP port, shared by the OpenNova and original targets
## (engine home: kNovaWorldGatePort in engine/net/npwire net_ports.h, beside
## GATE_DEFAULT_PORT in engine/net/novaworld/gate_probe.h).
const DEFAULT_GATE_PORT := NetProtocol.DEFAULT_GATE_PORT
## Default lobby player cap when no host UI supplied one (the NovaWorld panel has no
## cap control). The mp.mnu host screen always sets its own read-back value; the
## host-side clamp to the witnessed 1..65 applies either way.
const DEFAULT_MAX_PLAYERS := 32
## Width of the retail host dialog's SPECTATOR_PW edit buffer (17 chars, read by
## HostDialog_ReadSettings @ 0x555940 into the buffer at 0x555ecc). Both password
## entry surfaces clamp to it so a typed password never exceeds what a retail
## host could have configured.
const SPECTATOR_PASSWORD_MAX_LENGTH := 17
## Which browser/lobby the session was requested from. The LAN channel never reads or
## manufactures NovaWorld service configuration; the NovaWorld channel supplies the gate.
const CHANNEL_LAN := "LAN"
const CHANNEL_NOVAWORLD := "NovaWorld"

# The native base owns session fields and host-dialog readback policy.
# Rotation selection and the menu's mission filter belong to this shell.
var mission := ""       ## the .bms to load (host screens put the rotation's first pick here)
var missions: Array[String] = []
var game_type_attr := ""  ## the GAME_TYPE spin's raw value attr (HG_COOP=2, ...), for later
var dedicated: bool:
	get: return not serve_and_play
	set(value): serve_and_play = not value


func _init() -> void:
	dir = ""           ## resource-dir override (dev/tests); empty = the CLI directory
	server_name = "COOPGAME"
	player_name = "Player"  ## the host's own callsign (rides ClientAuth like any player's)
	expansion = ""     ## g_ExpansionName: what the process actually mounted; "" for base JO
	integrity_profile = ""  ## explicit registered retail corpus; empty = no host-side CRC validation
	max_players = DEFAULT_MAX_PLAYERS  ## session-list-advertised player cap
	spectator_slots = 0
	spectator_password = ""
	game_type = GAME_TYPE_COOP  ## the numeric session g_GameType [orig: @ 0x24D2128]
	class_allow_mask = WeaponDatabase.CLASS_ALLOW_ALL
	game_type_auto = false  ## derive g_GameType from the loaded mission's attrib mode
	channel = CHANNEL_LAN
	lan_mode = 1
	bind_port = DEFAULT_LAN_PORT
	custom_text = NetProtocol.custom_text_default()
	nw_gate_host = ""
	nw_gate_port = DEFAULT_GATE_PORT
	region = "us"
	advertise = ""     ## explicit advertised-IP override for the gate row
	respawn_time = NetProtocol.DEFAULT_RESPAWN_TIME
	time_limit_minutes = NetProtocol.DEFAULT_TIME_LIMIT_MINUTES
	replay_enabled = NetProtocol.DEFAULT_REPLAY_ENABLED
	max_team_lives = NetProtocol.DEFAULT_MAX_TEAM_LIVES
	score_limit = NetProtocol.DEFAULT_SCORE_LIMIT
	max_score = NetProtocol.DEFAULT_MAX_SCORE
	koth_delta = NetProtocol.DEFAULT_KOTH_DELTA
	flag_return_ticks = NetProtocol.DEFAULT_FLAG_RETURN_TICKS
	capture_duration_seconds = NetProtocol.DEFAULT_CAPTURE_DURATION_SECONDS
	capture_speed_setting = NetProtocol.DEFAULT_CAPTURE_SPEED_SETTING
	spawn_wave_time_base = NetProtocol.DEFAULT_SPAWN_WAVE_TIME_BASE
	spawn_wave_time_zone = NetProtocol.DEFAULT_SPAWN_WAVE_TIME_ZONE
	default_spawn_requires_no_team_zone = NetProtocol.DEFAULT_SPAWN_REQUIRES_NO_TEAM_ZONE
	num_teams = NetProtocol.DEFAULT_NUM_TEAMS
	respawn_timeout = NetProtocol.DEFAULT_RESPAWN_TIMEOUT
	start_delay = NetProtocol.DEFAULT_START_DELAY
	destroy_buildings = NetProtocol.DEFAULT_DESTROY_BUILDINGS
	death_messages = NetProtocol.DEFAULT_DEATH_MESSAGES


## Retail's mission-attrib -> g_GameType table — the engine home is
## engine/base/gameprofile/game_type.h for_mission_mode (MissionData.ATTRIB_* values
## are pinned to the engine's bms::AttribFlags by static_assert). The zero-mode
## case is important: 00TRg has no authored multiplayer mode, yet direct LAN
## hosting selects the stock/training Co-op word.
static func game_type_for_mission_mode(mode: int) -> int:
	return NetProtocol.game_type_for_mission_mode(mode)


## The session slice for Simulation.configure_host_session as the typed record
## the sim takes (the record SessionDrive stages as
## MissionSetupOptions.host_session). The mission root resolves game_type_auto
## and stamps the mission-derived identity (mission_name / mission_file /
## spawn_names / game_root) on top before handing it to the sim.
func to_session_options() -> HostSessionOptions:
	var options := duplicate_options()
	options.mission_file = mission if not mission.is_empty() \
			else (missions[0] if not missions.is_empty() else "")
	return options
