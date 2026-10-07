class_name MainGame
extends GameShell

# Runtime shell: boots into the game's menu front-end (MenuShell, driving the
# .mnu menu set + audio from the chosen resource dir) and hands off to a GameWorld
# when the player starts a mission, with pause + return-to-menu on demand.
# Everything (menus, audio, terrain, missions) loads from the --resource-dir
# supplied at launch; without one the shell boots OpenNova's own bundled
# assets/, whose placeholder menu hands over to a picked retail install through
# PLAY RETAIL (ADR 0048).

const PlayerOptionsScript := preload("res://game/player_options.gd")
const GameDebugAdapterScript := preload("res://game/game_debug_adapter.gd")
const WorldLoadCoordinatorScript := preload("res://game/world_load_coordinator.gd")
const ShellPresentationSessionScript := preload("res://game/shell_presentation_session.gd")
const HudHiddenCaptureWitness := preload("res://game/world/hud_hidden_capture_witness.gd")
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
# Insert: the in-engine dev tools (the DevTools node's ImGui windows, ADR 0039).
# Off F3, which is retail's `viewwithgun` default (catalog row 108). Insert is
# also retail Jump's secondary key (row 8): the polled Jump row still sees a
# toggle press, an accepted overlap for a debug-build key.
const DEV_TOOLS_KEY := KEY_INSERT
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
# g_SpawnSuccessGate — so this state is what makes the spawn list clickable.
enum State { MENU, WORLD, PAUSED, ARMORY, DEPLOY, END_ROUND, COMMAND_MAP }

@onready var _world: GameWorld = $World
@onready var _camera: FlyCamera = $Camera3D
@onready var _hud: CanvasLayer = $HUD
@onready var _menu_layer: CanvasLayer = $MenuLayer
@onready var _menu_shell: MenuShell = $MenuLayer/MenuShell

var _root: ResourceRoot
var _state: int = State.MENU
var _shell_wired := false
# The in-engine dev tools' seam: Insert opens them, the mouse policy follows them.
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
var _bundled_companion: BundledMenuCompanion  # the bundled menu's PLAY RETAIL / CHANGE FOLDER / EXIT
var _retail_picker: FileDialog  # the PLAY RETAIL folder picker, while open
var _web_retail_picking := false  # the web page's picker is open (ADR 0049)
var _lan_session: LanSession  # retail-style 0x41/0x81 LAN enumeration browser
var _player_info_companion: PlayerInfoMenuCompanion  # drives the PLAYER_INFO (player.mnu) character screen
var _armory_presenter: ArmoryPresenter  # the SHARED in-world armory surface (weapon.mnu WEAPON)
var _deploy_presenter: DeployScreenPresenter  # the joiner's deploy-map screen (death.mnu DEATH)
var _command_map_presenter: CommandMapPresenter  # the commander map (cmap.mnu CMAP)
var _end_round_presenter: EndRoundPresenter  # the MP end-of-round overlay + stat.mnu STAT
var _chosen_avatar: Dictionary = {}  # canonical active + per-side PLAYER_INFO selection
var _profile_root_key := ""  # reload the profile only when the mounted game/expansion changes
var _intro_tail_run := false  # the first menu start's profile save ran
var _world_load := WorldLoadCoordinatorScript.new()
var _world_load_pending := false
# The mission a `--mission` launch starts in, until its load ends: one that fails is named on a
# line of the log the editor's Play reads back (ResourceRoot.launch_mission_failed_marker()).
var _launch_mission := ""
var _end_flow := MissionEndFlow.new()  # the SP end-of-mission flow (round_end -> the cine's screen)
var _sp_restart_info: LoadingScreenInfo = null  # the last SP load: the restart's entry
var _sp_restart_loader := Callable()
# The join screen (pre.mnu PRE_GAME_MENU) a join runs on until the host starts the game.
var _join_screen := PreGameMenuPresenter.new()
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
	_join_screen.name = "PreGameMenuPresenter"
	add_child(_join_screen)
	_join_screen.cancelled.connect(_on_join_screen_cancelled)
	_join_screen.game_starting.connect(_on_join_game_starting)


