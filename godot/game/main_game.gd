class_name MainGame
extends GameShell

# Runtime shell: boots into the game's menu front-end (MenuShell, driving the
# .mnu menu set + audio from the chosen resource dir) and hands off to a GameWorld
# when the player starts a mission, with pause + return-to-menu on demand.
# Everything (menus, audio, terrain, missions) loads from the --resource-dir
# supplied at launch, otherwise the executable's directory. /game explicitly
# selects the product (OpenNova by default); GameRuntimeRoot installs missing
# OpenNova content before constructing this data-dependent shell.

const PlayerOptionsScript := preload("res://game/player_options.gd")
const GameDebugAdapterScript := preload("res://game/game_debug_adapter.gd")
const WorldLoadCoordinatorScript := preload("res://game/world_load_coordinator.gd")
const ShellPresentationSessionScript := preload("res://game/shell_presentation_session.gd")
const HudHiddenCaptureWitness := preload("res://game/world/hud_hidden_capture_witness.gd")
# The armory key: the USE-ITEM key (input action 177 "useitem"; retail default SHIFT on
# the shipped KeyChart, labeled "USE ITEM/ATTACH/ARMORY"), zone-gated to a type-6 armory
# volume (engine frontend::Frontend::use_item carries the witness). Out of zone the key's
# other arms are the input router's per-frame chain over the polled `useitem` row (the hold
# latch, the USE+digit seat pick, the mount toggle on release); the shell only matches the
# press event against that row's live keys (_is_use_item_key).
# Insert: the in-engine dev tools (the DevTools node's ImGui windows, ADR 0039).
# Off F3, which is retail's `viewwithgun` default (catalog row 108). Insert is
# also retail Jump's secondary key (row 8): the polled Jump row still sees a
# toggle press, an accepted overlap for a debug-build key.
const DEV_TOOLS_KEY := KEY_INSERT
# Shift+F6: pick the entity under the crosshair into the debug pick list
# (DebugPickSession). Works while playing or with the dev tools in Interact. Unmodified F6 stays
# with the retail-configurable binding rows (huddetail's default; hudcolor's is
# Ctrl+F6, D-CTRL-4).
const PICK_KEY := KEY_F6
# The cheat row 117's dispatch code: its record reaches the shell's arm past the engine's
# gates, out of a session with a local player (engine inmatch::Role::dispatch_record carries
# the witness), and toggles the WEAPON screen (_toggle_weapon_screen).
const WEAPON_SCREEN_CODE := 40

@onready var _world: GameWorld = $World
@onready var _camera: FlyCamera = $Camera3D
@onready var _hud: CanvasLayer = $HUD
@onready var _menu_layer: CanvasLayer = $MenuLayer
@onready var _menu_shell: MenuShell = $MenuLayer/MenuShell

var _root: ResourceRoot
# The game flow: the mode table, the in-mission screen up, the save points and the routing
# (engine frontend::Frontend); the shell supplies its legs (_install_frontend_legs).
var _frontend := GameFrontend.new()
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
# The probes holding the FrameStats board for their runs (hold_frame_stats).
var _frame_stats_holds := 0
var _mp_companion: MpMenuCompanion  # drives the multiplayer (mp.mnu) menu by control name
var _lan_session: LanSession  # retail-style 0x41/0x81 LAN enumeration browser
var _player_info_companion: PlayerInfoMenuCompanion  # drives the PLAYER_INFO (player.mnu) character screen
var _armory_presenter: ArmoryPresenter  # the SHARED in-world armory surface (weapon.mnu WEAPON)
# The in-world vehicle screen (vehicle.mnu VEHICLE) over the engine's VehicleMenu.
var _vehicle_menu_presenter: VehicleMenuPresenter
var _deploy_presenter: DeployScreenPresenter  # the joiner's deploy-map screen (death.mnu DEATH)
var _command_map_presenter: CommandMapPresenter  # the commander map (cmap.mnu CMAP)
var _end_round_presenter: EndRoundPresenter  # the MP end-of-round overlay + stat.mnu STAT
# The input frame's keyboard pass over the device's key queue: every key-down
# waits there for the next input frame (KeyboardPass). Over the player
# presenter's queue once the presenter exists (its input router owns it).
var _keys := KeyboardPass.new()
var _chosen_avatar: Dictionary = {}  # canonical active + per-side PLAYER_INFO selection
var _profile_root_key := ""  # reload the profile only when the mounted game/expansion changes
var _world_load := WorldLoadCoordinatorScript.new()
var _end_flow := MissionEndFlow.new()  # the SP end-of-mission flow (round_end -> the cine's screen)
var _sp_restart_info: LoadingScreenInfo = null  # the last SP load: the restart's entry
var _sp_restart_loader := Callable()
var _pending_info: LoadingScreenInfo = null  # the start in hand: its loading screen and loader
var _pending_loader := Callable()
# The join screen (pre.mnu PRE_GAME_MENU) a join runs on until the host starts the game.
var _join_screen := PreGameMenuPresenter.new()
# The enumerate-and-join's abort while its join screen is up (begin_join_enumerate),
# and the choice's (begin_join_choice).
var _join_enumerate_abort := Callable()
# The choice's commit while its panels are up (begin_join_choice).
var _join_choice_committed := Callable()
var _shutdown_prepared := false
var _shutdown_resources_released := false
var _quit_policy_installed := false
## How the process ends with an exit code: empty quits the SceneTree with it.
## A driver that hosts the shell in its own process (the GUT suite) sets one.
var process_exit_handler := Callable()
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
	# Under --verbose the heavy frames print where they went (an instrument).
	if OS.is_stdout_verbose():
		var slow_frames := SlowFrameReport.new()
		slow_frames.setup(_frame_stats, _frame_stats_read_elsewhere, _post_load_frames_open)
		add_child(slow_frames)
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
	_join_screen.choice_committed.connect(_on_join_choice_committed)
	_install_frontend_legs()


# The frontend's legs (engine frontend::FrontendLegs): the stores' files, the menu's and the
# mission load's device work, the process's exit. The engine runs them in its order.
func _install_frontend_legs() -> void:
	var f := _frontend
	f.set_leg(GameFrontend.LEG_PROFILES, func() -> PlayerProfiles: return PlayerProfile.store())
	f.set_leg(GameFrontend.LEG_GAME_CONFIG, func() -> GameConfigBlock: return GameCfgStore.block())
	f.set_leg(GameFrontend.LEG_SAVE_PROFILE, func() -> void: PlayerProfile.save())
	f.set_leg(GameFrontend.LEG_SAVE_GAME_CONFIG, func() -> void: GameCfgStore.save())
	f.set_leg(GameFrontend.LEG_READ_MENU_CONFIG,
			func() -> bool: return GameCfgStore.enter_main_menu(_root))
	f.set_leg(GameFrontend.LEG_RELOAD_MENU_CONFIG,
			func() -> bool: return GameCfgStore.reload_main_menu(_root))
	f.set_leg(GameFrontend.LEG_LOAD_PROFILE, refresh_local_profile_for_mount.bind(true))
	# The menu browser's cookie jar, nw_cdata.coo in the working directory, kept for the
	# process (engine MenuBrowserState; NovaWorldClient.create_cookie_jar / save_cookie_jar).
	f.set_leg(GameFrontend.LEG_LOAD_COOKIE_JAR,
			func() -> void: NovaWorldClient.create_cookie_jar(LaunchFlags.working_dir()))
	f.set_leg(GameFrontend.LEG_SAVE_COOKIE_JAR, func() -> void: NovaWorldClient.save_cookie_jar())
	# The mission table's high scores to hiscore.txt in the working directory (engine
	# runtime/mission/mission_catalog.h HighScoreTable).
	f.set_leg(GameFrontend.LEG_SAVE_HIGH_SCORES,
			func() -> void: MissionCatalog.save_high_scores(String(LaunchFlags.working_dir())))
	# The record's binding table and ENABLE_JOYSTICK word onto the live bindings (engine
	# profile::apply_controls carries the witness).
	f.set_leg(GameFrontend.LEG_APPLY_CONTROLS,
			func() -> void: ControlsBindings.apply_profile(PlayerProfile.store()))
	f.set_leg(GameFrontend.LEG_RELOAD_OPTIONS, _player_options.reload)
	f.set_leg(GameFrontend.LEG_SHOW_MENU, func() -> void: _menu_shell.show_menu())
	f.set_leg(GameFrontend.LEG_RESET_KEY_QUEUE, func() -> void: _keys.clear())
	f.set_leg(GameFrontend.LEG_LOAD_CAN_START, _world_load.can_start)
	f.set_leg(GameFrontend.LEG_STOP_LAN_BROWSE, _stop_lan_browse)
	f.set_leg(GameFrontend.LEG_STOP_MUSIC, func() -> void: MusicService.stop_context())
	f.set_leg(GameFrontend.LEG_REMEMBER_RESTART_ENTRY, _remember_restart_entry)
	f.set_leg(GameFrontend.LEG_OPEN_JOIN_SCREEN, func() -> bool: return _join_screen.open(_root, self))
	f.set_leg(GameFrontend.LEG_BEGIN_WORLD_LOAD, _begin_world_load)
	f.set_leg(GameFrontend.LEG_BEGIN_LOAD, _begin_load)
	f.set_leg(GameFrontend.LEG_LOAD_FAILED, _on_world_load_failed)
	f.set_leg(GameFrontend.LEG_EXIT_PROCESS, exit_process)


func _on_player_options_changed(state: PlayerOptions.State) -> void:
	# update() applied the device-global audio once already. The aspect mode
	# never reaches a running mission: the view and the HUD take it at the next
	# mission start (_on_world_loaded), as retail's projection takes only the
	# session copy its start applies (PlayerOptions.apply carries the witness).
	# The world copies the object detail at its next mission start; the
	# texture filter reaches the model effects at once and the terrain's
	# device filter at the next mission start.
	if _world != null:
		_world.set_object_polydetail(state.object_polydetail)
		_world.set_texfilter_level(state.texfilter_level)
		# The aspect mode each mission start selects for the local view (the
		# stretched target made and held there, GameWorld.set_aspect_mode).
		_world.set_aspect_mode(state.aspect_mode)
		# The particle density, the texture compression and the terrain
		# texture detail reach the next mission the world starts (its session
		# copies, D-RMAT-24, D-RMAT-20).
		_world.set_particle_density(state.particle_density)
		_world.set_texcompression_level(state.texcompression_level)
		_world.set_terrain_texdetail(state.terrain_texdetail)
		# The object texture detail reaches the model textures the next mission
		# loads (its session copy, D-RMAT-20).
		_world.set_object_texdetail(state.object_texdetail)
	if _hud_presenter != null:
		_hud_presenter.set_crosshair_style(state.crosshair_style)
		_hud_presenter.set_crosshair_color(state.crosshair_color)
		_hud_presenter.set_crosshair_spread_enabled(state.crosshair_spread)
		_hud_presenter.set_tip_options(state.keyboard_tips, state.gameplay_tips)


