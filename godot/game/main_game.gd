extends Node3D

# Runtime shell: boots into the game's menu front-end (NovaMenuHost, driving the
# .mnu menu set + audio from the chosen resource dir) and hands off to a GameWorld
# when the player starts a mission, with pause + return-to-menu on demand. The
# engine ships no game data; everything (menus, audio, terrain, missions) loads
# from the chosen resource dir. The first-launch directory picker lives here
# (runtime-only); headless probes set the dir explicitly and never block on it.

const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")
const DebugOverlayScript := preload("res://engine/debug/nova_debug_overlay.gd")
const NetKillFeedScript := preload("res://game/net_killfeed.gd")
const GameHudScript := preload("res://game/game_hud.gd")
const LocalPlayerHostScript := preload("res://engine/world/local_player_host.gd")

# Re-summon the game-folder picker. The original engine has no "change game dir"
# control (the game *is* its install folder); this is an OpenNova convenience so a
# wrong / menu-less folder can be re-picked without restarting. Front-end only.
const CHANGE_DIR_KEY := KEY_F9
# The mission debug overlay (entities / sim transport / script variables).
const DEBUG_OVERLAY_KEY := KEY_F3

enum State { MENU, WORLD, PAUSED }

@onready var _world: GameWorld = $World
@onready var _camera: Camera3D = $Camera3D
@onready var _hud: CanvasLayer = $HUD
@onready var _menu_host = $MenuLayer/MenuHost

var _picker: FileDialog
var _root: NovaResourceRoot
var _state: int = State.MENU
var _host_wired := false
var _debug_overlay  # NovaDebugOverlay, lazily built on the first F3
var _net_killfeed   # net spectator kill feed, built while in a net session
var _game_hud       # GameHud, built on the first frame a mission has a local player
var _warned_hud_no_player := false  # one-shot: warn if a loaded world never yields a local player
var _hud_objective := ""  # latest mission-effect text line shown by the HUD
var _player_host: LocalPlayerHost = null
var _mp_host  # MpMenuHost: drives the multiplayer (mp.mnu) menu by control name
var _player_info_host  # PlayerInfoMenuHost: drives the PLAYER_INFO (player.mnu) character screen
var _chosen_avatar: Dictionary = {}  # last avatar/name picked on PLAYER_INFO (the persistence seam)


