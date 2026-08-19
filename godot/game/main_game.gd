class_name MainGame
extends Node3D

# Runtime shell: boots into the game's menu front-end (MenuShell, driving the
# .mnu menu set + audio from the chosen resource dir) and hands off to a GameWorld
# when the player starts a mission, with pause + return-to-menu on demand. The
# engine ships no game data; everything (menus, audio, terrain, missions) loads
# from the chosen resource dir. The first-launch directory picker lives here
# (runtime-only); headless probes set the dir explicitly and never block on it.

const ResourceDirSettings := preload("res://game/resource_index/resource_dir_settings.gd")
const DebugOverlayScript := preload("res://game/debug/nova_debug_overlay.gd")
const DebugViewContext := preload("res://game/debug/nova_debug_view_context.gd")
const GameDebugAdapterScript := preload("res://game/game_debug_adapter.gd")
const LocalPlayerPresenterScript := preload("res://game/world/local_player_presenter.gd")
const VegAssetsScript := preload("res://game/terrain/veg_assets.gd")
const WorldLoadCoordinatorScript := preload("res://game/world_load_coordinator.gd")
const ShellPresentationSessionScript := preload("res://game/shell_presentation_session.gd")
const HudHiddenCaptureWitness := preload("res://game/world/hud_hidden_capture_witness.gd")
# Re-summon the game-folder picker. The original engine has no "change game dir"
# control (the game *is* its install folder); this is an OpenNova convenience so a
# wrong / menu-less folder can be re-picked without restarting. Front-end only.
const CHANGE_DIR_KEY := KEY_F9
# The HUD presenter's gameplay keys (objectives/friendly-tags) live with the
# presenter — GameHudPresenter.handle_gameplay_key.
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
# Shift+F6: pick the entity under the crosshair into the debug pick list
# (DebugPickFlow). Works while playing, no overlay needed. Unmodified F6 stays
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
enum State { MENU, WORLD, PAUSED, ARMORY, DEPLOY }

@onready var _world: GameWorld = $World
@onready var _camera: FlyCamera = $Camera3D
@onready var _hud: CanvasLayer = $HUD
@onready var _menu_layer: CanvasLayer = $MenuLayer
@onready var _menu_shell: MenuShell = $MenuLayer/MenuShell

var _picker: FileDialog
var _root: ResourceRoot
var _state: int = State.MENU
var _shell_wired := false
var _debug_overlay  # DebugOverlay, lazily built on the first F3
var _debug_adapter: GameDebugAdapter
# The debug pick list: SHELL-owned so F6 picks work before F3 ever opens and
# the set survives overlay toggles; cleared on every world load.
var _pick_list := DebugPickList.new()
var _pick_flow := DebugPickFlow.new()
var _net: NetSessionController  # every net-session entry (LAN/NovaWorld + env hooks)
# The in-game HUD rides GameHudPresenter. It owns the lazy GameHud build, the
# per-frame info rebuild, and the
# mission text feed (queued until the HUD exists); this shell only says when the
# player is in-world.
var _hud_presenter: GameHudPresenter
var _player_presenter: LocalPlayerPresenter = null
# The per-system frame-stats board behind F3 -> Stats. Created with the shell
# and handed to every feeding owner; it costs nothing until the tab opens
# (capture stays inactive, every feed site gates on it).
var _frame_stats := FrameStatsBoard.new()
# Root-viewport render-time sampling for the Stats tab; the sampler owns the
# RenderingServer measurement edge latch and the wall-frame clock.
var _render_stats := RootRenderStatsSampler.new()
var _mp_companion  # MpMenuCompanion: drives the multiplayer (mp.mnu) menu by control name
var _lan_session: LanSession  # retail-style 0x41/0x81 LAN enumeration browser
var _player_info_companion  # PlayerInfoMenuCompanion: drives the PLAYER_INFO (player.mnu) character screen
var _armory_presenter: ArmoryPresenter  # the SHARED in-world armory surface (weapon.mnu WEAPON)
var _deploy_presenter: DeployScreenPresenter  # the joiner's deploy-map screen (death.mnu DEATH)
var _use_latched := false  # USE-ITEM press latch; the mount toggle runs on RELEASE
var _chosen_avatar: Dictionary = {}  # canonical active + per-side PLAYER_INFO selection
var _profile_root_key := ""  # reload weapon.sav only when the mounted game/expansion changes
var _world_load := WorldLoadCoordinatorScript.new()
var _world_load_pending := false
# End-of-mission flow (SP): set by the sim's "round_end" effect [orig:
# Server_ProcessRoundEnd @0x5164f0 SP tail]. The world keeps ticking underneath
# (the SP world runs through the epilog — humans >= 1 keeps the run gate open);
# player input idles once the round is over [orig: the post-round input gate —
# the client input uplinks stop against g_spawn_success_gate @0x42c410].
var _round_ended := false
# Pending NW_SP_DEBUG_POSE teleport (see DebugPoseEnv).
var _debug_pose_env := OS.get_environment("NW_SP_DEBUG_POSE")
var _end_winner := 0
var _end_screen_delay := 0.0
var _end_screen: MissionEndScreen = null
var _shutdown_prepared := false
var _shutdown_resources_released := false
var _quit_requested := false
var _quit_policy_installed := false
var _previous_auto_accept_quit := true
var _shell_presentation := ShellPresentationSessionScript.new()