func _notification(what: int) -> void:
	if what == NOTIFICATION_WM_CLOSE_REQUEST:
		# Every path that ends the process names itself in the log, since the
		# SceneTree's quit prints nothing (D-NET-310: a joiner's process ended with
		# no log line).
		push_warning("MainGame: the window's close request ends the process")
		request_quit()
	elif what == NOTIFICATION_APPLICATION_FOCUS_IN or what == NOTIFICATION_APPLICATION_FOCUS_OUT:
		# The window procedure clears the key state on gaining and on losing the
		# focus, the key state alone (engine controls::KeyStateReset carries the
		# witness).
		ControlsBindings.model().reset_key_state()
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
	_frontend.finish_load()
	finish_hud_hidden_capture()
	_teardown_presenters()
	if _hud_presenter != null:
		_hud_presenter.teardown()
	if _world != null:
		_world.cancel_join_preload()
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
## failure (the frontend's load). This is the public shell-level observation seam for
## load lifecycle tests and rendered probes (ADR 0018).
func is_world_loading() -> bool:
	return _frontend.is_loading()


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
	# The launch checks, Num Lock's start and the start's read of game.cfg (its
	# mpreset exit) ran in GameRuntimeRoot.begin_game, the process start's one home.
	# The subsystems' init turns Scroll Lock off (engine controls::kVkNumLock
	# carries the witness, D-NET-563).
	LocalPlayerPresenter.start_process_scroll_lock()
	_previous_auto_accept_quit = get_tree().auto_accept_quit
	get_tree().auto_accept_quit = false
	_quit_policy_installed = true
	_player_options.apply()
	if _world == null or _camera == null or _menu_shell == null:
		return
	_menu_shell.set_player_options(_player_options)
	# The join screen comes up over the menu the join started from, and its keys
	# are its own while it is open.
	_menu_shell.covered = _join_screen.is_open
	var debug_adapter := get_game_debug_adapter()
	add_child(debug_adapter)
	debug_adapter.start_runtime_endpoint()
	# F3 drives the SAME debug-control table MCP's game_debug does (ADR 0043
	# d12): the windows' control requests drain into it with the shell's
	# local authority.
	_dev_tools.set_debug_control_table(debug_adapter.get_debug_controls())
	# Outside a debug build the free camera moves only while the dev tools are open.
	_camera.dev_tools_open = _dev_tools.is_open
	_player_presenter = LocalPlayerPresenter.new()
	_player_presenter.name = "LocalPlayerPresenter"
	add_child(_player_presenter)
	# setup binds the presenter as the world's local view presenter (the
	# D-RORD-8 view leg + the fixed-tick weapon drain); the live binding table
	# is the shell's ControlsBindings model.
	_player_presenter.setup(_world, _camera, _camera, ControlsBindings.model())
	_keys = KeyboardPass.new(_player_presenter.get_key_events())
	# The keyboard pass runs at each update's input-frame head, inside the session's
	# update (engine inmatch::PlayerInputFrame::run_update_rows), so a frame that runs no update keeps
	# its keys (engine inmatch::TickInputSource carries the witness).
	_player_presenter.set_keyboard_pass(_run_keyboard_pass)
	# The menu-screen bit, read again at each update's input-frame head off the frontend's
	# screen stack: a screen one update's keyboard pass opens (the escape row's INGAME) gates
	# the frame's later updates (PlayerInputRouter::set_frontend).
	_player_presenter.set_frontend(_frontend)
	# The escape row's and the deploy picks' records run the shell's arms when the
	# update's drain reaches them, in key order with the rows' records
	# (PlayerInputRouter::set_record_leg).
	_player_presenter.set_record_leg(_run_record_leg)
	# The console's SAVEKEYS dumps the live binding table (GameWorld.set_controls_model).
	_world.set_controls_model(ControlsBindings.model())
	# The remote admin's GOTO MENUSTATE reads whether an in-game menu screen is up
	# (GameWorld.set_frontend).
	_world.set_frontend(_frontend)
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
	_armory_presenter.set_player_profiles(PlayerProfile.store())
	_armory_presenter.opened.connect(_screen_opened.bind(MenuDriver.MISSION_SCREEN_WEAPON))
	_armory_presenter.closed.connect(resume)
	# The vehicle screen over live play, like the armory (VEHICLE holds a screen).
	_vehicle_menu_presenter = VehicleMenuPresenter.new()
	_vehicle_menu_presenter.name = "VehicleMenuPresenter"
	add_child(_vehicle_menu_presenter)
	_vehicle_menu_presenter.setup(_world.world_view(), _hud if _hud != null else self)
	_vehicle_menu_presenter.opened.connect(
			_screen_opened.bind(MenuDriver.MISSION_SCREEN_VEHICLE))
	_vehicle_menu_presenter.closed.connect(_leave_screen.bind(MenuDriver.MISSION_SCREEN_VEHICLE))
	# The joiner's deploy-map screen (death.mnu DEATH; net-re 5.61) owns the cursor.
	_deploy_presenter = DeployScreenPresenter.install(self, _world.world_view(),
			_hud if _hud != null else self,
			_screen_opened.bind(MenuDriver.MISSION_SCREEN_DEATH),
			_leave_screen.bind(MenuDriver.MISSION_SCREEN_DEATH))
	# DEATH's exit confirmation YES runs input action 3, a menu screen's confirmation.
	_deploy_presenter.exit_confirmed.connect(_on_screen_exit_confirmed)
	# A DEATH screen's RESTART_LEVEL restarts the mission as the in-game RESTART does.
	_deploy_presenter.restart_requested.connect(_on_restart_requested)
	# A title whose DEATH screen picks the loadout fills and applies the armory's class pages.
	_deploy_presenter.set_armory(_armory_presenter)
	_world.join_deploy_pick_required.connect(_on_join_deploy_pick_required)
	# The process's cfg block every mission's session create reads, and the
	# create's mpreset exit (docs/net/novaworld-net-re.md D-NET-385).
	_world.set_game_config(GameCfgStore.block())
	_world.process_exit_requested.connect(exit_process)
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
			_hud if _hud != null else self,
			_screen_opened.bind(MenuDriver.MISSION_SCREEN_COMMAND_MAP),
			_leave_screen.bind(MenuDriver.MISSION_SCREEN_COMMAND_MAP))
	_command_map_presenter.set_hud_source(_hud_presenter.get_game_hud)
	_hud_presenter.command_map_requested.connect(_on_command_map_requested)
	_hud_presenter.quit_confirmed.connect(_on_quit_confirmed)
	_on_player_options_changed(_player_options.current())
	# The MP end-of-round flow (net-re 5.68; HUD_DrawOverlayPanels @0x5c0072): STAT owns the cursor.
	_end_round_presenter = EndRoundPresenter.install(self, _world.world_view(),
			_hud if _hud != null else self, _hud_presenter, _deploy_presenter,
			_armory_presenter, _screen_opened.bind(MenuDriver.MISSION_SCREEN_STAT),
			_leave_screen.bind(MenuDriver.MISSION_SCREEN_STAT))
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
	if not _enter_menu(BootRootMount.launch_directory()):
		_quit_failed_boot()
		return
	print_verbose("OpenNova: menu up from %s (%s)" % [
			BootRootMount.launch_directory(), LaunchFlags.product()])
	if _frontend.is_process_exit_pending():
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
		_frontend.set_launch_mission(sp_mission)
		_on_start_requested(sp_mission)
		return
	# Co-op LAN launches (`--lan-host` / `--lan-join`) ride the controller.
	# Mirrors `--mission` above; two instances on localhost = the co-op demo.
	if _net.launch_from_flags():
		return
	# The relaunch after a mod's install for a join (`--novaworld-join`).
	_net.maybe_open_novaworld_from_flags()


# Consume Esc before weapon.mnu's shell-wired CANCEL hotkey and FlyCamera can both
# observe it. The menu button has no authored ACTION, so its generic hotkey path
# reports unhandled even after emitting pressed; without this early claim the same
# Esc closes ARMORY and then immediately opens PAUSE.
func _input(event: InputEvent) -> void:
	# A useitem key's release clears the engine's armory press latch whatever
	# is up (the original's key-message hook; engine LocalPlayer::
	# release_use_item_key carries the witness), the event left for the other
	# handlers.
	if event is InputEventKey and not (event as InputEventKey).pressed and _world != null \
			and _is_use_item_key(ArmoryPresenter.bound_key_of(event as InputEventKey)):
		var release_sim: Simulation = _world.get_sim()
		if release_sim != null:
			# Windows posts a release with Alt held (without Ctrl, which AltGr
			# also carries), or Alt's or F10's own, as WM_SYSKEYUP, which the
			# hook ignores.
			var released := event as InputEventKey
			release_sim.release_use_item_key(
					(released.alt_pressed and not released.ctrl_pressed) \
					or released.keycode == KEY_ALT or released.keycode == KEY_F10)
	# Every key-down, a press or an OS repeat (echo), sets its key again after a
	# menu close's key-state reset (engine controls::KeyStateReset carries the
	# witness).
	if event is InputEventKey and (event as InputEventKey).pressed:
		ControlsBindings.model().key_down_event(ArmoryPresenter.bound_key_of(event as InputEventKey))
	# An open chat line owns the keyboard: every key (Esc included) waits in the key
	# queue for the input frame's keyboard pass, whose chat leg takes it, and nothing
	# else sees it; only the shell's own window/tools keys pass.
	if event is InputEventKey and _frontend.in_world() and _hud_presenter != null \
			and _hud_presenter.is_chat_capturing() and not _is_shell_key(event as InputEventKey) \
			and (not is_dev_tools_open() or _dev_tools.is_game_playing()):
		if (event as InputEventKey).pressed:
			_queue_key_event(event as InputEventKey)
		get_viewport().set_input_as_handled()
		return
	if event is InputEventKey:
		var tools_key := event as InputEventKey
		if tools_key.pressed and not tools_key.echo and tools_key.keycode == KEY_ESCAPE \
				and _dev_tools.handle_game_escape():
			get_viewport().set_input_as_handled()
			return
		# Every other key-down, an OS repeat included, joins the device's key queue in
		# every shell state; the input frame's keyboard pass drains it (KeyboardPass).
		# The tools window's interact mode keeps the keyboard.
		if tools_key.pressed and not _is_shell_key(tools_key) \
				and (not is_dev_tools_open() or _dev_tools.is_game_playing()):
			_queue_key_event(tools_key)
	if _frontend.get_screen() != MenuDriver.MISSION_SCREEN_WEAPON or not (event is InputEventKey):
		return
	var key := event as InputEventKey
	if key.pressed and not key.echo and key.keycode == KEY_ESCAPE:
		# Esc is the WEAPON screen's CANCEL, which does nothing while the press
		# latch is held (ArmoryPresenter.can_dismiss).
		if _armory_presenter == null or _armory_presenter.can_dismiss():
			resume()
		get_viewport().set_input_as_handled()