func _on_player_options_changed(state: PlayerOptions.State) -> void:
	# update() applied the device-global audio once already; only the running
	# Simulation's mouse settings are this listener's to push.
	var sim: Simulation = _world.get_sim() if _world != null else null
	_player_options.apply_mouse(sim)
	# The world copies the object detail at its next mission start.
	if _world != null:
		_world.set_object_polydetail(state.object_polydetail)
	if _hud_presenter != null:
		_hud_presenter.set_crosshair_style(state.crosshair_style)
		_hud_presenter.set_crosshair_color(state.crosshair_color)
		_hud_presenter.set_crosshair_spread_enabled(state.crosshair_spread)
		_hud_presenter.set_aspect_mode(state.aspect_mode)
		_hud_presenter.set_tip_options(state.keyboard_tips, state.gameplay_tips)


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
	_close_retail_picker()
	for presenter in [
		_player_presenter, _armory_presenter, _deploy_presenter, _command_map_presenter,
		_hud_presenter,
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
	if dir.is_empty() and LaunchFlags.resource_dir_given():
		push_warning("OpenNova: --resource-dir needs a path. Usage: opennova.exe [-- --resource-dir <path> [--loose-root] [/d]]")
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
	# The player profile every mission's sim takes its current records from
	# (PlayerProfile, loaded again at each menu start on a mount).
	_world.set_player_profiles(PlayerProfile.store())
	_world.music_context_opened.connect(MusicService.open_game_context)
	_world.music_context_closed.connect(MusicService.stop_context)
	_world.music_var_changed.connect(MusicService.set_var)
	# The explicit launch directory and current game/expansion selection.
	_world.set_resource_root_resolver(LaunchResourceRootResolver.new())
	_hud_presenter = GameHudPresenter.new()
	_hud_presenter.name = "GameHudPresenter"
	add_child(_hud_presenter)
	_hud_presenter.setup(_world, _player_presenter, _hud if _hud != null else self)
	# The deploy screen's MAP window draws through the HUD overlay's map state.
	_deploy_presenter.set_hud_source(_hud_presenter.get_game_hud)
	# The commander map (cmap.mnu CMAP) over live play; the commander_menu row's
	# poll event opens it from WORLD, and it owns the cursor while up.
	_command_map_presenter = CommandMapPresenter.install(self, _world.world_view(),
			_hud if _hud != null else self, func() -> void: _state = State.COMMAND_MAP,
			_leave_screen.bind(State.COMMAND_MAP))
	_command_map_presenter.set_hud_source(_hud_presenter.get_game_hud)
	_hud_presenter.command_map_requested.connect(_on_command_map_requested)
	_hud_presenter.quit_confirmed.connect(_on_quit_confirmed)
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
	if dir.is_empty():
		# No --resource-dir: OpenNova's own game (ADR 0048), whose build is game
		# data as an install is, so the launch shortcuts below follow it too.
		if not _enter_bundled_menu():
			get_tree().quit(1)
			return
	elif not _enter_menu(dir):
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
		_launch_mission = sp_mission
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
	# An open chat line takes every key (Esc included) before anything else
	# sees it; only the shell's own window/tools keys pass.
	if event is InputEventKey and _state == State.WORLD and _hud_presenter != null \
			and _hud_presenter.is_chat_capturing():
		var chat_key := event as InputEventKey
		if not WindowState.is_toggle_event(event) and chat_key.keycode != DEV_TOOLS_KEY \
				and _hud_presenter.handle_chat_key(chat_key):
			get_viewport().set_input_as_handled()
			return
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
	# Once the SP round is over the special-key chain's round-over leg takes
	# every key: RESTART runs the splash re-run (over a custom loading
	# background) and the restart, ESC the quit; both reach the shell as the
	# session's mission exit (_on_session_lost).
	if _state == State.WORLD and _end_flow.is_round_ended() and _hud_presenter != null \
			and not _world_load_pending:
		var bits := _hud_presenter.handle_round_over_key(key)
		if bits & HudToggles.ROUND_OVER_RESTART:
			_begin_restart_splash()
		if bits & HudToggles.ROUND_OVER_CONSUMED:
			get_viewport().set_input_as_handled()
			return
	# The special-key chain takes its keys before the action rows see them:
	# the quit dialog's, the page keys (the Tab board, the status page, help,
	# the briefing) and the status page's Enter.
	if is_gameplay_input_active() and _hud_presenter != null \
			and _hud_presenter.handle_special_key(key):
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


# An open chat line does not park gameplay input: it owns the keyboard only
# (ControlsModel.set_keyboard_captured, set each frame in _process), so mouse
# look and the mouse rows stay live as retail's do.
func is_gameplay_input_active() -> bool:
	return _state == State.WORLD and not _end_flow.is_round_ended() \
			and (not is_dev_tools_open() or _dev_tools.is_game_playing())


# --- End of mission (SP) -------------------------------------------------------

# The sim's round_end effect: out of a session the engine started its
# end-of-round cine on the same round end, so the flow arms, the HUD's windows
# close (the cine start's respawn init), the cine's screen mounts at once, and
# the effect's b word (1 win / 2 lose) reaches the gamemus MessageHandler
# through MusicDirector.signal_end_track (the engine's mus_vm_signal). begin()
# refuses an MP round (EndRoundPresenter owns it), so a net session never
# signals. The exits are the session's (_on_session_lost routes them).
func _on_shell_mission_effects(effects: Array) -> void:
	for e_v in effects:
		var e := e_v as MissionEffect
		if e != null and e.kind == "round_end":
			var sim: Simulation = _world.get_sim() if _world != null else null
			var armed_before := _end_flow.is_round_ended()
			_end_flow.begin(sim)
			if not armed_before and _end_flow.is_round_ended():
				if _hud_presenter != null:
					_hud_presenter.begin_end_of_round_cine()
				_show_end_screen()
				var director: MusicDirector = MusicService.director()
				if director != null:
					director.signal_end_track(e.b)


func _show_end_screen() -> void:
	var banner := Callable()
	if _hud_presenter != null:
		banner = _hud_presenter.endround_banner_line
	_end_flow.show_screen(_world.get_sim() if _world != null else null, banner, _root,
			_hud if _hud != null else self)
	Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)


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
		State.COMMAND_MAP:
			return "command_map"
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
	# The menu, loading screen, and world are one runtime resource session.
	# GameWorld must not remount from mutable persisted settings after boot.
	_world.set_resource_root(_root)
	# The mounted expansion's text-override table (a loose expansion/<n>/<n>.bin
	# only, never the archived copy), installed as the table every string lookup
	# tries first and re-installed on each remount of this root, the menu's and the
	# join's expansion switches included (Strings.track_expansion_override).
	Strings.track_expansion_override(_root)
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
	# The player profile loads after the menu's text tables, as the original's
	# menu start loads game.bin before it (engine: runtime/profile/player_profiles.h).
	refresh_local_profile_for_mount()
	_run_intro_tail()
	_menu_shell.show_menu()
	return true