func _init() -> void:
	# The sampler observes the board's capture close edge directly (render-time
	# measurement is RenderingServer state, not Node-owned state).
	_render_stats.setup(_frame_stats)
	_world_load.load_failed.connect(_on_world_load_failed)


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
	_cleanup_picker()
	finish_hud_hidden_capture()
	for presenter in [
		_player_presenter, _armory_presenter, _deploy_presenter, _hud_presenter,
	]:
		if presenter != null:
			presenter.teardown()
	if _world != null:
		_world.cancel_join_preload()
		_world.cancel_join_admission()
		_world.unload()
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
	_cleanup_picker()
	if is_instance_valid(_menu_shell):
		_menu_shell.release_runtime_renderer_resources()
	else:
		Input.set_custom_mouse_cursor(null, Input.CURSOR_ARROW)
	if _world != null:
		_world.release_runtime_renderer_resources()
	if _root != null:
		_root.clear()
	VegAssetsScript.clear_cache()
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
	_previous_auto_accept_quit = get_tree().auto_accept_quit
	get_tree().auto_accept_quit = false
	_quit_policy_installed = true
	if _world == null or _camera == null or _menu_shell == null:
		return
	var debug_adapter := get_game_debug_adapter()
	add_child(debug_adapter)
	debug_adapter.start_runtime_endpoint()
	# Esc toggles pause/resume in a world (the fly camera reports the key; the
	# owner decides what it means).
	if not _camera.escape_pressed.is_connected(_on_camera_escape):
		_camera.escape_pressed.connect(_on_camera_escape)
	_player_presenter = LocalPlayerPresenterScript.new()
	_player_presenter.name = "LocalPlayerPresenter"
	add_child(_player_presenter)
	_player_presenter.setup(_world, _camera, _camera)
	_world.set_local_view_presenter(_player_presenter)  # D-RORD-8 view leg
	# The in-world armory + HUD ride their shared engine presenters. Created here,
	# not in _wire_shell, so menu-less entries (the env launch hooks) still get
	# them; the HUD presenter's
	# setup connects mission_effects before any world can tick (PreMission/WAC
	# effects may drain on the first runtime tick, and it queues them until the
	# lazy HUD exists).
	_armory_presenter = ArmoryPresenter.new()
	_armory_presenter.name = "ArmoryPresenter"
	add_child(_armory_presenter)
	_armory_presenter.setup(_world, _player_presenter, _hud if _hud != null else self)
	_armory_presenter.opened.connect(func() -> void: _state = State.ARMORY)
	_armory_presenter.closed.connect(_on_resume)
	# The joiner's deploy-map screen (death.mnu DEATH): opened when the join reaches
	# the player-paced deployment pick, self-closing on the deployment release
	# [orig: the 0x0A flags1 bit1 hold chain; net-re 5.61].
	_deploy_presenter = DeployScreenPresenter.new()
	_deploy_presenter.name = "DeployScreenPresenter"
	add_child(_deploy_presenter)
	_deploy_presenter.setup(_world, _hud if _hud != null else self)
	# Same contract as the armory: the screen owns the cursor while it is up, so the
	# shell must leave State.WORLD or LocalPlayerPresenter re-captures the mouse every
	# frame and the spawn rows become unclickable.
	_deploy_presenter.opened.connect(func() -> void: _state = State.DEPLOY)
	_deploy_presenter.closed.connect(func() -> void:
		if _state == State.DEPLOY:
			_state = State.WORLD
	)
	_world.join_deploy_pick_required.connect(_on_join_deploy_pick_required)
	_world.join_admission_ready.connect(_on_join_admission_ready)
	_world.session_lost.connect(_on_session_lost)
	_hud_presenter = GameHudPresenter.new()
	_hud_presenter.name = "GameHudPresenter"
	add_child(_hud_presenter)
	_hud_presenter.setup(_world, _player_presenter, _hud if _hud != null else self)
	# Every net-session ENTRY (LAN browser/host, NovaWorld panel + env hooks)
	# lives on the NetSessionController component; the shell keeps the state
	# machine, the load pipeline, and the session-presentation states.
	_net = NetSessionController.new()
	_net.name = "NetSessionController"
	add_child(_net)
	_net.setup(self, _world, _menu_shell, $MenuLayer)
	# One shared frame-stats board across the shell, the world, and the HUD
	# presenter; the world re-hands it to each mission runtime it creates.
	_world.set_frame_stats_board(_frame_stats)
	_hud_presenter.set_frame_stats_board(_frame_stats)
	# The shell's own round-outcome tap (the HUD presenter keeps its separate connection
	# for text/banner presentation): "round_end" starts the end-of-mission flow.
	if not _world.mission_effects.is_connected(_on_shell_mission_effects):
		_world.mission_effects.connect(_on_shell_mission_effects)
	# Editor-managed runs pass an exact process-local directory. It wins over
	# persisted settings but is never written back.
	var dir := LaunchFlags.resource_dir(ResourceDirSettings.get_resource_dir())
	if dir.is_empty():
		_request_resource_dir()
		return
	if not _enter_menu(dir):
		# The picker is up and the shell holds no root: none of the boot
		# continuations below could load anything.
		return
	# F6 is still the real standalone game and normal loading presentation; it
	# only selects the exact saved top-level loose BMS instead of an archive row.
	var loose_mission := LaunchFlags.loose_mission()
	if not loose_mission.is_empty():
		start_loose_mission(loose_mission)
		return
	# Dev/headless convenience: NW_SP_MISSION=<name.bms> boots straight into a single-player
	# mission via the same path as the menu's Start button, so the runtime (and its HUD) can be
	# exercised without menu navigation. Off by default. NW_SP_DEBUG_POSE rides
	# beside it (one-shot post-spawn teleport; see DebugPoseEnv).
	var sp_mission := OS.get_environment("NW_SP_MISSION")
	if not sp_mission.is_empty():
		_on_start_requested(sp_mission)
		return
	# Co-op LAN demo hooks (NW_LAN_HOST / NW_LAN_JOIN) ride the controller.
	# Mirrors NW_SP_MISSION above; two instances on localhost = the co-op demo.
	_net.maybe_launch_lan_from_env()