func _unhandled_key_input(event: InputEvent) -> void:
	var key := event as InputEventKey
	if key == null or not key.pressed:
		return
	# Esc, a repeat's included: the frontend's route (_on_escape).
	if key.keycode == KEY_ESCAPE:
		_on_escape()
	if key.echo:
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
	# The special-key chain and the binding scan run in the input frame's keyboard
	# pass over the key queue (_input feeds it; KeyboardPass carries the order).
	if key.keycode == PICK_KEY and key.shift_pressed and _world != null \
			and _world.is_loaded() and (is_gameplay_input_active() \
			or (is_dev_tools_open() and not _dev_tools.is_game_playing())):
		if _player_presenter != null:
			# The chord consumed the USE hold: no mount toggle on its release.
			_player_presenter.consume_use_hold()
		pick_at_crosshair()
		get_viewport().set_input_as_handled()
		return
	# Under a pass-through screen (death.mnu's DEATH, stat.mnu's STAT) the screen's
	# hotkey scan has seen the key-down already (its presenter's key leg runs
	# first), and the key waits in the key queue for the input frame the screen's
	# word lets run, the Escape key's included: nothing else takes it. The tools
	# window's interact mode keeps the keyboard.
	var screen := _frontend.get_screen()
	if (screen == MenuDriver.MISSION_SCREEN_DEATH or screen == MenuDriver.MISSION_SCREEN_STAT) \
			and not _tools_interacting():
		get_viewport().set_input_as_handled()


## The shell's own window and tools keys (F11 fullscreen, the tools toggle), which
## never reach the game's key queue.
func _is_shell_key(key: InputEventKey) -> bool:
	return WindowState.is_toggle_event(key) or key.keycode == DEV_TOOLS_KEY


## One key-down into the device's key queue: the player presenter's input router
## reads Ctrl and Shift from the key state (LocalPlayerPresenter.push_key_event).
func _queue_key_event(key: InputEventKey) -> void:
	if _player_presenter != null:
		_player_presenter.push_key_event(key)
	else:
		_keys.push(key)


## Whether the keys belong to the input frame's keyboard pass: over live play and
## under a pass-through screen (frontend::Frontend::keyboard_pass_owns_keys). The
## tools window's interact mode keeps the keyboard.
func _keyboard_pass_owns_keys() -> bool:
	return _frontend.keyboard_pass_owns_keys(_tools_interacting())


## The dev tools' interact mode keeps the input (the window open, the game not playing).
func _tools_interacting() -> bool:
	return is_dev_tools_open() and not _dev_tools.is_game_playing()


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
	return _frontend.in_world() and not _end_flow.is_round_ended() \
			and not _frontend.is_loading() and not _end_flow.has_screen() \
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


func hold_frame_stats(held: bool) -> void:
	_frame_stats_holds = maxi(_frame_stats_holds + (1 if held else -1), 0)


func is_frame_stats_held() -> bool:
	return _frame_stats_holds > 0


# The board's reader is elsewhere this frame: the open dev tools, or a probe's
# hold (hold_frame_stats). The slow-frame readout then leaves it undrained.
func _frame_stats_read_elsewhere() -> bool:
	return is_dev_tools_open() or is_frame_stats_held()


# Whether the world still times its first frames past a load (the slow-frame readout's post-load
# lines, an instrument).
func _post_load_frames_open() -> bool:
	return _world != null and _world.is_post_load_frames_open()


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
	return _frontend.gameplay_input_active(_end_flow.is_round_ended(), _tools_interacting())


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
		if e != null and e.kind == "high_score":
			# The win screen's new best into the mission table's row, which the menu's
			# next teardown saves to hiscore.txt (engine EpilogCine carries the witness).
			MissionCatalog.set_high_score(e.text, e.a)
		elif e != null and e.kind == "round_end":
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
	return _frontend.state_name()


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
	# The LAN browse speaks the mounted game's title.
	if _lan_session != null:
		_lan_session.resource_root = _root
	# The mounted expansion's text-override table (a loose expansion/<n>/<n>.bin
	# only, never the archived copy), installed as the table every string lookup
	# tries first and re-installed on each remount of this root, the menu's and the
	# join's expansion switches included (Strings.track_expansion_override).
	Strings.track_expansion_override(_root)
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
		push_warning(
			"MainGame: no menu found in resource dir (looked for %s)" % _menu_shell.main_menu_file
		)
	# The MainMenu's init over the mount (frontend::Frontend::enter_main_menu): game.cfg's
	# read (GameCfgStore.enter_main_menu; its mpreset ends the process with exit code 0),
	# the options, the profile after the menu's text tables, the first start's intro tail,
	# the menu's cookie jar, the menu shown.
	_frontend.enter_main_menu()
	return true


# The one-shot MenuShell wiring (every return to the menu re-enters _enter_menu):
# the five menu intents in this order, then the mp.mnu / player.mnu companions
# and the LAN browser they drive.
func _wire_shell() -> void:
	if _shell_wired:
		return
	_shell_wired = true
	_menu_shell.start_requested.connect(_on_start_requested)
	_menu_shell.exit_to_desktop_requested.connect(exit_to_desktop)
	_menu_shell.return_to_menu_requested.connect(_on_menu_return_to_menu)
	_menu_shell.restart_requested.connect(_on_restart_requested)
	_menu_shell.resume_requested.connect(resume)
	_menu_shell.novaworld_requested.connect(_net.open_novaworld_panel)
	_menu_shell.game_reloaded.connect(_on_game_reloaded)
	_menu_shell.ingame_controls_accepted.connect(_on_ingame_controls_accepted)
	_menu_shell.ingame_crosshair_reload_requested.connect(_on_ingame_crosshair_reload_requested)
	_menu_shell.screen_activated.connect(_on_menu_screen_activated)
	# Delegate mp.mnu and player.mnu to their respective companions.
	_mp_companion = MpMenuCompanion.new()
	_player_info_companion = PlayerInfoMenuCompanion.new()
	_lan_session = LanSession.new()
	_lan_session.name = "LanSession"
	_lan_session.resource_root = _root
	# The LAN search's socket binds on the block's client quad (engine
	# inmatch::transport_bind_ports, D-NET-958).
	_lan_session.game_config = GameCfgStore.block()
	# The LAN screen's browse stands for the process's one enumerator: a join's session reset
	# and the process exit drain its records (engine: ClientRuntime::drain_enumerator).
	_lan_session.set_process_enumerator(true)
	add_child(_lan_session)
	_mp_companion.set_lan_session(_lan_session)
	_menu_shell.add_companion(_mp_companion)
	_menu_shell.add_companion(_player_info_companion)
	_net.wire_menu_companions(_mp_companion)
	_player_info_companion.set_persisted_profile(_chosen_avatar)
	_player_info_companion.avatar_chosen.connect(_on_avatar_chosen)
	_player_info_companion.player_preview_drawn.connect(_on_player_preview_drawn)


# The Mods list switched the game: game.cfg is saved and read again over the switch,
# the menu made anew, the profile loaded again under the expansion it took (the
# expansion's weapon.sav), the one the menu shows and the next spawn uses
# (frontend::Frontend::reload_main_menu; refresh_local_profile_for_mount hands it to
# PLAYER_INFO).
func _on_game_reloaded() -> void:
	_frontend.reload_main_menu()


# The main menu's activation (the STARTUP screen) sets transport mode 0: the LAN
# screen's browse tears its own open socket down with its connect-log line, and its
# enumerator runs on over no socket (LanSession.tear_down_transport; engine
# inmatch::write_transport_socket_teardown_line carries the witness). The NovaWorld
# session's socket goes with its panel's close (NovaWorldClient.stop).
func _on_menu_screen_activated(screen_name: String) -> void:
	if screen_name.nocasecmp_to("STARTUP") == 0 and _lan_session != null:
		_lan_session.tear_down_transport()


# The in-game options Accept wrote the dialog into the current profile record:
# the record's binding table and ENABLE_JOYSTICK word onto the live bindings,
# the mouse look's live words into the running session, then the save, as the
# original's Accept does (engine menu::OptionsScreen::ApplyControls carries
# the witness).
func _on_ingame_controls_accepted() -> void:
	ControlsBindings.apply_profile(PlayerProfile.store())
	var sim: Simulation = _world.get_sim() if _world != null else null
	if sim != null:
		sim.apply_ingame_options(PlayerProfile.store())
	_frontend.save_options()


# The in-game options Accept reloads the crosshair art in alpha mode (engine
# menu::OptionsScreen::ReloadCrosshairAlpha carries the witness, D-HUD-48).
func _on_ingame_crosshair_reload_requested() -> void:
	if _hud_presenter != null:
		_hud_presenter.reload_crosshair_alpha()


# PLAYER_INFO's ACCEPT: the companion has saved the dialog into the current
# profile record in memory, as the original's dialog does (the next save point
# writes the files); the selected loadout continues through the existing
# spawn-kit seam.
func _on_avatar_chosen(profile: Dictionary) -> void:
	set_local_player_profile(profile)


# The PLAYER_INFO portrait's draw stores the time of day it lights by into the process's
# clock, never restored (engine inmatch::RemoteAdminConsoles::player_preview_drawn).
func _on_player_preview_drawn() -> void:
	if _world != null:
		_world.player_preview_drawn()