# The first menu start's intro step without its videos (which OpenNova does not
# play): every profile record's +1412 cleared and the profile saved, so a first
# run writes player.sav and weapon.sav as the original's does
# (engine: PlayerProfiles.clear_intro_pending).
func _run_intro_tail() -> void:
	if _intro_tail_run:
		return
	_intro_tail_run = true
	PlayerProfile.store().clear_intro_pending()
	PlayerProfile.save()


# --- Bundled menu + retail picker (ADR 0048) ----------------------------------

# No --resource-dir: mount OpenNova's own bundled assets/ and show its
# placeholder main menu, whose PLAY RETAIL hands over to a retail install.
func _enter_bundled_menu() -> bool:
	var root := BootRootMount.mount_bundled(BootRootMount.bundled_assets_dir())
	if root == null:
		return false
	_root = root
	if not _enter_menu(root.get_root_dir()):
		return false
	# The boot marker the CLI startup contract test reads (--verbose).
	print_verbose("OpenNova: bundled menu up from %s" % root.get_root_dir())
	return true


## PLAY RETAIL: the saved retail install when it still mounts, else the picker.
## A saved install that fails to mount is forgotten, so the next PLAY RETAIL
## goes straight to the picker. Public so lifecycle tests drive the same leg
## the button does.
func play_retail() -> bool:
	# The web build's staged install lives only as long as the tab, so there
	# is nothing saved to mount: the page's own dialog offers the folder the
	# browser remembers (ADR 0049).
	if OS.has_feature("web"):
		request_retail_dir()
		return false
	var saved := ResourceDirSettings.get_retail_dir()
	if not saved.is_empty():
		if enter_retail_dir(saved):
			return true
		ResourceDirSettings.set_retail_dir("")
	request_retail_dir()
	return false


## Mount `dir` as the session's retail install and open its menu, saving it for
## the next PLAY RETAIL. False (nothing saved, the current menu stays) when the
## directory holds no game archives.
func enter_retail_dir(dir: String) -> bool:
	var root := BootRootMount.mount(dir, false)
	if root == null:
		return false
	if not OS.has_feature("web"):
		ResourceDirSettings.set_retail_dir(dir)
	_root = root
	return _enter_menu(root.get_root_dir())


## CHANGE FOLDER (and PLAY RETAIL with nothing saved): the native folder picker.
## Headless runs and an already-open picker skip it.
func request_retail_dir() -> void:
	if GameRuntimeRoot.is_headless() or _retail_picker != null:
		return
	if OS.has_feature("web"):
		_request_web_retail_dir()
		return
	var picker := FileDialog.new()
	picker.file_mode = FileDialog.FILE_MODE_OPEN_DIR
	picker.access = FileDialog.ACCESS_FILESYSTEM
	picker.use_native_dialog = true
	picker.title = "Select your Joint Operations folder"
	var saved := ResourceDirSettings.get_retail_dir()
	if not saved.is_empty():
		picker.current_dir = saved
	picker.dir_selected.connect(_on_retail_dir_selected)
	picker.canceled.connect(_close_retail_picker)
	_retail_picker = picker
	add_child(picker)
	picker.popup_centered_ratio(0.6)


# The browser has no native folder dialog: the page stages the picked install
# into its in-memory filesystem and answers with the mount path (ADR 0049).
func _request_web_retail_dir() -> void:
	if _web_retail_picking:
		return
	_web_retail_picking = WebRetailPicker.request(func(dir: String) -> void:
		_web_retail_picking = false
		if not dir.is_empty():
			_on_retail_dir_selected(dir))


