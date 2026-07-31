extends Node3D

# Runtime shell: boots into the game's menu front-end (NovaMenuShell, driving the
# .mnu menu set + audio from the chosen resource dir) and hands off to a GameWorld
# when the player starts a mission, with pause + return-to-menu on demand. The
# engine ships no game data; everything (menus, audio, terrain, missions) loads
# from the chosen resource dir. The first-launch directory picker lives here
# (runtime-only); headless probes set the dir explicitly and never block on it.

const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")
const DebugOverlayScript := preload("res://engine/debug/nova_debug_overlay.gd")
const DebugViewContext := preload("res://engine/debug/nova_debug_view_context.gd")
const GameDebugAdapterScript := preload("res://game/game_debug_adapter.gd")
const LocalPlayerPresenterScript := preload("res://engine/world/local_player_presenter.gd")

# Re-summon the game-folder picker. The original engine has no "change game dir"
# control (the game *is* its install folder); this is an OpenNova convenience so a
# wrong / menu-less folder can be re-picked without restarting. Front-end only.
const CHANGE_DIR_KEY := KEY_F9
# The objectives-panel toggle. The retail action toggles the panel's alpha byte
# in co-op [orig: Input_HandleActionBinding case @0x49b68b — dword_24C18CC ^=
# 0xFF]; the authored default binding rides the unported input-binding layer
# (D-CTRL-3), so the key itself is a reimpl mapping.
const OBJECTIVES_KEY := KEY_O
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
# F6: pick the entity under the crosshair into the debug pick list (the F3
# Entities page renders it; snapshots embed it). Works while playing, no
# overlay needed; a brief toast confirms what was picked.
const PICK_KEY := KEY_F6
const PICK_TOAST_SECONDS := 1.6
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
@onready var _camera: Camera3D = $Camera3D
@onready var _hud: CanvasLayer = $HUD
@onready var _menu_shell = $MenuLayer/MenuShell

var _picker: FileDialog
var _root: NovaResourceRoot
var _state: int = State.MENU
var _shell_wired := false
var _debug_overlay  # NovaDebugOverlay, lazily built on the first F3
var _debug_adapter: GameDebugAdapter
# The debug pick list: SHELL-owned so F6 picks work before F3 ever opens and
# the set survives overlay toggles; cleared on every world load.
var _pick_list := NovaDebugPickList.new()
var _pick_toast: Label = null
var _net: NetSessionController  # every net-session entry (LAN/NovaWorld/replay + env hooks)
# The in-game HUD rides NovaGameHudPresenter. It owns the lazy GameHud build, the
# per-frame info rebuild, and the
# mission text feed (queued until the HUD exists); this shell only says when the
# player is in-world.
var _hud_presenter: NovaGameHudPresenter
var _player_presenter: LocalPlayerPresenter = null
# The per-system frame-stats board behind F3 -> Stats. Created with the shell
# and handed to every feeding owner; it costs nothing until the tab opens
# (capture stays inactive, every feed site gates on it).
var _frame_stats := FrameStatsBoard.new()
# Root-viewport render-time sampling for the Stats tab; the sampler owns the
# RenderingServer measurement edge latch and the wall-frame clock.
var _render_stats := RootRenderStatsSampler.new()
var _mp_companion  # MpMenuCompanion: drives the multiplayer (mp.mnu) menu by control name
var _lan_session  # NovaLanSession: retail-style 0x41/0x81 LAN enumeration browser
var _player_info_companion  # PlayerInfoMenuCompanion: drives the PLAYER_INFO (player.mnu) character screen
var _armory_presenter: NovaArmoryPresenter  # the SHARED in-world armory surface (weapon.mnu WEAPON)
var _deploy_presenter: NovaDeployScreenPresenter  # the joiner's deploy-map screen (death.mnu DEATH)
var _use_latched := false  # USE-ITEM press latch; the mount toggle runs on RELEASE
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


func _init() -> void:
	# The sampler observes the board's capture close edge directly (render-time
	# measurement is RenderingServer state, not Node-owned state).
	_render_stats.setup(_frame_stats)


func _notification(what: int) -> void:
	if what == NOTIFICATION_EXIT_TREE:
		_render_stats.stop()


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


## The mounted menu/runtime resource root (null before the first mount) —
## so shell components (NetSessionController) and lifecycle tests resolve
## missions/titles through one seam instead of shell internals.
func current_resource_root() -> NovaResourceRoot:
	return _root


