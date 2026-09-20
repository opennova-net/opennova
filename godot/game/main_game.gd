class_name MainGame
extends GameShell

# Runtime shell: boots into the game's menu front-end (MenuShell, driving the
# .mnu menu set + audio from the chosen resource dir) and hands off to a GameWorld
# when the player starts a mission, with pause + return-to-menu on demand. The
# engine ships no game data; everything (menus, audio, terrain, missions) loads
# from the required --resource-dir supplied at launch.

const PlayerOptionsScript := preload("res://game/player_options.gd")
const GameDebugAdapterScript := preload("res://game/game_debug_adapter.gd")
const WorldLoadCoordinatorScript := preload("res://game/world_load_coordinator.gd")
const ShellPresentationSessionScript := preload("res://game/shell_presentation_session.gd")
const HudHiddenCaptureWitness := preload("res://game/world/hud_hidden_capture_witness.gd")
# The HUD presenter's gameplay keys (objectives/friendly-tags) live with the
# presenter — GameHudPresenter.handle_gameplay_key.
# The armory key — the USE-ITEM key (input action 177 "useitem"; retail default =
# SHIFT on the shipped KeyChart, labeled "USE ITEM/ATTACH/ARMORY"). Zone-gated: it
# opens weapon.mnu's WEAPON screen only while the player stands inside a type-6
# armory volume (entity Flags 0x400000, maintained by the collision resolver)
# [orig: Input_HandleActionBinding_0 case 0xB1 @0x4e0b3f ->
# UI_OpenMenuScreen("weapon.mnu", "WEAPON"); the parallel action 218 @0x49b8e3
# ships with no binding row]. Out of zone the key's other arms are the input
# router's per-frame chain over the polled `useitem` row (the hold latch, the
# USE+digit seat pick, the mount toggle on release); the shell only matches
# the press event against that row's live keys (_is_use_item_key).
# F3: the in-engine dev tools (the DevTools node's ImGui windows, ADR 0039).
const DEV_TOOLS_KEY := KEY_F3
# Shift+F6: pick the entity under the crosshair into the debug pick list
# (DebugPickSession). Works while playing or with F3 in Interact. Unmodified F6 stays
# with the retail-configurable binding rows (huddetail's default, shadowing
# hudcolor's — the retail first-match order, D-CTRL-4).
const PICK_KEY := KEY_F6
# ARMORY = the WEAPON screen over LIVE play: the world keeps ticking (the witnessed
# armory runs with no world-stop leg — and under the listen-server model a pausing
# host would freeze every peer), only the mouse is released and player input idles.
# [orig: the useitem armory leg @0x4e0b3f -> UI_OpenMenuScreen("weapon.mnu",
# "WEAPON"); ADR 0009/0011]
# DEPLOY = the joiner's death.mnu DEATH screen over LIVE play, on the same terms
# as ARMORY: the world (and therefore the session socket) keeps ticking, only the
# mouse is released and player input idles. Retail's dead player has no gameplay
# input anyway — the uplink is held by dword_81474C and the input legs gate on
# g_spawn_success_gate — so this state is what makes the spawn list clickable.
enum State { MENU, WORLD, PAUSED, ARMORY, DEPLOY, END_ROUND }

@onready var _world: GameWorld = $World
@onready var _camera: FlyCamera = $Camera3D
@onready var _hud: CanvasLayer = $HUD
@onready var _menu_layer: CanvasLayer = $MenuLayer
@onready var _menu_shell: MenuShell = $MenuLayer/MenuShell

var _root: ResourceRoot
var _state: int = State.MENU
var _shell_wired := false
# The in-engine dev tools' seam: F3 opens them, the mouse policy follows them.
var _dev_tools: DevTools
var _debug_adapter: GameDebugAdapter
# The debug pick state (list, toast flow, click-catcher latch): SHELL-owned so
# F6 picks work before F3 ever opens; every landed pick selects its Entities row.
var _pick_session := DebugPickSession.new()
var _net: NetSessionController  # every net-session entry (LAN/NovaWorld + env hooks)
# The in-game HUD rides GameHudPresenter. It owns the lazy GameHud build, the
# per-frame info rebuild, and the
# mission text feed (queued until the HUD exists); this shell only says when the
# player is in-world.
var _hud_presenter: GameHudPresenter
var _player_presenter: LocalPlayerPresenter = null
# The per-system frame-stats board behind the dev tools' Stats window. Created
# with the shell and handed to every feeding owner; it costs nothing until that
# window opens (capture stays inactive, every feed site gates on it).
var _frame_stats := FrameStats.new()
# Root-viewport render-time sampling for the Stats window; the sampler owns the
# RenderingServer measurement edge latch and the wall-frame clock.
var _render_stats := RootRenderStatsSampler.new()
var _frame_phase_sampler := RootFramePhaseSampler.new()
var _mp_companion: MpMenuCompanion  # drives the multiplayer (mp.mnu) menu by control name
var _lan_session: LanSession  # retail-style 0x41/0x81 LAN enumeration browser
var _player_info_companion: PlayerInfoMenuCompanion  # drives the PLAYER_INFO (player.mnu) character screen
var _armory_presenter: ArmoryPresenter  # the SHARED in-world armory surface (weapon.mnu WEAPON)
var _deploy_presenter: DeployScreenPresenter  # the joiner's deploy-map screen (death.mnu DEATH)
var _end_round_presenter: EndRoundPresenter  # the MP end-of-round overlay + stat.mnu STAT
var _chosen_avatar: Dictionary = {}  # canonical active + per-side PLAYER_INFO selection
var _profile_root_key := ""  # reload weapon.sav only when the mounted game/expansion changes
var _world_load := WorldLoadCoordinatorScript.new()
var _world_load_pending := false
var _end_flow := MissionEndFlow.new()  # the SP end-of-mission flow (round_end -> score screen)
var _shutdown_prepared := false
var _shutdown_resources_released := false
var _quit_requested := false
var _quit_drain_pending := false
var _quit_policy_installed := false
var _previous_auto_accept_quit := true
var _shell_presentation := ShellPresentationSessionScript.new()
# One process-lifetime settings owner feeds both menu surfaces and every world
# or HUD instance constructed during this shell session.
var _player_options: PlayerOptions = PlayerOptionsScript.new()


func _init() -> void:
	_player_options.changed.connect(_on_player_options_changed)
	# The sampler observes the board's capture close edge directly (render-time
	# measurement is RenderingServer state, not Node-owned state).
	_render_stats.setup(_frame_stats)
	_frame_phase_sampler.setup(_frame_stats)
	add_child(_frame_phase_sampler)
	_dev_tools = DevTools.new()
	_dev_tools.name = "DevTools"
	_dev_tools.set_frame_stats(_frame_stats)
	_dev_tools.open_changed.connect(_on_dev_tools_open_changed)
	_dev_tools.game_input_mode_changed.connect(_on_dev_tools_game_input_mode_changed)
	add_child(_dev_tools)
	_pick_session.setup(_dev_tools)
	_world_load.load_failed.connect(_on_world_load_failed)


