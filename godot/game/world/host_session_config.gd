class_name HostSessionConfig
extends RefCounted

## Typed record for a hosted co-op game-session request (ADR 0017): what the player chose
## on a host screen, carried menu -> shell -> GameWorld.load_mission_as_host -> the mission
## runtime. Every host producer builds one — the mp.mnu host screen (MpMenuCompanion), the
## NovaWorld panel, and the --lan-host launch flag. A HostSessionConfig always requests a
## SOCKETED LAN listen server; single-player passes none and keeps the in-process
## (socketless) listen server. At the FFI boundary the runtime encodes it back to a
## Dictionary via to_session_options() for Simulation.configure_host_session.

## The g_GameType code words, re-exported from the NetProtocol binding so host
## producers keep the HostSessionConfig.* spelling. The canonical home (and every
## witness, including [orig: AI_GetTaskTypeFromFlags @ 0x40DAE0 ->
## Game_StartMission @ 0x524360] for the Co-op derivation) lives at
## engine/net/npwire game_type.h; the GUT pin test holds the re-export chain to
## the witnessed hex. Code words are opaque beyond the objective bit — never
## decompose them. GAME_TYPE_TRAINING_COOP is what a mission without a
## multiplayer attrib (the retail training mission case) resolves to when
## launched by LAN automation.
const GAME_TYPE_COOP := NetProtocol.GAME_TYPE_COOP                           # LTGT_COOP (objective)
const GAME_TYPE_TRAINING_COOP := NetProtocol.GAME_TYPE_TRAINING_COOP         # LTGT_COOP (stock)
const GAME_TYPE_DEATHMATCH := NetProtocol.GAME_TYPE_DEATHMATCH               # LTGT_DM
const GAME_TYPE_KING_OF_THE_HILL := NetProtocol.GAME_TYPE_KING_OF_THE_HILL   # LTGT_KOTH
const GAME_TYPE_FLAG_ME := NetProtocol.GAME_TYPE_FLAG_ME                     # LTGT_FM
const GAME_TYPE_TEAM_DEATHMATCH := NetProtocol.GAME_TYPE_TEAM_DEATHMATCH     # LTGT_TDM
const GAME_TYPE_TEAM_KING_OF_THE_HILL := NetProtocol.GAME_TYPE_TEAM_KING_OF_THE_HILL # LTGT_TKOTH
const GAME_TYPE_ATTACK_AND_DEFEND := NetProtocol.GAME_TYPE_ATTACK_AND_DEFEND # LTGT_AD
const GAME_TYPE_CAPTURE_THE_FLAG := NetProtocol.GAME_TYPE_CAPTURE_THE_FLAG   # LTGT_CTF
const GAME_TYPE_FLAGBALL := NetProtocol.GAME_TYPE_FLAGBALL                   # LTGT_FB
const GAME_TYPE_ADVANCE_AND_SECURE := NetProtocol.GAME_TYPE_ADVANCE_AND_SECURE # LTGT_AAS
const GAME_TYPE_SEARCH_AND_DESTROY := NetProtocol.GAME_TYPE_SEARCH_AND_DESTROY # LTGT_SD
const GAME_TYPE_CONQUER_AND_CONTROL := NetProtocol.GAME_TYPE_CONQUER_AND_CONTROL # LTGT_CAC
## The 0x0A sub-block-3 gate bit (npwire game_type::kObjectiveBit):
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

var mission := ""       ## the .bms to load (host screens put the rotation's first pick here)
var missions: Array[String] = []  ## the SELECTED_MISSIONS rotation (rotation advance is future work)
var dir := ""           ## resource-dir override (dev/tests); empty = the persisted directory
var server_name := "COOPGAME"
var player_name := "Player"  ## the host's own callsign (rides ClientAuth like any player's)
var expansion := ""     ## g_ExpansionName: what the process actually mounted; "" for base JO
var integrity_profile := ""  ## explicit registered retail corpus; empty = no host-side CRC validation
var max_players := DEFAULT_MAX_PLAYERS  ## lobby-advertised player cap
## Retail's signed spectator setting: 0 disables, -1 shares max_players, and a
## positive value adds that many spectator-only slots.
var spectator_slots := 0
var spectator_password := ""
var game_type := GAME_TYPE_COOP  ## the numeric session g_GameType [orig: @ 0x24D2128]
## Enabled Soldier Class ids; the no-restriction mask's engine home is
## engine/runtime/world player_loadout.h [orig: g_hostClassAllowMask @ 0x24D59FC].
var class_allow_mask := WeaponDatabase.CLASS_ALLOW_ALL
var game_type_auto := false  ## derive g_GameType from the loaded mission's attrib mode
var game_type_attr := ""  ## the GAME_TYPE spin's raw value attr (HG_COOP=2, ...), for later
var channel := CHANNEL_LAN
## Retail g_LanMode: authority LAN send divider 1..4 -> 12/6/4/3 ticks.
## Invalid values fall back to retail mode 2; the shipped/default mode is 1.
var lan_mode := 1
var bind_port := DEFAULT_LAN_PORT
var dedicated := false  ## dedicated host: the listen server runs with NO local player
## Retail host default; the witness [orig: g_sessionvar_custom_text @ 0x522123]
## lives at the engine home, engine/net/npwire game_type.h kCustomTextDefault.
var custom_text := NetProtocol.custom_text_default()
# NovaWorld gate registration (CHANNEL_NOVAWORLD): where GameWorld registers the browsable
# listen host. An empty gate host means pure LAN — nothing is registered.
var nw_gate_host := ""
var nw_gate_port := DEFAULT_GATE_PORT
var region := "us"
var advertise := ""     ## explicit advertised-IP override for the gate row