## Load the player profile (player.sav, and the mounted expansion's weapon.sav)
## again: at every main-menu entry (`force`), as the original's menu init loads
## it, and at a join's pre-dial switch to the host's expansion when the mounted
## root or its expansion changed, so the next spawn kit and the join's
## character vars come from the mounted expansion's profile. The menu's text
## tables must be registered first: a fresh record's macros are the menu table's.
func refresh_local_profile_for_mount(force := false) -> void:
	if _root == null:
		return
	var profile_root_key := "%s|%s|%s" % [String(_root.get_root_dir()),
			String(_root.get_expansion()).to_lower(), String(LaunchFlags.working_dir())]
	if force or profile_root_key != _profile_root_key:
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


# The held USE row's armory arm (engine controls::PlayerActions): the shared
# ArmoryPresenter opens weapon.mnu's WEAPON screen over LIVE play, the world
# ticking underneath (the frontend's WEAPON screen rides the presenter's opened signal). It
# opens what the engine named, unre-gated: the same frame's release-edge toggle
# may mount the player after the open, as retail's dispatch opens the screen
# before that edge runs. The ACCEPT apply and the class/current-loadout open
# protocol live on the host.
func _open_armory() -> bool:
	if _armory_presenter == null:
		return false
	return _armory_presenter.open()


# Whether a key event is the `useitem` row's (retail default Shift; catalog row
# 44, rebindable in Options -> Controls).
func _is_use_item_key(keycode: Key) -> bool:
	return ControlsBindings.model().godot_keys_for_token("useitem").has(int(keycode))


# --- Menu <-> world transitions ----------------------------------------------

func _on_start_requested(bms_name: String, campaign := -1, campaign_mission := 0) -> void:
	# Single-player: the loading screen is the sidecar image alone — no session
	# text [orig: the not-in-session path draws only the background @ 0x521ebe].
	_start_single_player(LoadingScreenInfo.for_mission(bms_name),
			_world.load_mission.bind(bms_name), campaign, campaign_mission)


# The single-player start (frontend::Frontend::start_single_player): the started mission's
# campaign words (the menu pick's, -1 for none: a launch that names a file, or a mission no
# campaign holds), game.cfg's save ahead of its attribute word, the profile's save, then the
# load.
func _start_single_player(load_info: LoadingScreenInfo, operation: Callable, campaign := -1,
		campaign_mission := 0) -> void:
	_pending_info = load_info
	_pending_loader = operation
	_frontend.start_single_player(campaign, campaign_mission)