func _on_player_options_changed(state: PlayerOptions.State) -> void:
	# update() applied the device-global audio once already; only the running
	# Simulation's mouse settings are this listener's to push.
	var sim: Simulation = _world.get_sim() if _world != null else null
	_player_options.apply_mouse(sim)
	if _hud_presenter != null:
		_hud_presenter.set_crosshair_style(state.crosshair_style)
		_hud_presenter.set_crosshair_color(state.crosshair_color)
		_hud_presenter.set_crosshair_spread_enabled(state.crosshair_spread)
		_hud_presenter.set_aspect_mode(state.aspect_mode)


func _notification(what: int) -> void:
	if what == NOTIFICATION_WM_CLOSE_REQUEST:
		request_quit()
	elif what == NOTIFICATION_EXIT_TREE:
		begin_runtime_shutdown()
		finish_runtime_shutdown()
		_restore_quit_policy()
		_render_stats.stop()


## Synchronously invalidate mission ownership and request cancellation of the
## active load. The returned operation settles only after its awaiting stack has
## released the loading screen and bound Callable references.
func begin_runtime_shutdown() -> WorldLoadOperation:
	if _shutdown_prepared:
		return _world_load.current_operation()
	_shutdown_prepared = true
	var load_operation := _world_load.cancel_current()
	_world_load_pending = false
	finish_hud_hidden_capture()
	for presenter in [
		_player_presenter, _armory_presenter, _deploy_presenter, _hud_presenter,
	]:
		if presenter != null:
			presenter.teardown()
	if _world != null:
		_world.cancel_join_preload()
		_world.cancel_join_admission()
		# The F3 windows' Simulation dies with the runtime unload frees.
		_dev_tools.set_simulation(null)
		_world.unload()
	MusicService.stop_context()
	Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)
	return load_operation


## Idempotent renderer/resource release. Normal quit calls this only after the
## active WorldLoadOperation settles; EXIT_TREE uses it as a synchronous fallback.
func finish_runtime_shutdown() -> void:
	if _shutdown_resources_released:
		return
	begin_runtime_shutdown()
	_shutdown_resources_released = true
	_world_load.dismiss()
	if is_instance_valid(_menu_shell):
		_menu_shell.release_runtime_renderer_resources()
	else:
		Input.set_custom_mouse_cursor(null, Input.CURSOR_ARROW)
	if _world != null:
		_world.release_runtime_renderer_resources()
	if _root != null:
		_root.clear()
	# The vegetation asset caches live on the world's foliage dispatcher for
	# the world's whole life; the shell's exit empties them here.
	if _world != null:
		var dispatcher: FoliageDispatcher = _world.get_foliage_dispatcher()
		if dispatcher != null:
			dispatcher.clear_asset_cache()
	if _debug_adapter != null:
		_dev_tools.set_debug_control_table(null)
		_debug_adapter.release_shell()


## True from the menu-to-loading handoff until the world reports success or
## failure. This is the public shell-level observation seam for load lifecycle
## tests and rendered probes (ADR 0018).
func is_world_loading() -> bool:
	return _world_load_pending


## Whether the active loading handoff resolved and decoded its background art.
## This keeps lifecycle probes on the shell's public surface instead of reaching
## into the transient LoadingScreen node.
func has_loading_background() -> bool:
	return _world_load.has_background()


## The active loading screen's exact real-stage checkpoint, or -1 after the
## presentation has been released.
func loading_progress_percent() -> int:
	return _world_load.progress_percent()


## Pixel extent of the active loading surface, or (-1, -1) after release.
## This is the public render-probe seam for fullscreen coverage (ADR 0018).
func loading_surface_size() -> Vector2i:
	return _world_load.surface_size()


## Dismiss an active SP start-mission splash without manufacturing a key or
## mouse event. The coordinator preserves the normal closing-frame/reveal
## sequence, so rendered probes enter gameplay through the production seam.
func dismiss_start_mission_splash() -> bool: return _world_load.dismiss_start_mission_splash()


## The mounted menu/runtime resource root (null before the first mount) —
## so shell components (NetSessionController) and lifecycle tests resolve
## missions/titles through one seam instead of shell internals.
func current_resource_root() -> ResourceRoot:
	return _root


