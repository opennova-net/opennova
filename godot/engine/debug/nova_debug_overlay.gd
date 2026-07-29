class_name NovaDebugOverlay
extends CanvasLayer
## The mission debug overlay: a read-only window into the live runtime
## (entities, sim transport, script variables), summonable over ANY
## MissionRuntime host — F3 in the game, mountable over the editor's mission
## preview. Host-neutral on purpose: engine/ui primitives + a duck-typed
## runtime only, no editor-shell imports, so the shipped game carries it.
##
## The overlay is the shell: the panel chrome, the tab host, the refresh
## timer, and the public host surface. Each tab is its own pane script under
## engine/debug/pages/; pane-local toggle signals are re-emitted here so hosts
## keep one connection point.
##
## The runtime is re-resolved through a Callable on EVERY refresh — mission
## reloads free and recreate the MissionRuntime, so a held reference would go
## stale. Refresh runs on a low-Hz timer (paused while hidden), never per
## frame; the entity list reads ONE packed snapshot per refresh and only the
## selected entity pays for the scalar detail card.

## Fired after a Sim-tab transport press (play/pause/step/stop) or the script
## pause toggle acted on the runtime. Hosts whose own UI mirrors the runtime's
## transport state (the editor sim bar) listen and re-read; hosts without one
## (the game's F3 overlay) ignore it.
signal transport_used(action: String)

## Fired when the View tab's "Show skeletons" checkbox is toggled. The overlay is
## host-neutral (no reach into the 3D scene), so it only emits intent; the host that owns
## the world (the game's main_game, the editor's mission workspace) builds/frees the bone
## debug view in response.
signal skeleton_debug_toggled(enabled: bool)

## Fired when the View tab's "Show user points" checkbox is toggled. The host
## builds/frees the world-wide labeled user-point view in response; the overlay
## itself remains independent of the 3D scene.
signal user_points_toggled(enabled: bool)

## Fired when the View tab's "Show collision" checkbox is toggled. Same host-neutral
## contract as skeleton_debug_toggled: the host that owns the world builds/frees the
## collision debug view (object collision volumes + the player capsule) in response.
signal collision_debug_toggled(enabled: bool)

## Fired when the View tab's "Hide foliage" checkbox is toggled. Same host-neutral
## contract as skeleton_debug_toggled: the host hides/shows the world's foliage.
signal foliage_hidden_toggled(hidden: bool)

## Fired when the View tab's "Always draw FP arms" checkbox is toggled. Debug
## experiment: the host keeps the first-person arms viewmodel visible in every
## camera mode instead of first person only.
signal viewmodel_forced_toggled(enabled: bool)

## Fired when the View tab's "Show body in first person" checkbox is toggled. Debug
## experiment (the "see our feet" probe): the host moves the player's third-person
## body onto the world layer even in first person, so looking down shows your own
## torso/legs/feet posed by the aim overlay (world-wac-ai-re.md §14).
signal body_in_first_person_toggled(enabled: bool)

## Fired after the Player tab writes a fresh local-player pose snapshot. The
## path is absolute so it can be pasted into an issue or opened directly.
signal local_player_pose_dumped(path: String)

## Fired when the Particles tab's "Hide particles" checkbox is toggled — the
## retail master particle switch, mimicked [orig: byte_24D261D — every effect
## facade no-ops when set]. Same host-neutral contract: the host that owns the
## effect world hides/shows it.
signal particles_hidden_toggled(hidden: bool)

## Fired when the Particles tab's "Show effect boxes" checkbox is toggled. The
## host builds/frees the ParticleDebugView (per-emitter wireframe bounds +
## effect-name labels), the collision-view contract.
signal particle_boxes_toggled(enabled: bool)

## Fired when the Occlusion tab's "Show portal faces" checkbox is toggled. The
## host builds/frees the OcclusionDebugView (type-colored portal-face outlines +
## section labels over the world), the collision-view contract.
signal occlusion_debug_toggled(enabled: bool)

## Fired when the Rounds tab's "Show round trails" checkbox is toggled. The
## host builds/frees the RoundDebugView (flight segments + hit markers +
## detail labels over the world), the collision-view contract.
signal round_debug_toggled(enabled: bool)

## Fired when the Rounds tab's "Show hit meshes" checkbox is toggled. The host
## builds/frees the HitboxDebugView (the CFAC bullet-mesh wireframes rounds
## actually test, bound spheres, posed organic bone spheres), the collision-view
## contract.
signal hitbox_debug_toggled(enabled: bool)

const REFRESH_INTERVAL := 0.25
const PANEL_WIDTH := 560.0

var _runtime_source := Callable()
var _timer: Timer
var _tabs: TabContainer
var _status_label: Label

# The tab panes, in tab order (engine/debug/pages/ + the stats/perf panes).
var _entities_pane: DebugEntitiesPage
var _sim_pane: DebugSimPage
var _vars_pane: DebugVarsPage
var _particles_pane: DebugParticlesPage
var _occlusion_pane: DebugOcclusionPage
var _rounds_pane: DebugRoundsPage
var _stats_pane: DebugStatsPane
var _net_pane: DebugNetPage
var _perf_pane: DebugPerfPane
var _player_pane: DebugPlayerPage
var _view_pane: DebugViewPage


