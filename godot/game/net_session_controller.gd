class_name NetSessionController
extends Node

# The game shell's NET SESSION entries: every way a networked session starts —
# the mp.mnu LAN browser/host screens, the NovaWorld panel, and the NW_* env
# launch hooks — plus the callsign, panel lifecycle, and spectator kill feed
# that ride them. The shell (MainGame) keeps the state machine and the load
# pipeline; this component builds the typed requests (HostSessionConfig /
# JoinTarget, ADR 0017) and hands each load to the shell's start_world_load
# seam. Game-layer only: the engine world below knows nothing of menus or env
# hooks.

var _shell  # MainGame: start_world_load / current_resource_root
var _world: GameWorld
var _menu_shell: MenuShell
var _panel_layer: Node  # where the NovaWorld panel mounts (the menu layer)
var _novaworld_panel: NovaWorldPanel


func setup(shell, world: GameWorld, menu_shell, panel_layer: Node) -> void:
	_shell = shell
	_world = world
	_menu_shell = menu_shell
	_panel_layer = panel_layer


# The multiplayer menu companion's session requests route here.
func wire_menu_companions(mp_companion: MpMenuCompanion) -> void:
	mp_companion.lan_host_start_requested.connect(_on_lan_host_start_requested)
	mp_companion.lan_join_requested.connect(join_lan_server)


# --- Env launch hooks (dev/demo scaffolding) -----------------------------------

# Co-op LAN demo hooks. These remain useful for deterministic smoke runs even
# though mp.mnu's LAN_SEARCH now browses live hosts through LanSession.
# NW_LAN_HOST=<mission.bms> boots straight in as a co-op host;
# NW_LAN_JOIN=<ip[:port]> boots as a joiner dialing that host. The normal path
# learns the mission from S2C 0x7B after authentication; NW_LAN_MISSION is only
# a debug/display expectation and never a local load override (D-NET-194).
# Two instances on localhost = the bidirectional co-op demo.
func maybe_launch_lan_from_env() -> bool:
	var lan_mission := OS.get_environment("NW_LAN_HOST")
	if not lan_mission.is_empty():
		# game_type = the numeric session g_GameType the host config chooses at host start
		# [orig: g_GameType = session gametype setting @0x4a6657]. An absent override
		# means the hook's `auto`: derive it from the loaded mission. That distinction
		# matters for 00TRg, whose zero mode resolves to retail's 0x10020 training Co-op.
		var lan_gametype := OS.get_environment("NW_LAN_GAMETYPE")
		var lan_port := OS.get_environment("NW_LAN_PORT")
		var lan_mode := OS.get_environment("NW_LAN_MODE")
		var lan_max_players := OS.get_environment("NW_LAN_MAX_PLAYERS")
		var demo_config := HostSessionConfig.new()
		demo_config.mission = lan_mission
		# NW_LAN_NAME is the host player's callsign, matching LanHostCallsign in
		# onhook.cfg. Retail's default GameName is the localized `Untitled ` value.
		demo_config.server_name = "Untitled "
		demo_config.max_players = resolve_lan_max_players_override(lan_max_players, 4)
		if not lan_port.is_empty():
			demo_config.bind_port = int(lan_port)
		demo_config.game_type_auto = lan_gametype.strip_edges().is_empty()
		if not demo_config.game_type_auto:
			demo_config.game_type = int(lan_gametype)
		demo_config.integrity_profile = OS.get_environment(
				"NW_LAN_INTEGRITY_PROFILE").strip_edges()
		demo_config.lan_mode = resolve_lan_mode_override(
			lan_mode, demo_config.lan_mode)
		_on_lan_host_start_requested(demo_config)
		return true
	var lan_join := OS.get_environment("NW_LAN_JOIN")
	if not lan_join.is_empty():
		var jp := lan_join.split(":")
		var demo_target := JoinTarget.new()
		if jp.size() > 0 and not jp[0].is_empty():
			demo_target.host_ip = jp[0]
		if jp.size() > 1:
			demo_target.port = int(jp[1])
		demo_target.mission = OS.get_environment("NW_LAN_MISSION")
		demo_target.integrity_profile = OS.get_environment(
				"NW_LAN_INTEGRITY_PROFILE").strip_edges()
		join_lan_server(demo_target)
		return true
	return false


