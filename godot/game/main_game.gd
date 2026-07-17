extends Node3D

# Runtime shell: boots into the game's menu front-end (NovaMenuHost, driving the
# .mnu menu set + audio from the chosen resource dir) and hands off to a GameWorld
# when the player starts a mission, with pause + return-to-menu on demand. The
# engine ships no game data; everything (menus, audio, terrain, missions) loads
# from the chosen resource dir. The first-launch directory picker lives here
# (runtime-only); headless probes set the dir explicitly and never block on it.

const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")
const DebugOverlayScript := preload("res://engine/debug/nova_debug_overlay.gd")
const DebugViewContext := preload("res://engine/debug/nova_debug_view_context.gd")
const NetKillFeedScript := preload("res://game/net_killfeed.gd")
const LocalPlayerHostScript := preload("res://engine/world/local_player_host.gd")

# Re-summon the game-folder picker. The original engine has no "change game dir"
# control (the game *is* its install folder); this is an OpenNova convenience so a
# wrong / menu-less folder can be re-picked without restarting. Front-end only.
const CHANGE_DIR_KEY := KEY_F9
# The armory key — the USE-ITEM key (input action 177 "useitem"; retail default =
# SHIFT on the shipped KeyChart, labeled "USE ITEM/ATTACH/ARMORY"). Zone-gated: it
# opens weapon.mnu's WEAPON screen only while the player stands inside a type-6
# armory volume (entity Flags 0x400000, maintained by the collision resolver)
# [orig: Input_HandleActionBinding_0 case 0xB1 @0x4e0b3f ->
# UI_OpenMenuScreen("weapon.mnu", "WEAPON"); the parallel action 218 @0x49b8e3
# ships with no binding row]. Out of zone the key falls through to its use-item
# leg (unported; our motor separately polls Shift as the run modifier).
const ARMORY_KEY := KEY_SHIFT
# The mission debug overlay (entities / sim transport / script variables).
const DEBUG_OVERLAY_KEY := KEY_F3
# ARMORY = the WEAPON screen over LIVE play: the world keeps ticking (the witnessed
# armory runs with no world-stop leg — and under the listen-server model a pausing
# host would freeze every peer), only the mouse is released and player input idles.
# [orig: the useitem armory leg @0x4e0b3f -> UI_OpenMenuScreen("weapon.mnu",
# "WEAPON"); ADR 0009/0011]
enum State { MENU, WORLD, PAUSED, ARMORY }

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
# The in-game HUD rides the SHARED NovaGameHudHost — the same component ONED
# play-in-editor mounts, so both shells run one HUD code path (editor-runtime
# parity). It owns the lazy GameHud build, the per-frame info rebuild, and the
# mission text feed (queued until the HUD exists); this shell only says when the
# player is in-world.
var _hud_host: NovaGameHudHost
var _player_host: LocalPlayerHost = null
var _mp_host  # MpMenuHost: drives the multiplayer (mp.mnu) menu by control name
var _player_info_host  # PlayerInfoMenuHost: drives the PLAYER_INFO (player.mnu) character screen
var _armory_host: NovaArmoryHost  # the SHARED in-world armory surface (weapon.mnu WEAPON)
var _chosen_avatar: Dictionary = {}  # last avatar/name picked on PLAYER_INFO (the persistence seam)
# The mission loading screen (per-mission sidecar image / loadscrn.pcx + the red
# progress bar), mounted over everything for the duration of a world load
# [orig: render_loading_screen @ 0x521d10 + LoadingScreen_UpdateAndPresent @ 0x586be0].
var _loading_screen: NovaLoadingScreen
var _loading_layer: CanvasLayer
var _world_load_pending := false
var _world_load_request_id := 0
# End-of-mission flow (SP): set by the sim's "round_end" effect [orig:
# Server_ProcessRoundEnd @0x5164f0 SP tail]. The world keeps ticking underneath
# (the SP world runs through the epilog — humans >= 1 keeps the run gate open);
# player input idles once the round is over [orig: the post-round input gate —
# the client input uplinks stop against g_spawn_success_gate @0x42c410].
var _round_ended := false
var _end_winner := 0
var _end_screen_delay := 0.0
var _end_screen: MissionEndScreen = null