func _ready() -> void:
	if _world == null or _camera == null or _menu_host == null:
		return
	# Esc toggles pause/resume in a world (the fly camera reports the key; the
	# host decides what it means).
	if _camera.has_signal("escape_pressed") and not _camera.is_connected("escape_pressed", _on_camera_escape):
		_camera.connect("escape_pressed", _on_camera_escape)
	_player_host = LocalPlayerHostScript.new()
	_player_host.name = "LocalPlayerHost"
	add_child(_player_host)
	_player_host.setup(_world, _camera)
	# Net-replay connect mode: when NW_REPLAY is set (the env all F5/F6 instances
	# inherit from the editor), skip the menu and dial the replay tool / server
	# directly — each instance gets slotted into a role on connect.
	if not OS.get_environment("NW_REPLAY").is_empty():
		_enter_net_session()
		return
	var dir := ResourceDirSettings.get_runtime_resource_dir()
	if dir.is_empty():
		_request_resource_dir()
		return
	_enter_menu(dir)
	# Dev/headless convenience: NW_SP_MISSION=<name.bms> boots straight into a single-player
	# mission via the same path as the menu's Start button, so the runtime (and its HUD) can be
	# exercised without menu navigation. Off by default; mirrors the NW_REPLAY direct-launch above.
	var sp_mission := OS.get_environment("NW_SP_MISSION")
	if not sp_mission.is_empty():
		_on_start_requested(sp_mission)
		return
	# Co-op LAN demo hooks (LAN discovery isn't built yet, so there's no server to click):
	# NW_LAN_HOST=<mission.bms> boots straight in as a co-op host on port 32768;
	# NW_LAN_JOIN=<ip[:port]> boots as a joiner dialing that host (mission from NW_LAN_MISSION).
	# Two instances on localhost = the bidirectional co-op demo. Mirrors NW_SP_MISSION above.
	var lan_host := OS.get_environment("NW_LAN_HOST")
	if not lan_host.is_empty():
		# "gametype" = the numeric session g_GameType the host CONFIG chooses at host start
		# [orig: g_GameType = session gametype setting @0x4a6657; ServerConfig_ApplyHostSetting
		# @0x4a6000]. Bit 0x10000 = team-based: it drives the per-side character pick at player
		# add (D-NET-146) and the S2C 0x08 gameType dword. Default 0x10010 = the golden retail
		# ASH_I5A session's value (its 0x08 advertises gameType=65552); NW_LAN_GAMETYPE overrides.
		var lan_gametype := OS.get_environment("NW_LAN_GAMETYPE")
		_on_lan_host_start_requested({
			"mission": lan_host,
			"net_transport": "lan",
			"bind_port": int(OS.get_environment("NW_LAN_PORT")) if not OS.get_environment("NW_LAN_PORT").is_empty() else 32768,
			"game_type": "AS",  # the bring-up target: Advance and Secure on ASH_I5A (gametype 0x10010)
			"gametype": int(lan_gametype) if not lan_gametype.is_empty() else 0x10010,
			"server_name": "DEMOHOST",
			"max_players": 4,
		})
		return
	var lan_join := OS.get_environment("NW_LAN_JOIN")
	if not lan_join.is_empty():
		var jp := lan_join.split(":")
		_on_lan_join_requested({
			"host_ip": jp[0] if jp.size() > 0 else "127.0.0.1",
			"port": int(jp[1]) if jp.size() > 1 else 32768,
			"mission": OS.get_environment("NW_LAN_MISSION"),
			"player_name": _resolve_player_callsign(),
		})


# F9 (re)opens the asset-folder picker from the front-end so the player can point
# the runtime at a different game folder. Restricted to the menu state so an active
# mission is never yanked out from under a remount; ignored while a picker is open.
func _unhandled_key_input(event: InputEvent) -> void:
	var key := event as InputEventKey
	if key == null or not key.pressed or key.echo:
		return
	# F11 fullscreen — the core-engine window concept (NovaWindow), shared with ONED.
	if NovaWindow.is_toggle_event(event):
		NovaWindow.toggle_fullscreen(get_window())
		get_viewport().set_input_as_handled()
		return
	if key.keycode == CHANGE_DIR_KEY and _can_summon_dir_picker():
		_request_resource_dir()
		get_viewport().set_input_as_handled()
		return
	if key.keycode == DEBUG_OVERLAY_KEY:
		_toggle_debug_overlay()
		get_viewport().set_input_as_handled()
		return
	# The gameplay keys (F4 first/third person, C/Z stance) live on the shared
	# LocalPlayerHost — the same host ONED play-in-editor routes to.
	if _player_host != null and _player_host.handle_key_input(event, _state == State.WORLD):
		get_viewport().set_input_as_handled()


# F3: the mission debug overlay over the live runtime. Built lazily; without a
# running mission it just reports so (the runtime source re-resolves per
# refresh, so reloads and menu round-trips never leave it stale).
func _toggle_debug_overlay() -> void:
	if _debug_overlay == null:
		_debug_overlay = DebugOverlayScript.new()
		_debug_overlay.name = "DebugOverlay"
		var host: Node = _hud if _hud != null else self
		host.add_child(_debug_overlay)
		_debug_overlay.set_runtime_source(_current_runtime)
		# The View tab toggles: the overlay only emits intent; we own the world.
		_debug_overlay.skeleton_debug_toggled.connect(_on_skeleton_debug_toggled)
		_debug_overlay.foliage_hidden_toggled.connect(_on_foliage_hidden_toggled)
		_debug_overlay.viewmodel_forced_toggled.connect(_on_viewmodel_forced_toggled)
		_debug_overlay.body_in_first_person_toggled.connect(_on_body_in_first_person_toggled)
	_debug_overlay.toggle()