## Parse the optional env-host LAN rate selector without letting a malformed
## development override silently change the retail default. Values 1..4 map to
## the witnessed holdoffs 12/6/4/3; blank, non-numeric, and out-of-range values
## preserve the supplied fallback.
static func resolve_lan_mode_override(value: String, fallback: int) -> int:
	var normalized := value.strip_edges()
	if not normalized.is_valid_int():
		return fallback
	var mode := int(normalized)
	return mode if mode >= 1 and mode <= 4 else fallback


## The retail hook accepts 1..64 for direct listen-host capacity. Keep a bad
## automation value from silently changing the typed request's fallback.
static func resolve_lan_max_players_override(value: String, fallback: int) -> int:
	var normalized := value.strip_edges()
	if not normalized.is_valid_int():
		return fallback
	var max_players := int(normalized)
	return max_players if max_players >= 1 and max_players <= 64 else fallback


# --- LAN co-op (mp.mnu) --------------------------------------------------------

# Host a LAN co-op game: the same menu->world handoff as a single-player start, but the
# world loads as a listen-server host (ADR 0011) configured from the mp.mnu host screen.
func _on_lan_host_start_requested(config: HostSessionConfig) -> void:
	# The callsign is part of the local game session. LAN does not inspect or inherit
	# any NovaWorld service configuration; online registration is owned exclusively
	# by _on_novaworld_host_requested and the config that panel supplies.
	config.player_name = resolve_player_callsign()
	# g_ExpansionName is the expansion the process actually mounted (empty for base
	# JO), not a session template or a value copied from one capture.
	var root := _resource_root()
	config.expansion = root.get_expansion() if root != null else ""
	var load_info := {
		"mission_file": config.mission,
		"in_session": true,
		"server_name": config.server_name,
		"mission_name": _resolve_mission_title(config.mission),
		"game_type": config.game_type,
		"custom_text": config.custom_text,
	}
	_shell.start_world_load(
		load_info,
		Callable(_world, "load_mission_as_host").bind(config))


## Public entry for "join this server" — the seam behind the LAN browser's
## `lan_join_requested` signal, the NovaWorld panel row, the `NW_LAN_JOIN` env
## hook, and the shell's ADR-0018 delegate. Dial it as a co-op JOINER: the world
## loads as a non-authority client that runs the witnessed in-match JOIN and
## renders the host + NPCs wire-direct (net-re §5.38b). The LAN row carries only
## the observed host_ip/port and browse-time server fields; the mission arrives
## after authentication in the normal S2C 0x7B session record.
func join_lan_server(target: JoinTarget) -> void:
	if target == null:
		return
	# Joiner: the retail client obtains the full session-variable set from the
	# connect stream before wire-header world load [orig: parse_server_session_variables
	# @ 0x5202f0]. Browse-time values are display hints only; GameWorld replaces
	# them with the authoritative post-auth record before starting MissionPresentation.
	# Retail also holds the screen through the post-world-load connection/game-start
	# waits [orig: NapiClient_WaitForDisconnect @ 0x42cb20 then
	# NapiClient_WaitForGameStart @ 0x42cc10]. GameWorld pumps the loaded runtime
	# while hidden and reports the authoritative admission/deploy edge separately
	# (docs/interface/loading-screen-re.md, the load-flow case matrix).
	if target.player_name.is_empty():
		target.player_name = resolve_player_callsign()
	var load_info := {
		"mission_file": target.mission,
		"in_session": true,
		"server_name": target.server_name,
		"game_type": target.game_type,
	}
	_shell.start_world_load(
		load_info,
		Callable(_world, "load_mission_as_joiner").bind(target))


## The local player's callsign — rides the game ClientAuth.NA (the host echoes it back so we
## self-identify by name-match, which makes a duplicate callsign unjoinable — D-NET-169).
## The persisted profile default is uniquified per machine (PlayerProfile); NW_LAN_NAME
## overrides for the two-instance demo.
func resolve_player_callsign() -> String:
	# The override rides the same Name[16] wire echo as the profile value, so it gets
	# the same 15-character clamp — a longer callsign can never satisfy the name-match
	# self-ID and the join would die 60 s later with a misleading stall reason.
	var n := OS.get_environment("NW_LAN_NAME").strip_edges() \
			.left(PlayerProfile.MAX_CALLSIGN_LENGTH)
	return n if not n.is_empty() else PlayerProfile.load_callsign()


# --- NovaWorld (online multiplayer) ---------------------------------------------