## Enter the world DIRECTLY (no menu, no loading screen): the replay-spectate
## entry's shell half — reveal the world + HUD, enter WORLD state, and wire the
## load-result signals. NetSessionController drives the actual net-session load.
func enter_net_world() -> void:
	_menu_shell.hide_menu()
	_world.visible = true
	_set_hud_visible(true)
	_state = State.WORLD
	if not _world.world_loaded.is_connected(_on_world_loaded):
		_world.world_loaded.connect(_on_world_loaded)
	if not _world.load_failed.is_connected(_on_world_load_failed):
		_world.load_failed.connect(_on_world_load_failed)


## Roll a rejected net-session entry back to the front-end. The world's null
## legs emit load_failed before load_net_session returns, so the synchronous
## rollback has usually already run — this is the deterministic backstop for
## any error leg that returns without emitting, idempotent via the same guard
## as _on_session_lost. (That re-entrant rollback is safe only because
## load_net_session emits load_failed strictly BEFORE constructing children;
## an emit added mid-construction would tear down live construction.)
func abort_net_session(reason: String) -> void:
	if _state == State.MENU and not _world_load_pending:
		return
	_abort_to_menu("net session entry failed", reason)


func _ready() -> void:
	if _world == null or _camera == null or _menu_shell == null:
		return
	var debug_adapter := get_game_debug_adapter()
	add_child(debug_adapter)
	debug_adapter.start_runtime_endpoint()
	# Esc toggles pause/resume in a world (the fly camera reports the key; the
	# owner decides what it means).
	if _camera.has_signal("escape_pressed") and not _camera.is_connected("escape_pressed", _on_camera_escape):
		_camera.connect("escape_pressed", _on_camera_escape)
	_player_presenter = LocalPlayerPresenterScript.new()
	_player_presenter.name = "LocalPlayerPresenter"
	add_child(_player_presenter)
	_player_presenter.setup(_world, _camera)
	# The in-world armory + HUD ride their shared engine presenters. Created here,
	# not in _wire_shell, so the NW_REPLAY
	# spectator path (which never enters the menu) still gets them; the HUD presenter's
	# setup connects mission_effects before any world can tick (PreMission/WAC
	# effects may drain on the first runtime tick, and it queues them until the
	# lazy HUD exists).
	_armory_presenter = NovaArmoryPresenter.new()
	_armory_presenter.name = "ArmoryPresenter"
	add_child(_armory_presenter)
	_armory_presenter.setup(_world, _player_presenter, _hud if _hud != null else self)
	_armory_presenter.opened.connect(func() -> void: _state = State.ARMORY)
	_armory_presenter.closed.connect(_on_resume)
	# The joiner's deploy-map screen (death.mnu DEATH): opened when the join reaches
	# the player-paced deployment pick, self-closing on the deployment release
	# [orig: the 0x0A flags1 bit1 hold chain; net-re 5.61].
	_deploy_presenter = NovaDeployScreenPresenter.new()
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
	_hud_presenter = NovaGameHudPresenter.new()
	_hud_presenter.name = "GameHudPresenter"
	add_child(_hud_presenter)
	_hud_presenter.setup(_world, _player_presenter, _hud if _hud != null else self)
	# Every net-session ENTRY (LAN browser/host, NovaWorld panel, replay + env hooks)
	# lives on the NetSessionController component; the shell keeps the state
	# machine, the load pipeline, and the session-presentation states.
	_net = NetSessionController.new()
	_net.name = "NetSessionController"
	add_child(_net)
	_net.setup(self, _world, _menu_shell, _camera,
			_hud if _hud != null else self, $MenuLayer)
	# One shared frame-stats board across the shell, the world, and the HUD
	# presenter; the world re-hands it to each mission runtime it creates.
	_world.set_frame_stats_board(_frame_stats)
	_hud_presenter.set_frame_stats_board(_frame_stats)
	# The shell's own round-outcome tap (the HUD presenter keeps its separate connection
	# for text/banner presentation): "round_end" starts the end-of-mission flow.
	if not _world.mission_effects.is_connected(_on_shell_mission_effects):
		_world.mission_effects.connect(_on_shell_mission_effects)
	if _net.maybe_launch_replay_from_env():
		return
	# A rejected replay boot ran the world->menu rollback re-entrantly above; its
	# rootless leg raises the folder picker (GUI runs). The normal boot below owns
	# the front-end from here — it mounts the configured dir or re-raises the
	# picker itself — so dismiss the stale one.
	_cleanup_picker()
	# Editor-managed runs pass an exact process-local directory. It wins over
	# persisted settings but is never written back.
	var dir := NovaLaunchFlags.resource_dir(ResourceDirSettings.get_resource_dir())
	if dir.is_empty():
		_request_resource_dir()
		return
	if not _enter_menu(dir):
		# The picker is up and the shell holds no root: none of the boot
		# continuations below could load anything.
		return
	# F6 is still the real standalone game and normal loading presentation; it
	# only selects the exact saved top-level loose BMS instead of an archive row.
	var loose_mission := NovaLaunchFlags.loose_mission()
	if not loose_mission.is_empty():
		start_loose_mission(loose_mission)
		return
	# Dev/headless convenience: NW_SP_MISSION=<name.bms> boots straight into a single-player
	# mission via the same path as the menu's Start button, so the runtime (and its HUD) can be
	# exercised without menu navigation. Off by default; mirrors the NW_REPLAY direct-launch above.
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
	if key.keycode == PICK_KEY and is_gameplay_input_active() \
			and _world != null and _world.is_loaded():
		pick_at_crosshair()
		get_viewport().set_input_as_handled()
		return
	# The MISSION OBJECTIVES panel toggle, in-world only.
	# [orig: the co-op action toggle @0x49b68b -> HUD_DrawWinConditions @0x5be163]
	if key.keycode == OBJECTIVES_KEY and is_gameplay_input_active() and _hud_presenter != null:
		_hud_presenter.toggle_objectives()
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
				NovaDebugOverlay.DEFAULT_CONFIG_PATH,
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


