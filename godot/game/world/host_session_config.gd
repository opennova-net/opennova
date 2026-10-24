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
## First port of the retail LAN host range — the witness
## [orig: game.cfg mplanserverportmin/max, JO_SERVER] lives at the engine home,
## engine/net/npwire net_ports.h kRetailLanPortMin/Max (the maturity lint
## hard-fails new bare literals of these values outside the canonical homes).
const DEFAULT_LAN_PORT := NetProtocol.DEFAULT_LAN_PORT
## The NovaWorld gate's UDP port, shared by the OpenNova and original targets
## (engine home: kNovaWorldGatePort in engine/net/npwire net_ports.h, beside
## GATE_DEFAULT_PORT in engine/net/novaworld/gate_probe.h).
const DEFAULT_GATE_PORT := NetProtocol.DEFAULT_GATE_PORT
## Width of the retail host dialog's SPECTATOR_PW edit buffer (17 chars, read by
## HostDialog_ReadSettings @ 0x555940 into the buffer at 0x555ecc). Both password
## entry surfaces clamp to it so a typed password never exceeds what a retail
## host could have configured.
const SPECTATOR_PASSWORD_MAX_LENGTH := 17
## Which browser/lobby the session was requested from. The LAN channel never reads or
## manufactures NovaWorld service configuration; a NovaWorld host is hosted by the
## NovaWorld panel's logged-in session (NovaWorldClient.start_hosting).
const CHANNEL_LAN := "LAN"
const CHANNEL_NOVAWORLD := "NovaWorld"

# The native base owns session fields and host-dialog readback policy.
# Rotation selection and the menu's mission filter belong to this shell.
var mission := ""       ## the .bms to load (host screens put the rotation's first pick here)
var missions: Array[String] = []  ## the rotation: SELECTED_MISSIONS' files in table order
var mission_launch_options: Array[int] = []  ## each row's Switch cell (its launch option)
var game_type_attr := ""  ## the GAME_TYPE spin's raw value attr (HG_COOP=2, ...), for later


func _init() -> void:
	## Every session word is the process block's: retail's session create reads
	## the cfg block, which its host screen showed and wrote back at START, so a
	## word no control shows, and a host with no screen in front of it (the
	## --lan-host launch), keeps the file's: the name (game_name), the cap, the
	## server type, the rules, the attribute word (D-NET-335, D-NET-NEW-LP-4;
	## the engine's inmatch::host_screen_from_block, host_config.h), and the LAN
	## authority's first bind port (mplanserverportmin).
	seed_from_block(GameCfgStore.block())
	dir = ""           ## resource-dir override (dev/tests); empty = the CLI directory
	player_name = "Player"  ## the host's own callsign (rides ClientAuth like any player's)
	expansion = ""     ## g_ExpansionName: what the process actually mounted; "" for base JO
	channel = CHANNEL_LAN


## The session slice for Simulation.configure_host_session as the typed record
## the sim takes (the record SessionDrive stages as
## MissionSetupOptions.host_session). The mission root resolves game_type_auto
## and stamps the mission-derived identity (mission_name / mission_file /
## spawn_names / game_root) on top before handing it to the sim.
func to_session_options() -> HostSessionOptions:
	var options := duplicate_options()
	options.mission_file = mission if not mission.is_empty() \
			else (missions[0] if not missions.is_empty() else "")
	options.rotation_missions = PackedStringArray(missions)
	options.rotation_launch_options = PackedInt32Array(mission_launch_options)
	return options