# The menu's NovaWorld control: open the panel over the hidden menu.
func open_novaworld_panel() -> void:
	if _novaworld_panel != null:
		return
	_novaworld_panel = NovaWorldPanel.new()
	# Dev default: localhost. A prod build sets the server host from the
	# resolved server IP before showing the panel.
	# Hand the panel the mounted menu root so its host Map picker can list .bms missions (the world's
	# own root is null until a mission loads). Set BEFORE add_child so the panel's _build_ui sees it.
	_novaworld_panel.resource_root = _resource_root()
	_menu_shell.hide_menu()
	_panel_layer.add_child(_novaworld_panel)
	_novaworld_panel.closed.connect(_on_novaworld_closed)
	# Bridge the panel's resolved join into the ONE joiner path (the same handler the LAN browser +
	# NW_LAN_JOIN env use); the panel emits the same typed JoinTarget load_mission_as_joiner
	# consumes. Hosting from the panel routes through the shared host bring-up.
	_novaworld_panel.join_in_match_requested.connect(_on_novaworld_join_requested)
	_novaworld_panel.host_requested.connect(_on_novaworld_host_requested)


func _on_novaworld_closed() -> void:
	_dismiss_novaworld_panel()
	_menu_shell.show_menu()


func _dismiss_novaworld_panel() -> void:
	if _novaworld_panel != null:
		_novaworld_panel.queue_free()
		_novaworld_panel = null


# The NovaWorld panel asked to host. Resolve a mission (the menu's selected one, else the first
# available .bms), fill the callsign, and stand up a browsable listen host through the SAME bring-up
# the mp.mnu host screen uses — the panel supplied the gate (nw_gate_host) + the NovaWorld channel,
# so net_session_drive._maybe_start_nw_host registers it. (A mission picker in the panel is a follow-up.)
func _on_novaworld_host_requested(config: HostSessionConfig) -> void:
	# The panel picks the map; fall back to the first available .bms only if it sent none.
	var mission := config.mission
	if mission.is_empty():
		mission = _resolve_default_mission()
	if mission.is_empty():
		# Report back so the panel leaves "Starting..." instead of hanging silently.
		push_warning("NetSessionController: NovaWorld host requested but no mission is available")
		if _novaworld_panel != null:
			_novaworld_panel.host_failed("No mission available to host (check the game folder).")
		return
	_dismiss_novaworld_panel()
	config.mission = mission
	config.player_name = resolve_player_callsign()
	if config.server_name.is_empty():
		config.server_name = "OpenNova Host"
	_shell.start_world_load({
		"mission_file": mission,
		"in_session": true,
		"server_name": config.server_name,
		"mission_name": _resolve_mission_title(mission),
		"game_type": config.game_type,
		"custom_text": config.custom_text,
	}, Callable(_world, "load_mission_as_host").bind(config))


# The NovaWorld panel resolved a join target. Tear down the panel overlay, then enter the match
# through the SAME joiner entry the LAN browser + NW_LAN_JOIN env use (the target already
# carries host_ip/port/mission/player_name).
func _on_novaworld_join_requested(target: JoinTarget) -> void:
	_dismiss_novaworld_panel()
	join_lan_server(target)


# A default mission for a panel-initiated host: the mission highlighted in the menu if any, else the
# first .bms the resource root exposes. Empty when no mission is reachable.
func _resolve_default_mission() -> String:
	if _menu_shell != null:
		var sel := String(_menu_shell.get_selected_mission())
		if not sel.is_empty():
			return sel
	# The mounted menu root — the world's own root stays null until a mission loads. This is the same
	# object the menu shell + mp host list missions from, and is non-null whenever the panel can open.
	return MissionCatalog.first_mission_name(_resource_root())


# --- Shared lookups --------------------------------------------------------------

func _resource_root() -> ResourceRoot:
	return _shell.current_resource_root() if _shell != null else null


# MISSIONNAME for the loading screen = the mission text .bin's [info]/title
# [orig: serialize_mission_info_to_datastream @ 0x523620 ->
# TextResource_FindEntryBySectionAndKey(g_TextMission, "info", "title"); an
# empty title falls back to the mission-header title]. Our fallback: the
# mission basename.
func _resolve_mission_title(bms_name: String) -> String:
	var base := bms_name.get_file().get_basename()
	var root := _resource_root()
	if root == null:
		return base
	var bytes := root.read_file(base + ".bin")
	if bytes.is_empty():
		return base
	var table := RtxtStringFile.new()
	if table.load_from_byte_array(bytes) != OK:
		return base
	if not table.has_string_in_section("info", "title"):
		return base
	var title := table.get_string_in_section("info", "title")
	return title if not title.is_empty() else base