# Consume Esc before weapon.mnu's shell-wired CANCEL hotkey and FlyCamera can both
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
	if key == null:
		return
	# The USE-ITEM release edge: a latched press runs the mount toggle on RELEASE
	# [orig: Input_ProcessFrame @0x49d520 consumes the latch on key release
	# -> Entity_ToggleVehicleMount @0x49d6dc].
	if not key.pressed and key.keycode == ARMORY_KEY:
		if _use_latched:
			_use_latched = false
			if is_gameplay_input_active() and _try_toggle_mount():
				get_viewport().set_input_as_handled()
		return
	if not key.pressed or key.echo:
		return
	# F11 fullscreen — the core-engine window concept (WindowState), shared with ONED.
	if WindowState.is_toggle_event(event):
		WindowState.toggle_fullscreen(get_window())
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
	if key.keycode == PICK_KEY and key.shift_pressed and is_gameplay_input_active() \
			and _world != null and _world.is_loaded():
		pick_at_crosshair()
		get_viewport().set_input_as_handled()
		return
	# The HUD presenter's gameplay keys (objectives toggle / friendly-tags
	# cycle), in-world only — bindings + orig cites at the presenter.
	if is_gameplay_input_active() and _hud_presenter != null \
			and _hud_presenter.handle_gameplay_key(key.keycode):
		get_viewport().set_input_as_handled()
		return
	# The USE-ITEM key: in-world only. Zone legs first — the armory volume opens
	# weapon.mnu [orig: useitem action 177, Flags & 0x400000 @0x4e0b4d] — otherwise the
	# key is the vehicle mount/dismount toggle on the same witnessed action [orig: the
	# LABEL_121 latch @0x4e0b71 -> Input_ProcessFrame release edge @0x49d6dc ->
	# Entity_ToggleVehicleMount @0x436950]. (The vehicle-loadout-volume vehicle.mnu leg
	# @0x4e0bfe awaits that screen's port.)
	if key.keycode == ARMORY_KEY and is_gameplay_input_active():
		if _try_open_armory():
			get_viewport().set_input_as_handled()
		else:
			# No zone leg consumed the press: latch — the toggle runs on the release
			# edge [orig: dword_24C18DC set @0x4e0b71; a press consumed by a zone leg
			# suppresses the release, our latch-only-on-miss].
			_use_latched = true
			get_viewport().set_input_as_handled()
		return
	# The gameplay keys (F4 first/third person, C/Z stance) live on LocalPlayerPresenter.
	if _player_presenter != null and _player_presenter.handle_key_input(
			event, is_gameplay_input_active()):
		get_viewport().set_input_as_handled()