func _init() -> void:
	layer = 90
	_build_panel()
	_timer = Timer.new()
	_timer.wait_time = REFRESH_INTERVAL
	_timer.autostart = true
	_timer.timeout.connect(_refresh)
	add_child(_timer)
	visible = false
	_sync_timer()


## The runtime supplier: a Callable returning the current MissionRuntime (or
## null). Re-resolved every refresh because reloads recreate the runtime.
func set_runtime_source(source: Callable) -> void:
	_runtime_source = source
	if visible:
		_refresh()


## Optional supplier for the exact NovaDebugViewContext used to render and
## dispatch foliage. It is sampled with the player pose so a disk snapshot
## reproduces the visual viewpoint, not just the player root.
func set_view_context_source(source: Callable) -> void:
	_player_pane.set_view_context_source(source)


## Convenience for hosts holding one runtime instance directly.
func set_runtime(runtime) -> void:
	var ref: WeakRef = weakref(runtime)
	set_runtime_source(func(): return ref.get_ref())


## The effect-world supplier for the Particles tab: a Callable returning the
## live NovaEffectWorld (or null). Re-resolved every refresh — mission loads
## free and rebuild the effect world.
func set_effect_world_source(source: Callable) -> void:
	_particles_pane.set_effect_world_source(source)
	if visible:
		_refresh()


func toggle() -> void:
	visible = not visible
	_sync_timer()
	# Controls under a hidden CanvasLayer don't observe the layer hide; tell the
	# stats pane explicitly so its capture window closes with the overlay.
	_stats_pane.set_capture_active(visible)
	if visible:
		_refresh()


## The host-owned FrameStatsBoard feeding the Stats tab (null detaches).
func set_frame_stats_board(board) -> void:
	_stats_pane.set_frame_stats_board(board)


## Select a named diagnostic tab without exposing the TabContainer.
func select_tab(tab_name: StringName) -> bool:
	var tab := _tabs.get_node_or_null(NodePath(String(tab_name))) as Control
	if tab == null:
		return false
	_tabs.current_tab = tab.get_index()
	return true


func is_stats_capturing() -> bool:
	return _stats_pane.is_capturing()


func get_stats_display_snapshot() -> Array[DebugStatsDisplayRow]:
	return _stats_pane.get_display_snapshot()


## Supplier of the world host (GameWorld or null) for the Stats tab's counters.
func set_world_source(source: Callable) -> void:
	_stats_pane.set_world_source(source)


## One-way lock on the variable-edit toggle, for hosts that must not let the
## overlay mutate the live sim (the editor summons it over a mission preview).
## `reason` is the caller's artist-facing tooltip copy. Deliberately no
## unlock: a locked overlay stays read-only for its whole life, so a host
## mode change can never silently re-arm edits.
func lock_writes(reason: String) -> void:
	_vars_pane.lock_writes(reason)
	if visible:
		_refresh()


## Sample the live runtime now and write one exact JSON pose snapshot. An
## optional target is useful for automation; the button uses the timestamped
## user-data location. Returns the absolute file path, or an empty string.
func dump_local_player_pose(path_override: String = "") -> String:
	return _player_pane.dump_local_player_pose(path_override)


func refresh_now() -> void:
	_refresh()


func _sync_timer() -> void:
	if _timer != null:
		_timer.paused = not visible


# --- Panel construction --------------------------------------------------