func _ready() -> void:
	var dir := LaunchFlags.resource_dir()
	if dir.is_empty():
		push_warning("OpenNova requires game data. Usage: opennova.exe -- --resource-dir <path> [--loose-root] [/d]")
		get_tree().quit(2)
		return
	_previous_auto_accept_quit = get_tree().auto_accept_quit
	get_tree().auto_accept_quit = false
	_quit_policy_installed = true
	_player_options.apply()
	if _world == null or _camera == null or _menu_shell == null:
		return
	_menu_shell.set_player_options(_player_options)
	var debug_adapter := get_game_debug_adapter()
	add_child(debug_adapter)
	debug_adapter.start_runtime_endpoint()
	# F3 drives the SAME debug-control table MCP's game_debug does (ADR 0043
	# d12): the windows' control requests drain into it with the shell's
	# local authority.
	_dev_tools.set_debug_control_table(debug_adapter.get_debug_controls())
	# Esc toggles pause/resume in a world (the fly camera reports the key; the
	# owner decides what it means).
	if not _camera.escape_pressed.is_connected(_on_camera_escape):
		_camera.escape_pressed.connect(_on_camera_escape)
	_player_presenter = LocalPlayerPresenter.new()
	_player_presenter.name = "LocalPlayerPresenter"
	add_child(_player_presenter)
	# setup binds the presenter as the world's local view presenter (the
	# D-RORD-8 view leg + the fixed-tick weapon drain); the live binding table
	# is the shell's ControlsBindings model.
	_player_presenter.setup(_world, _camera, _camera, ControlsBindings.model())
	# The in-world armory + HUD ride their shared engine presenters. Created here,
	# not in _wire_shell, so menu-less entries (the env launch hooks)
	# still get them; the HUD presenter's
	# setup connects mission_effects before any world can tick (PreMission/WAC
	# effects may drain on the first runtime tick, and it queues them until the
	# lazy HUD exists).
	_armory_presenter = ArmoryPresenter.new()
	_armory_presenter.name = "ArmoryPresenter"
	add_child(_armory_presenter)
	_armory_presenter.setup(_world.armory_view(), _player_presenter,
			_hud if _hud != null else self)
	_armory_presenter.opened.connect(func() -> void: _state = State.ARMORY)
	_armory_presenter.closed.connect(resume)
	# The joiner's deploy-map screen (death.mnu DEATH; net-re 5.61) owns the cursor.
	_deploy_presenter = DeployScreenPresenter.install(self, _world.world_view(),
			_hud if _hud != null else self, func() -> void: _state = State.DEPLOY,
			_leave_screen.bind(State.DEPLOY))
	_world.join_deploy_pick_required.connect(_on_join_deploy_pick_required)
	_world.join_admission_ready.connect(_on_join_admission_ready)
	_world.session_lost.connect(_on_session_lost)
	# The one interactive-music context is the shell's (MusicService); the
	# world names the mission-start open, the mission-end teardown and the
	# per-frame gamemus var pump through these three signals.
	_world.set_music_director(MusicService.director())
	_world.music_context_opened.connect(MusicService.open_game_context)
	_world.music_context_closed.connect(MusicService.stop_context)
	_world.music_var_changed.connect(MusicService.set_var)
	# The explicit launch directory and current game/expansion selection.
	_world.set_resource_root_resolver(LaunchResourceRootResolver.new())
	_hud_presenter = GameHudPresenter.new()
	_hud_presenter.name = "GameHudPresenter"
	add_child(_hud_presenter)
	_hud_presenter.setup(_world, _player_presenter, _hud if _hud != null else self)
	_on_player_options_changed(_player_options.current())
	# The MP end-of-round flow (net-re 5.68; HUD_DrawOverlayPanels @0x5c0072): STAT owns the cursor.
	_end_round_presenter = EndRoundPresenter.install(self, _world.world_view(),
			_hud if _hud != null else self, _hud_presenter, _deploy_presenter,
			_armory_presenter, func() -> void: _state = State.END_ROUND,
			_leave_screen.bind(State.END_ROUND))
	# The STAT confirm's Yes exits the mission (the pause menu's same
	# CONFIRM_YES command), riding the guarded return-to-menu teardown.
	_end_round_presenter.exit_to_menu_requested.connect(_on_return_to_menu)
	# Every net-session ENTRY (LAN browser/host, NovaWorld panel + env hooks)
	# lives on the NetSessionController component; the shell keeps the state
	# machine, the load pipeline, and the session-presentation states.
	_net = NetSessionController.new()
	_net.name = "NetSessionController"
	add_child(_net)
	_net.setup(self, _world, _menu_shell, $MenuLayer)
	# One shared frame-stats board across the shell, the world, and the HUD
	# presenter; the world re-hands it to each mission runtime it creates.
	_world.set_frame_stats(_frame_stats)
	_hud_presenter.set_frame_stats(_frame_stats)
	_menu_shell.set_frame_stats(_frame_stats)
	# The shell's own round-outcome tap (the HUD presenter keeps its separate connection
	# for text/banner presentation): "round_end" starts the end-of-mission flow.
	if not _world.mission_effects.is_connected(_on_shell_mission_effects):
		_world.mission_effects.connect(_on_shell_mission_effects)
	if not _enter_menu(dir):
		get_tree().quit(1)
		return
	# F6 is still the real standalone game and normal loading presentation; it
	# only selects the exact saved top-level loose BMS instead of an archive row.
	var loose_mission := LaunchFlags.loose_mission()
	if not loose_mission.is_empty():
		start_loose_mission(loose_mission)
		return
	# Dev/probe launches: `--mission <name.bms>` (LaunchFlags) boots straight into a
	# single-player mission via the same path as the menu's Start button, so the
	# runtime (and its HUD) can be exercised without menu navigation. Off by
	# default; a post-spawn pose rides the game_debug teleport action over MCP.
	var sp_mission := LaunchFlags.mission()
	if not sp_mission.is_empty():
		_on_start_requested(sp_mission)
		return
	# Co-op LAN launches (`--lan-host` / `--lan-join`) ride the controller.
	# Mirrors `--mission` above; two instances on localhost = the co-op demo.
	_net.maybe_launch_lan_from_flags()


# Consume Esc before weapon.mnu's shell-wired CANCEL hotkey and FlyCamera can both
# observe it. The menu button has no authored ACTION, so its generic hotkey path
# reports unhandled even after emitting pressed; without this early claim the same
# Esc closes ARMORY and then immediately opens PAUSE.
func _input(event: InputEvent) -> void:
	if event is InputEventKey:
		var tools_key := event as InputEventKey
		if tools_key.pressed and not tools_key.echo and tools_key.keycode == KEY_ESCAPE \
				and _dev_tools.handle_game_escape():
			get_viewport().set_input_as_handled()
			return
	if _state != State.ARMORY or not (event is InputEventKey):
		return
	var key := event as InputEventKey
	if key.pressed and not key.echo and key.keycode == KEY_ESCAPE:
		resume()
		get_viewport().set_input_as_handled()


# F9 (re)opens the asset-folder picker from the front-end so the player can point
# the runtime at a different game folder. Restricted to the menu state so an active
# mission is never yanked out from under a remount; ignored while a picker is open.
func _unhandled_key_input(event: InputEvent) -> void:
	var key := event as InputEventKey
	if key == null:
		return
	if not key.pressed or key.echo:
		return
	# F11 fullscreen uses the shared runtime window policy.
	if WindowState.is_toggle_event(event):
		_toggle_fullscreen()
		get_viewport().set_input_as_handled()
		return
	if key.keycode == DEV_TOOLS_KEY:
		if _dev_tools.handle_tools_toggle():
			get_viewport().set_input_as_handled()
		return
	if key.keycode == PICK_KEY and key.shift_pressed and _world != null \
			and _world.is_loaded() and (is_gameplay_input_active() \
			or (is_dev_tools_open() and not _dev_tools.is_game_playing())):
		if _player_presenter != null:
			# The chord consumed the USE hold: no mount toggle on its release.
			_player_presenter.consume_use_hold()
		pick_at_crosshair()
		get_viewport().set_input_as_handled()
		return
	# The HUD presenter's gameplay keys (objectives toggle / friendly-tags
	# cycle), in-world only — bindings + orig cites at the presenter.
	if is_gameplay_input_active() and _hud_presenter != null \
			and _hud_presenter.handle_gameplay_key(key.keycode):
		get_viewport().set_input_as_handled()
		return
	# The USE-ITEM key's armory arm: in-world, standing in an armory volume, the
	# press opens weapon.mnu [orig: useitem action 177, Flags & 0x400000 @0x4e0b4d].
	# Every other arm of the key is the input router's per-frame chain over the
	# polled `useitem` row -- the hold latch, the USE+digit seat pick, the mount
	# toggle on the release edge (PlayerInputRouter::sample_use_item); the screen
	# this arm opens leaves gameplay input inactive, which resets that chain.
	if is_gameplay_input_active() and _is_use_item_key(key.keycode):
		if _try_open_armory():
			get_viewport().set_input_as_handled()
			return
		# The vehicle-loadout-volume arm @0x4e0bfe (the engine's
		# local_player_in_vehicle_loadout_zone: an unmounted local player carrying
		# the type-11 volume touch): the press is consumed here and never latches
		# the router's chain -- no mount toggle on the release, no USE+digit chord.
		# The bay's team gate (local_player_vehicle_zone_team_matches: the ground
		# entity's team 0 or the player's own) selects vehicle.mnu's VEHICLE
		# screen, unported (D-HUD-14). A mounted player never reaches this arm (the
		# predicate rejects a seated player), so the seat chain stays unconditional.
		var use_sim: Simulation = _world.get_sim() if _world != null else null
		if use_sim != null and use_sim.local_player_in_vehicle_loadout_zone():
			if _player_presenter != null:
				_player_presenter.consume_use_hold()
			if use_sim.local_player_vehicle_zone_team_matches():
				pass  # vehicle.mnu VEHICLE (D-HUD-14)
			get_viewport().set_input_as_handled()
		return
	# Gameplay keys (B/N/NVG, Z/X/C stance) live on LocalPlayerPresenter; view rows on GameHudPresenter.
	if _player_presenter != null and _player_presenter.handle_key_input(
			event, is_gameplay_input_active()):
		get_viewport().set_input_as_handled()