# Retail's Config_SetDefaults rule baseline, also witnessed in 00TRg's S2C 0x08
# block. The engine home is engine/net/npwire game_type.h game_rules::kDefault*;
# these ride the typed request so every host producer shares one rule baseline.
var respawn_time := NetProtocol.DEFAULT_RESPAWN_TIME
var time_limit_minutes := NetProtocol.DEFAULT_TIME_LIMIT_MINUTES
var replay_enabled := NetProtocol.DEFAULT_REPLAY_ENABLED
var max_team_lives := NetProtocol.DEFAULT_MAX_TEAM_LIVES
var score_limit := NetProtocol.DEFAULT_SCORE_LIMIT
var max_score := NetProtocol.DEFAULT_MAX_SCORE
var koth_delta := NetProtocol.DEFAULT_KOTH_DELTA
var flag_return_ticks := NetProtocol.DEFAULT_FLAG_RETURN_TICKS
var capture_duration_seconds := NetProtocol.DEFAULT_CAPTURE_DURATION_SECONDS
var capture_speed_setting := NetProtocol.DEFAULT_CAPTURE_SPEED_SETTING
var spawn_wave_time_base := NetProtocol.DEFAULT_SPAWN_WAVE_TIME_BASE
var spawn_wave_time_zone := NetProtocol.DEFAULT_SPAWN_WAVE_TIME_ZONE
## Retail cfg `nodefaultspawnpoints`: when enabled, Default Spawn is available
## only when this team has no unnumbered or fully controlled numbered zone.
## [orig: Server_ProcessClientRequestRespawn @0x519C8E;
## Entity_HasAliveEntityOfTeam @0x4FC7B0]
var default_spawn_requires_no_team_zone := NetProtocol.DEFAULT_SPAWN_REQUIRES_NO_TEAM_ZONE
var num_teams := NetProtocol.DEFAULT_NUM_TEAMS
var respawn_timeout := NetProtocol.DEFAULT_RESPAWN_TIMEOUT
var start_delay := NetProtocol.DEFAULT_START_DELAY
var destroy_buildings := NetProtocol.DEFAULT_DESTROY_BUILDINGS
var death_messages := NetProtocol.DEFAULT_DEATH_MESSAGES


## Retail's mission-attrib -> g_GameType table — the engine home is
## engine/net/npwire game_type.h for_mission_mode (MissionData.ATTRIB_* values
## are pinned to the engine's bms::AttribFlags by static_assert). The zero-mode
## case is important: 00TRg has no authored multiplayer mode, yet direct LAN
## hosting selects the stock/training Co-op word.
static func game_type_for_mission_mode(mode: int) -> int:
	return NetProtocol.game_type_for_mission_mode(mode)


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
		"spectator_slots": spectator_slots,
		"spectator_password": spectator_password,
		"serve_and_play": not dedicated,
		"respawn_time": respawn_time,
		"time_limit_minutes": time_limit_minutes,
		"replay_enabled": replay_enabled,
		"max_team_lives": max_team_lives,
		"score_limit": score_limit,
		"max_score": max_score,
		"koth_delta": koth_delta,
		"flag_return_ticks": flag_return_ticks,
		"capture_duration_seconds": capture_duration_seconds,
		"capture_speed_setting": capture_speed_setting,
		"spawn_wave_time_base": spawn_wave_time_base,
		"spawn_wave_time_zone": spawn_wave_time_zone,
		"default_spawn_requires_no_team_zone": default_spawn_requires_no_team_zone,
		"num_teams": num_teams,
		"respawn_timeout": respawn_timeout,
		"start_delay": start_delay,
		"destroy_buildings": destroy_buildings,
		"death_messages": death_messages,
	}