# F3: the mission debug overlay over the live runtime. Built lazily; without a
# running mission it just reports so (the runtime source re-resolves per
# refresh, so reloads and menu round-trips never leave it stale).
func toggle_debug_overlay() -> void:
	if _debug_overlay == null:
		_debug_overlay = DebugOverlayScript.new(
				DebugOverlay.DEFAULT_CONFIG_PATH,
				get_debug_session())
		_debug_overlay.name = "DebugOverlay"
		var mount: Node = _hud if _hud != null else self
		mount.add_child(_debug_overlay)
		_debug_overlay.visibility_changed.connect(
				_on_debug_overlay_visibility_changed)
		_debug_overlay.set_runtime_source(_current_runtime)
		_debug_overlay.set_view_context_source(_current_player_view_context)
		_debug_overlay.set_frame_stats_board(_frame_stats)
		_debug_overlay.set_world_source(func(): return _world)
		_debug_overlay.set_player_source(func(): return _player_presenter)
		_debug_overlay.set_effect_world_source(_current_effect_world)
		_debug_overlay.set_pick_list(_pick_list)
	_debug_overlay.toggle()


func _on_debug_overlay_visibility_changed() -> void:
	var open := is_debug_overlay_open()
	# While the overlay is up the mouse is free: clicks on the world ray-pick
	# into the same list F6 feeds. Every close path (F3, Escape, Close button)
	# reaches this inherited visibility edge.
	if _world != null:
		_world.set_pick_click_enabled(open)
	if open:
		# A press begun before F3 must not turn into a mount action when Shift is
		# released behind the overlay.
		_use_latched = false


func is_debug_overlay_open() -> bool:
	return _debug_overlay != null and is_instance_valid(_debug_overlay) \
			and _debug_overlay.visible


## Shift+F6 (and the probe/test seam): pick whatever the crosshair is on into the
## debug pick list, with a brief on-screen confirmation (DebugPickFlow).
func pick_at_crosshair() -> void:
	var sim = _world.get_sim() if _world != null else null
	_pick_flow.pick_at_crosshair(sim, _camera, _pick_list,
			_hud if _hud != null else self)


func get_debug_overlay() -> DebugOverlay:
	return _debug_overlay if _debug_overlay != null \
			and is_instance_valid(_debug_overlay) else null
func get_debug_session() -> DebugSession:
	return get_game_debug_adapter().get_debug_session()
func get_game_debug_adapter() -> GameDebugAdapter:
	if _debug_adapter == null:
		_debug_adapter = GameDebugAdapterScript.new()
		_debug_adapter.configure(
			_current_runtime,
			func(): return _world,
			func(): return _player_presenter,
			_shell_state_name,
			func(): return _world_load_pending,
			is_debug_overlay_open,
			_on_resume,
			_on_return_to_menu,
			request_quit)
		_debug_adapter.set_menu_shell_source(func(): return _menu_shell)
		_debug_adapter.set_ingame_screen_actions(
				mcp_open_ingame_menu, mcp_open_armory)
		_debug_adapter.set_render_capture_actions(
				mcp_begin_world_only_capture, mcp_end_world_only_capture)
		_debug_adapter.set_hud_hidden_capture_actions(
				begin_hud_hidden_capture,
				finish_hud_hidden_capture,
				hud_hidden_capture_witness)
	return _debug_adapter
func get_frame_stats_board() -> FrameStatsBoard:
	return _frame_stats