# The workspace's shell policy. Interact owns the cursor and world click-pick;
# Play reuses the normal gameplay gate and capture route without hiding ImGui.
func _on_dev_tools_open_changed(open: bool) -> void:
	_refresh_dev_tools_game_state()
	if open and _player_presenter != null:
		# A USE hold begun before F3 must not turn into a mount action when the
		# key is released behind the tools.
		_player_presenter.consume_use_hold()


func _on_dev_tools_game_input_mode_changed(_playing: bool) -> void:
	_sync_dev_tools_pick_policy()


func _is_game_play_available() -> bool:
	return _state == State.WORLD and not _end_flow.is_round_ended() \
			and not _world_load_pending and not _end_flow.has_screen() \
			and _world != null and _world.is_loaded()


func _refresh_dev_tools_game_state() -> void:
	var available := _is_game_play_available()
	_dev_tools.set_game_play_available(available)
	_sync_dev_tools_pick_policy()


func _sync_dev_tools_pick_policy() -> void:
	_pick_session.sync_click_policy(_world, is_dev_tools_open()
			and not _dev_tools.is_game_playing() and _is_game_play_available())


func is_dev_tools_open() -> bool:
	return _dev_tools != null and _dev_tools.is_open()


## Shift+F6 (and the probe/test seam): pick whatever the crosshair is on into the
## debug pick list (selecting it in the F3 Entities window), with a brief toast.
func pick_at_crosshair() -> void:
	_pick_session.pick_at_crosshair(_world.get_sim() if _world != null else null,
			_camera, _player_presenter, _hud if _hud != null else self)


## F11. ImGui multi-viewport must already be off at the NewFrame that first
## sees the fullscreen size (imgui-godot 6.3.2 on Godot 4.6.1 / D3D12 presents
## black otherwise; the window_fullscreen probe pins it), so the tools'
## platform windows are withdrawn before the switch and allowed again after
## the return to windowed.
func _toggle_fullscreen() -> void:
	var window := get_window()
	var entering := not WindowState.is_fullscreen(window)
	if entering:
		_dev_tools.set_platform_windows_allowed(false)
	WindowState.toggle_fullscreen(window)
	if not entering:
		_dev_tools.set_platform_windows_allowed(true)


func get_dev_tools() -> DevTools:
	return _dev_tools


# --- The GameShell suppliers (ADR 0043 rule 11): the tooling reads the shell's
# presenters through these typed getters, never through its privates. ---

func get_world() -> GameWorld:
	return _world


func get_player_presenter() -> LocalPlayerPresenter:
	return _player_presenter


func get_hud_presenter() -> GameHudPresenter:
	return _hud_presenter


func get_menu_shell() -> MenuShell:
	return _menu_shell


func get_armory_presenter() -> ArmoryPresenter:
	return _armory_presenter


func get_deploy_presenter() -> DeployScreenPresenter:
	return _deploy_presenter
func get_game_debug_adapter() -> GameDebugAdapter:
	if _debug_adapter == null:
		_debug_adapter = GameDebugAdapterScript.new()
		# The adapter depends on the GameShell surface this class overrides;
		# the probe runner reads the same shell through get_shell() (ADR 0041).
		_debug_adapter.configure(self)
	return _debug_adapter
func get_frame_stats() -> FrameStats:
	return _frame_stats

func is_root_render_stats_measured() -> bool:
	return _render_stats.is_measured()


func is_gameplay_input_active() -> bool:
	return _state == State.WORLD and not _end_flow.is_round_ended() \
			and (not is_dev_tools_open() or _dev_tools.is_game_playing())


# --- End of mission (SP) -------------------------------------------------------

# The sim's round_end effect arms MissionEndFlow; the flow's beat and screen run
# from _process. The effect's b word is the end track the SP tail hands the
# gamemus MessageHandler (1 win / 2 lose; the world's process_round_end selects
# it): MusicDirector.signal_end_track runs the VM's restart frame (the engine's
# mus_vm_signal, MusicCtx_SelectEndTrack's step) once the flow arms. Retail
# signals right after the cine starts; the flow's lead-in beat stands in for
# the cine, so the signal rides the arm. begin() refuses an MP round
# (EndRoundPresenter owns it), so a net session never signals.
func _on_shell_mission_effects(effects: Array) -> void:
	for e_v in effects:
		var e := e_v as MissionEffect
		if e != null and e.kind == "round_end":
			var armed_before := _end_flow.is_round_ended()
			_end_flow.begin(e.a, _world.get_sim() if _world != null else null)
			if not armed_before and _end_flow.is_round_ended():
				var director: MusicDirector = MusicService.director()
				if director != null:
					director.signal_end_track(e.b)


func _show_end_screen() -> void:
	var banner := _hud_presenter.endround_banner_line() if _hud_presenter != null else ""
	_end_flow.show_screen(_world.get_sim() if _world != null else null, banner, _root,
			_hud if _hud != null else self, _on_end_screen_exit)


# [orig: g_mission_exit_reason = 1 (ESC / the epilog timeout) -> the main loop pushes
# the "Post Menu" scene @0x526867 — our post-mission menu is the main menu.]
func _on_end_screen_exit() -> void:
	_on_return_to_menu()


func get_runtime() -> MissionRoot:
	return _world.get_runtime() if _world != null else null


func shell_state_name() -> String:
	match _state:
		State.WORLD:
			return "world"
		State.PAUSED:
			return "paused"
		State.ARMORY:
			return "armory"
		State.DEPLOY:
			return "deploy"
		_:
			return "menu"


# --- Menu state ---------------------------------------------------------------

# Returns false when the requested directory cannot mount.
func _enter_menu(dir: String) -> bool:
	if _root == null or _root.get_root_dir() != dir:
		var root := BootRootMount.mount(dir, LaunchFlags.loose_root_allowed())
		if root == null:
			return false
		_root = root
	var profile_root_key := "%s|%s" % [String(_root.get_root_dir()),
			String(_root.get_expansion()).to_lower()]
	if profile_root_key != _profile_root_key:
		_chosen_avatar = PlayerProfile.load_character_profile(_root)
		_profile_root_key = profile_root_key
	# The menu, loading screen, and world are one runtime resource session.
	# GameWorld must not remount from mutable persisted settings after boot.
	_world.set_resource_root(_root)
	_state = State.MENU
	_shell_presentation.enter_menu(_world, _hud)
	_wire_shell()
	if _player_info_companion != null:
		_player_info_companion.set_persisted_profile(_chosen_avatar)
	# The boot's text tables: the menu shell registers menutxt/gametext/gameui
	# in setup(); keyhelp.bin (the "Keys" binding-label table) is registered
	# here beside them, which installs it for the engine's binding formatters
	# (Strings.TABLE_KEYHELP -> RtxtStringFile.install_key_strings).
	# keyhelp.bin off the mounted root; null when the root carries none or it
	# does not parse (every binding label then renders its literal fallback).
	Strings.register_table(Strings.TABLE_KEYHELP, Strings.load_rtxt(_root, "keyhelp.bin"))
	if not _menu_shell.setup(_root):
		push_warning("MainGame: no menu found in resource dir (looked for %s)"
				% _menu_shell.main_menu_file)
	_menu_shell.show_menu()
	return true