## True from the menu-to-loading handoff until the world reports success or
## failure. This is the public shell-level observation seam for load lifecycle
## tests and rendered probes (ADR 0018).
func is_world_loading() -> bool:
	return _world_load_pending


## Whether the active loading handoff resolved and decoded its background art.
## This keeps lifecycle probes on the shell's public surface instead of reaching
## into the transient NovaLoadingScreen node.
func has_loading_background() -> bool:
	return _loading_screen != null and _loading_screen.has_background()


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
	# The in-world armory + HUD ride the SHARED hosts — the same components ONED
	# play-in-editor mounts, so both shells run one armory/HUD code path
	# (editor-runtime parity). Created here, not in _wire_host, so the NW_REPLAY
	# spectator path (which never enters the menu) still gets them; the HUD host's
	# setup connects mission_effects before any world can tick (PreMission/WAC
	# effects may drain on the first runtime tick, and it queues them until the
	# lazy HUD exists).
	_armory_host = NovaArmoryHost.new()
	_armory_host.name = "ArmoryHost"
	add_child(_armory_host)
	_armory_host.setup(_world, _player_host, _hud if _hud != null else self)
	_armory_host.opened.connect(func() -> void: _state = State.ARMORY)
	_armory_host.closed.connect(_on_resume)
	_hud_host = NovaGameHudHost.new()
	_hud_host.name = "GameHudHost"
	add_child(_hud_host)
	_hud_host.setup(_world, _player_host, _hud if _hud != null else self)
	# The shell's own round-outcome tap (the HUD host keeps its separate connection
	# for text/banner presentation): "round_end" starts the end-of-mission flow.
	if _world.has_signal("mission_effects") \
			and not _world.mission_effects.is_connected(_on_shell_mission_effects):
		_world.mission_effects.connect(_on_shell_mission_effects)
	# Net-replay connect mode: when NW_REPLAY is set (the env all F5/F6 instances
	# inherit from the editor), skip the menu and dial the replay tool / server
	# directly — each instance gets slotted into a role on connect.
	if not OS.get_environment("NW_REPLAY").is_empty():
		_enter_net_session()
		return
	var dir := ResourceDirSettings.get_resource_dir()
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


# Consume Esc before weapon.mnu's host-wired CANCEL hotkey and FlyCamera can both
# observe it. The menu button has no authored ACTION, so its generic hotkey path
# reports unhandled even after emitting pressed; without this early claim the same
# Esc closes ARMORY and then immediately opens PAUSE.
func _input(event: InputEvent) -> void:
	if _state != State.ARMORY or not (event is InputEventKey):
		return
	var key := event as InputEventKey
	if key.pressed and not key.echo and key.keycode == KEY_ESCAPE:
		_on_resume()
		get_viewport().set_input_as_handled()


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
		toggle_debug_overlay()
		get_viewport().set_input_as_handled()
		return
	# The USE-ITEM key: in-world only. Zone legs first — the armory volume opens
	# weapon.mnu [orig: useitem action 177, Flags & 0x400000 @0x4e0b4d] — otherwise the
	# key is the vehicle mount/dismount toggle on the same witnessed action [orig: the
	# LABEL_121 latch @0x4e0b71 -> Input_ProcessFrame release edge @0x49d6dc ->
	# Entity_ToggleVehicleMount @0x436950]. (The vehicle-loadout-volume vehicle.mnu leg
	# @0x4e0bfe awaits that screen's port.)
	if key.keycode == ARMORY_KEY and _state == State.WORLD:
		if _try_open_armory():
			get_viewport().set_input_as_handled()
		elif _try_toggle_mount():
			get_viewport().set_input_as_handled()
		return
	# The gameplay keys (F4 first/third person, C/Z stance) live on the shared
	# LocalPlayerHost — the same host ONED play-in-editor routes to.
	if _player_host != null and _player_host.handle_key_input(event, _state == State.WORLD):
		get_viewport().set_input_as_handled()