func is_root_render_stats_measured() -> bool:
	return _render_stats.is_measured()


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
	var sim = _world.get_sim() if _world != null else null
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
	var sim = _world.get_sim() if _world != null else null
	if sim != null:
		outcome = sim.get_round_outcome_debug()
	if outcome.is_empty():
		outcome = {"ended": true, "winner_team": _end_winner}
	_end_screen = MissionEndScreen.new()
	_end_screen.name = "MissionEndScreen"
	var banner := _hud_presenter.endround_banner_line() if _hud_presenter != null else ""
	_end_screen.setup(outcome, banner, _root)
	var mount: Node = _hud if _hud != null else self
	mount.add_child(_end_screen)
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


func _shell_state_name() -> String:
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


func _current_player_view_context() -> DebugViewContext:
	var context := DebugViewContext.new()
	if _camera != null and is_instance_valid(_camera):
		context.camera = _camera
	if _player_presenter != null and is_instance_valid(_player_presenter):
		context.camera_mode_known = true
		context.third_person = bool(_player_presenter.is_third_person())
	return context


func _current_effect_world():
	return _world.get_effect_world() if _world != null else null


# Mission-effect passthrough + the last-text read seam: the surface lives on the
# shared GameHudPresenter (queued until the lazy HUD exists); these stay callable
# on the shell for drains routed here and for the parity tests (ADR 0018).
func apply_mission_effects(effects: Array) -> void:
	if _hud_presenter != null:
		_hud_presenter.apply_mission_effects(effects)


func hud_objective_line() -> String:
	return _hud_presenter.hud_objective_line() if _hud_presenter != null else ""


# Whether the folder picker may be summoned right now: only from the menu front-end
# and only when one is not already open. Pure predicate so it is unit-testable
# headless (the native dialog itself cannot be shown without a display).
func _can_summon_dir_picker() -> bool:
	return _state == State.MENU and _picker == null


# --- Menu state ---------------------------------------------------------------

# Returns false when the directory would not mount (the picker is raised and
# the shell holds no root) so boot continuations can gate on it.
func _enter_menu(dir: String) -> bool:
	if _root == null or _root.get_root_dir() != dir:
		var root := mount_boot_root(dir, LaunchFlags.loose_root_allowed())
		if root == null:
			_request_resource_dir()
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
	if not _menu_shell.setup(_root):
		push_warning("MainGame: no menu found in resource dir (looked for %s)" % _menu_shell.main_menu_file)
	_menu_shell.show_menu()
	return true


func _wire_shell() -> void:
	if _shell_wired:
		return
	_shell_wired = true
	_menu_shell.start_requested.connect(_on_start_requested)
	_menu_shell.exit_to_desktop_requested.connect(_on_exit_to_desktop)
	_menu_shell.return_to_menu_requested.connect(_on_return_to_menu)
	_menu_shell.resume_requested.connect(_on_resume)
	_menu_shell.novaworld_requested.connect(_net.open_novaworld_panel)
	_menu_shell.crosshair_style_changed.connect(_on_crosshair_style_changed)
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


# Install the in-memory local-player profile used by the next mission spawn.
func set_local_player_profile(profile: Dictionary) -> void:
	_chosen_avatar = profile.duplicate(true)
	if _player_info_companion != null:
		_player_info_companion.set_persisted_profile(_chosen_avatar)


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
			push_warning("MainGame: could not save PLAYER_INFO profile (error %d)" % save_error)


func _on_crosshair_style_changed(style: int) -> void:
	if _hud_presenter != null:
		_hud_presenter.set_crosshair_style(style)


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


# The USE-ITEM mount toggle: outside the armory volume the same key enters/exits
# vehicles (deck best-seat, nearest-seat scan, seat-swap-or-detach — all sim-side).
# [orig: Entity_ToggleVehicleMount @0x436950 via the useitem release edge @0x49d6dc]
func _try_toggle_mount() -> bool:
	var runtime = _current_runtime()
	if runtime == null:
		return false
	var sim: Simulation = runtime.get_sim()
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
	apply_picked_resource_dir(dir, not LaunchFlags.resource_dir().is_empty())


## The picker's accept leg. `editor_managed` is resolved from --resource-dir at
## the signal callback above: an editor-managed run's directory is process-local,
## so persisting a picker escape would overwrite the SHARED editor+game key and
## repoint ONED's authoring root at whatever was picked here. Parameterized for
## the same ADR-0018 reason as mount_boot_root; returns false when the pick
## would not mount (the picker is re-raised).
func apply_picked_resource_dir(dir: String, editor_managed: bool) -> bool:
	var root := mount_boot_root(dir, LaunchFlags.loose_root_allowed())
	if root == null:
		_request_resource_dir()
		return false
	_root = root
	if not editor_managed:
		ResourceDirSettings.set_resource_dir(dir)
	_enter_menu(dir)
	return true