# The one-shot MenuShell wiring (every return to the menu re-enters _enter_menu):
# the five menu intents in this order, then the mp.mnu / player.mnu companions
# and the LAN browser they drive.
func _wire_shell() -> void:
	if _shell_wired:
		return
	_shell_wired = true
	_menu_shell.start_requested.connect(_on_start_requested)
	_menu_shell.exit_to_desktop_requested.connect(_on_exit_to_desktop)
	_menu_shell.return_to_menu_requested.connect(_on_return_to_menu)
	_menu_shell.resume_requested.connect(resume)
	_menu_shell.novaworld_requested.connect(_net.open_novaworld_panel)
	# Delegate mp.mnu and player.mnu to their respective companions.
	_mp_companion = MpMenuCompanion.new()
	_player_info_companion = PlayerInfoMenuCompanion.new()
	_lan_session = LanSession.new()
	_lan_session.name = "LanSession"
	add_child(_lan_session)
	_mp_companion.set_lan_session(_lan_session)
	_menu_shell.add_companion(_mp_companion)
	_menu_shell.add_companion(_player_info_companion)
	_net.wire_menu_companions(_mp_companion)
	_player_info_companion.set_persisted_profile(_chosen_avatar)
	_player_info_companion.avatar_chosen.connect(_on_avatar_chosen)


# PLAYER_INFO ACCEPT persists both side records and the shared callsign, while
# the selected loadout continues through the existing spawn-kit seam.
func _on_avatar_chosen(profile: Dictionary) -> void:
	set_local_player_profile(profile)
	var typed_name := String(profile.get("name", "")).strip_edges()
	if not typed_name.is_empty():
		PlayerProfile.save_callsign(typed_name)
	if _root != null:
		var save_error := PlayerProfile.save_character_profile(_root, profile)
		if save_error != OK:
			push_warning("MainGame: could not save PLAYER_INFO profile (error %d)"
					% save_error)


# Install the in-memory local-player profile used by the next mission spawn.
func set_local_player_profile(profile: Dictionary) -> void:
	_chosen_avatar = profile.duplicate(true)
	if _player_info_companion != null:
		_player_info_companion.set_persisted_profile(_chosen_avatar)


# The armory key while in-world: the shared ArmoryPresenter opens weapon.mnu's
# WEAPON screen over LIVE play when the player stands in an armory zone — the
# world keeps ticking underneath (State.ARMORY rides the presenter's opened signal)
# [orig: useitem action 177 -> UI_OpenMenuScreen("weapon.mnu", "WEAPON")
# @0x4e0b44, gated on Flags & 0x400000 @0x4e0b4d + the host weapons rule
# (dword_A85B6C, BSS 0 in SP = allowed); no world-stop leg]. Returns false when
# out of zone (key ignored, the original's silent gate). The ACCEPT apply and
# the class/current-loadout open protocol live on the host.
func _try_open_armory() -> bool:
	if _armory_presenter == null:
		return false
	_armory_presenter.set_player_team(int(_chosen_avatar.get("team", 0)))
	return _armory_presenter.try_open()


# Whether a key event is the `useitem` row's (retail default Shift; catalog row
# 44, rebindable in Options -> Controls).
func _is_use_item_key(keycode: Key) -> bool:
	return ControlsBindings.model().godot_keys_for_token("useitem").has(int(keycode))


# --- Menu <-> world transitions ----------------------------------------------

func _on_start_requested(bms_name: String) -> void:
	# Single-player: the loading screen is the sidecar image alone — no session
	# text [orig: the not-in-session path draws only the background @ 0x521ebe].
	start_world_load(
		LoadingScreenInfo.for_mission(bms_name),
		_world.load_mission.bind(bms_name))


## Boot an exact loose mission through the same loading
## presentation and GameWorld lifecycle as menu play.
func start_loose_mission(bms_name: String) -> void:
	start_world_load(
		LoadingScreenInfo.for_mission(bms_name),
		_world.load_loose_mission.bind(bms_name))


## The probe runner's mission verbs (GameShell, ADR 0041): the menu's
## Start path, the saved-BMS path parity captures stage, and the return leg.
func start_mission(bms_name: String) -> Error:
	var gate := _mission_start_gate()
	if gate == OK:
		_on_start_requested(bms_name)
	return gate


func start_saved_mission(saved_path: String, bms_name: String, profile: Dictionary = {}) -> Error:
	var gate := _mission_start_gate()
	if gate != OK:
		return gate
	var mission := MissionData.new()
	if mission.open_file(saved_path) != OK:
		return ERR_FILE_CANT_OPEN
	if not profile.is_empty():
		set_local_player_profile(profile)
	start_world_load(LoadingScreenInfo.for_mission(bms_name),
			_world.load_mission_data.bind(mission, bms_name))
	return OK


func return_to_menu() -> Error:
	if _world_load_pending:
		return ERR_BUSY
	if not _world.is_loaded():
		return ERR_UNAVAILABLE
	_on_return_to_menu()
	return OK


func _mission_start_gate() -> Error:
	if _root == null:
		return ERR_UNCONFIGURED
	if _world_load_pending or not _world_load.can_start():
		return ERR_BUSY
	return ERR_ALREADY_IN_USE if _world.is_loaded() else OK


## Graceful runtime stop seam used by the shell and optional control service.
## The shutdown itself runs once; a repeat close request while the music
## drain is still pending ends the drain and quits right away, so a mixer
## that stopped advancing can never swallow the window's close.
func request_quit() -> void:
	if _quit_requested:
		if _quit_drain_pending:
			_quit_drain_pending = false
			_finish_quit()
		return
	_quit_requested = true
	_complete_runtime_shutdown(begin_runtime_shutdown())


## True while request_quit() is waiting for the music playbacks to drain
## (ADR 0018 read seam for the lifecycle tests).
func is_quit_drain_pending() -> bool:
	return _quit_drain_pending


func _complete_runtime_shutdown(load_operation: WorldLoadOperation) -> void:
	if load_operation != null and not load_operation.is_settled():
		await load_operation.settled
	finish_runtime_shutdown()
	_quit_drain_pending = true
	await MusicService.await_playback_stopped()
	if not _quit_drain_pending:
		return # a repeat close request already finished the quit
	_quit_drain_pending = false
	_finish_quit()


func _finish_quit() -> void:
	_restore_quit_policy()
	if is_inside_tree():
		get_tree().quit()


func _restore_quit_policy() -> void:
	if _quit_policy_installed and get_tree() != null:
		get_tree().auto_accept_quit = _previous_auto_accept_quit
	_quit_policy_installed = false