func _on_retail_dir_selected(dir: String) -> void:
	_close_retail_picker()
	if enter_retail_dir(dir):
		return
	if OS.has_feature("web"):
		WebRetailPicker.discard(dir)
	var notice := AcceptDialog.new()
	notice.title = "OpenNova"
	notice.dialog_text = ("No Joint Operations game data was found in\n%s\n\n"
			+ "Choose the folder that holds resource.pff, localres.pff and language.pff.") % dir
	notice.confirmed.connect(notice.queue_free)
	notice.canceled.connect(notice.queue_free)
	add_child(notice)
	notice.popup_centered()


func _close_retail_picker() -> void:
	if _retail_picker != null:
		_retail_picker.queue_free()
		_retail_picker = null


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
	_menu_shell.restart_requested.connect(_on_restart_requested)
	_menu_shell.resume_requested.connect(resume)
	_menu_shell.novaworld_requested.connect(_net.open_novaworld_panel)
	_menu_shell.game_reloaded.connect(_on_game_reloaded)
	_bundled_companion = BundledMenuCompanion.new()
	_bundled_companion.play_retail_requested.connect(play_retail)
	_bundled_companion.change_folder_requested.connect(request_retail_dir)
	_bundled_companion.exit_requested.connect(_on_exit_to_desktop)
	_menu_shell.add_companion(_bundled_companion)
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


# The Mods list switched the game: the profile loads again under the expansion it
# took (the expansion's weapon.sav), the one the menu shows and the next spawn
# uses (refresh_local_profile_for_mount hands it to PLAYER_INFO).
func _on_game_reloaded() -> void:
	refresh_local_profile_for_mount()


# PLAYER_INFO ACCEPT writes the name and both side records into the current
# profile record in memory, as the original's dialog does (the next save point
# writes the files), while the selected loadout continues through the existing
# spawn-kit seam.
func _on_avatar_chosen(profile: Dictionary) -> void:
	set_local_player_profile(profile)
	var accept_error := PlayerProfile.accept_player_info(profile)
	if accept_error != OK:
		push_warning("MainGame: the PLAYER_INFO selection was not taken (error %d)"
				% accept_error)


## Load the player profile (player.sav, and the mounted expansion's weapon.sav)
## again when the mounted root or its expansion changed, as the original's menu
## start loads it; the menu entry and a join's pre-dial switch to the host's
## expansion both land here, so the next spawn kit and the join's character vars
## come from the mounted expansion's profile. The menu's text tables must be
## registered first: a fresh record's macros are the menu table's.
func refresh_local_profile_for_mount() -> void:
	if _root == null:
		return
	var profile_root_key := "%s|%s|%s" % [String(_root.get_root_dir()),
			String(_root.get_expansion()).to_lower(), String(LaunchFlags.working_dir())]
	if profile_root_key != _profile_root_key:
		var load_error := PlayerProfile.load_for(_root)
		if load_error != OK:
			push_warning("MainGame: the player profile could not be read (error %d); defaults stand"
					% load_error)
		_chosen_avatar = PlayerProfile.load_character_profile(_root)
		_profile_root_key = profile_root_key
		if _player_info_companion != null:
			_player_info_companion.set_persisted_profile(_chosen_avatar)


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
	_save_profile_for_single_player()
	# Single-player: the loading screen is the sidecar image alone — no session
	# text [orig: the not-in-session path draws only the background @ 0x521ebe].
	start_world_load(
		LoadingScreenInfo.for_mission(bms_name),
		_world.load_mission.bind(bms_name))


# The single-player start's profile legs: the started mission's campaign index
# into the current record (the catalog's, -1 on every row), then the profile
# saved ahead of the session (engine: PlayerProfiles.record_mission_start).
func _save_profile_for_single_player() -> void:
	PlayerProfile.store().record_mission_start(-1)
	PlayerProfile.save()


## Boot an exact loose mission through the same loading
## presentation and GameWorld lifecycle as menu play.
func start_loose_mission(bms_name: String) -> void:
	_save_profile_for_single_player()
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
	_save_profile_for_single_player()
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
## `restart` is the SP restart's start: no loading screen comes up (and so no
## closing splash). A single-player load is remembered as the restart's entry.
func start_world_load(load_info: LoadingScreenInfo, operation: Callable,
		restart := false) -> void:
	if not _world_load.can_start():
		return
	if _lan_session != null:
		_lan_session.stop()
	if load_info != null and not load_info.in_session:
		_sp_restart_info = load_info
		_sp_restart_loader = operation
	_world_load_pending = true
	_world.set_local_player_spawn_loadout(PlayerSpawnLoadout.from_profile(_chosen_avatar))
	_begin_world_load()
	if _world_load.start(self, _root, _world, load_info, operation, restart) == null:
		_on_world_load_failed("mission load handoff could not start")