## Mount `dir` as this shell's resource root: packed PFFs, `/exp` expansion,
## `/d` loose override, and `/game` SCR policy. With `allow_loose_root` (the
## `--loose-root` flag, passed by every ONED-managed run) a directory holding
## none of the packed archives falls back to the editor's loose mount — the
## same data contract ONED authors against, so F5/F6 can play-test a loose
## extract (ADR 0025). The no-archives fatal stays the standalone default
## [orig: PFF_OpenAllArchives @ 0x4a4310; Game_InitSubsystems @ 0x4a6f44].
## Warns and returns null on failure. Public and parameterized so the fallback
## contract is testable without process arguments (ADR 0018).
func mount_boot_root(dir: String, allow_loose_root: bool) -> ResourceRoot:
	var root := ResourceRoot.new()
	var expansion := LaunchFlags.expansion(ResourceDirSettings.get_expansion())
	var game := LaunchFlags.game(ResourceDirSettings.get_game())
	var err: int = root.mount_runtime(
			dir, expansion, LaunchFlags.loose_override_enabled(), game)
	if err != OK:
		# ERR_FILE_NOT_FOUND is specifically the zero-archives fatal; other
		# errors (missing dir, unreadable root) fail the loose mount too.
		if err == ERR_FILE_NOT_FOUND and allow_loose_root \
				and root.set_root_dir(dir) == OK:
			_report_missing_boot_resources(root)
			return root
		push_warning("MainGame: %s" % root.get_last_error())
		return null
	_report_missing_boot_resources(root)
	return root


# Honest missing-resource errors over the witnessed boot manifest (ENG-6,
# docs/required-resources.md): name each missing fatal-set file with retail's
# witnessed failure behavior instead of dead-ending silently later. Reported,
# not enforced — this shell keeps running so a partial dir stays inspectable
# (the picker flow), where retail shows a MessageBox and exits. On the
# sanctioned loose-root play-test mount an authoring extract is expectedly
# partial, so the same report warns instead of erroring.
func _report_missing_boot_resources(root: ResourceRoot) -> void:
	for name in root.list_missing_boot_resources():
		var text := "MainGame: boot-required resource missing: %s — retail: %s" \
				% [name, root.boot_resource_failure_text(name)]
		if root.is_runtime_mount():
			push_error(text)
		else:
			push_warning(text)


func _on_dir_canceled() -> void:
	_cleanup_picker()
	_request_resource_dir()


func _cleanup_picker() -> void:
	if _picker != null:
		_picker.queue_free()
		_picker = null


# --- Menu <-> world transitions ----------------------------------------------

func _on_start_requested(bms_name: String) -> void:
	# Single-player: the loading screen is the sidecar image alone — no session
	# text [orig: the not-in-session path draws only the background @ 0x521ebe].
	start_world_load(
		{"mission_file": bms_name},
		Callable(_world, "load_mission").bind(bms_name))


## Public F6 entry: boot the exact saved loose mission through the same loading
## presentation and GameWorld lifecycle as menu play.
func start_loose_mission(bms_name: String) -> void:
	start_world_load(
		{"mission_file": bms_name},
		Callable(_world, "load_loose_mission").bind(bms_name))


## Graceful cross-process stop seam used by an editor-managed runtime peer.
func request_quit() -> void:
	if _quit_requested:
		return
	_quit_requested = true
	_complete_runtime_shutdown(begin_runtime_shutdown())


func _complete_runtime_shutdown(load_operation: WorldLoadOperation) -> void:
	if load_operation != null and not load_operation.is_settled():
		await load_operation.settled
	finish_runtime_shutdown()
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
func start_world_load(load_info: Dictionary, operation: Callable) -> void:
	if not _world_load.can_start():
		return
	if _lan_session != null:
		_lan_session.stop()
	_world_load_pending = true
	_world.set_local_player_spawn_loadout(_chosen_avatar)
	_begin_world_load()
	if _world_load.start(self, _root, _world,
			load_info.duplicate(true), operation) == null:
		_on_world_load_failed("mission load handoff could not start")


func _begin_world_load() -> void:
	_shell_presentation.begin_world_load(
			_menu_shell, _world, _hud, _on_world_loaded, _on_world_load_failed)
	_state = State.WORLD