## Public delegate for "join this server" — kept on the shell so lifecycle tests
## and external drivers keep one ADR-0018 entry; the controller owns the path.
func join_lan_server(target: JoinTarget) -> void:
	_net.join_lan_server(target)


## The common mission-start seam; ShellPresentationSession owns its visibility
## transition while this shell owns load state and the operation handoff.
func start_world_load(load_info: LoadingScreenInfo, operation: Callable) -> void:
	if not _world_load.can_start():
		return
	if _lan_session != null:
		_lan_session.stop()
	_world_load_pending = true
	_world.set_local_player_spawn_loadout(PlayerSpawnLoadout.from_profile(_chosen_avatar))
	_begin_world_load()
	if _world_load.start(self, _root, _world, load_info, operation) == null:
		_on_world_load_failed("mission load handoff could not start")


func _begin_world_load() -> void:
	_shell_presentation.begin_world_load(
			_menu_shell, _world, _hud, _on_world_loaded, _on_world_load_failed)
	_state = State.WORLD
	_refresh_dev_tools_game_state()



func _on_world_loaded() -> void:
	# The GAME music context is the world's to open at mission start (GameWorld
	# emits music_context_opened into MusicService.open_game_context, so every
	# live mission entry path
	# gets the same music); nothing to do here for audio. The witnessed release
	# then reveals the world + HUD at the tail
	# of Game_StartMission [orig: LoadingScreen_ReleaseEffect @ 0x586b80, final
	# call @ 0x525d45]. For SP/host this fires at true load completion. For a
	# joiner this is only wire-header world construction completion; keep pumping the hidden runtime
	# under the loading presentation until the separate authoritative edge.
	# A fresh mission gets a fresh pick set (stale handles never cross
	# sessions); the world renders/curates the shell-owned list from here on.
	_pick_session.begin_world(_world)
	_on_dev_tools_open_changed(is_dev_tools_open())
	var sim := _world.get_sim()
	_player_options.apply(sim)
	# Every mission start re-seeds the live HUD declutter level from the
	# persisted config value (retail's session-settings apply), so a death
	# screen's forced blank HUD ends with the mission it happened in.
	if _hud_presenter != null:
		_hud_presenter.reapply_persisted_hud_detail()
	# The F3 engine-fact windows read and mutate through this Simulation from
	# here until unload (ADR 0042 d6): DevTools holds it in C++ (the stats-board
	# pattern) and does the record push / request drain with no GDScript relay.
	_dev_tools.set_simulation(sim)
	if sim != null and bool(sim.is_joiner()) \
			and not bool(sim.is_joined_in_match()):
		return
	# The SP start-mission splash holds the reveal until its dismissal edge;
	# the gate + device legs live on the coordinator
	# [orig: show_start_mission_splash @ 0x520820 precedes the release @ 0x525d45].
	if _world_load.maybe_begin_start_mission_splash(_world.get_mission_audio()):
		if not _world_load.splash_dismissed.is_connected(
				_finish_world_load_presentation):
			_world_load.splash_dismissed.connect(_finish_world_load_presentation)
		return
	_finish_world_load_presentation()


func _finish_world_load_presentation() -> void:
	if not _world_load_pending:
		return
	_world_load_pending = false
	_shell_presentation.finish_world_load(_world_load, _world, _hud)


func _on_join_admission_ready() -> void:
	_finish_world_load_presentation()


func _on_join_deploy_pick_required() -> void:
	# Initial admission transitions from loading to the player-paced DEATH screen;
	# on a later death the presentation is already down and the same screen simply
	# reopens on the new pending edge.
	# The DEATH-screen edge forces the declutter level to max through the same
	# seam the huddetail cycle uses (it writes the persisted global like
	# retail; the death.mnu screen itself draws outside the blanked gameplay
	# overlay pass). [orig: NapiNPClientMsg_0x00F @0x42E410..0x42E41C —
	# level = 3 -> CRenderState_SetLayerVisibility @0x59B0F0]
	if _hud_presenter != null:
		_hud_presenter.apply_death_screen_hud_detail()
	_finish_world_load_presentation()
	if _deploy_presenter.open():
		return
	# The admission watchdog has already ended at the player-paced stage (retail
	# waits at the DEATH screen), so a failed open with the pick still owed is a
	# dead join, not a warning: abort to the menu with a reason instead of
	# parking the player on the loading screen forever.
	var sim := _world.get_sim()
	if sim != null and bool(sim.is_join_deploy_pick_pending()):
		_on_world_load_failed("join: the deploy screen failed to open (death.mnu)")


## The host's round cycle: the 2790-tick post-round linger expiry EXITS THE
## MISSION into the map cycle — retail's server sets exit reason 3 and reloads
## the next rotation entry; the rotation itself is not modeled, so the shell
## returns to the menu through the same teardown every mission exit takes. A
## joiner's session dies with the host's exit and lands here too (its
## net-session drive may also route the loss through _on_session_lost first —
## whichever fires first tears down, the other sees MENU).
## [orig: Server_TickUpdate linger drain @0x51da04..; g_mission_exit_reason = 3
##  @0x51db63; every exit reason lands on the same teardown + nav push
##  @0x568654. SP mission end runs the epilog flow instead.]
# The frame loop's death.mnu DEATH open off the host-driven deploy-map overlay.
# The once-per-arming open latch, its result-blind stamp, and its clear when
# the host drops the bit all live on the engine's ClientState
# (client_state.h deploy_overlay_open_latch; hud-re D-HUD-19) — this leg is
# the device call. State.WORLD stands in for retail's no-active-menu gate
# (PAUSED / ARMORY / DEPLOY / END_ROUND all hold a screen), so the latch is
# never burned under another screen and the open retries on the next clear
# frame. Closing is the presenter's own affair. NOT yet modeled (hud-re
# D-HUD-19 residuals): the second open trigger — the local entity's undeployed
# bit — and the spawn-success suppression gate.
func _maybe_open_deploy_overlay() -> void:
	if _state != State.WORLD or _world_load_pending:
		return
	var sim: Simulation = _world.get_sim()
	if sim == null or not bool(sim.take_join_deploy_overlay_open()):
		return
	_deploy_presenter.open()


func _maybe_exit_round_cycle() -> void:
	if _state == State.MENU or _world_load_pending or _world == null:
		return
	var sim: Simulation = _world.get_sim()
	if sim == null:
		return
	if not sim.is_mp_session():
		return
	var er: EndRoundState = sim.get_end_round_state()
	if not er.is_header_known():
		return
	if er.is_session_open():
		return
	_abort_to_menu("round cycle", "post-round linger expired (mission exit 3)")