# F3: the mission debug overlay over the live runtime. Built lazily; without a
# running mission it just reports so (the runtime source re-resolves per
# refresh, so reloads and menu round-trips never leave it stale).
func toggle_debug_overlay() -> void:
	if _debug_overlay == null:
		_debug_overlay = DebugOverlayScript.new()
		_debug_overlay.name = "DebugOverlay"
		var host: Node = _hud if _hud != null else self
		host.add_child(_debug_overlay)
		_debug_overlay.set_runtime_source(_current_runtime)
		_debug_overlay.set_view_context_source(_current_player_view_context)
		# The View tab toggles: the overlay only emits intent; we own the world.
		_debug_overlay.skeleton_debug_toggled.connect(_on_skeleton_debug_toggled)
		_debug_overlay.user_points_toggled.connect(_on_user_points_toggled)
		_debug_overlay.collision_debug_toggled.connect(_on_collision_debug_toggled)
		_debug_overlay.foliage_hidden_toggled.connect(_on_foliage_hidden_toggled)
		_debug_overlay.viewmodel_forced_toggled.connect(_on_viewmodel_forced_toggled)
		_debug_overlay.body_in_first_person_toggled.connect(_on_body_in_first_person_toggled)
		_debug_overlay.particles_hidden_toggled.connect(_on_particles_hidden_toggled)
		_debug_overlay.particle_boxes_toggled.connect(_on_particle_boxes_toggled)
		_debug_overlay.occlusion_debug_toggled.connect(_on_occlusion_debug_toggled)
		_debug_overlay.set_effect_world_source(_current_effect_world)
	_debug_overlay.toggle()


func is_debug_overlay_open() -> bool:
	return _debug_overlay != null and is_instance_valid(_debug_overlay) \
			and _debug_overlay.visible


func is_gameplay_input_active() -> bool:
	return _state == State.WORLD and not is_debug_overlay_open() and not _round_ended


# --- End of mission (SP) -------------------------------------------------------

func _on_shell_mission_effects(effects: Array) -> void:
	for e in effects:
		if e is Dictionary and String(e.get("kind", "")) == "round_end":
			_begin_end_of_mission(int(e.get("a", 0)))


func _begin_end_of_mission(winner: int) -> void:
	if _round_ended:
		return
	# The end screen is the SP presentation; the MP post-round flow (scoreboard
	# broadcast + the 2790-tick linger + round cycling) is the net track.
	var sim = _world.get_sim() if _world != null and _world.has_method("get_sim") else null
	if sim != null and bool(sim.get_round_outcome_debug().get("mp_session", false)):
		return
	_round_ended = true
	_end_winner = winner
	# The short beat between the round end and the score/failed screen stands in for
	# the cine lead-in (the lose letterbox+fade, the win flyaway — D-AI-10).
	# [orig: Cine_StartPlayback @0x577840 / Cine_InitPlayback @0x578390]
	_end_screen_delay = 3.0


func _show_end_screen() -> void:
	if _end_screen != null:
		return
	var outcome: Dictionary = {}
	var sim = _world.get_sim() if _world != null and _world.has_method("get_sim") else null
	if sim != null:
		outcome = sim.get_round_outcome_debug()
	if outcome.is_empty():
		outcome = {"ended": true, "winner_team": _end_winner}
	_end_screen = MissionEndScreen.new()
	_end_screen.name = "MissionEndScreen"
	var banner := _hud_host.endround_banner_line() if _hud_host != null else ""
	_end_screen.setup(outcome, banner, _root)
	var host: Node = _hud if _hud != null else self
	host.add_child(_end_screen)
	_end_screen.exit_requested.connect(_on_end_screen_exit)
	Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)


# [orig: g_mission_exit_reason = 1 (ESC / the epilog timeout) -> the main loop pushes
# the "Post Menu" scene @0x526867 — our post-mission menu is the main menu.]
func _on_end_screen_exit() -> void:
	if _world_load_pending:
		return
	_teardown_world_to_menu()


func _current_runtime():
	return _world.get_runtime() if _world != null else null