func _current_runtime():
	return _world.get_runtime() if _world != null else null


# The in-game HUD over the live runtime: built lazily the first frame a mission has a
# local player (so net spectators, which have none, never get it). Reads the witnessed
# hudpos.def layout from the world's mounted VFS and draws under $HUD, so
# _set_hud_visible hides it behind menus. [orig: HUD_RenderAllOverlays @0x5a8070]
func _ensure_game_hud() -> void:
	if _game_hud != null:
		return
	_game_hud = GameHudScript.new()
	_game_hud.name = "GameHud"
	_game_hud.mouse_filter = Control.MOUSE_FILTER_IGNORE
	var host: Node = _hud if _hud != null else self
	host.add_child(_game_hud)
	_game_hud.set_anchors_preset(Control.PRESET_FULL_RECT)
	var hudpos := NovaHudPos.new()
	var root: NovaResourceRoot = _world.get_resource_root() if _world != null and _world.has_method("get_resource_root") else null
	if root == null:
		push_warning("GameHud: world exposed no resource root; the HUD layout cannot load.")
	elif hudpos.load_from_resource_root(root, "hudpos.def") != OK:
		push_warning("GameHud: hudpos.def did not load: %s" % hudpos.get_last_error())
	_game_hud.set_layout(hudpos, root)
	if _world != null and _world.has_signal("mission_effects") and not _world.mission_effects.is_connected(_on_mission_effects):
		_world.mission_effects.connect(_on_mission_effects)


# Rebuild the HUD's per-frame info from the authoritative local player, mirroring the
# original rebuilding its HUD info struct each frame. [orig: HUD_BuildEntityInfo @0x4b8440]
func _update_game_hud() -> void:
	if _state != State.WORLD or not _world.is_loaded():
		return
	if not _world.has_local_player():
		if not _warned_hud_no_player:
			_warned_hud_no_player = true
			push_warning("GameHud: world loaded but has no local player — the in-game HUD will not appear (net spectator, or the mission was not loaded as playable).")
		return
	_ensure_game_hud()
	if _game_hud == null:
		return
	var max_h: int = _world.local_player_max_health()
	var frac := float(_world.local_player_health()) / float(max_h) if max_h > 0 else 0.0
	# Stance from the motor's selected anim-state (crouch/prone is encoded in the clip key).
	var anim_key := _world.local_player_anim_key()
	var stance := 0
	if "prone" in anim_key:
		stance = 2
	elif "crouch" in anim_key:
		stance = 1
	_game_hud.update_info({
		"health_fraction": clampf(frac, 0.0, 1.0),
		"stance": stance,
		"team": _world.local_player_team(),
		"objective": _hud_objective,
	})


# Mission effects feed the HUD's objective/subtitle line (the WAC/mission text the
# original routes to the HUD). Best-effort: pick up any text-bearing effect.
func _on_mission_effects(effects: Array) -> void:
	for e in effects:
		if e is Dictionary:
			var t := String(e.get("text", e.get("message", "")))
			if not t.is_empty():
				_hud_objective = t


func _on_skeleton_debug_toggled(enabled: bool) -> void:
	if _world != null:
		_world.set_skeleton_debug(enabled)


func _on_foliage_hidden_toggled(hidden: bool) -> void:
	if _world != null:
		_world.set_foliage_hidden(hidden)


func _on_viewmodel_forced_toggled(enabled: bool) -> void:
	if _player_host != null:
		_player_host.set_debug_force_viewmodel(enabled)


func _on_body_in_first_person_toggled(enabled: bool) -> void:
	if _player_host != null:
		_player_host.set_debug_body_in_first_person(enabled)