func _on_world_loaded() -> void:
	# The GAME music context is the world's to open at mission start (GameWorld
	# calls NovaMusicService.open_game_context, so every live mission entry path
	# gets the same music); nothing to do here for audio. The witnessed release
	# then reveals the world + HUD at the tail
	# of Game_StartMission [orig: LoadingScreen_ReleaseEffect @ 0x586b80, final
	# call @ 0x525d45]. For SP/host this fires at true load completion. For a
	# joiner this is only wire-header world construction completion; keep pumping the hidden runtime
	# under the loading presentation until the separate authoritative edge.
	# A fresh mission gets a fresh pick set (stale handles never cross
	# sessions); the world renders/curates the shell-owned list from here on.
	_pick_list.clear()
	_world.set_pick_debug(_pick_list)
	_on_debug_overlay_visibility_changed()
	var sim := _world.get_sim()
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
	# Round over: ESC leaves the mission instead of pausing [orig: ESC (0x1B) sets
	# g_mission_exit_reason = 1 during the epilog, Input_HandleSpecialKeys @0x49c8e2].
	if _round_ended:
		if _end_screen != null:
			_end_screen.request_exit()
		else:
			_on_end_screen_exit()
		return
	if _state == State.WORLD or _state == State.DEPLOY:
		# ESC from the deploy screen still reaches the in-game menu (and therefore
		# RETURN TO MENU): a joiner parked at the pick must be able to leave.
		_pause()
	elif _state == State.PAUSED or _state == State.ARMORY:
		_on_resume()


func _pause() -> void:
	_state = State.PAUSED
	_menu_shell.open_ingame_menu()  # game.mnu overlay over the kept-loaded world
	_menu_shell.show_menu()


func _on_resume() -> void:
	if _state != State.PAUSED and _state != State.ARMORY:
		return
	if _armory_presenter != null and _armory_presenter.is_open():
		_armory_presenter.close()  # Esc from ARMORY closes the overlay (no re-entry: closed
							  # only fires while open)
	_menu_shell.hide_menu()
	# A joiner who paused from the deploy screen still owes its pick, so resume back
	# into DEPLOY rather than handing the cursor back to the world.
	_state = State.DEPLOY if (_deploy_presenter != null and _deploy_presenter.is_open()) \
			else State.WORLD


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
			or _round_ended:
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
			or _round_ended or _armory_presenter == null:
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
	_round_ended = false
	_end_winner = 0
	_end_screen_delay = 0.0
	if _end_screen != null:
		_end_screen.queue_free()
		_end_screen = null
	if _player_presenter != null:
		_player_presenter.teardown()
	if _armory_presenter != null:
		_armory_presenter.teardown()  # the built menu holds the OLD world's resource root
	if _deploy_presenter != null:
		_deploy_presenter.teardown()  # same stale-root hazard, and the shell's blanket
		# HUD visibility toggle would re-show a surviving DEATH shroud next mission
	_world.unload()
	if _player_presenter != null:
		_player_presenter.setup(_world, _camera, _camera)
	if _hud_presenter != null:
		_hud_presenter.teardown()
	if _root != null and _enter_menu(_root.get_root_dir()):
		return
	# No mountable root to return to: land on the pre-mount front-end state so
	# the picker/F9 contract (MENU-only) holds, with the picker as the only
	# recovery surface.
	_state = State.MENU
	_request_resource_dir()


func _on_exit_to_desktop() -> void: request_quit()


## Keep the public adapter callback while the capture module owns mutation.
func mcp_begin_world_only_capture() -> Error:
	return _shell_presentation.begin_world_only_capture(self, _hud, _menu_layer, _camera)


func mcp_end_world_only_capture() -> void: _shell_presentation.finish_world_only_capture()


## Begin a reversible retail HUD-detail-3 capture. Unlike world_only this
## deliberately keeps the HUD CanvasLayer, PlayerViewEffects, viewmodel pass,
## and world presentation mounted; the shell suppresses compiled gameplay HUD
## commands and its FPS counter without persisting either override.
func begin_hud_hidden_capture() -> Error:
	return _shell_presentation.begin_hud_hidden_capture(_hud_presenter, _hud)


func finish_hud_hidden_capture() -> void: _shell_presentation.finish_hud_hidden_capture(_hud_presenter)


## Public semantic witness consumed by render-fixture capture probes. The
## The presentation session combines presenter draw-list/effects/card facts
## with the CanvasLayer boundary so a hidden parent cannot masquerade as an
## empty gameplay HUD.
func hud_hidden_capture_witness() -> HudHiddenCaptureWitness:
	return _shell_presentation.hud_hidden_capture_witness(_hud_presenter, _hud)