func _current_player_view_context() -> DebugViewContext:
	var context := DebugViewContext.new()
	if _camera != null and is_instance_valid(_camera):
		context.camera = _camera
	if _player_host != null and is_instance_valid(_player_host) \
			and _player_host.has_method("is_third_person"):
		context.camera_mode_known = true
		context.third_person = bool(_player_host.is_third_person())
	return context


func _current_effect_world():
	return _world.get_effect_world() if _world != null else null


# Mission-effect passthrough + the last-text read seam: the surface lives on the
# shared NovaGameHudHost (queued until the lazy HUD exists); these stay callable
# on the shell for drains routed here and for the parity tests (ADR 0018).
func apply_mission_effects(effects: Array) -> void:
	if _hud_host != null:
		_hud_host.apply_mission_effects(effects)


func hud_objective_line() -> String:
	return _hud_host.hud_objective_line() if _hud_host != null else ""


func _on_skeleton_debug_toggled(enabled: bool) -> void:
	if _world != null:
		_world.set_skeleton_debug(enabled)


func _on_user_points_toggled(enabled: bool) -> void:
	if _world != null:
		_world.set_user_point_debug(enabled)


func _on_collision_debug_toggled(enabled: bool) -> void:
	if _world != null:
		_world.set_collision_debug(enabled)


func _on_foliage_hidden_toggled(hidden: bool) -> void:
	if _world != null:
		_world.set_foliage_hidden(hidden)


func _on_viewmodel_forced_toggled(enabled: bool) -> void:
	if _player_host != null:
		_player_host.set_debug_force_viewmodel(enabled)


func _on_body_in_first_person_toggled(enabled: bool) -> void:
	if _player_host != null:
		_player_host.set_debug_body_in_first_person(enabled)


func _on_particles_hidden_toggled(hidden: bool) -> void:
	if _world != null:
		_world.set_particles_hidden(hidden)


func _on_particle_boxes_toggled(enabled: bool) -> void:
	if _world != null:
		_world.set_particle_debug(enabled)


func _on_occlusion_debug_toggled(enabled: bool) -> void:
	if _world != null:
		_world.set_occlusion_debug(enabled)


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
	# The menu, loading screen, and world are one runtime resource session.
	# GameWorld must not remount from mutable persisted settings after boot.
	_world.set_resource_root(_root)
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
	if _menu_host.has_signal("crosshair_style_changed"):
		_menu_host.crosshair_style_changed.connect(_on_crosshair_style_changed)
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


func _on_crosshair_style_changed(style: int) -> void:
	if _hud_host != null:
		_hud_host.set_crosshair_style(style)


# The armory key while in-world: the shared NovaArmoryHost opens weapon.mnu's
# WEAPON screen over LIVE play when the player stands in an armory zone — the
# world keeps ticking underneath (State.ARMORY rides the host's opened signal)
# [orig: useitem action 177 -> UI_OpenMenuScreen("weapon.mnu", "WEAPON")
# @0x4e0b44, gated on Flags & 0x400000 @0x4e0b4d + the host weapons rule
# (dword_A85B6C, BSS 0 in SP = allowed); no world-stop leg]. Returns false when
# out of zone (key ignored, the original's silent gate). The ACCEPT apply and
# the class/current-loadout open protocol live on the host.
func _try_open_armory() -> bool:
	if _armory_host == null:
		return false
	_armory_host.set_player_team(int(_chosen_avatar.get("team", 0)))
	return _armory_host.try_open()


# The USE-ITEM mount toggle: outside the armory volume the same key enters/exits
# vehicles (deck best-seat, nearest-seat scan, seat-swap-or-detach — all sim-side).
# [orig: Entity_ToggleVehicleMount @0x436950 via the useitem release edge @0x49d6dc]
func _try_toggle_mount() -> bool:
	var runtime = _current_runtime()
	if runtime == null:
		return false
	var sim: NovaSimulation = runtime.get_sim()
	if sim == null:
		return false
	return sim.local_player_toggle_mount()


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
# override, and `/game` SCR policy). Warns and returns null on failure.
func _mount_runtime_root(dir: String) -> NovaResourceRoot:
	var root := NovaResourceRoot.new()
	var expansion := NovaLaunchFlags.expansion(ResourceDirSettings.get_expansion())
	var game := NovaLaunchFlags.game(ResourceDirSettings.get_game())
	if root.mount_runtime(dir, expansion, NovaLaunchFlags.loose_override_enabled(), game) != OK:
		push_warning("MainGame: %s" % root.get_last_error())
		return null
	_report_missing_boot_resources(root)
	return root