# Whether the folder picker may be summoned right now: only from the menu front-end
# and only when one is not already open. Pure predicate so it is unit-testable
# headless (the native dialog itself cannot be shown without a display).
func _can_summon_dir_picker() -> bool:
	return _state == State.MENU and _picker == null


# --- Menu state ---------------------------------------------------------------

func _enter_menu(dir: String) -> void:
	if _root == null or _root.get_root_dir() != dir:
		var root := _mount_runtime_root(dir)
		if root == null:
			_request_resource_dir()
			return
		_root = root
	_state = State.MENU
	_world.visible = false
	_set_hud_visible(false)
	_wire_host()
	if not _menu_host.setup(_root):
		push_warning("MainGame: no menu found in resource dir (looked for %s)" % _menu_host.main_menu_file)
	_menu_host.show_menu()


func _wire_host() -> void:
	if _host_wired:
		return
	_host_wired = true
	_menu_host.start_requested.connect(_on_start_requested)
	_menu_host.exit_to_desktop_requested.connect(_on_exit_to_desktop)
	_menu_host.return_to_menu_requested.connect(_on_return_to_menu)
	_menu_host.resume_requested.connect(_on_resume)
	if _menu_host.has_signal("novaworld_requested"):
		_menu_host.novaworld_requested.connect(_on_novaworld_requested)
	# The multiplayer menu (mp.mnu) and the PLAYER_INFO character screen (player.mnu) are
	# each driven by a companion the shell delegates to (whichever owns the loaded menu).
	_mp_host = MpMenuHost.new()
	_player_info_host = PlayerInfoMenuHost.new()
	if _menu_host.has_method("add_companion"):
		_menu_host.add_companion(_mp_host)
		_menu_host.add_companion(_player_info_host)
	elif _menu_host.has_method("set_companion"):
		_menu_host.set_companion(_mp_host)
	_mp_host.lan_host_start_requested.connect(_on_lan_host_start_requested)
	_mp_host.lan_join_requested.connect(_on_lan_join_requested)
	_player_info_host.avatar_chosen.connect(_on_avatar_chosen)


# The player pressed OK on the PLAYER_INFO screen. The on-disk player-profile format
# and the in-world soldier appearance (D-PLAYERINFO-1) are not yet ported, so we hold
# the chosen selection in memory as the seam for those later phases rather than invent
# a profile format. See docs/playerinfo/avatars-re.md (ACCEPT / commit, D-PLAYERINFO-9).
func _on_avatar_chosen(profile: Dictionary) -> void:
	_chosen_avatar = profile


# --- Resource dir picker (first launch) ---------------------------------------

func _request_resource_dir() -> void:
	if DisplayServer.get_name() == "headless" or _picker != null:
		return
	_picker = FileDialog.new()
	_picker.file_mode = FileDialog.FILE_MODE_OPEN_DIR
	_picker.access = FileDialog.ACCESS_FILESYSTEM
	_picker.use_native_dialog = true
	_picker.title = "Select your OpenNova asset directory"
	_picker.dir_selected.connect(_on_dir_selected)
	_picker.canceled.connect(_on_dir_canceled)
	add_child(_picker)
	_picker.popup_centered_ratio(0.6)


func _on_dir_selected(dir: String) -> void:
	_cleanup_picker()
	var root := _mount_runtime_root(dir)
	if root == null:
		_request_resource_dir()
		return
	_root = root
	ResourceDirSettings.set_resource_dir(dir)
	_enter_menu(dir)


# Mount `dir` as the runtime resource root (packed PFFs, `/exp` expansion, `/d` loose
# override). Warns and returns null on failure.
func _mount_runtime_root(dir: String) -> NovaResourceRoot:
	var root := NovaResourceRoot.new()
	var expansion := NovaLaunchFlags.expansion(ResourceDirSettings.get_expansion())
	if root.mount_runtime(dir, expansion, NovaLaunchFlags.loose_override_enabled()) != OK:
		push_warning("MainGame: %s" % root.get_last_error())
		return null
	return root