## F6 (and the probe/test seam): pick whatever the crosshair is on into the
## debug pick list, with a brief on-screen confirmation.
func pick_at_crosshair() -> void:
	var sim = _world.get_sim() if _world != null else null
	var pick := DebugEntityPicker.pick_at_crosshair(sim, _camera)
	if pick.is_empty():
		return
	if not bool(pick.get("hit", false)):
		var blocked := String(pick.get("blocked", ""))
		if blocked.is_empty():
			_show_pick_toast("No entity in range.")
		else:
			_show_pick_toast("No entity (%s, %.0fu)." % [
					blocked, float(pick.get("distance_units", 0.0))])
		return
	var row := _pick_list.add(pick)
	if row < 0:
		_show_pick_toast("Pick list full (%d) — remove one on the F3 Entities page." %
				NovaDebugPickList.MAX_PICKS)
		return
	var pick_name := String(pick.get("name", ""))
	if pick_name.is_empty():
		pick_name = String(pick.get("hit_class", "entity"))
	_show_pick_toast("Picked: %s #%d  (%.0fu)" % [
			pick_name, int(pick.get("bms_id", 0)),
			float(pick.get("distance_units", 0.0))])


func _show_pick_toast(text: String) -> void:
	if _pick_toast != null and is_instance_valid(_pick_toast):
		_pick_toast.queue_free()
	var label := Label.new()
	label.name = "PickToast"
	label.text = text
	label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	label.set_anchors_preset(Control.PRESET_CENTER_TOP)
	label.offset_top = 96.0
	label.mouse_filter = Control.MOUSE_FILTER_IGNORE
	var mount: Node = _hud if _hud != null else self
	mount.add_child(label)
	_pick_toast = label
	var tween := label.create_tween()
	tween.tween_interval(PICK_TOAST_SECONDS)
	tween.tween_property(label, "modulate:a", 0.0, 0.4)
	tween.tween_callback(label.queue_free)


func get_debug_overlay() -> NovaDebugOverlay:
	return _debug_overlay if _debug_overlay != null \
			and is_instance_valid(_debug_overlay) else null
func get_debug_session() -> NovaDebugSession:
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
# shared NovaGameHudPresenter (queued until the lazy HUD exists); these stay callable
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
		var root := mount_boot_root(dir, NovaLaunchFlags.loose_root_allowed())
		if root == null:
			_request_resource_dir()
			return false
		_root = root
	# The menu, loading screen, and world are one runtime resource session.
	# GameWorld must not remount from mutable persisted settings after boot.
	_world.set_resource_root(_root)
	_state = State.MENU
	_world.visible = false
	_set_hud_visible(false)
	_wire_shell()
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
	if _menu_shell.has_signal("novaworld_requested"):
		_menu_shell.novaworld_requested.connect(_net.open_novaworld_panel)
	if _menu_shell.has_signal("crosshair_style_changed"):
		_menu_shell.crosshair_style_changed.connect(_on_crosshair_style_changed)
	# The multiplayer menu (mp.mnu) and the PLAYER_INFO character screen (player.mnu) are
	# each driven by a companion the shell delegates to (whichever owns the loaded menu).
	_mp_companion = MpMenuCompanion.new()
	_player_info_companion = PlayerInfoMenuCompanion.new()
	if ClassDB.class_exists("NovaLanSession"):
		_lan_session = ClassDB.instantiate("NovaLanSession")
		_lan_session.name = "LanSession"
		add_child(_lan_session)
		_mp_companion.set_lan_session(_lan_session)
	else:
		push_warning("MainGame: NovaLanSession is unavailable; LAN browsing is disabled")
	_menu_shell.add_companion(_mp_companion)
	_menu_shell.add_companion(_player_info_companion)
	_net.wire_menu_companions(_mp_companion)
	_player_info_companion.avatar_chosen.connect(_on_avatar_chosen)