# Honest missing-resource errors over the witnessed boot manifest (ENG-6,
# docs/required-resources.md): name each missing fatal-set file with retail's
# witnessed failure behavior instead of dead-ending silently later. Reported,
# not enforced — this shell keeps running so a partial dir stays inspectable
# (the picker flow), where retail shows a MessageBox and exits.
func _report_missing_boot_resources(root: NovaResourceRoot) -> void:
	for name in root.list_missing_boot_resources():
		push_error("MainGame: boot-required resource missing: %s — retail: %s"
				% [name, root.boot_resource_failure_text(name)])


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
	config = config.duplicate(true)
	config["mission"] = mission
	config["net_transport"] = "lan"
	config["bind_port"] = 32768
	config["player_name"] = _resolve_player_callsign()
	config["server_name"] = String(config.get("server_name", "OpenNova Host"))
	_start_world_load({
		"mission_file": mission,
		"in_session": true,
		"server_name": String(config["server_name"]),
		"mission_name": _resolve_mission_title(mission),
		"game_type": int(config.get("gametype", 0)),
		"custom_text": String(config.get("custom_text", "")),
	}, Callable(_world, "load_mission_as_host").bind(config))


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
	# Single-player: the loading screen is the sidecar image alone — no session
	# text [orig: the not-in-session path draws only the background @ 0x521ebe].
	_start_world_load(
		{"mission_file": bms_name},
		Callable(_world, "load_mission").bind(bms_name))


# Host a LAN co-op game: the same menu->world handoff as a single-player start, but the
# world loads as a listen-server host (ADR 0011) configured from the mp.mnu host screen.
func _on_lan_host_start_requested(config: Dictionary) -> void:
	config = config.duplicate(true)
	var host_mission := String(config.get("mission", ""))
	var load_info := {
		"mission_file": host_mission,
		"in_session": true,
		"server_name": String(config.get("server_name", "")),
		"mission_name": _resolve_mission_title(host_mission),
		"game_type": int(config.get("gametype", 0)),
		"custom_text": String(config.get("custom_text", "")),
	}
	# Make the listen host browsable on the NovaWorld gate (F1) when a gate is
	# configured: prod injects NW_GATE_HOST (the resolved gate IP); dev sets it to
	# 127.0.0.1 to test against the local compose. Unset = pure LAN, no registration.
	var gate_host := OS.get_environment("NW_GATE_HOST")
	if not gate_host.is_empty():
		config["nw_gate_host"] = gate_host
		var gate_port_env := OS.get_environment("NW_GATE_PORT")
		config["nw_gate_port"] = int(gate_port_env) if gate_port_env.is_valid_int() else NovaWorldSettings.GATE_PORT
		config["player_name"] = _resolve_player_callsign()
	_start_world_load(
		load_info,
		Callable(_world, "load_mission_as_host").bind(config))


# The player picked a discovered LAN server to join: dial it as a co-op JOINER. Same
# menu->world handoff as a host start; the world loads as a non-authority client that runs
# the witnessed in-match JOIN and renders the host + NPCs wire-direct (net-re §5.38b). The
# server row carries host_ip/port (+ mission, until LAN discovery streams it).
func _on_lan_join_requested(server: Dictionary) -> void:
	server = server.duplicate(true)
	# Joiner: the retail client has the full session-variable set from the
	# connect stream by load time [orig: parse_server_session_variables
	# @ 0x5202f0]; our LAN row carries only mission (+ maybe a server name), so
	# absent vars stay blank and game_type -1 leaves the game-type line empty.
	# Retail also HOLDS the screen through TWO blocking waits after the local
	# load — NapiClient_WaitForDisconnect @ 0x42cb20 (connect handshake) then
	# NapiClient_WaitForGameStart @ 0x42cc10 (spawn gate g_spawn_success_gate,
	# S2C 0x1D) — revealing only on the spawn leg; we drop the screen when the
	# local load lands and run the handshake in-world (the wire spawn gate is
	# not surfaced above libs/npwire) (docs/interface/loading-screen-re.md
	# D-LOADSCR-3, the load-flow case matrix).
	var load_info := {
		"mission_file": String(server.get("mission", "")),
		"in_session": true,
		"server_name": String(server.get("server_name", String(server.get("name", "")))),
		"game_type": int(server.get("gametype", -1)),
	}
	var pname := String(server.get("player_name", _resolve_player_callsign()))
	_start_world_load(
		load_info,
		Callable(_world, "load_mission_as_joiner").bind(server, pname))