func _on_dir_canceled() -> void:
	_cleanup_picker()
	_request_resource_dir()


func _cleanup_picker() -> void:
	if _picker != null:
		_picker.queue_free()
		_picker = null


# --- NovaWorld (online multiplayer) ------------------------------------------

var _novaworld_panel: NovaWorldPanel

func _on_novaworld_requested() -> void:
	if _novaworld_panel != null:
		return
	_novaworld_panel = NovaWorldPanel.new()
	# Dev default: localhost. A prod build sets the server host from the
	# resolved server IP before showing the panel.
	# Hand the panel the mounted menu root so its host Map picker can list .bms missions (the world's
	# own root is null until a mission loads). Set BEFORE add_child so the panel's _build_ui sees it.
	_novaworld_panel.resource_root = _root
	_menu_host.hide_menu()
	$MenuLayer.add_child(_novaworld_panel)
	_novaworld_panel.closed.connect(_on_novaworld_closed)
	# Bridge the panel's resolved join into the ONE joiner path (the same handler the LAN browser +
	# NW_LAN_JOIN env use); the panel's join dict { host_ip, port, mission, player_name } matches
	# load_mission_as_joiner's row. Hosting from the panel routes through the shared host bring-up.
	_novaworld_panel.join_in_match_requested.connect(_on_novaworld_join_requested)
	_novaworld_panel.host_requested.connect(_on_novaworld_host_requested)


func _on_novaworld_closed() -> void:
	_dismiss_novaworld_panel()
	_menu_host.show_menu()


func _dismiss_novaworld_panel() -> void:
	if _novaworld_panel != null:
		_novaworld_panel.queue_free()
		_novaworld_panel = null


# The NovaWorld panel asked to host. Resolve a mission (the menu's selected one, else the first
# available .bms), fill the callsign, and stand up a browsable listen host through the SAME bring-up
# the mp.mnu host screen uses — the panel supplied the gate (nw_gate_host) + channel=NovaWorld, so
# game_world._maybe_start_nw_host registers it. (A mission picker in the panel is a follow-up.)
func _on_novaworld_host_requested(config: Dictionary) -> void:
	# The panel picks the map; fall back to the first available .bms only if it sent none.
	var mission := String(config.get("mission", ""))
	if mission.is_empty():
		mission = _resolve_default_mission()
	if mission.is_empty():
		# Report back so the panel leaves "Starting..." instead of hanging silently.
		push_warning("MainGame: NovaWorld host requested but no mission is available")
		if _novaworld_panel != null and _novaworld_panel.has_method("host_failed"):
			_novaworld_panel.host_failed("No mission available to host (check the game folder).")
		return
	_dismiss_novaworld_panel()
	config = config.duplicate()
	config["mission"] = mission
	config["net_transport"] = "lan"
	config["bind_port"] = 32768
	config["player_name"] = _resolve_player_callsign()
	config["server_name"] = String(config.get("server_name", "OpenNova Host"))
	_begin_world_load()
	_world.load_mission_as_host(config)


# The NovaWorld panel resolved a join target. Tear down the panel overlay, then enter the match
# through the SAME joiner entry the LAN browser + NW_LAN_JOIN env use (info already carries
# host_ip/port/mission/player_name).
func _on_novaworld_join_requested(info: Dictionary) -> void:
	_dismiss_novaworld_panel()
	_on_lan_join_requested(info)


# A default mission for a panel-initiated host: the mission highlighted in the menu if any, else the
# first .bms the resource root exposes. Empty when no mission is reachable.
func _resolve_default_mission() -> String:
	if _menu_host != null and _menu_host.has_method("get_selected_mission"):
		var sel := String(_menu_host.get_selected_mission())
		if not sel.is_empty():
			return sel
	# The mounted menu root — the world's own root stays null until a mission loads. This is the same
	# object the menu shell + mp host list missions from, and is non-null whenever the panel can open.
	if _root != null and _root.has_method("list_files"):
		for m in _root.list_files(".bms"):
			return String(m).get_file()
	return ""