func _build_panel() -> void:
	var panel := PanelContainer.new()
	panel.name = "DebugPanel"
	panel.anchor_left = 1.0
	panel.anchor_right = 1.0
	panel.anchor_bottom = 1.0
	panel.offset_left = -PANEL_WIDTH
	panel.offset_top = 8.0
	panel.offset_right = -8.0
	panel.offset_bottom = -8.0
	panel.grow_horizontal = Control.GROW_DIRECTION_BEGIN
	add_child(panel)

	var box := VBoxContainer.new()
	box.name = "DebugContent"
	box.add_theme_constant_override("separation", 6)
	panel.add_child(box)

	var title := Label.new()
	title.name = "DebugTitle"
	title.text = "Mission debug"
	box.add_child(title)

	_status_label = Label.new()
	_status_label.name = "DebugStatus"
	_status_label.text = "No mission running."
	_status_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	box.add_child(_status_label)

	_tabs = TabContainer.new()
	_tabs.name = "DebugTabs"
	_tabs.size_flags_vertical = Control.SIZE_EXPAND_FILL
	box.add_child(_tabs)

	_entities_pane = DebugEntitiesPage.new()
	_entities_pane.setup(_resolve_live_sim)
	_tabs.add_child(_entities_pane)

	_sim_pane = DebugSimPage.new()
	_sim_pane.setup(_resolve_runtime, _resolve_live_sim, refresh_now)
	_sim_pane.transport_used.connect(
			func(action: String): transport_used.emit(action))
	_tabs.add_child(_sim_pane)

	_vars_pane = DebugVarsPage.new()
	_vars_pane.setup(_resolve_live_sim, refresh_now)
	_tabs.add_child(_vars_pane)

	_particles_pane = DebugParticlesPage.new()
	_particles_pane.particles_hidden_toggled.connect(
			func(hidden: bool): particles_hidden_toggled.emit(hidden))
	_particles_pane.particle_boxes_toggled.connect(
			func(enabled: bool): particle_boxes_toggled.emit(enabled))
	_tabs.add_child(_particles_pane)

	_occlusion_pane = DebugOcclusionPage.new()
	_occlusion_pane.occlusion_debug_toggled.connect(
			func(enabled: bool): occlusion_debug_toggled.emit(enabled))
	_tabs.add_child(_occlusion_pane)

	_rounds_pane = DebugRoundsPage.new()
	_rounds_pane.round_debug_toggled.connect(
			func(enabled: bool): round_debug_toggled.emit(enabled))
	_rounds_pane.hitbox_debug_toggled.connect(
			func(enabled: bool): hitbox_debug_toggled.emit(enabled))
	_tabs.add_child(_rounds_pane)

	_stats_pane = DebugStatsPane.new()
	_stats_pane.name = "Stats"
	_tabs.add_child(_stats_pane)

	_net_pane = DebugNetPage.new()
	_tabs.add_child(_net_pane)

	_perf_pane = DebugPerfPane.new()
	_perf_pane.name = "Perf"
	_tabs.add_child(_perf_pane)

	_player_pane = DebugPlayerPage.new()
	_player_pane.setup(_resolve_runtime, _resolve_live_sim)
	_player_pane.local_player_pose_dumped.connect(
			func(path: String): local_player_pose_dumped.emit(path))
	_tabs.add_child(_player_pane)

	_view_pane = DebugViewPage.new()
	_view_pane.skeleton_debug_toggled.connect(
			func(enabled: bool): skeleton_debug_toggled.emit(enabled))
	_view_pane.user_points_toggled.connect(
			func(enabled: bool): user_points_toggled.emit(enabled))
	_view_pane.collision_debug_toggled.connect(
			func(enabled: bool): collision_debug_toggled.emit(enabled))
	_view_pane.foliage_hidden_toggled.connect(
			func(hidden: bool): foliage_hidden_toggled.emit(hidden))
	_view_pane.viewmodel_forced_toggled.connect(
			func(enabled: bool): viewmodel_forced_toggled.emit(enabled))
	_view_pane.body_in_first_person_toggled.connect(
			func(enabled: bool): body_in_first_person_toggled.emit(enabled))
	_tabs.add_child(_view_pane)


# --- Runtime resolution ----------------------------------------------------

func _resolve_runtime() -> Object:
	if not _runtime_source.is_valid():
		return null
	var runtime: Variant = _runtime_source.call()
	if runtime == null or not is_instance_valid(runtime):
		return null
	if not (runtime as Object).has_method("get_sim"):
		return null
	return runtime


func _resolve_sim(runtime: Object) -> Object:
	if runtime == null:
		return null
	var sim: Variant = runtime.get_sim()
	if sim == null or not is_instance_valid(sim):
		return null
	return sim


## The pane-facing sim resolver: runtime + sim in one hop, re-resolved on
## every call so a pane handler can never act on a freed sim.
func _resolve_live_sim() -> Object:
	return _resolve_sim(_resolve_runtime())


# --- Refresh ---------------------------------------------------------------

func _refresh() -> void:
	var runtime := _resolve_runtime()
	var sim := _resolve_sim(runtime)
	var live := sim != null
	_status_label.visible = not live
	# The perf pane is fed by HOST-WIDE state (the PerfTimeline ring + live
	# monitors), not the sim — it refreshes regardless, so "that load was slow,
	# let me look" works from the menu after returning from a mission.
	_perf_pane.refresh()
	# The stats pane drains the host-fed FrameStatsBoard window; it self-gates
	# on its own tab visibility and no-ops without a board.
	_stats_pane.refresh(runtime, sim)
	# The particles pane rides its own effect-world source (the effect world is
	# render-side, not the sim) and null-clears itself, so it also refreshes
	# regardless of the sim.
	_particles_pane.refresh()
	# The occlusion pane reads the sim's frame state and clears itself when the
	# sim (or its occlusion surface) is gone — harness sims without the debug
	# accessor just show the empty state.
	_occlusion_pane.refresh(sim)
	# The rounds pane reads the RoundSim debug ring the same way.
	_rounds_pane.refresh(sim)
	# The net pane reads the sim's public net accessors and self-clears.
	_net_pane.refresh(sim)
	# The other tabs stay usable without a sim too: the panes that need one
	# clear to their empty states (their handlers already null-check).
	if not live:
		_clear_live_panes()
		return
	_entities_pane.refresh(sim)
	_sim_pane.refresh(runtime, sim)
	_vars_pane.refresh(sim)
	_player_pane.refresh(runtime, sim)


func _clear_live_panes() -> void:
	_entities_pane.clear_live()
	_sim_pane.clear_live()
	_vars_pane.clear_live()
	_player_pane.clear_live()