# The local player's callsign — rides the ClientHello.co (the host echoes it back so we
# self-identify by name-match, so any stable value works). NW_LAN_NAME overrides for the
# two-instance demo; a persisted-profile callsign is a follow-up.
func _resolve_player_callsign() -> String:
	var n := OS.get_environment("NW_LAN_NAME")
	return n if not n.is_empty() else "Player"


# Shared menu->world handoff: hide the menu, raise the loading screen, enter WORLD
# state, and connect the load-result signals. The caller then starts the specific
# load. The world + HUD stay hidden until the load lands — during the load only
# the loading screen presents [orig: Game_StartMission renders via
# render_loading_screen @ 0x521d10 / LoadingScreen_UpdateAndPresent @ 0x586be0
# until LoadingScreen_ReleaseEffect @ 0x525d52 at the end of the load].
# `load_info` feeds the screen: mission_file, and for a net session the session
# variables (in_session, server_name, mission_name, game_type, custom_text)
# [orig: the SERVERNAME/MISSIONNAME/GAMETYPE/CUSTOMTEXT session vars @ 0x5202f0].
func _start_world_load(load_info: Dictionary, operation: Callable) -> void:
	if _world_load_pending:
		return
	_world_load_pending = true
	_world_load_request_id += 1
	var request_id := _world_load_request_id
	_begin_world_load(load_info.duplicate(true))
	_run_world_load(request_id, operation)


# The loader APIs are synchronous, so mounting a Control and immediately calling
# one blocks the SceneTree before that Control can finish a frame. Let the screen
# cross its completed-frame barrier, then enter the load.
func _run_world_load(request_id: int, operation: Callable) -> void:
	var screen := _loading_screen
	if screen != null:
		var prepared := await screen.prepare_for_blocking_load()
		if request_id != _world_load_request_id or not _world_load_pending:
			return
		if not prepared:
			_on_world_load_failed("loading screen left the SceneTree before mission load")
			return
	else:
		await get_tree().process_frame
		if request_id != _world_load_request_id or not _world_load_pending:
			return
	var result = operation.call()
	var err := int(result) if result != null else OK
	# GameWorld normally emits load_failed before returning an error. Preserve a
	# deterministic rollback for any implementation that returns without emitting.
	if err != OK and request_id == _world_load_request_id and _world_load_pending:
		_on_world_load_failed(error_string(err))


func _begin_world_load(load_info: Dictionary = {}) -> void:
	_menu_host.hide_menu()
	_world.visible = false
	_set_hud_visible(false)
	_state = State.WORLD
	if not _world.world_loaded.is_connected(_on_world_loaded):
		_world.world_loaded.connect(_on_world_loaded)
	if not _world.load_failed.is_connected(_on_world_load_failed):
		_world.load_failed.connect(_on_world_load_failed)
	_show_loading_screen(load_info)