# --- Menu <-> world transitions ----------------------------------------------

func _on_start_requested(bms_name: String) -> void:
	_begin_world_load()
	_world.load_mission(bms_name)


# Host a LAN co-op game: the same menu->world handoff as a single-player start, but the
# world loads as a listen-server host (ADR 0011) configured from the mp.mnu host screen.
func _on_lan_host_start_requested(config: Dictionary) -> void:
	_begin_world_load()
	# Make the listen host browsable on the NovaWorld gate (F1) when a gate is
	# configured: prod injects NW_GATE_HOST (the resolved gate IP); dev sets it to
	# 127.0.0.1 to test against the local compose. Unset = pure LAN, no registration.
	var gate_host := OS.get_environment("NW_GATE_HOST")
	if not gate_host.is_empty():
		config = config.duplicate()
		config["nw_gate_host"] = gate_host
		var gate_port_env := OS.get_environment("NW_GATE_PORT")
		config["nw_gate_port"] = int(gate_port_env) if gate_port_env.is_valid_int() else NovaWorldSettings.GATE_PORT
		config["player_name"] = _resolve_player_callsign()
	_world.load_mission_as_host(config)


# The player picked a discovered LAN server to join: dial it as a co-op JOINER. Same
# menu->world handoff as a host start; the world loads as a non-authority client that runs
# the witnessed in-match JOIN and renders the host + NPCs wire-direct (net-re §5.38b). The
# server row carries host_ip/port (+ mission, until LAN discovery streams it).
func _on_lan_join_requested(server: Dictionary) -> void:
	_begin_world_load()
	var pname := String(server.get("player_name", _resolve_player_callsign()))
	_world.load_mission_as_joiner(server, pname)


# The local player's callsign — rides the ClientHello.co (the host echoes it back so we
# self-identify by name-match, so any stable value works). NW_LAN_NAME overrides for the
# two-instance demo; a persisted-profile callsign is a follow-up.
func _resolve_player_callsign() -> String:
	var n := OS.get_environment("NW_LAN_NAME")
	return n if not n.is_empty() else "Player"


# Shared menu->world handoff: hide the menu, show the world + HUD, enter WORLD state, and
# connect the load-result signals. The caller then starts the specific load.
func _begin_world_load() -> void:
	_menu_host.hide_menu()
	_world.visible = true
	_set_hud_visible(true)
	_state = State.WORLD
	if not _world.world_loaded.is_connected(_on_world_loaded):
		_world.world_loaded.connect(_on_world_loaded)
	if not _world.load_failed.is_connected(_on_world_load_failed):
		_world.load_failed.connect(_on_world_load_failed)


# Spectate a net session (no menu). The source (replay tool or a real server) is
# at NW_REPLAY="host:port"; the map name comes off the wire, so only the resource
# dir is needed: NW_REPLAY_DIR (else the persisted one), NW_REPLAY_LOOSE for a flat
# extract, and NW_REPLAY_ITEMS as an optional items.def override.
func _enter_net_session() -> void:
	var ep := OS.get_environment("NW_REPLAY")
	var parts := ep.split(":")
	_menu_host.hide_menu()
	_world.visible = true
	_set_hud_visible(true)
	_state = State.WORLD
	if not _world.world_loaded.is_connected(_on_world_loaded):
		_world.world_loaded.connect(_on_world_loaded)
	if not _world.load_failed.is_connected(_on_world_load_failed):
		_world.load_failed.connect(_on_world_load_failed)
	var err := _world.load_net_session({
		"replay_host": parts[0] if parts.size() > 0 else "127.0.0.1",
		"replay_port": int(parts[1]) if parts.size() > 1 else 42000,
		"dir": OS.get_environment("NW_REPLAY_DIR"),
		"loose": not OS.get_environment("NW_REPLAY_LOOSE").is_empty(),
		"items": OS.get_environment("NW_REPLAY_ITEMS"),
		"camera": _camera,
	})
	if err != OK:
		push_warning("MainGame: net session failed to start (%d)" % err)
		return
	# Kill feed over the spectator: reads the same decoded event stream NetEventView
	# draws in 3D, posting kill / objective lines to a top-right HUD feed.
	if _net_killfeed == null:
		_net_killfeed = NetKillFeedScript.new()
		_net_killfeed.name = "NetKillFeed"
		var host: Node = _hud if _hud != null else self
		host.add_child(_net_killfeed)
	_net_killfeed.set_client(_world.get_net_client())