## An established session ended without the player asking: the host closed it on its own
## terms (its punt channel — a CRC mismatch, a violation sweep, the six-minute deploy-screen
## idle kick), or it went silent past the connection reap window. Retail EXITS THE MISSION
## with a reason here and raises no in-world dialog, so this takes the shell's existing
## abort-to-menu leg with the decoded reason named.
## [orig: the punt record CNapiNPConnection_HandleDescriptionPacket @ 0x621ae0 and the
##  cs_dir0.timeout_ms = 120000 reap CNapiNetwork_Init @ 0x4ca4a0, both ->
##  CNapiNetwork_OnDisconnectedFromServer @ 0x4c63d0. The captured DPC 33 falls to
##  Input_QueueEvent(3) @ 0x4c67a4, whose action sets g_mission_exit_reason = 1 and drops
##  the connection (Input_HandleActionBinding case 3 @ 0x49af2c) — reason 1 is the same
##  teardown + "MainMenu" push every abort leg takes @ 0x568654]
func _on_session_lost(reason: String) -> void:
	# Already back in the menu with nothing loading: the teardown ran (this is the
	# double-notification guard, not a state test the loss depends on). A loss during
	# the load presentation still routes through the same leg — it clears
	# _world_load_pending on its way to the menu.
	if _state == State.MENU and not _world_load_pending:
		return
	_abort_to_menu("session ended", reason)


func _on_world_load_failed(reason: String) -> void:
	# A load-step failure or abort returns to the menu — the witnessed early
	# return that sets reason=1 and nav-pushes "Post Menu" out of
	# Game_StartMission [orig: the "Mission loading aborted" legs @ 0x520270
	# (Client_CheckDisconnectOrEscDuringLoad) and the network-wait failure legs;
	# scene_entry = "Post Menu"] (docs/interface/loading-screen-re.md, the
	# load-flow case matrix).
	_abort_to_menu("mission load failed", reason)


# THE abort-to-menu leg. Every caller names the stage it aborted from; the presentation
# is one teardown because retail's is one too (every reason lands on the same nav push).
func _abort_to_menu(stage: String, reason: String) -> void:
	_world_load_pending = false
	push_warning("MainGame: %s: %s" % [stage, reason])
	_teardown_world_to_menu()


func _on_camera_escape() -> void:
	# Esc: pause <-> resume while in a world (the armory closes back to play);
	# ignored in the main menu (EXIT quits). During a load, BOTH joiner waits are
	# interruptible legs — the pre-load connect/session wait and the post-load
	# admission tail, which is polled once per world tick. Aborting either
	# is the reachable analog of the original's per-asset ESC/disconnect abort
	# poll [orig: Client_CheckDisconnectOrEscDuringLoad @ 0x520270]. The
	# SP/host map load remains a single synchronous call the SceneTree cannot
	# interrupt (docs/interface/loading-screen-re.md D-LOADSCR-7).
	if _world_load_pending:
		if _world != null and _world.cancel_join_preload():
			return
		if _world != null:
			_world.cancel_join_admission()
		return
	# Round over: ESC leaves the mission instead of pausing (the witness rides
	# MissionEndFlow.request_screen_exit).
	if _end_flow.is_round_ended():
		if not _end_flow.request_screen_exit():
			_on_end_screen_exit()
		return
	if _state in [State.WORLD, State.DEPLOY, State.END_ROUND]:
		# ESC from the deploy screen still reaches the in-game menu (and therefore
		# RETURN TO MENU): a joiner parked at the pick must be able to leave.
		_pause()
	elif _state == State.PAUSED or _state == State.ARMORY:
		resume()


func _leave_screen(from_state: int) -> void:  # a closing screen hands play back
	if _state == from_state:
		_state = State.WORLD


func _pause() -> void:
	_state = State.PAUSED
	# The engine session pauses with the shell; net roles refuse natively.
	_world.set_shell_paused(true)
	_menu_shell.open_ingame_menu()  # game.mnu overlay over the kept-loaded world
	_menu_shell.show_menu()


func resume() -> void:
	if _state != State.PAUSED and _state != State.ARMORY:
		return
	if _armory_presenter != null and _armory_presenter.is_open():
		_armory_presenter.close()  # Esc from ARMORY closes the overlay (no re-entry: closed
							  # only fires while open)
	_menu_shell.hide_menu()
	# A joiner who paused from the deploy screen still owes its pick, so resume back
	# into DEPLOY rather than handing the cursor back to the world.
	_state = State.DEPLOY if (_deploy_presenter != null and _deploy_presenter.is_open()) \
			else State.END_ROUND if (_end_round_presenter != null and _end_round_presenter.is_open()) \
			else State.WORLD
	# Every resume leg lands here, so the session cannot stay stuck Paused.
	_world.set_shell_paused(false)


func _on_return_to_menu() -> void:
	if _world_load_pending:
		return
	_teardown_world_to_menu()


## The MCP screen verbs behind game_control (the in-world screens get eyeballed
## over the runtime MCP without hand-play). Both keep the key paths' state
## gates; "open" is not a toggle, so an already-open screen reports OK and the
## resume verb hands play back.
func mcp_open_ingame_menu() -> Error:
	if _world == null or not _world.is_loaded() or _world_load_pending \
			or _end_flow.is_round_ended():
		return ERR_UNAVAILABLE
	if _state == State.PAUSED:
		return OK
	# ESC in the armory resumes rather than pausing, so the verb requires an
	# explicit resume first instead of silently stacking screens.
	if _state != State.WORLD and _state != State.DEPLOY:
		return ERR_UNAVAILABLE
	_pause()
	return OK


func mcp_open_armory() -> Error:
	if _world == null or not _world.is_loaded() or _world_load_pending \
			or _end_flow.is_round_ended() or _armory_presenter == null:
		return ERR_UNAVAILABLE
	if _state == State.ARMORY:
		return OK
	if _state != State.WORLD:
		return ERR_UNAVAILABLE
	# The armory key's leg minus the zone gate: standing in a type-6 volume is
	# the useitem key's gameplay rule [orig: Flags & 0x400000 @0x4e0b4d], not a
	# screen precondition — open() is the presenter's staged direct-open seam.
	_armory_presenter.set_player_team(int(_chosen_avatar.get("team", 0)))
	return OK if _armory_presenter.open() else ERR_UNAVAILABLE


# One idempotent rollback for a normal return and every load failure. Runtime
# presenters keep references to the old world/root, so their teardown order is part
# of the shell boundary rather than a menu-specific detail.
func _teardown_world_to_menu() -> void:
	_world_load.dismiss()
	finish_hud_hidden_capture()
	_end_flow.reset()
	if _player_presenter != null:
		_player_presenter.teardown()
	if _armory_presenter != null:
		_armory_presenter.teardown()  # the built menu holds the OLD world's resource root
	if _deploy_presenter != null:
		_deploy_presenter.teardown()  # same stale-root hazard, and the shell's blanket
		# HUD visibility toggle would re-show a surviving DEATH shroud next mission
	if _end_round_presenter != null:
		_end_round_presenter.teardown()  # same stale-root hazard: the stat.mnu frame and
		# its MenuAudio hold the OLD world's resource root, and a frame left visible when
		# the session ends mid-STAT would be re-shown over the next mission
	# The F3 windows' Simulation dies with the runtime unload frees.
	_dev_tools.set_simulation(null)
	_world.unload()
	if _player_presenter != null:
		_player_presenter.setup(_world, _camera, _camera, ControlsBindings.model())
	if _hud_presenter != null:
		_hud_presenter.teardown()
	if _root != null and _enter_menu(_root.get_root_dir()):
		return
	push_warning("OpenNova: the game-data directory is no longer mountable")
	get_tree().quit(1)