# Install the in-memory local-player profile used by the next mission spawn. This
# public seam keeps lifecycle tests and future persistence adapters out of shell
# internals.
func set_local_player_profile(profile: Dictionary) -> void:
	_chosen_avatar = profile.duplicate(true)


# The player pressed OK on the PLAYER_INFO screen. The in-world soldier appearance
# remains a later phase; the selected loadout travels through the existing
# spawn-kit seam, and the PLAYERNAME field persists as the callsign every session
# leg rides (ClientAuth.NA) — without this the profile default could never be
# changed in-product and two GUI instances on one machine would collide into the
# duplicate-callsign fail-fast.
func _on_avatar_chosen(profile: Dictionary) -> void:
	set_local_player_profile(profile)
	var typed_name := String(profile.get("name", "")).strip_edges()
	if not typed_name.is_empty():
		NovaPlayerProfile.save_callsign(typed_name)


func _on_crosshair_style_changed(style: int) -> void:
	if _hud_presenter != null:
		_hud_presenter.set_crosshair_style(style)


# The armory key while in-world: the shared NovaArmoryPresenter opens weapon.mnu's
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
	apply_picked_resource_dir(dir, not NovaLaunchFlags.resource_dir().is_empty())


## The picker's accept leg. `editor_managed` is resolved from --resource-dir at
## the signal callback above: an editor-managed run's directory is process-local,
## so persisting a picker escape would overwrite the SHARED editor+game key and
## repoint ONED's authoring root at whatever was picked here. Parameterized for
## the same ADR-0018 reason as mount_boot_root; returns false when the pick
## would not mount (the picker is re-raised).
func apply_picked_resource_dir(dir: String, editor_managed: bool) -> bool:
	var root := mount_boot_root(dir, NovaLaunchFlags.loose_root_allowed())
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
func mount_boot_root(dir: String, allow_loose_root: bool) -> NovaResourceRoot:
	var root := NovaResourceRoot.new()
	var expansion := NovaLaunchFlags.expansion(ResourceDirSettings.get_expansion())
	var game := NovaLaunchFlags.game(ResourceDirSettings.get_game())
	var err: int = root.mount_runtime(
			dir, expansion, NovaLaunchFlags.loose_override_enabled(), game)
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
func _report_missing_boot_resources(root: NovaResourceRoot) -> void:
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
	get_tree().quit()


## Public delegate for "join this server" — kept on the shell so lifecycle tests
## and external drivers keep one ADR-0018 entry; the controller owns the path.
func join_lan_server(target: JoinTarget) -> void:
	_net.join_lan_server(target)




## THE load seam every mission start (the menu's SP start and every one of
## NetSessionController's LAN/NovaWorld/env entries) routes through: hide the
## menu, raise the loading screen, enter WORLD
# state, and connect the load-result signals. The caller then starts the specific
# load. The world + HUD stay hidden until the load lands — during the load only
# the loading screen presents [orig: Game_StartMission renders via
# render_loading_screen @ 0x521d10 / LoadingScreen_UpdateAndPresent @ 0x586be0
# until LoadingScreen_ReleaseEffect @ 0x525d52 at the end of the load].
# `load_info` feeds the screen: mission_file, and for a net session the session
# variables (in_session, server_name, mission_name, game_type, custom_text)
# [orig: the SERVERNAME/MISSIONNAME/GAMETYPE/CUSTOMTEXT session vars @ 0x5202f0].
func start_world_load(load_info: Dictionary, operation: Callable) -> void:
	if _world_load_pending:
		return
	if _lan_session != null and _lan_session.has_method("stop"):
		_lan_session.stop()
	_world_load_pending = true
	_world_load_request_id += 1
	var request_id := _world_load_request_id
	_world.set_local_player_spawn_loadout(_chosen_avatar)
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
	_menu_shell.hide_menu()
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
	if not _world.join_session_identified.is_connected(_on_join_session_identified):
		_world.join_session_identified.connect(_on_join_session_identified)