## Boot an exact loose mission through the same loading
## presentation and GameWorld lifecycle as menu play.
func start_loose_mission(bms_name: String) -> void:
	_start_single_player(LoadingScreenInfo.for_mission(bms_name),
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
	_start_single_player(LoadingScreenInfo.for_mission(bms_name),
			_world.load_mission_data.bind(mission, bms_name))
	return OK


func return_to_menu() -> Error:
	if _frontend.is_loading():
		return ERR_BUSY
	if not _world.is_loaded():
		return ERR_UNAVAILABLE
	_on_return_to_menu()
	return OK


func _mission_start_gate() -> Error:
	if _root == null:
		return ERR_UNCONFIGURED
	if _frontend.is_loading() or not _world_load.can_start():
		return ERR_BUSY
	return ERR_ALREADY_IN_USE if _world.is_loaded() else OK


## Graceful runtime stop seam used by the shell and optional control service.
## The shutdown runs once. The music context's stop flushes its channels at once
## (engine audio::MusicStream::close), so the quit waits on no music drain. The main
## loop's exit saves game.cfg before the shutdown and the menu's teardown on the way
## out its cookie jar and the high scores (frontend::Frontend::request_quit).
func request_quit() -> void:
	if not _frontend.request_quit():
		return
	var load_operation := begin_runtime_shutdown()
	# The subsystems' shutdown, after the cursor's show: Scroll Lock off, Num Lock
	# as the start found it, Caps Lock off (engine controls::kVkNumLock carries the
	# witness, D-NET-563).
	LocalPlayerPresenter.exit_process_key_locks()
	_complete_runtime_shutdown(load_operation)


## The process exit on the cfg block's mpreset word: a read of game.cfg that
## sets it (a mount's; the start's is GameRuntimeRoot.begin_game's) or a session
## create that reads it
## (GameWorld.process_exit_requested), and on a mission load's warning box
## answered Cancel (the same signal, `code` -1). The process ends with `code` there:
## nothing is saved (neither game.cfg nor the profile), the NovaWorld session
## the shell holds sends its goodbye burst alone, and the runtime comes down
## (docs/net/novaworld-net-re.md D-NET-385). Out of the tree only the latch
## and the teardown run (the lifecycle tests' read seam).
func exit_process(code := 0) -> void:
	if not _frontend.begin_process_exit():
		return
	push_warning("MainGame: the process ends (exit code %d)" % code)
	var client: NovaWorldClient = null
	if _world != null:
		client = _world.release_novaworld_client() as NovaWorldClient
	if client != null:
		client.disconnect_at_exit()
		client.queue_free()
	if _net != null:
		_net.disconnect_at_exit()
	begin_runtime_shutdown()
	finish_runtime_shutdown()
	_restore_quit_policy()
	if process_exit_handler.is_valid():
		process_exit_handler.call(code)
	elif is_inside_tree():
		get_tree().quit(code)


## True once exit_process ran (ADR 0018 read seam for the lifecycle tests).
func is_process_exit_pending() -> bool:
	return _frontend.is_process_exit_pending()


# A mount that failed (at the boot or a return to the menu) ends with exit code
# 1 after the main loop's save of game.cfg, which every end of the scene loop
# runs (docs/gamecfg/game-cfg-re.md, the save points).
func _quit_failed_boot() -> void:
	push_warning("MainGame: the game-data mount failed; the process ends (exit code 1)")
	_frontend.fail_boot()
	get_tree().quit(1)


func _complete_runtime_shutdown(load_operation: WorldLoadOperation) -> void:
	if load_operation != null and not load_operation.is_settled():
		await load_operation.settled
	finish_runtime_shutdown()
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
	_net.join(target)


## The common mission-start seam (frontend::Frontend::start_mission): the start's
## save points and its routing are the frontend's; ShellPresentationSession owns its
## visibility transition while this shell owns the operation handoff. `restart` is
## the SP restart's start: no loading screen comes up (and so no closing splash). A
## single-player load is remembered as the restart's entry.
func start_world_load(load_info: LoadingScreenInfo, operation: Callable,
		restart := false) -> void:
	_pending_info = load_info
	_pending_loader = operation
	_frontend.start_mission(load_info != null and load_info.in_session, restart)


func _stop_lan_browse() -> void:
	if _lan_session != null:
		_lan_session.stop()


func _remember_restart_entry() -> void:
	_sp_restart_info = _pending_info
	_sp_restart_loader = _pending_loader


# The load's start over the start in hand (`join`: under the join screen, which follows
# the world); false when the coordinator's handoff could not start.
func _begin_load(restart: bool, join: bool) -> bool:
	if join:
		_join_screen.follow(_world)
	return _world_load.start(self, _root, _world, _pending_info, _pending_loader, restart) != null


## The next mission's spawn record: the PLAYER_INFO selection, with the current
## profile record's voice override pair, which the session start reads for each
## side's avatar byte (engine inmatch::join_character_profile).
func _staged_spawn_loadout() -> PlayerSpawnLoadout:
	var loadout := PlayerSpawnLoadout.from_profile(_chosen_avatar)
	for side in 2:
		loadout.set_side_voice(side, PlayerProfile.store().get_voice(side))
	return loadout


## A join's mission start: the join runs on the join screen (pre.mnu
## PRE_GAME_MENU) until the host starts the game, and the loading screen takes
## over only then (frontend::Frontend::start_join; engine: inmatch/pre_game_menu.h).
func start_join_load(load_info: LoadingScreenInfo, operation: Callable) -> void:
	_pending_info = load_info
	_pending_loader = operation
	_frontend.start_join()


## The process has one enumerator, so a join's enumerate-and-join drains the LAN
## screen's session records first, each writing its removed line (engine:
## net/npwire/lan_discovery.h drain_records, D-NET-956).
func drain_lan_records() -> void:
	if _lan_session != null:
		_lan_session.drain_records()


## The browse a join went on from (the enumerate-and-join's, the launch search's) hands its
## records to the process's one enumerator, the LAN screen's browse, where the join's session
## reset drains them (engine: net/npwire/lan_discovery.h adopt_records, D-NET-956).
func adopt_lan_records(from: LanSession) -> void:
	if _lan_session != null and from != null:
		_lan_session.adopt_records(from)


## A join's enumerate-and-join: the join screen shows retail's PRE_ENUMSESSION
## line with ABORT_CANCEL while the browse runs (no time limit); Cancel runs
## `abort`, which leaves to the menu (engine: inmatch/pre_game_menu.h, D-NET-703).
func begin_join_enumerate(abort: Callable) -> void:
	if not _join_screen.open(_root, self):
		return
	_join_enumerate_abort = abort
	_join_screen.show_progress(JoinScreenStatus.stage_text(JoinScreenStatus.STAGE_ENUMERATING,
			Strings.get_table(Strings.TABLE_GAMEERR)))


## The enumerate ended (a session answered, or the join was abandoned): its
## join screen closes; a join that goes on opens it again.
func end_join_enumerate() -> void:
	if not _join_enumerate_abort.is_valid():
		return
	_join_enumerate_abort = Callable()
	_join_screen.close()


## A join the join machine refuses before it dials: the join screen parks on `text`
## with Cancel only, which runs `abort` (its error arm, then its state 9). False when
## the screen cannot open.
func show_join_refusal(text: String, abort: Callable) -> bool:
	if not _join_screen.open(_root, self):
		return false
	_join_enumerate_abort = abort
	_join_screen.show_failure(text)
	return true


## The join machine's choice (its state 8): a discovered session whose flag word
## offers one opens pre.mnu's choice panels on the join screen; ACCEPT walks them
## and its commit hands the three controls to `committed`; BACK or Cancel runs
## `abort`, which leaves to the menu. False when the screen cannot show a choice.
## (engine: inmatch/pre_game_menu.h pre_game_entry_panel / pre_game_accept)
func begin_join_choice(server_flags: int, committed: Callable, abort: Callable) -> bool:
	if not _join_screen.open(_root, self):
		return false
	if not _join_screen.show_choice(server_flags):
		_join_screen.close()
		return false
	_join_enumerate_abort = abort
	_join_choice_committed = committed
	return true


# The choice's commit: the screen closes and the join goes on with the choice.
func _on_join_choice_committed(game_password: String, spectate: bool,
		team_password: String) -> void:
	var committed := _join_choice_committed
	_join_choice_committed = Callable()
	_join_enumerate_abort = Callable()
	_join_screen.close()
	if committed.is_valid():
		committed.call(game_password, spectate, team_password)


## The join screen is up (the probe and test read).
func is_join_screen_open() -> bool:
	return _join_screen.is_open()


func get_join_screen() -> PreGameMenuPresenter:
	return _join_screen


# The host started the game: the join screen hands over to the loading screen.
func _on_join_game_starting() -> void:
	_join_screen.close()
	_frontend.join_game_starting()
	_world_load.show_deferred_screen()


# Cancel on the join screen: an abandoned join leaves the way a cancelled load
# does; a failure already shown leaves to the menu it came from.
func _on_join_screen_cancelled() -> void:
	if _join_enumerate_abort.is_valid():
		var abort := _join_enumerate_abort
		_join_enumerate_abort = Callable()
		_join_choice_committed = Callable()
		_join_screen.close()
		abort.call()
		return
	_join_screen.close()
	if _frontend.is_loading() and _world != null and _world.cancel_join_preload():
		return
	_teardown_world_to_menu()


# The load's presentation (the frontend's begin_world_load leg): the next spawn's kit,
# the chat presets, the menu left (it keeps the screen it was started from, which the
# return after the mission shows again, D-MNU-28) and the loading presentation.
func _begin_world_load() -> void:
	_world.set_local_player_spawn_loadout(_staged_spawn_loadout())
	if _hud_presenter != null:
		_hud_presenter.set_chat_presets(PlayerProfile.store().get_macros())
	_menu_shell.leave_menu_mode()
	_shell_presentation.begin_world_load(
			_menu_shell, _world, _hud, _on_world_loaded, _on_world_load_failed)
	_refresh_dev_tools_game_state()



func _on_world_loaded() -> void:
	# The GAME music context is the world's to open at mission start (GameWorld
	# emits music_context_opened into MusicService.open_game_context, so every
	# live mission entry path gets the same music). The reveal follows at the tail
	# of the mission start (frontend::Frontend::mission_loaded carries the witness):
	# for SP/host at true load completion; for a joiner this is only wire-header world
	# construction completion, and the hidden runtime pumps under the loading
	# presentation until the separate authoritative edge.
	# A fresh mission gets a fresh pick set (stale handles never cross
	# sessions); the pick session curates the shell-owned list from here on.
	_pick_session.begin_world(_world)
	_on_dev_tools_open_changed(is_dev_tools_open())
	var sim := _world.get_sim()
	_player_options.apply(sim)
	# Every mission start re-seeds the live HUD declutter level from the
	# persisted config value (retail's session-settings apply), so a death
	# screen's forced blank HUD ends with the mission it happened in. The same
	# start's aspect mode reaches the HUD's sights and scope geometry, which
	# draw at it until the next mission start (PlayerOptions.apply).
	if _hud_presenter != null:
		_hud_presenter.reapply_persisted_hud_detail()
		_hud_presenter.set_aspect_mode(_player_options.current().aspect_mode)
	# The F3 engine-fact windows read and mutate through this Simulation from
	# here until unload (ADR 0042 d6): DevTools holds it in C++ (the stats-board
	# pattern) and does the record push / request drain with no GDScript relay.
	_dev_tools.set_simulation(sim)
	# Every playing process's mission start stages the armory's class pages from the
	# profile and the spawn buffer (ArmoryPresenter.on_mission_started).
	if _armory_presenter != null:
		_armory_presenter.on_mission_started()
	# Every mission start, a restart's included, runs the whole input reset; a
	# joiner's runs later, after its game-start wait (the frame loop's
	# Simulation.take_input_reset; engine controls::KeyStateReset carries the
	# witness), and a joiner not yet admitted waits for its admission.
	var started := _frontend.mission_loaded(sim != null and bool(sim.is_joiner()),
			sim != null and bool(sim.is_joined_in_match()))
	if started == GameFrontend.STARTED_AWAIT_ADMISSION:
		return
	if started == GameFrontend.STARTED_RESET_AND_REVEAL:
		_reset_key_state()
	# The SP start-mission splash holds the reveal until its dismissal edge;
	# the gate + device legs live on the coordinator
	# [orig: Game_ShowStartMissionSplash @ 0x520820 precedes the release @ 0x525d45].
	if _world_load.maybe_begin_start_mission_splash(_world.get_mission_audio()):
		if not _world_load.splash_dismissed.is_connected(_on_start_mission_splash_dismissed):
			_world_load.splash_dismissed.connect(_on_start_mission_splash_dismissed)
		return
	_finish_world_load_presentation()


## The SP start-mission splash's exit test takes the key that ended it off the key
## queue (none when a mouse button ended it): the queue was reset as the splash
## began (the mission start's input reset just ahead of it), so that key is the
## first one queued (engine controls::KeyEventQueue::reset carries the witness).
func _on_start_mission_splash_dismissed() -> void:
	_keys.take_key()
	_finish_world_load_presentation()


func _finish_world_load_presentation() -> void:
	if _frontend.finish_load():
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


# The frame loop's death.mnu DEATH open, every frame and every role (the
# listen host's own client included): the engine's triggers (the host-driven
# deploy-map overlay, or the in-session local death), its once-per-arming
# open latch with its result-blind stamp, and its clear when the triggers
# fall all live on the engine side (inmatch::death_menu_triggered,
# ClientState::take_death_menu_open; hud-re D-HUD-19); this leg is the device
# call. A screen up (or a load) stands in for retail's no-active-menu gate, so
# the latch is never burned under another screen and the open retries on the
# next clear frame. Closing is the presenter's own affair. NOT yet modeled (hud-re
# D-HUD-19 residuals): the spawn-success suppression gate. The latch is the
# scene frame's, so a frame the status page stands in for neither opens the
# screen nor clears the latch (GameWorld's scene gate; the latch's witness
# lives on ClientState.death_menu_open_latch).
func _maybe_open_death_menu() -> void:
	if _frontend.in_menu() or _world == null:
		return
	# The latch's open and close legs sit in the scene frame's world block: a frame the
	# status page stands in for, or one that skips its world (a cine, the command map over
	# the menu), takes neither (engine inmatch::FrameOutcome::world_skipped).
	if not _world.get_last_frame_world_drawn():
		return
	var sim: Simulation = _world.get_sim()
	if sim == null:
		return
	var opening := bool(sim.take_death_menu_open(not _frontend.in_world() or _frontend.is_loading()))
	# The clear's close leg, a title's own call ahead of the screens' close.
	if bool(sim.take_death_menu_close()):
		_deploy_presenter.on_death_menu_close()
	if not opening:
		return
	_deploy_presenter.open()


## An established session ended without the player asking (the host's punt, the
## connection reap), or the mission stored its exit: the frontend's route over the
## reason the session stored (frontend::Frontend::route_exit carries the witness),
## each route's device work here. A joiner's session dies with the host's exit and
## lands here too; whichever notification fires first tears down, the other sees the
## menu (the route's None).
func _on_session_lost(reason: String) -> void:
	var sim: Simulation = _world.get_sim() if _world != null else null
	var route := _frontend.route_exit(sim != null,
			sim.get_mission_exit_reason() if sim != null else 0,
			sim != null and sim.is_mp_session(), sim != null and bool(sim.is_joiner()),
			sim != null and not String(sim.get_mission_exit_map()).is_empty())
	var exit_reason := _frontend.get_exit_reason()
	match route:
		GameFrontend.EXIT_NONE:
			return
		GameFrontend.EXIT_RESTART_ROUND:
			_restart_mission()
		GameFrontend.EXIT_MISSION_BOX:
			_start_mission_box_mission(sim.get_mission_exit_map())
		GameFrontend.EXIT_LEAVE_TO_MENU:
			_teardown_world_to_menu(exit_reason)
		GameFrontend.EXIT_JOINER_RELOAD:
			# The session block is the kept connection's last S2C 0x60 until the
			# reload's own transfer replaces it.
			if _world.begin_joiner_reload():
				_reload_in_session(sim.get_join_server_info(), _world.reload_joiner)
			else:
				_abort_to_menu("session ended", reason, exit_reason)
		GameFrontend.EXIT_MAP_CHANGE:
			# The next rotation entry loads inside the session, every joiner kept; at
			# the rotation's end the session ends (its close sends the joiners the
			# STOP goodbye) and the shell leaves for the menu.
			var next_map: String = _world.begin_map_change()
			if next_map.is_empty():
				_teardown_world_to_menu(exit_reason)
			else:
				_reload_in_session(_world.map_change_loading_info(
						sim.get_host_session_config()), _world.load_next_mission.bind(next_map))
		_:
			_abort_to_menu("session ended", reason, exit_reason)


## The world comes down around the session the world kept, and the next mission
## loads into it with no menu between.
func _reload_in_session(load_info: LoadingScreenInfo, loader: Callable) -> void:
	_teardown_world(false)
	start_world_load(load_info, loader)


## A load-step failure or abort (frontend::Frontend::load_failed carries the
## witness): a join that fails before the game start stays on the join screen
## with the connection's reason and only Cancel (inmatch/pre_game_menu.h); every
## other failure returns to the menu (docs/interface/loading-screen-re.md, the
## load-flow case matrix).
func _on_world_load_failed(reason: String) -> void:
	if _frontend.load_failed(_join_screen.is_open()):
		push_warning("MainGame: join failed: %s" % reason)
		var error: ConnectionError = _world.get_last_connection_error() if _world != null else null
		var text := reason
		if error != null and error.is_set():
			text = error.reason_text(Strings.get_table(Strings.TABLE_GAMEERR))
		_net.note_join_failure(error)
		_join_screen.show_failure(text)
		return
	# The launch's own mission (`--mission`, the editor's Play mission): said on a line of
	# its own, which Play reads back by its marker.
	var launch := _frontend.take_launch_mission()
	if not launch.is_empty():
		push_warning("MainGame: %s%s %s"
				% [ResourceRoot.launch_mission_failed_marker(), launch, reason])
	_abort_to_menu("mission load failed", reason)


# THE abort-to-menu leg. Every caller names the stage it aborted from; the presentation
# is one teardown because retail's is one too (every reason lands on the same nav push),
# and the exit reason picks the post-mission route.
func _abort_to_menu(stage: String, reason: String,
		exit_reason := GameWorld.MISSION_EXIT_QUIT) -> void:
	push_warning("MainGame: %s: %s" % [stage, reason])
	_teardown_world_to_menu(exit_reason)


## Esc outside the keyboard pass: the join screen's Cancel, a join load's wait (the
## game-start wait aborts the join; the final wait leaves for the game loop, aborting
## nothing, GameWorld.cancel_join_admission), the tools window's interact mode over live
## play (the HUD's escape chain closes one open HUD window first, else the in-game
## menu opens), and the in-mission screens (frontend::Frontend::escape carries the
## witness). The cold-spawn hold after the S2C 0x0F takes none: retail's game loop
## already runs there (D-NET-1030).
func _on_escape() -> void:
	match _frontend.escape(_join_screen.is_open(), _end_flow.is_round_ended(),
			_tools_interacting()):
		GameFrontend.ESCAPE_CANCEL_JOIN_SCREEN:
			_join_screen.cancel()
		GameFrontend.ESCAPE_CANCEL_LOAD:
			if _world != null and not _world.cancel_join_preload():
				_world.cancel_join_admission()
		GameFrontend.ESCAPE_ACTION:
			if not _run_escape_action():
				_pause()
		GameFrontend.ESCAPE_RESUME:
			resume()
		GameFrontend.ESCAPE_CLOSE_SCREEN:
			_close_screen_presenter()


# The Escape key's action (the escape row's case 18): the input reset first (its
# Player_ResetLocalInput, ahead of its spawn gate; engine
# inmatch::Role::reset_local_input, D-NET-560), whose Scroll Lock turn-off goes out
# at the next frame's head (D-NET-563); then the HUD's escape chain, which closes
# one open HUD window (true) or runs its tail (false: the caller opens the in-game
# menu only over live play, as the case's menu-screen gate does).
func _run_escape_action() -> bool:
	if _world != null and _world.get_sim() != null:
		if _world.get_sim().escape_input_reset() and _player_presenter != null:
			_player_presenter.owe_scroll_lock_turn_off()
	return _hud_presenter != null and _hud_presenter.handle_escape()


## The menu's pass-through word into the frontend's screen stack, as the screen up
## holds it: death.mnu's DEATH (its show raises the word, the exit confirmation lowers
## it and the confirmation's NO raises it again) and stat.mnu's STAT (raised for the
## whole screen); engine menu::ScreenStack reads it.
func _sync_screen_word() -> void:
	var screen := _frontend.get_screen()
	_frontend.set_pass_through(screen == MenuDriver.MISSION_SCREEN_STAT \
			or (screen == MenuDriver.MISSION_SCREEN_DEATH and _deploy_presenter != null \
			and _deploy_presenter.is_input_pass_through()))


# The chat line's keyboard capture for this frame's samplers (the witness rides
# ControlsModel.set_keyboard_captured).
func _sync_keyboard_capture() -> void:
	ControlsBindings.model().set_keyboard_captured(_frontend.keyboard_captured(
			_hud_presenter != null and _hud_presenter.is_chat_capturing(), _tools_interacting()))


# --- The keyboard pass's legs (KeyboardPass.drain) -----------------------------

## The input frame's keyboard pass, which the player input router runs at the head
## of each update's input frame (under no menu screen): over live play and under a
## pass-through screen whose word holds, it drains the key queue, so a key its
## special-key chain takes never reaches the rows, and its legs (the round-over
## keys, the escape row's in-game menu, a deploy pick) land in that update; a frame
## that runs no update keeps its keys (KeyboardPass and engine
## inmatch::TickInputSource carry the witness).
func _run_keyboard_pass() -> void:
	if _frontend.input_frame_drains_keys(_tools_interacting()):
		_keys.drain(self)


## An open chat line takes the queue's key (the keyboard pass's capture branch);
## a key that closes the line hands the keyboard back to the frame's samplers at
## once, as the capture word does.
func ring_chat_key(key: InputEventKey) -> bool:
	if _hud_presenter == null or not _hud_presenter.is_chat_capturing():
		return false
	_hud_presenter.handle_chat_key(key)
	_sync_keyboard_capture()
	return true


## The special-key chain over the queue's key (GameHudPresenter.handle_special_key;
## engine inmatch::special_key_chain carries the chain): the shell's half of its
## outcome. The round-over RESTART out of a session re-runs the splash (over a
## custom loading background), and its restart, like ESC's quit, reaches the shell
## as the session's mission exit (_on_session_lost); an input event 12 the chain
## queued (a deploy key's pick, the quit dialog's or the round-over RESTART's) waits
## in key order with the rows' events (LocalPlayerPresenter.queue_pick_event). The
## deploy keys' test is the engine's (dead, or the deploy overlay up), not the deploy
## screen's: under stat.mnu's STAT a dead player's deploy key picks as under DEATH.
func ring_special_key(key: InputEventKey) -> bool:
	if _hud_presenter == null:
		return false
	# The chain runs over the session the shell's world view names, the one the deploy
	# pick's send (the record leg's DeployScreenPresenter.send_pick) serves.
	var outcome := _hud_presenter.handle_special_key(key,
			_deploy_presenter.view_sim() if _deploy_presenter != null else null)
	if outcome == null:
		return false
	if outcome.is_restart_splash():
		_begin_restart_splash()
	if outcome.get_pick() >= 0:
		_queue_pick(outcome.get_pick())
	return outcome.is_taken()


## The binding scan over a key the chain left: the dispatch code of the row it
## fires (engine BindingSet::event_row, through ControlsModel.event_code).
func ring_binding_code(scan_code: int) -> int:
	return ControlsBindings.model().event_code(scan_code)


## Any row the scan fired, the escape row's included: its event, with the fire's
## sign (Shift on a fallback-pass fire) and the event's key, waits for the player
## input router, which dispatches it once (PlayerInputRouter::queue_row_event). A
## key the scripted device replayed for its hold fires its row through the hold
## instead, the escape row's excepted (it has no hold arm).
func ring_row_event(key: InputEventKey, code: int, scan_code: int) -> void:
	if _player_presenter == null or (code != KeyboardPass.ESCAPE_CODE \
			and ControlsModel.is_scripted_key_event(key)):
		return
	_player_presenter.queue_row_event(code, ControlsBindings.model().event_signed(scan_code),
			scan_code)


## The keyboard pass's release pass, ahead of its first dequeue: a latched key that
## reads up queues its release row's event (PlayerInputRouter::run_key_release_pass).
func ring_release_pass() -> void:
	if _player_presenter != null:
		_player_presenter.run_key_release_pass()


## The special-key chain's pick (input event 12), queued in key order with the
## rows' events for the update's drain.
func _queue_pick(param: int) -> void:
	if _player_presenter != null:
		_player_presenter.queue_pick_event(param)


## A record of the update's drain whose arm is the shell's, reached in key order
## (PlayerInputRouter::set_record_leg): the escape row's (case 18), the Escape
## action: over live play, with no HUD window to close, the in-game menu opens;
## under a pass-through screen never (a menu screen is open), nor once the round
## is over; the cheat row 117's weapon-screen toggle (case 40); or input event 12,
## the pick (the deploy keys', the RESTART keys'), with or without the
## deploy screen open. A console line's record reaches these arms as a key's does.
## A case that reset the input (its Input_ResetAllState: the role's memset of the
## event queue ends the drain there) owes the devices' reset, which runs at once.
func _run_record_leg(code: int, param: int) -> void:
	if code == KeyboardPass.ESCAPE_CODE:
		if not _run_escape_action() and is_gameplay_input_active():
			_pause()
	elif code == KeyboardPass.SCREEN_SHOT_CODE:
		_take_screen_shot()
	elif code == WEAPON_SCREEN_CODE:
		_toggle_weapon_screen()
	elif _deploy_presenter != null:
		_deploy_presenter.send_pick(param)
	var sim: Simulation = _world.get_sim() if _world != null else null
	if sim != null and bool(sim.take_input_reset()):
		_reset_key_state()


## The weapon-screen toggle (case 40, the cheat row 117's), past the engine's gates (out of a
## session, a local player): an open menu screen closes, else weapon.mnu's WEAPON opens; no
## armory zone gates it (frontend::Frontend::weapon_screen_toggle and engine
## inmatch::Role::dispatch_record carry the witness).
func _toggle_weapon_screen() -> void:
	if _frontend.weapon_screen_toggle() == GameFrontend.USE_CLOSE_SCREEN:
		_close_screen_presenter()
	else:
		_open_armory()


## The screen up closes through its presenter (the USE row's arms and the weapon-screen
## toggle under death.mnu's DEATH or stat.mnu's STAT, Esc under CMAP and VEHICLE), each
## close handing play back with its input reset (_leave_screen).
func _close_screen_presenter() -> void:
	match _frontend.get_screen():
		MenuDriver.MISSION_SCREEN_DEATH:
			_deploy_presenter.close()
		MenuDriver.MISSION_SCREEN_STAT:
			_end_round_presenter.close()
		MenuDriver.MISSION_SCREEN_COMMAND_MAP:
			_command_map_presenter.close()
		MenuDriver.MISSION_SCREEN_VEHICLE:
			_vehicle_menu_presenter.close()


## The Screen Shot action's device leg (case 2): the frame as it last drew, written as the
## first free SS%0.5d.jpg in the working directory, or under /sstga the first free SS%0.5d.tga
## through the engine's run-length TGA writer (LaunchFlags.screen_shot_path and
## screen_shot_tga_file; engine gameprofile/player_files.h screenshot_file_name and
## formats/tga/tga.h tga_write_screenshot carry the witness). Nothing is written once every
## index is taken, or when the device holds no frame.
func _take_screen_shot() -> void:
	var path := LaunchFlags.screen_shot_path()
	if path.is_empty():
		return
	var image := get_viewport().get_texture().get_image()
	if image == null or image.is_empty():
		return
	if not LaunchFlags.screen_shot_tga():
		image.save_jpg(path)
		return
	image.convert(Image.FORMAT_RGBA8)
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file != null:
		file.store_buffer(LaunchFlags.screen_shot_tga_file(
				image.get_data(), image.get_width(), image.get_height()))


## The quit dialog's yes runs input action 3 inline: the Exit Mission tone, then the
## exit (a joiner's CIDEMIS goodbye leaves at once, so no later C2S follows the
## click), and the shell returns to the menu (the host's session goes with it)
## (engine inmatch/remote_admin.h input_action_exit carries the witness).
func _on_quit_confirmed() -> void:
	_play_exit_tone()
	if _world != null and _world.get_sim() != null:
		_world.get_sim().input_action_exit()
	_on_return_to_menu.call_deferred()


## DEATH's exit confirmation YES, a menu screen's: input action 3 inline after the screen's
## close, whose input reset drops what the action's disconnect queued, then the return to the
## menu (engine inmatch/remote_admin.h ExitSite carries the witness).
func _on_screen_exit_confirmed() -> void:
	_play_exit_tone()
	if _world != null and _world.get_sim() != null:
		_world.get_sim().input_action_exit(true)
	_on_return_to_menu.call_deferred()


## Input action 3's Exit Mission tone, which the action plays ahead of its
## disconnect on every path that runs it, the quit dialog's yes and the INGAME
## CONFIRM_YES alike (engine inmatch/remote_admin.h input_action_exit carries the
## witness).
func _play_exit_tone() -> void:
	var audio: MissionAudio = _world.get_mission_audio() if _world != null else null
	if audio != null:
		audio.ui_soundset(GameHudPresenter.QUIT_CONFIRM_SOUNDSET)


func _on_command_map_requested() -> void:
	if _frontend.in_world() and _command_map_presenter != null:
		_command_map_presenter.open()


## A screen's presenter put it up (frontend::Frontend::open_screen; another screen up
## refuses it), its pass-through word read at once for the frame's later updates.
func _screen_opened(screen: int) -> void:
	_frontend.open_screen(screen)
	_sync_screen_word()


## A closing screen hands play back (frontend::Frontend::close_screen): death.mnu's DEATH
## and stat.mnu's STAT pass input through while their word holds, so their close's input
## frame takes the menu cursor's offset from the re-centre point; DEATH's exit
## confirmation holds the word down (engine world::pass_through_close_axis carries the
## witness).
func _leave_screen(screen: int) -> void:
	_sync_screen_word()
	var closed := _frontend.close_screen(screen)
	if closed == GameFrontend.CLOSE_NONE:
		return
	if closed == GameFrontend.CLOSE_PASS_THROUGH and _player_presenter != null:
		_player_presenter.close_pass_through_screen()
	_reset_screen_close_input()


## Every menu screen's close runs the whole input reset: the queued events
## (a console line's action, an echo's binding arm) and the deferred dispatches
## are dropped, and the devices' reset follows at the next frame's head
## (Simulation.take_input_reset); with no sim the devices reset at once
## (engine inmatch/session.h Role::reset_all_input_state carries the witness,
## UI_CloseMenuScreen @0x54E5E0 tail-calling Input_ResetAllState @0x54E63F).
func _reset_screen_close_input() -> void:
	# The reset's key-queue leg (Input_ResetKeyQueue) runs with the close: the keys
	# waiting under the screen never reach the world (KeyboardPass).
	_keys.clear()
	var sim: Simulation = _world.get_sim() if _world != null else null
	if sim != null:
		sim.reset_all_input_state()
	else:
		_reset_key_state()


# Every menu screen's close resets the input state: a key held through it
# reads up until its next key-down and dispatches no release, and a deferred
# dispatch is dropped (engine controls::KeyStateReset and KeyPressLatch carry the
# witness); the key queue goes with it (Input_ResetAllState's Input_ResetKeyQueue;
# the input router resets it).
func _reset_key_state() -> void:
	if _player_presenter != null:
		_player_presenter.reset_input_state()
	else:
		_keys.clear()
		ControlsBindings.model().reset_key_state()


func _pause() -> void:
	if not _frontend.pause():
		return
	# The engine session pauses with the shell; net roles refuse natively.
	_world.set_shell_paused(true)
	if _hud_presenter != null:
		_hud_presenter.set_menu_pause(true)
	_menu_shell.open_ingame_menu()  # game.mnu overlay over the kept-loaded world
	_menu_shell.show_menu()


## INGAME's or WEAPON's close back to play (frontend::Frontend::resume); every resume
## leg lands here, so the session cannot stay stuck Paused.
func resume() -> void:
	if not _frontend.resume():
		return
	if _armory_presenter != null and _armory_presenter.is_open():
		_armory_presenter.close()  # its closed edge finds the screen gone: no re-entry
	_menu_shell.hide_menu()
	_world.set_shell_paused(false)
	_reset_screen_close_input()
	if _hud_presenter != null:
		_hud_presenter.set_menu_pause(false)


func _on_return_to_menu() -> void:
	if _frontend.is_loading():
		return
	_teardown_world_to_menu()


## The menu shell's CONFIRM_YES. The INGAME screen's (the in-game menu over live
## play) runs input action 3 inline as a menu screen's confirmation, so a joiner's
## goodbye leaves at the click; then the shell returns to the menu (engine
## inmatch/remote_admin.h input_action_exit and ExitSite carry the witness).
func _on_menu_return_to_menu() -> void:
	if _frontend.get_screen() == MenuDriver.MISSION_SCREEN_INGAME and not _frontend.is_loading() \
			and _world != null and _world.get_sim() != null:
		_play_exit_tone()
		_world.get_sim().input_action_exit(true)
	_on_return_to_menu()


## The MCP screen verbs behind game_control (the in-world screens get eyeballed
## over the runtime MCP without hand-play). Both keep the key paths' state
## gates; "open" is not a toggle, so an already-open screen reports OK and the
## resume verb hands play back. Another screen up refuses the open, as the
## frontend's screen stack does.
func mcp_open_ingame_menu() -> Error:
	if _world == null or not _world.is_loaded() or _frontend.is_loading() \
			or _end_flow.is_round_ended():
		return ERR_UNAVAILABLE
	if _frontend.get_screen() == MenuDriver.MISSION_SCREEN_INGAME:
		return OK
	if not _frontend.in_world():
		return ERR_UNAVAILABLE
	_pause()
	return OK


func mcp_open_armory() -> Error:
	if _world == null or not _world.is_loaded() or _frontend.is_loading() \
			or _end_flow.is_round_ended() or _armory_presenter == null:
		return ERR_UNAVAILABLE
	if _frontend.get_screen() == MenuDriver.MISSION_SCREEN_WEAPON:
		return OK
	if not _frontend.in_world():
		return ERR_UNAVAILABLE
	# The armory key's leg minus the arm's gate: a type-6 volume or the pre-round
	# is the useitem key's gameplay rule (Simulation.local_player_use_item_arm)
	# [orig: Input_HandleActionBinding_0 Flags & 0x400000 @0x4e0aa1], not a
	# screen precondition — open() is the presenter's staged direct-open seam.
	return OK if _armory_presenter.open() else ERR_UNAVAILABLE


# One idempotent rollback for a normal return and every load failure. Runtime
# presenters keep references to the old world/root, so their teardown order is part
# of the shell boundary rather than a menu-specific detail. The exit reason picks the
# post-mission route (GameWorld.post_mission_route): a NovaWorld session the route keeps
# leaves the world before it unloads and re-enters the NovaWorld menu; an error route
# shows its text first. The route reads the exit the session's teardown left, as the
# Post Menu router reads it after the mission's teardown pump: the profile step, then the
# session's close (GameWorld.close_session_for_route, whose own-client teardown can remap
# a listen host's quit through the process latch: D-NET-823), then the route.
func _teardown_world_to_menu(exit_reason := GameWorld.MISSION_EXIT_QUIT) -> void:
	_frontend.enter_post_menu()
	_record_round_end()
	var route: PostMissionRoute = PostMissionRoute.new()
	if _world != null:
		route = _world.post_mission_route(_world.close_session_for_route(exit_reason))
	if route.exits_game:
		# The "quit" row's exit (reason 2): the mission comes down (its session's reset sends
		# the record as it stands), and the game ends as the main loop's does, game.cfg saved
		# (engine inmatch/mission_exit.h PostMissionRoute::exits_game carries the witness).
		push_warning("MainGame: exit reason 2 (the session reset) ends the process")
		_teardown_world(false, false)
		request_quit()
		return
	var novaworld_client: NovaWorldClient = null
	if route.keep_session:
		novaworld_client = _world.release_novaworld_client() as NovaWorldClient
	_teardown_world(false, false)
	# The return takes back the expansion a join's switch left, ahead of the menu's
	# entry, whose game.cfg read and profile load then run over it (D-NET-622; engine
	# SessionDrive::restore_kept_expansion carries the witness).
	if _root != null:
		_world.restore_join_expansion(_root)
	if _root != null and _enter_menu(_root.get_root_dir()):
		if _frontend.is_process_exit_pending():
			# The menu entry's read ended the process: the session this exit
			# kept goes as the exit's teardown takes it, its goodbye burst
			# alone, and no NovaWorld menu follows.
			if novaworld_client != null:
				novaworld_client.disconnect_at_exit()
				novaworld_client.free()
			return
		_net.return_from_mission(novaworld_client, route.error, post_mission_error_text(route))
		return
	if novaworld_client != null:
		novaworld_client.stop()
		novaworld_client.free()
	push_warning("OpenNova: the game-data directory is no longer mountable")
	_quit_failed_boot()


# The world and every presenter over it come down: the menu teardown's head, and
# the SP restart's (`restart` keeps the HUD tip's once-counters, as the
# restart's start does). The menu teardown runs the profile step itself, ahead
# of its session's close (`record_round_end` false).
func _teardown_world(restart: bool, record_round_end := true) -> void:
	if record_round_end:
		_record_round_end()
	_world_load.dismiss()
	_join_screen.close()
	finish_hud_hidden_capture()
	_end_flow.reset()
	_teardown_presenters()
	_frontend.teardown_mission()
	# The F3 windows' Simulation dies with the runtime unload frees.
	_dev_tools.set_simulation(null)
	_world.unload()
	if _player_presenter != null:
		_player_presenter.setup(_world, _camera, _camera, ControlsBindings.model())
	if _hud_presenter != null:
		_hud_presenter.teardown(restart)


# The in-world presenters come down with their world, at a mission teardown and at
# the runtime's shutdown alike (the HUD presenter's teardown is each caller's own).
func _teardown_presenters() -> void:
	if _player_presenter != null:
		_player_presenter.teardown()
	if _armory_presenter != null:
		_armory_presenter.teardown()  # the built menu holds the OLD world's resource root
	if _vehicle_menu_presenter != null:
		_vehicle_menu_presenter.teardown()  # vehicle.mnu's frame, the same root
	if _deploy_presenter != null:
		_deploy_presenter.teardown()  # same stale-root hazard, and the shell's blanket
		# HUD visibility toggle would re-show a surviving DEATH shroud next mission
	if _command_map_presenter != null:
		_command_map_presenter.teardown()  # the cmap.mnu frame holds the OLD world's root
	if _end_round_presenter != null:
		_end_round_presenter.teardown()  # same stale-root hazard: the stat.mnu frame and
		# its MenuAudio hold the OLD world's resource root, and a frame left visible when
		# the session ends mid-STAT would be re-shown over the next mission


# The mission teardown's profile step (and the SP restart's): outside a session
# it runs only once the round is over; a won round (the 0x1D's winner team 1) of a
# campaign mission writes its completion byte, then the profile's +1460 word goes back
# and the profile is saved (frontend::Frontend::record_round_end, over the started
# mission's campaign words).
func _record_round_end() -> void:
	var sim: Simulation = _world.get_sim() if _world != null else null
	if sim != null:
		_frontend.record_round_end(sim.is_host_listening() or sim.is_joiner(), sim.is_round_over(),
				sim.get_end_round_winner_team())


## The Mission box's Game Loop out of a session: the mission comes down and the mission the
## box names starts from its own load entry, loose when its file stands in the game folder
## (retail's g_MissionSourceIsLoose test, File_CheckExists). False with no name.
func _start_mission_box_mission(bms_name: String) -> bool:
	if bms_name.is_empty() or _world == null:
		return false
	var root := _world.get_resource_root()
	var loose := root != null and FileAccess.file_exists(
			String(root.get_root_dir()).path_join(bms_name))
	_teardown_world(false)
	start_world_load(LoadingScreenInfo.for_mission(bms_name),
			_world.load_loose_mission.bind(bms_name) if loose
			else _world.load_mission.bind(bms_name))
	return true


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
	# The restart's start: the boot skips the fresh start's intro cine and profile word
	# and keeps the first start's UP-TIME stamp (engine KernelBootOptions::restart).
	_world.set_restart_boot(true)
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
	if sim == null or _frontend.is_loading() or not sim.ingame_restart():
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
			return connection_error.reason_text(Strings.get_table(Strings.TABLE_GAMEERR))
		return route.error_text
	var text := Strings.lookup_or(Strings.TABLE_GAMEERR, Strings.SECTION_GENERIC_ERRORS, key, "")
	if text.is_empty() and key == "STRE_BADMISSION":
		return "Error - Mission requires assets or an expansion that is not present"
	return text if not text.is_empty() else key


## The main menu's EXIT, or a started updater (the menu's UPDATE, the NovaWorld
## menu's mod install), saves the player profile and game.cfg before the game
## quits (frontend::Frontend::exit_to_desktop).
func exit_to_desktop(reason: String) -> void:
	push_warning("MainGame: %s; the process ends" % reason)
	_frontend.exit_to_desktop()
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
	# The game loop's screen pass leads the frame: an in-game menu screen's click
	# runs ahead of the frame's net frame and tick, so the INGAME CONFIRM_YES's
	# exit (a joiner's goodbye) leaves before this frame's session sends
	# (MenuShell.pump_screen_pass carries the witness). The GameWorld leg table is
	# unchanged; the pump precedes the whole world frame.
	if _menu_shell != null and _menu_shell.is_in_game():
		_menu_shell.pump_screen_pass()
		if _shutdown_prepared:
			return
	_sync_screen_word()
	_refresh_dev_tools_game_state()
	_sync_keyboard_capture()
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
	var screen_state := _frontend.is_screen_open()
	if screen_state or dev_tools_interacting or _end_flow.has_screen() or not _world.is_loaded():
		if Input.get_mouse_mode() == Input.MOUSE_MODE_CAPTURED:
			# A game screen's free cursor starts at the re-centre point every gameplay
			# update left the cursor on (engine world::kCursorCentreX / kCursorCentreY
			# carry the witness).
			if screen_state and _player_presenter != null:
				_player_presenter.release_capture_for_screen()
			else:
				Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)
	# The open-menu-screen flag the HUD's armory line yields to, every frame
	# ahead of the early returns (the SP pause stops the HUD tick): every state
	# but WORLD holds an in-game screen (PAUSED the in-game menu, ARMORY,
	# DEPLOY, END_ROUND, COMMAND_MAP), the stand-in _maybe_open_death_menu
	# reads too (hud-re D-HUD-14).
	if _hud_presenter != null:
		_hud_presenter.set_menu_screen_open(not _frontend.in_world())
	# The shell-control span closes before the early returns so every frame banks it.
	var probe_t0 := Time.get_ticks_usec() if timing else 0
	_frame_phase_sampler.finish_shell_control(probe_t0)
	# The deferred flush's queuers: a marker at each gameplay leg's end (an instrument).
	if timing:
		_frame_phase_sampler.queue_flush_leg(FrameStats.FRAME_FLUSH_AHEAD)
	# Only the pause menu freezes the world, and only in a SINGLE-PLAYER session: the
	# in-mission screens run over live play and a net session never freezes
	# (frontend::Frontend::open_screen carries the witness).
	if not _world.is_loaded():
		# The menu mode's frames bank on the main loop's one bank, whose drained menu
		# updates the PLAYER_INFO portrait steps by (GameFrontend.last_menu_updates).
		if _frontend.get_mode() == GameFrontend.MODE_MAIN_MENU:
			_frontend.bank_menu_frame()
		return
	# The start-mission splash holds the world un-ticked (frontend::Frontend::mission_loaded
	# carries the witness); session loads never splash, so the joiner pump is untouched.
	if _world_load.is_splash_active():
		return
	# The single-player pause menu still runs every frame: the engine session's Paused
	# state holds the tick, and the paused frame's own legs (the main-frame counter, the
	# weather tick ahead of the pause test, the cine pass and the HUD radar) run through
	# the session's paused branch exactly as under the debug pause row
	# (engine inmatch::Session::run_paused_update_legs carries the witness).
	# A joiner's mission start owes its input reset once its game-start wait
	# ends (engine inmatch::ClientRuntime::take_input_reset).
	var reset_sim: Simulation = _world.get_sim()
	if reset_sim != null and reset_sim.take_input_reset():
		_reset_key_state()
	# The input frame's keyboard pass runs inside each update the world tick's session
	# drains, at the input frame's head (_run_keyboard_pass).
	# The session banks the frame's measured elapsed wall time, not Godot's
	# process delta, which Godot caps (GameWorld.take_frame_elapsed_seconds and
	# engine world::TickAccumulator::bank carry the witness).
	var frame_elapsed: float = _world.take_frame_elapsed_seconds()
	var frame_input: MissionFrameInput
	if _player_presenter != null:
		# The input frame runs over live play and, under a pass-through screen
		# (death.mnu's DEATH, stat.mnu's STAT), while the screen's word holds: every
		# action row then meets the action head's dead-player, deploy-overlay and
		# round-over gates, and the death screen's binding scan. No input frame runs
		# under the INGAME (pause), WEAPON (armory), CMAP (command map) or VEHICLE
		# screen, nor under DEATH's exit confirmation (engine
		# controls::PlayerActionPoll::menu_open carries the witness). Only live play
		# captures the mouse.
		frame_input = _player_presenter.before_world_tick(
				frame_elapsed, is_gameplay_input_active(),
				is_gameplay_input_active() or _frontend.screen_input_frame(_tools_interacting()),
				_frontend.screen_holds_input())
	else:
		frame_input = MissionFrameInput.new()
		frame_input.delta_seconds = frame_elapsed
	# The frame loop feeds the session the time since the last render: the
	# mission-start frames bank only that (Session::advance).
	frame_input.since_render_seconds = _world.get_seconds_since_render()
	# The command map as the open menu screen: the scene frame then skips its world
	# (engine inmatch::FrameInput::command_map_open).
	frame_input.command_map_open = _frontend.get_screen() == MenuDriver.MISSION_SCREEN_COMMAND_MAP
	var probe_t1 := Time.get_ticks_usec() if timing else 0
	if timing:
		_frame_phase_sampler.queue_flush_leg(FrameStats.FRAME_FLUSH_PLAYER)
	var skip_world := probe_enabled and _perf_probe.skip_world
	if not skip_world:
		_world.tick(_camera.global_position, _camera.global_transform,
				frame_elapsed, frame_input)
	var probe_t2 := Time.get_ticks_usec() if timing else 0
	if timing:
		_frame_phase_sampler.queue_flush_leg(FrameStats.FRAME_FLUSH_WORLD)
	if _player_presenter != null:
		# The held USE row's armory and vehicle-loadout arms took their press in an update's
		# sample (engine controls::PlayerActions, through the sim's press latch). With a menu
		# screen up (a pass-through one) either arm closes it instead of opening its own;
		# over live play the armory opens weapon.mnu WEAPON, the vehicle arm vehicle.mnu
		# VEHICLE past its team test (engine world::local_player_use_item_arm carries the
		# witness, D-NET-1320).
		var armory: bool = _player_presenter.take_use_item_armory_request()
		var vehicle: bool = _player_presenter.take_use_item_vehicle_menu_request()
		var use_sim: Simulation = _world.get_sim() if _world != null else null
		match _frontend.use_item(armory, vehicle, vehicle and use_sim != null
				and use_sim.local_player_vehicle_zone_team_matches()):
			GameFrontend.USE_CLOSE_SCREEN:
				_close_screen_presenter()
			GameFrontend.USE_OPEN_ARMORY:
				_open_armory()
			GameFrontend.USE_OPEN_VEHICLE:
				_vehicle_menu_presenter.open()
	# Camera placement runs in the world frame now (local-view device leg,
	# D-RORD-8); this covers frames that skip it (probe world-skip, no live world).
	if _player_presenter != null and (skip_world or not _world.is_loaded()):
		_player_presenter.after_world_tick()
	var probe_t3 := Time.get_ticks_usec() if timing else 0
	if timing:
		_frame_phase_sampler.queue_flush_leg(FrameStats.FRAME_FLUSH_PLAYER)
	# The shared HUD presenter rebuilds the per-frame info while the player is in-world
	# (WORLD or the live-play ARMORY) [orig: HUD_BuildEntityInfo @0x4b8440 per frame].
	var skip_hud := probe_enabled and _perf_probe.skip_hud
	if _hud_presenter != null and not skip_hud and _frontend.hud_frame_runs():
		_hud_presenter.tick()
		_end_round_presenter.tick()  # the same HUD frame [orig: HUD_DrawOverlayPanels]
	var probe_t4 := Time.get_ticks_usec() if timing else 0
	if timing:
		_frame_phase_sampler.queue_flush_leg(FrameStats.FRAME_FLUSH_HUD)
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