# Drive the loaded world's per-frame foliage coverage. Tick whenever a world is
# loaded and not paused (the pause menu freezes it); tick() itself no-ops until the
# world finishes loading. Gating on "loaded, not paused" rather than State.WORLD
# also lets a caller that drives load_world() directly (the headless runtime probe,
# which stays in MENU) keep dispatching foliage.
var _perf_probe_enabled := false
var _perf_probe_spans: Dictionary = {}
var _perf_probe_skip_world := false
var _perf_probe_skip_hud := false


## The frame-span probe is deliberately opt-in: its clock reads and Dictionary
## writes would otherwise perturb every retail frame it is meant to measure.
func set_perf_probe_enabled(enabled: bool) -> void:
	_perf_probe_enabled = enabled
	_perf_probe_spans.clear()
	if not enabled:
		_perf_probe_skip_world = false
		_perf_probe_skip_hud = false


func _process(delta: float) -> void:
	if _shutdown_prepared:
		return
	if not _debug_pose_env.is_empty() and _state == State.WORLD:
		_debug_pose_env = DebugPoseEnv.apply(_debug_pose_env,
				_world.get_sim() if _world != null else null)
	var probe_enabled := _perf_probe_enabled
	var stats_on := _frame_stats.is_capture_active()
	# One shared gate for the frame-leg clock reads: the manual A/B probe and
	# the F3 Stats capture both consume the same measurements.
	var timing := probe_enabled or stats_on
	if probe_enabled:
		_perf_probe_spans.clear()
	_render_stats.sample(get_viewport(), stats_on)
	var debug_overlay_open := is_debug_overlay_open()
	# Release the captured mouse while UI overlays the world or nothing is loaded.
	if _state == State.PAUSED or _state == State.ARMORY or _state == State.DEPLOY \
			or debug_overlay_open or _end_screen != null or not _world.is_loaded():
		if Input.get_mouse_mode() == Input.MOUSE_MODE_CAPTURED:
			Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)
	# The end-of-mission lead-in: the world keeps ticking; the score/failed screen
	# mounts after the short beat [orig: the SP world runs through the epilog cine].
	if _round_ended and _end_screen == null and _world.is_loaded():
		_end_screen_delay -= delta
		if _end_screen_delay <= 0.0:
			_show_end_screen()
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
	var probe_t0 := Time.get_ticks_usec() if timing else 0
	var frame_input := MissionFrameInput.new()
	frame_input.delta_seconds = delta
	if _player_presenter != null:
		var player_live := is_gameplay_input_active()
		frame_input = _player_presenter.before_world_tick(
				delta, player_live, player_live)
	var probe_t1 := Time.get_ticks_usec() if timing else 0
	var skip_world := probe_enabled and _perf_probe_skip_world
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
	var skip_hud := probe_enabled and _perf_probe_skip_hud
	if _hud_presenter != null and (_state == State.WORLD or _state == State.ARMORY \
			or _state == State.DEPLOY) and not skip_hud:
		_hud_presenter.tick(is_gameplay_input_active())
	if timing:
		var probe_t4 := Time.get_ticks_usec()
		if probe_enabled:
			_perf_probe_spans["before"] = probe_t1 - probe_t0
			_perf_probe_spans["world"] = probe_t2 - probe_t1
			_perf_probe_spans["after"] = probe_t3 - probe_t2
			_perf_probe_spans["hud"] = probe_t4 - probe_t3
		if stats_on:
			_frame_stats.add(FrameStatsBoard.FRAME_PLAYER_BEFORE, probe_t1 - probe_t0)
			_frame_stats.add(FrameStatsBoard.FRAME_WORLD, probe_t2 - probe_t1)
			_frame_stats.add(FrameStatsBoard.FRAME_PLAYER_AFTER, probe_t3 - probe_t2)
			_frame_stats.add(FrameStatsBoard.FRAME_HUD, probe_t4 - probe_t3)


# Mouse-look rides the shared LocalPlayerPresenter (the yaw/pitch witnesses live there);
# the shell only says when the player is live: gameplay input is active, the
# world is loaded, and the mouse is captured.
func _unhandled_input(event: InputEvent) -> void:
	if _player_presenter != null and _player_presenter.handle_input(
			event,
			is_gameplay_input_active() and _world.is_loaded() \
					and Input.get_mouse_mode() == Input.MOUSE_MODE_CAPTURED):
		get_viewport().set_input_as_handled()