## A join's mission start: the join runs on the join screen (pre.mnu
## PRE_GAME_MENU) until the host starts the game, and the loading screen takes
## over only then. The join screen's mode stops the menu music on entry.
## (engine: inmatch/pre_game_menu.h)
func start_join_load(load_info: LoadingScreenInfo, operation: Callable) -> void:
	if not _world_load.can_start():
		return
	if not _join_screen.open(_root, self):
		start_world_load(load_info, operation)
		return
	MusicService.stop_context()
	if _lan_session != null:
		_lan_session.stop()
	_world_load_pending = true
	_world.set_local_player_spawn_loadout(PlayerSpawnLoadout.from_profile(_chosen_avatar))
	_begin_world_load()
	_join_screen.follow(_world)
	if _world_load.start(self, _root, _world, load_info, operation, true) == null:
		_on_world_load_failed("mission load handoff could not start")


## The join screen is up (the probe and test read).
func is_join_screen_open() -> bool:
	return _join_screen.is_open()


func get_join_screen() -> PreGameMenuPresenter:
	return _join_screen


# The host started the game: the join screen hands over to the loading screen.
func _on_join_game_starting() -> void:
	_join_screen.close()
	_world_load.show_deferred_screen()


# Cancel on the join screen: an abandoned join leaves the way a cancelled load
# does; a failure already shown leaves to the menu it came from.
func _on_join_screen_cancelled() -> void:
	_join_screen.close()
	if _world_load_pending and _world != null and _world.cancel_join_preload():
		return
	_teardown_world_to_menu()


func _begin_world_load() -> void:
	# Every mission start's session apply copies the profile's +1460 word, which
	# the round end writes back, and its ten macros, the chat presets
	# (engine: runtime/profile/player_profiles.h).
	PlayerProfile.store().begin_session()
	if _hud_presenter != null:
		_hud_presenter.set_chat_presets(PlayerProfile.store().get_macros())
	# A mission start from the menu leaves it: the menu keeps the screen it was
	# started from, which the return after the mission shows again (D-MNU-28).
	_menu_shell.leave_menu_mode()
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
	# sessions); the pick session curates the shell-owned list from here on.
	_launch_mission = ""  # the launch's mission loaded: a later load's failure is not its
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
	# [orig: Game_ShowStartMissionSplash @ 0x520820 precedes the release @ 0x525d45].
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
	# reopens on the new pending edge. The HUD declutter blank is NOT this
	# edge's: retail's 0x0F forces it only with the death screen up (the replica
	# fold queues it, the HUD presenter's drain applies it; a fresh join leaves
	# the level alone, hud-re.md "Forced levels").
	_finish_world_load_presentation()
	# The frame loop's death trigger may have opened it already.
	if _deploy_presenter.is_open() or _deploy_presenter.open():
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
## [orig: Server_TickUpdate linger drain @0x51da04..; g_MissionExitReason = 3
##  @0x51db63; every exit reason lands on the same teardown + nav push
##  @0x568654. SP mission end runs the epilog flow instead.]
# The frame loop's death.mnu DEATH open, every frame and every role (the
# listen host's own client included): the engine's triggers (the host-driven
# deploy-map overlay, or the in-session local death), its once-per-arming
# open latch with its result-blind stamp, and its clear when the triggers
# fall all live on the engine side (inmatch::death_menu_triggered,
# ClientState::take_death_menu_open; hud-re D-HUD-19); this leg is the device
# call. Any state but State.WORLD stands in for retail's no-active-menu gate
# (PAUSED / ARMORY / DEPLOY / END_ROUND all hold a screen), so the latch is
# never burned under another screen and the open retries on the next clear
# frame. Closing is the presenter's own affair. NOT yet modeled (hud-re
# D-HUD-19 residuals): the spawn-success suppression gate.
func _maybe_open_death_menu() -> void:
	if _state == State.MENU or _world == null:
		return
	var sim: Simulation = _world.get_sim()
	if sim == null:
		return
	if not bool(sim.take_death_menu_open(_state != State.WORLD or _world_load_pending)):
		return
	_deploy_presenter.open()