# Build and present the loading screen for this load. A missing background image
# leaves the screen dark, exactly like the original's texture-miss path (no
# draw at all) [orig: tex_data_ptr null -> return @ 0x521eb0].
func _show_loading_screen(load_info: Dictionary) -> void:
	_dismiss_loading_screen()
	if _root == null:
		return
	if _loading_layer == null:
		_loading_layer = CanvasLayer.new()
		_loading_layer.name = "LoadingLayer"
		_loading_layer.layer = 3  # above MenuLayer (2): nothing overdraws the load
		add_child(_loading_layer)
	_loading_screen = NovaLoadingScreen.new()
	_loading_screen.name = "LoadingScreen"
	_loading_layer.add_child(_loading_screen)
	_loading_screen.setup(_root, load_info)
	# CanvasLayer is not a Control parent, so full-rect anchors have no layout
	# rectangle to resolve against. Use top-left anchors before assigning the
	# viewport size; changing size under full-rect anchors emits a Godot warning.
	_loading_screen.set_anchors_preset(Control.PRESET_TOP_LEFT)
	_loading_screen.position = Vector2.ZERO
	_loading_screen.size = _loading_screen.get_viewport_rect().size
	if not _world.load_progress.is_connected(_on_load_progress):
		_world.load_progress.connect(_on_load_progress)


func _on_load_progress(percent: int) -> void:
	if _loading_screen != null:
		_loading_screen.set_progress(percent)
		_loading_screen.present()


func _dismiss_loading_screen() -> void:
	if _world != null and _world.load_progress.is_connected(_on_load_progress):
		_world.load_progress.disconnect(_on_load_progress)
	if _loading_screen != null:
		_loading_screen.queue_free()
		_loading_screen = null


# MISSIONNAME for the loading screen = the mission text .bin's [info]/title
# [orig: serialize_mission_info_to_datastream @ 0x523620 ->
# TextResource_FindEntryBySectionAndKey(g_TextMission, "info", "title"); an
# empty title falls back to the mission-header title]. Our fallback: the
# mission basename.
func _resolve_mission_title(bms_name: String) -> String:
	var base := bms_name.get_file().get_basename()
	if _root == null:
		return base
	var bytes := _root.read_file(base + ".bin")
	if bytes.is_empty():
		return base
	var table := RtxtStringFile.new()
	if table.load_from_byte_array(bytes) != OK:
		return base
	if not table.has_string_in_section("info", "title"):
		return base
	var title := table.get_string_in_section("info", "title")
	return title if not title.is_empty() else base


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
	# The GAME music context is the world's to open at mission start (GameWorld
	# calls NovaMusicService.open_game_context — host-neutral, so ONED play gets
	# the same music); nothing to do here for audio. Drop the loading screen and
	# reveal the world + HUD — the witnessed release-then-enter-game at the tail
	# of Game_StartMission [orig: LoadingScreen_ReleaseEffect @ 0x586b80, final
	# call @ 0x525d45]. For SP/host this fires at true load completion (parity).
	# For the JOINER it fires at LOCAL-load completion, one wire spawn gate too
	# early (D-LOADSCR-3). The SP start-mission arrow splash (newarow1.tga +
	# START_MISSION), gated !is_multiplayer_session && g_loadscreen_has_custom_bg
	# && !is_in_session, is a follow-up [orig: show_start_mission_splash
	# @ 0x520820, called @ 0x525d42] (docs/interface/loading-screen-re.md
	# D-LOADSCR-4, the load-flow case matrix).
	_world_load_pending = false
	_dismiss_loading_screen()
	_world.visible = true
	_set_hud_visible(true)


func _on_world_load_failed(reason: String) -> void:
	# A load-step failure or abort returns to the menu — the witnessed early
	# return that sets reason=1 and nav-pushes "Post Menu" out of
	# Game_StartMission [orig: the "Mission loading aborted" legs @ 0x520270
	# (Client_CheckDisconnectOrEscDuringLoad) and the network-wait failure legs;
	# scene_entry = "Post Menu"] (docs/interface/loading-screen-re.md, the
	# load-flow case matrix).
	_world_load_pending = false
	push_warning("MainGame: mission load failed: %s" % reason)
	_teardown_world_to_menu()