func _on_world_loaded() -> void:
	_menu_host.enter_game_music()


func _on_world_load_failed(reason: String) -> void:
	push_warning("MainGame: mission load failed: %s" % reason)
	if _root != null:
		_enter_menu(_root.get_root_dir())


func _on_camera_escape() -> void:
	# Esc: pause <-> resume while in a world; ignored in the main menu (EXIT quits).
	if _state == State.WORLD:
		_pause()
	elif _state == State.PAUSED:
		_on_resume()


func _pause() -> void:
	_state = State.PAUSED
	_menu_host.open_ingame_menu()  # game.mnu overlay over the kept-loaded world
	_menu_host.show_menu()


func _on_resume() -> void:
	if _state != State.PAUSED:
		return
	_menu_host.hide_menu()
	_state = State.WORLD


func _on_return_to_menu() -> void:
	if _player_host != null:
		_player_host.teardown()
	_world.unload()
	if _player_host != null:
		_player_host.setup(_world, _camera)
	if _net_killfeed != null:
		_net_killfeed.queue_free()
		_net_killfeed = null
	if _game_hud != null:
		if _world != null and _world.has_signal("mission_effects") and _world.mission_effects.is_connected(_on_mission_effects):
			_world.mission_effects.disconnect(_on_mission_effects)
		_game_hud.queue_free()
		_game_hud = null
		_hud_objective = ""
	_warned_hud_no_player = false
	if _root != null:
		_enter_menu(_root.get_root_dir())


func _on_exit_to_desktop() -> void:
	get_tree().quit()


# CanvasLayer contents toggle: hide/show the HUD's CanvasItem children (the FPS
# label + debug label) so they do not draw over the menu.
func _set_hud_visible(v: bool) -> void:
	if _hud == null:
		return
	for c in _hud.get_children():
		if c is CanvasItem:
			(c as CanvasItem).visible = v


# Drive the loaded world's per-frame foliage coverage. Tick whenever a world is
# loaded and not paused (the pause menu freezes it); tick() itself no-ops until the
# world finishes loading. Gating on "loaded, not paused" rather than State.WORLD
# also lets a host that drives load_world() directly (the headless runtime probe,
# which stays in MENU) keep dispatching foliage.
func _process(delta: float) -> void:
	# Release the captured mouse while paused / unloaded so the menus stay usable.
	if _state == State.PAUSED or not _world.is_loaded():
		if Input.get_mouse_mode() == Input.MOUSE_MODE_CAPTURED:
			Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)
		return
	if _player_host != null:
		_player_host.before_world_tick(delta, _state == State.WORLD)
	_world.tick(_camera.global_position, _camera.global_transform, delta)
	if _player_host != null:
		_player_host.after_world_tick()
	_update_game_hud()


# Mouse-look rides the shared LocalPlayerHost (the yaw/pitch witnesses live there);
# the shell only says when the player is live: in-world, loaded, mouse captured.
func _unhandled_input(event: InputEvent) -> void:
	if _player_host != null and _player_host.handle_input(
			event,
			_state == State.WORLD and _world.is_loaded() and Input.get_mouse_mode() == Input.MOUSE_MODE_CAPTURED):
		get_viewport().set_input_as_handled()