## An established session ended without the player asking: the host closed it on its own
## terms (its punt channel — a CRC mismatch, a violation sweep, the six-minute deploy-screen
## idle kick), or it went silent past the connection reap window. Retail EXITS THE MISSION
## with a reason here and raises no in-world dialog, so this takes the shell's existing
## abort-to-menu leg with the decoded reason named.
## [orig: the punt record CNapiNPConnection_HandleDescriptionPacket @ 0x621ae0 and the
##  cs_dir0.timeout_ms = 120000 reap CNapiNetwork_Init @ 0x4ca4a0, both ->
##  CNapiNetwork_OnDisconnectedFromServer @ 0x4c63d0. The captured DPC 33 falls to
##  Input_QueueEvent(3) @ 0x4c67a4, whose action sets g_MissionExitReason = 1 and drops
##  the connection (Input_HandleActionBinding case 3 @ 0x49af2c) — reason 1 is the same
##  teardown + "MainMenu" push every abort leg takes @ 0x568654]
func _on_session_lost(reason: String) -> void:
	# Already back in the menu with nothing loading: the teardown ran (this is the
	# double-notification guard, not a state test the loss depends on). A loss during
	# the load presentation still routes through the same leg — it clears
	# _world_load_pending on its way to the menu.
	if _state == State.MENU and not _world_load_pending:
		return
	# The exit reason the session stored (the NovaWorld exit, a mapped disconnect record);
	# an unmapped loss leaves the mission the way the player's own quit would.
	var sim: Simulation = _world.get_sim() if _world != null else null
	var exit_reason := sim.get_mission_exit_reason() if sim != null else 0
	if exit_reason == 0:
		exit_reason = GameWorld.MISSION_EXIT_QUIT
	# The main frame's router (engine inmatch/mission_exit.h main_frame_exit):
	# the SP restart's reason starts the same mission again; every other reason
	# leaves for the menu (the Post Menu's route).
	if _world.main_frame_exit(exit_reason) == GameWorld.MAIN_FRAME_EXIT_RESTART_ROUND_SP:
		_restart_mission()
		return
	if sim != null and not sim.is_mp_session():
		# A single-player mission's own exit (the end screens' ESC and timeout):
		# the ordinary way out, not an abort.
		_world_load_pending = false
		_teardown_world_to_menu(exit_reason)
		return
	if _route_round_end(sim, exit_reason):
		return
	_abort_to_menu("session ended", reason, exit_reason)


## The in-session round end's routes (engine inmatch/mission_exit.h main_frame_exit,
## inmatch/map_change.h). A joiner's round over (4) is the Game Loop: it reloads the host's
## next mission in place on its kept connection. An in-session host's round end (3, or 4
## under REPLAY with LASTGAME off) is the Post Menu's map change: the next rotation entry
## loads inside the session, every joiner kept; at the rotation's end the session ends
## (its close sends the joiners the STOP goodbye) and the shell leaves for the menu.
## False when the exit is not a round end (the caller aborts to the menu).
func _route_round_end(sim: Simulation, exit_reason: int) -> bool:
	if sim == null or _world_load_pending:
		return false
	var verdict: int = _world.main_frame_exit(exit_reason)
	if verdict == GameWorld.MAIN_FRAME_EXIT_GAME_LOOP:
		if not _world.begin_joiner_reload():
			return false
		_reload_in_session(LoadingScreenInfo.make("", true, sim.get_join_server_name(), "",
				0, ""), _world.reload_joiner)
		return true
	if verdict != GameWorld.MAIN_FRAME_EXIT_POST_MENU or sim.is_joiner():
		return false
	if exit_reason != GameWorld.MISSION_EXIT_MAP_CYCLE \
			and exit_reason != GameWorld.MISSION_EXIT_ROUND_OVER:
		return false
	var next_map: String = _world.begin_map_change()
	if next_map.is_empty():
		# The rotation ran out: the session's own end, not an abort.
		_world_load_pending = false
		_teardown_world_to_menu(exit_reason)
		return true
	var config: HostSessionOptions = sim.get_host_session_config()
	_reload_in_session(LoadingScreenInfo.make(next_map, true,
			config.server_name if config != null else "", "", 0, ""),
			_world.load_next_mission.bind(next_map))
	return true


## The world comes down around the session the world kept, and the next mission
## loads into it with no menu between.
func _reload_in_session(load_info: LoadingScreenInfo, loader: Callable) -> void:
	_teardown_world(false)
	_state = State.WORLD
	start_world_load(load_info, loader)


func _on_world_load_failed(reason: String) -> void:
	# A join that fails before the game start stays on the join screen with the
	# connection's reason and only Cancel (inmatch/pre_game_menu.h).
	if _join_screen.is_open():
		_world_load_pending = false
		push_warning("MainGame: join failed: %s" % reason)
		var error: ConnectionError = _world.get_last_connection_error() if _world != null else null
		var text := reason
		if error != null and error.is_set():
			text = error.reason_text(Strings.get_override_table(),
					Strings.get_table(Strings.TABLE_GAMEERR))
		_net.note_join_failure(error)
		_join_screen.show_failure(text)
		return
	# A load-step failure or abort returns to the menu — the witnessed early
	# return that sets reason=1 and nav-pushes "Post Menu" out of
	# Game_StartMission [orig: the "Mission loading aborted" legs @ 0x520270
	# (Client_CheckDisconnectOrEscDuringLoad) and the network-wait failure legs;
	# scene_entry = "Post Menu"] (docs/interface/loading-screen-re.md, the
	# load-flow case matrix).
	if not _launch_mission.is_empty():
		# The launch's own mission (`--mission`, the editor's Play mission): said on a line of
		# its own, which Play reads back by its marker.
		push_warning("MainGame: %s%s %s"
				% [ResourceRoot.launch_mission_failed_marker(), _launch_mission, reason])
		_launch_mission = ""
	_abort_to_menu("mission load failed", reason)