func _on_camera_escape() -> void:
	# Esc: pause <-> resume while in a world (the armory closes back to play);
	# ignored in the main menu (EXIT quits). During a load Esc is inert: our
	# SP/host load is a single synchronous call the SceneTree cannot interrupt,
	# so there is no reachable analog of the original's per-asset ESC/disconnect
	# abort poll [orig: Client_CheckDisconnectOrEscDuringLoad @ 0x520270]
	# (docs/interface/loading-screen-re.md D-LOADSCR-7).
	if _world_load_pending:
		return
	# Round over: ESC leaves the mission instead of pausing [orig: ESC (0x1B) sets
	# g_mission_exit_reason = 1 during the epilog, Input_HandleSpecialKeys @0x49c8e2].
	if _round_ended:
		if _end_screen != null:
			_end_screen.request_exit()
		else:
			_on_end_screen_exit()
		return
	if _state == State.WORLD:
		_pause()
	elif _state == State.PAUSED or _state == State.ARMORY:
		_on_resume()


func _pause() -> void:
	_state = State.PAUSED
	_menu_host.open_ingame_menu()  # game.mnu overlay over the kept-loaded world
	_menu_host.show_menu()


func _on_resume() -> void:
	if _state != State.PAUSED and _state != State.ARMORY:
		return
	if _armory_host != null and _armory_host.is_open():
		_armory_host.close()  # Esc from ARMORY closes the overlay (no re-entry: closed
		                      # only fires while open)
	_menu_host.hide_menu()
	_state = State.WORLD


func _on_return_to_menu() -> void:
	if _world_load_pending:
		return
	_teardown_world_to_menu()


# One idempotent rollback for a normal return and every load failure. Runtime
# hosts keep references to the old world/root, so their teardown order is part
# of the shell boundary rather than a menu-specific detail.
func _teardown_world_to_menu() -> void:
	_dismiss_loading_screen()
	_round_ended = false
	_end_winner = 0
	_end_screen_delay = 0.0
	if _end_screen != null:
		_end_screen.queue_free()
		_end_screen = null
	if _player_host != null:
		_player_host.teardown()
	if _armory_host != null:
		_armory_host.teardown()  # the built menu holds the OLD world's resource root
	_world.unload()
	if _player_host != null:
		_player_host.setup(_world, _camera)
	if _net_killfeed != null:
		_net_killfeed.queue_free()
		_net_killfeed = null
	if _hud_host != null:
		_hud_host.teardown()
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
	var debug_overlay_open := is_debug_overlay_open()
	# Release the captured mouse while UI overlays the world or nothing is loaded.
	if _state == State.PAUSED or _state == State.ARMORY or debug_overlay_open \
			or _end_screen != null or not _world.is_loaded():
		if Input.get_mouse_mode() == Input.MOUSE_MODE_CAPTURED:
			Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)
	# The end-of-mission lead-in: the world keeps ticking; the score/failed screen
	# mounts after the short beat [orig: the SP world runs through the epilog cine].
	if _round_ended and _end_screen == null and _world.is_loaded():
		_end_screen_delay -= delta
		if _end_screen_delay <= 0.0:
			_show_end_screen()
	# Only the pause menu freezes the world. The armory runs over LIVE play: the
	# match keeps simulating around the player while the WEAPON screen is up
	# [orig: the useitem armory leg @0x4e0b3f has no world-stop leg; input idles
	# because player_live below is false outside State.WORLD].
	if _state == State.PAUSED or not _world.is_loaded():
		return
	if _player_host != null:
		var player_live := is_gameplay_input_active()
		_player_host.before_world_tick(delta, player_live, player_live)
	_world.tick(_camera.global_position, _camera.global_transform, delta)
	if _player_host != null:
		_player_host.after_world_tick()
	# The shared HUD host rebuilds the per-frame info while the player is in-world
	# (WORLD or the live-play ARMORY) [orig: HUD_BuildEntityInfo @0x4b8440 per frame].
	if _hud_host != null and (_state == State.WORLD or _state == State.ARMORY):
		_hud_host.tick()


# Mouse-look rides the shared LocalPlayerHost (the yaw/pitch witnesses live there);
# the shell only says when the player is live: in-world, loaded, mouse captured.
func _unhandled_input(event: InputEvent) -> void:
	if _player_host != null and _player_host.handle_input(
			event,
			_state == State.WORLD and _world.is_loaded() and Input.get_mouse_mode() == Input.MOUSE_MODE_CAPTURED):
		get_viewport().set_input_as_handled()