func _on_exit_to_desktop() -> void: request_quit()


## Keep the public adapter callback while the capture module owns mutation.
func mcp_begin_world_only_capture() -> Error:
	return _shell_presentation.begin_world_only_capture(_hud, _menu_layer, _player_presenter)


func mcp_end_world_only_capture() -> void: _shell_presentation.finish_world_only_capture()
## A frozen-shell fixture moved the beauty camera: the FP gun follows it again.
func mcp_restamp_viewmodel_for_capture() -> void: _shell_presentation.restamp_viewmodel_for_capture(_player_presenter)


## Begin a reversible retail HUD-detail-3 capture. Unlike world_only this
## deliberately keeps the HUD CanvasLayer, PlayerViewEffects, viewmodel pass,
## and world presentation mounted; the shell suppresses compiled gameplay HUD
## commands and its FPS counter without persisting either override.
func begin_hud_hidden_capture() -> Error:
	return _shell_presentation.begin_hud_hidden_capture(_hud_presenter, _hud)


func finish_hud_hidden_capture() -> void: _shell_presentation.finish_hud_hidden_capture(_hud_presenter)


## Public semantic witness consumed by render-fixture capture probes. The
## presentation session combines presenter draw-list/effects/card facts
## with the CanvasLayer boundary so a hidden parent cannot masquerade as an
## empty gameplay HUD.
func hud_hidden_capture_witness() -> HudHiddenCaptureWitness:
	return _shell_presentation.hud_hidden_capture_witness(_hud_presenter, _hud)


## The frame-span probe switches: opt-in, since their clock reads and span
## writes would perturb every retail frame they measure; the typed seam the
## perf probes read and set.
var _perf_probe := PerfProbeSwitches.new()


func get_perf_probe_switches() -> PerfProbeSwitches:
	return _perf_probe


func set_perf_probe_enabled(enabled: bool) -> void:
	_perf_probe.set_enabled(enabled)


func _process(delta: float) -> void:
	if _shutdown_prepared:
		_dev_tools.set_game_play_available(false)
		return
	_refresh_dev_tools_game_state()
	var stats_on: bool = _frame_phase_sampler.begin_shell_control()
	var probe_enabled := _perf_probe.enabled
	# One shared gate for the frame-leg clock reads: the manual A/B probe and
	# the F3 Stats capture both consume the same measurements.
	var timing: bool = probe_enabled or stats_on
	if probe_enabled:
		_perf_probe.spans.clear()
	_frame_phase_sampler.sample_render(_render_stats, get_viewport(), _menu_shell)
	var dev_tools_open := is_dev_tools_open()
	# Release the captured mouse while UI overlays the world or nothing is loaded.
	var dev_tools_interacting := dev_tools_open and not _dev_tools.is_game_playing()
	if _state in [State.PAUSED, State.ARMORY, State.DEPLOY, State.END_ROUND] \
			or dev_tools_interacting or _end_flow.has_screen() or not _world.is_loaded():
		if Input.get_mouse_mode() == Input.MOUSE_MODE_CAPTURED:
			Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)
	# The end-of-mission lead-in: the world keeps ticking; the score/failed screen
	# mounts after the short beat [orig: the SP world runs through the epilog cine].
	if _world.is_loaded() and _end_flow.tick(delta):
		_show_end_screen()
	# The shell-control span closes before the early returns so every frame banks it.
	var probe_t0 := Time.get_ticks_usec() if timing else 0
	_frame_phase_sampler.finish_shell_control(probe_t0)
	# Only the pause menu freezes the world, and only in a SINGLE-PLAYER session. The
	# armory runs over LIVE play: the match keeps simulating around the player while the
	# WEAPON screen is up [orig: the useitem armory leg @0x4e0b3f has no world-stop leg;
	# input idles because player_live below is false outside State.WORLD].
	# A NET session never freezes: retail multiplayer cannot pause (the ESC menu overlays
	# a running match), and the world tick is the only pump for the session socket — a
	# frozen tick transmits nothing, so a stock peer drops us at its 120 s connection
	# timeout [orig: cs_dir0.timeout_ms = 120000, CNapiNetwork_Init @0x4ca4a0] and a
	# frozen listen host does the same to every joiner.
	if not _world.is_loaded():
		return
	# The start-mission splash holds the world un-ticked (retail has not yet
	# returned from Game_StartMission [orig: @ 0x525d42 precedes the first
	# tick]); session loads never splash, so the joiner pump is untouched.
	if _world_load.is_splash_active():
		return
	if _state == State.PAUSED and not _world.is_net_session():
		return
	var frame_input: MissionFrameInput
	if _player_presenter != null:
		var player_live := is_gameplay_input_active()
		frame_input = _player_presenter.before_world_tick(
				delta, player_live, player_live)
	else:
		frame_input = MissionFrameInput.new()
		frame_input.delta_seconds = delta
	var probe_t1 := Time.get_ticks_usec() if timing else 0
	var skip_world := probe_enabled and _perf_probe.skip_world
	if not skip_world:
		_world.tick(_camera.global_position, _camera.global_transform,
				delta, frame_input)
	var probe_t2 := Time.get_ticks_usec() if timing else 0
	# Camera placement runs in the world frame now (local-view device leg,
	# D-RORD-8); this covers frames that skip it (probe world-skip, no live world).
	if _player_presenter != null and (skip_world or not _world.is_loaded()):
		_player_presenter.after_world_tick()
	var probe_t3 := Time.get_ticks_usec() if timing else 0
	# The shared HUD presenter rebuilds the per-frame info while the player is in-world
	# (WORLD or the live-play ARMORY) [orig: HUD_BuildEntityInfo @0x4b8440 per frame].
	var skip_hud := probe_enabled and _perf_probe.skip_hud
	if _hud_presenter != null and not skip_hud \
			and _state in [State.WORLD, State.ARMORY, State.DEPLOY, State.END_ROUND]:
		_hud_presenter.tick(is_gameplay_input_active())
		_end_round_presenter.tick()  # the same HUD frame [orig: HUD_DrawOverlayPanels]
	var probe_t4 := Time.get_ticks_usec() if timing else 0
	_maybe_open_deploy_overlay()
	_maybe_exit_round_cycle()
	if timing:
		_frame_phase_sampler.record_shell_spans(
				probe_t0, probe_t1, probe_t2, probe_t3, probe_t4,
				Time.get_ticks_usec(), _perf_probe.spans, probe_enabled)


# Mouse-look rides the shared LocalPlayerPresenter (the yaw/pitch witnesses live there);
# the shell only says when the player is live: gameplay input is active, the
# world is loaded, and the mouse is captured.
func _unhandled_input(event: InputEvent) -> void:
	if _player_presenter != null and _player_presenter.handle_input(
			event,
			is_gameplay_input_active() and _world.is_loaded() \
					and Input.get_mouse_mode() == Input.MOUSE_MODE_CAPTURED):
		get_viewport().set_input_as_handled()