# THE abort-to-menu leg. Every caller names the stage it aborted from; the presentation
# is one teardown because retail's is one too (every reason lands on the same nav push),
# and the exit reason picks the post-mission route.
func _abort_to_menu(stage: String, reason: String,
		exit_reason := GameWorld.MISSION_EXIT_QUIT) -> void:
	_world_load_pending = false
	push_warning("MainGame: %s: %s" % [stage, reason])
	_teardown_world_to_menu(exit_reason)


func _on_camera_escape() -> void:
	# Esc: pause <-> resume while in a world (the armory closes back to play);
	# ignored in the main menu (EXIT quits). During a load, BOTH joiner waits are
	# interruptible legs — the pre-load connect/session wait and the post-load
	# admission tail, which is polled once per world tick. Aborting either
	# is the reachable analog of the original's per-asset ESC/disconnect abort
	# poll [orig: Client_CheckDisconnectOrEscDuringLoad @ 0x520270]. The
	# SP/host map load remains a single synchronous call the SceneTree cannot
	# interrupt (docs/interface/loading-screen-re.md D-LOADSCR-7).
	# The join screen's Cancel answers ESC.
	if _join_screen.is_open():
		_join_screen.cancel()
		return
	if _world_load_pending:
		if _world != null and _world.cancel_join_preload():
			return
		if _world != null:
			_world.cancel_join_admission()
		return
	# Round over: the special-key chain's round-over leg owns ESC (it stores the
	# quit exit), and the escape action does nothing out of a session (engine
	# hud_toggles.h hud_toggles_escape / hud_round_over_key).
	if _end_flow.is_round_ended():
		return
	# Over live play the HUD's escape chain closes one open HUD window first;
	# only with none open does the in-game menu open.
	if _state == State.WORLD and _hud_presenter != null and _hud_presenter.handle_escape():
		return
	if _state in [State.WORLD, State.DEPLOY, State.END_ROUND]:
		# ESC from the deploy screen still reaches the in-game menu (and therefore
		# RETURN TO MENU): a joiner parked at the pick must be able to leave.
		_pause()
	elif _state == State.PAUSED or _state == State.ARMORY:
		resume()
	elif _state == State.COMMAND_MAP and _command_map_presenter != null:
		_command_map_presenter.close()


## The quit dialog's yes: the Exit Mission tone, then the mission ends and the
## shell returns to the menu (the host's session goes with it).
func _on_quit_confirmed() -> void:
	var audio: MissionAudio = _world.get_mission_audio() if _world != null else null
	if audio != null:
		audio.ui_soundset(GameHudPresenter.QUIT_CONFIRM_SOUNDSET)
	_on_return_to_menu.call_deferred()


func _on_command_map_requested() -> void:
	if _state == State.WORLD and _command_map_presenter != null:
		_command_map_presenter.open()


func _leave_screen(from_state: int) -> void:  # a closing screen hands play back
	if _state == from_state:
		_state = State.WORLD


func _pause() -> void:
	_state = State.PAUSED
	# The engine session pauses with the shell; net roles refuse natively.
	_world.set_shell_paused(true)
	if _hud_presenter != null:
		_hud_presenter.set_menu_pause(true)
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
	if _hud_presenter != null:
		_hud_presenter.set_menu_pause(false)


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
# of the shell boundary rather than a menu-specific detail. The exit reason picks the
# post-mission route (GameWorld.post_mission_route): a NovaWorld session the route keeps
# leaves the world before it unloads and re-enters the NovaWorld menu; an error route
# shows its text first.
func _teardown_world_to_menu(exit_reason := GameWorld.MISSION_EXIT_QUIT) -> void:
	var route: PostMissionRoute = _world.post_mission_route(exit_reason) if _world != null \
			else PostMissionRoute.new()
	var novaworld_client: NovaWorldClient = null
	if route.keep_session:
		novaworld_client = _world.release_novaworld_client() as NovaWorldClient
	_teardown_world(false)
	if _root != null and _enter_menu(_root.get_root_dir()):
		_net.return_from_mission(novaworld_client, route.error, post_mission_error_text(route))
		return
	if novaworld_client != null:
		novaworld_client.stop()
		novaworld_client.free()
	push_warning("OpenNova: the game-data directory is no longer mountable")
	get_tree().quit(1)


# The world and every presenter over it come down: the menu teardown's head, and
# the SP restart's (`restart` keeps the HUD tip's once-counters, as the
# restart's start does).
func _teardown_world(restart: bool) -> void:
	_record_round_end()
	_world_load.dismiss()
	_join_screen.close()
	finish_hud_hidden_capture()
	_end_flow.reset()
	if _player_presenter != null:
		_player_presenter.teardown()
	if _armory_presenter != null:
		_armory_presenter.teardown()  # the built menu holds the OLD world's resource root
	if _deploy_presenter != null:
		_deploy_presenter.teardown()  # same stale-root hazard, and the shell's blanket
		# HUD visibility toggle would re-show a surviving DEATH shroud next mission
	if _command_map_presenter != null:
		_command_map_presenter.teardown()  # the cmap.mnu frame holds the OLD world's root
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
		_hud_presenter.teardown(restart)