# The joiner's 0x7B session record resolved mid-load: refresh the screen's
# session text and sidecar background the way retail's connect stream fills
# the same buffers before its local load [orig: parse_server_session_variables
# @ 0x5202f0 -> the loading-screen title/mission bufs @ 0x51f533/0x51f53a].
func _on_join_session_identified(info: Dictionary) -> void:
	if _loading_screen != null and _world_load_pending:
		_loading_screen.update_session_info(_root, info)


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



func _on_world_loaded() -> void:
	# The GAME music context is the world's to open at mission start (GameWorld
	# calls NovaMusicService.open_game_context, so every live mission entry path
	# gets the same music); nothing to do here for audio. The witnessed release
	# then reveals the world + HUD at the tail
	# of Game_StartMission [orig: LoadingScreen_ReleaseEffect @ 0x586b80, final
	# call @ 0x525d45]. For SP/host this fires at true load completion. For a
	# joiner this is only LOCAL-load completion; keep pumping the hidden runtime
	# under the loading presentation until the separate authoritative edge.
	# The SP start-mission arrow splash (newarow1.tga +
	# START_MISSION), gated !is_multiplayer_session && g_loadscreen_has_custom_bg
	# && !is_in_session, is a follow-up [orig: show_start_mission_splash
	# @ 0x520820, called @ 0x525d42] (docs/interface/loading-screen-re.md
	# D-LOADSCR-4, the load-flow case matrix).
	# A fresh mission gets a fresh pick set (stale handles never cross
	# sessions); the world renders/curates the shell-owned list from here on.
	_pick_list.clear()
	_world.set_pick_debug(_pick_list)
	_on_debug_overlay_visibility_changed()
	var sim := _world.get_sim()
	if sim != null and bool(sim.is_joiner()) \
			and not bool(sim.is_joined_in_match()):
		return
	_finish_world_load_presentation()


func _finish_world_load_presentation() -> void:
	if not _world_load_pending:
		return
	_world_load_pending = false
	_dismiss_loading_screen()
	_world.visible = true
	_set_hud_visible(true)


func _on_join_admission_ready() -> void:
	_finish_world_load_presentation()


func _on_join_deploy_pick_required() -> void:
	# Initial admission transitions from loading to the player-paced DEATH screen;
	# on a later death the presentation is already down and the same screen simply
	# reopens on the new pending edge.
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
	# admission tail, which awaits process_frame every iteration. Aborting either
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


# One idempotent rollback for a normal return and every load failure. Runtime
# presenters keep references to the old world/root, so their teardown order is part
# of the shell boundary rather than a menu-specific detail.
func _teardown_world_to_menu() -> void:
	_dismiss_loading_screen()
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
		_player_presenter.setup(_world, _camera)
	if _net != null:
		_net.on_world_teardown()
	if _hud_presenter != null:
		_hud_presenter.teardown()
	if _root != null and _enter_menu(_root.get_root_dir()):
		return
	# No mountable root to return to: land on the pre-mount front-end state so
	# the picker/F9 contract (MENU-only) holds, with the picker as the only
	# recovery surface.
	_state = State.MENU
	_request_resource_dir()


func _on_exit_to_desktop() -> void:
	get_tree().quit()


# CanvasLayer contents toggle: hide/show the HUD's CanvasItem children (the FPS
# label + any mounted feeds) so they do not draw over the menu.
func _set_hud_visible(v: bool) -> void:
	if _hud == null:
		return
	for c in _hud.get_children():
		if c is CanvasItem:
			(c as CanvasItem).visible = v


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
	if _state == State.PAUSED and not _world.is_net_session():
		return
	var probe_t0 := Time.get_ticks_usec() if timing else 0
	if _player_presenter != null:
		var player_live := is_gameplay_input_active()
		_player_presenter.before_world_tick(delta, player_live, player_live)
	var probe_t1 := Time.get_ticks_usec() if timing else 0
	var skip_world := probe_enabled and _perf_probe_skip_world
	if not skip_world:
		_world.tick(_camera.global_position, _camera.global_transform, delta)
	var probe_t2 := Time.get_ticks_usec() if timing else 0
	if _player_presenter != null:
		_player_presenter.after_world_tick()
	var probe_t3 := Time.get_ticks_usec() if timing else 0
	# The shared HUD presenter rebuilds the per-frame info while the player is in-world
	# (WORLD or the live-play ARMORY) [orig: HUD_BuildEntityInfo @0x4b8440 per frame].
	var skip_hud := probe_enabled and _perf_probe_skip_hud
	if _hud_presenter != null and (_state == State.WORLD or _state == State.ARMORY \
			or _state == State.DEPLOY) and not skip_hud:
		_hud_presenter.tick()
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