# The mission teardown's profile step (and the SP restart's): outside a session
# it runs only once the round is over, then the profile's +1460 word goes back
# and the profile is saved. No catalog row carries a campaign, so no
# completion byte is written (engine: PlayerProfiles.record_round_end).
func _record_round_end() -> void:
	var sim: Simulation = _world.get_sim() if _world != null else null
	if sim == null:
		return
	var in_session := sim.is_host_listening() or sim.is_joiner()
	if PlayerProfile.store().record_round_end(in_session, sim.is_round_over(), -1, -1, false):
		PlayerProfile.save()


## The SP restart: the main frame routed exit reason 4 out of a session (engine
## inmatch/mission_exit.h main_frame_exit). The mission comes down and the same
## mission starts again from its own load entry, with no menu, no loading
## screen and no closing splash (the restart's start draws neither).
func _restart_mission() -> void:
	var loader := _sp_restart_loader
	var info := _sp_restart_info
	if not loader.is_valid() or info == null:
		_teardown_world_to_menu(GameWorld.MISSION_EXIT_QUIT)
		return
	_teardown_world(true)
	_state = State.WORLD
	start_world_load(info, loader, true)


## The round-over RESTART key's splash re-run, over a custom loading background
## only (WorldLoadCoordinator.begin_restart_splash): the shell's frame holds the
## world while it is up, so the restart exit follows its dismissal.
func _begin_restart_splash() -> void:
	if _sp_restart_info == null or _world == null:
		return
	_world_load.begin_restart_splash(self, _root, _world, _sp_restart_info,
			_world.get_mission_audio())


## The in-game menu's RESTART (World::ingame_restart_command): out of a session
## the in-game screens close and the session resumes, and its next frame
## carries the restart exit.
func _on_restart_requested() -> void:
	var sim: Simulation = _world.get_sim() if _world != null else null
	if sim == null or _world_load_pending or not sim.ingame_restart():
		return
	resume()


# The error text the post-mission route stores: a gameerr.bin generic error, else the
# in-match connection's disconnect reason (empty while it was healthy; the dialog then
# shows its unknown-error text).
static func post_mission_error_text(route: PostMissionRoute) -> String:
	var key := route.error_key
	if key.is_empty():
		# A lost connection reads retail's reason text over its error record
		# (engine: inmatch/disconnect_reason.h).
		var connection_error := route.connection_error
		if connection_error != null and connection_error.is_set():
			return connection_error.reason_text(Strings.get_override_table(),
					Strings.get_table(Strings.TABLE_GAMEERR))
		return route.error_text
	var text := Strings.lookup_or(Strings.TABLE_GAMEERR, Strings.SECTION_GENERIC_ERRORS, key, "")
	if text.is_empty() and key == "STRE_BADMISSION":
		return "Error - Mission requires assets or an expansion that is not present"
	return text if not text.is_empty() else key


# A browser tab cannot be quit from inside: stopping the engine would only
# leave a frozen canvas, so the web build ignores EXIT (ADR 0049).
func _on_exit_to_desktop() -> void:
	if not OS.has_feature("web"):
		# The main menu's EXIT saves the player profile before the game quits
		# (engine: runtime/profile/player_profiles.h, the save points).
		PlayerProfile.save()
		request_quit()


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
	# The chat line's keyboard capture for this frame's samplers (the witness
	# rides ControlsModel.set_keyboard_captured).
	ControlsBindings.model().set_keyboard_captured(_state == State.WORLD \
			and _hud_presenter != null and _hud_presenter.is_chat_capturing())
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
	if _state in [State.PAUSED, State.ARMORY, State.DEPLOY, State.END_ROUND, State.COMMAND_MAP] \
			or dev_tools_interacting or _end_flow.has_screen() or not _world.is_loaded():
		if Input.get_mouse_mode() == Input.MOUSE_MODE_CAPTURED:
			Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)
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
	# The frame loop feeds the session the time since the last render: the
	# mission-start frames bank only that (Session::advance).
	frame_input.since_render_seconds = _world.get_seconds_since_render()
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
			and _state in [State.WORLD, State.ARMORY, State.DEPLOY, State.END_ROUND,
					State.COMMAND_MAP]:
		_hud_presenter.tick(is_gameplay_input_active())
		_end_round_presenter.tick()  # the same HUD frame [orig: HUD_DrawOverlayPanels]
	var probe_t4 := Time.get_ticks_usec() if timing else 0
	_maybe_open_death_menu()
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
