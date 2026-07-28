class_name NovaDebugOverlay
extends CanvasLayer
## The mission debug overlay: a read-only window into the live runtime
## (entities, sim transport, script variables), summonable over ANY
## MissionRuntime host — F3 in the game, mountable over the editor's mission
## preview. Host-neutral on purpose: engine/ui primitives + a duck-typed
## runtime only, no editor-shell imports, so the shipped game carries it.
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
const PLAYER_POSE_DUMP_DIR := "user://debug/player_locations"
const PLAYER_POSE_SCHEMA := "opennova.player_pose.v1"
const DEFAULT_PLAYER_FOV_H_DEG := 80.0
const MICROSECONDS_PER_SECOND := 1_000_000.0
const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")
const DebugViewContext := preload("res://engine/debug/nova_debug_view_context.gd")

var _runtime_source := Callable()
var _view_context_source := Callable()
var _timer: Timer
var _tabs: TabContainer

# Entities pane
var _entity_list: ItemList
var _entity_detail: Label
var _selected_entity := -1

# Sim pane
var _play_button: Button
var _pause_button: Button
var _step_button: Button
var _stop_button: Button
var _tick_label: Label
var _entities_label: Label
var _events_label: Label
var _wac_label: Label
var _wac_pause_check: CheckBox

# Vars pane
var _nonzero_check: CheckBox
var _writes_check: CheckButton
var _writes_locked := false
var _vars_rows: VBoxContainer
# (bank, index) key -> the row's value Control, so steady-state refreshes
# update text in place instead of rebuilding ~800 rows.
var _var_controls: Dictionary = {}
var _var_rows_signature := ""

var _status_label: Label

# Perf pane (C11): the PerfTimeline ring + live monitors.
var _perf_pane: DebugPerfPane

# Stats pane: the per-system frame stats surface over the host-fed
# FrameStatsBoard (window-averaged ms per system + live counters). The board
# arrives from the host via set_frame_stats_board; hosts without one (editor
# preview) leave the tab in its empty state.
var _stats_pane: DebugStatsPane

# View pane: render-debug toggles the host acts on (skeleton bone overlay, foliage, ...).
var _skeleton_check: CheckBox
var _user_points_check: CheckBox
var _collision_check: CheckBox
var _foliage_check: CheckBox
var _viewmodel_check: CheckBox
var _body_fp_check: CheckBox

# Particles pane: the retail particle debug pages, mimicked (ptl-format-re.md
# §11 — Debug_DrawParticleStats @ 0x44c840 counts + entry list;
# Debug_DrawEffectBrowser @ 0x44c950 name + source file). Fed by its own
# effect-world source; the peak latch lives here like retail's debug global.
var _effect_world_source := Callable()
var _ptl_count_label: Label
var _ptl_list: ItemList
var _ptl_hide_check: CheckBox
var _ptl_boxes_check: CheckBox
var _ptl_peak := 0

# Occlusion pane: the render-occlusion frame state read off the sim each
# refresh (docs/render/render-occlusion-re.md §3/§5) — the camera's
# indoor/portal view, the per-building section masks, the weld links — plus
# the 3D portal-face view toggle. Developer window into our port, not a
# mimicked retail debug page.
var _occ_status_label: Label
var _occ_list: ItemList
var _occ_portals_check: CheckBox

# Rounds pane: the RoundSim debug ring (libs/world round_sim.cpp) — every
# recently resolved hit-test outcome including the face-miss fly-ons, read off
# NovaSimulation.get_round_debug() each refresh. Developer window into both
# retail narrow phases (item CFAC faces and person bone spheres), not a
# mimicked retail page.
var _rnd_status_label: Label
var _rnd_list: ItemList
var _rnd_trails_check: CheckBox
var _rnd_hitbox_check: CheckBox

# Net pane: the sim's net-session state — every session is a listen server
# (ADR 0011), so this shows which ROLE this sim runs (SP local loopback, LAN
# host with a bound socket + peers, or joiner with its connect phase), read
# off the sim's public net accessors each refresh. Read-only, no toggles.
var _net_status_label: Label
var _net_list: ItemList
var _net_rows_signature := ""
# npruntime JoinerConnection::Phase, surfaced by NovaSimulation.get_joiner_phase().
const JOINER_PHASE_NAMES := ["Idle", "Hello", "Auth", "Driving", "InMatch", "Error"]


# Player pane: the authoritative local-player pose plus a one-click disk dump.
var _player_mission_label: Label
var _player_position_label: Label
var _player_orientation_label: Label
var _player_dump_button: Button
var _player_dump_status: Label
var _player_dump_sequence := 0
var _player_context_key := ""

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
	_view_context_source = source


## Convenience for hosts holding one runtime instance directly.
func set_runtime(runtime) -> void:
	var ref: WeakRef = weakref(runtime)
	set_runtime_source(func(): return ref.get_ref())


## The effect-world supplier for the Particles tab: a Callable returning the
## live NovaEffectWorld (or null). Re-resolved every refresh — mission loads
## free and rebuild the effect world.
func set_effect_world_source(source: Callable) -> void:
	_effect_world_source = source
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
	_writes_locked = true
	_writes_check.set_pressed_no_signal(false)
	_writes_check.disabled = true
	if not reason.is_empty():
		_writes_check.tooltip_text = reason
	if visible:
		_refresh()


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

	_build_entities_tab()
	_build_sim_tab()
	_build_vars_tab()
	_build_particles_tab()
	_build_occlusion_tab()
	_build_rounds_tab()
	_build_stats_tab()
	_build_net_tab()
	_build_perf_tab()
	_build_player_tab()
	_build_view_tab()


func _build_perf_tab() -> void:
	_perf_pane = DebugPerfPane.new()
	_perf_pane.name = "Perf"
	_tabs.add_child(_perf_pane)


func _build_stats_tab() -> void:
	_stats_pane = DebugStatsPane.new()
	_stats_pane.name = "Stats"
	_tabs.add_child(_stats_pane)

func _build_player_tab() -> void:
	var tab := VBoxContainer.new()
	tab.name = "Player"
	tab.add_theme_constant_override("separation", 8)
	_tabs.add_child(tab)

	_player_mission_label = _info_label(tab, "PlayerMission")
	_player_mission_label.text = "Mission: --"

	_player_position_label = _info_label(tab, "PlayerPosition")
	_player_position_label.text = "No local player."

	_player_orientation_label = _info_label(tab, "PlayerOrientation")
	_player_orientation_label.text = ""

	_player_dump_button = Button.new()
	_player_dump_button.name = "DumpPlayerPose"
	_player_dump_button.text = "Dump pose to disk"
	_player_dump_button.tooltip_text = \
			"Write a fresh local-player position/orientation snapshot as JSON."
	_player_dump_button.disabled = true
	_player_dump_button.pressed.connect(_on_dump_player_pose_pressed)
	tab.add_child(_player_dump_button)

	_player_dump_status = Label.new()
	_player_dump_status.name = "PlayerDumpStatus"
	_player_dump_status.text = "Snapshots are written under the OpenNova user-data folder."
	_player_dump_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	tab.add_child(_player_dump_status)


# The retail particle debug pages, mimicked (ptl-format-re.md §11): the counts
# header keeps retail's current/peak form INCLUDING the peak reset when the
# current count hits zero [orig: Debug_DrawParticleStats @ 0x44c840]; the list
# shows each live group "%02d   name" with indented per-emitter rows [orig:
# the "%02ld   %s" / "      %s" pair], plus our diagnostics: the source .ptl
# (the browser's "File:" row [orig: Debug_DrawEffectBrowser @ 0x44c950]) and
# a catalog-level list of authored-but-unresolved texture names.
func _build_particles_tab() -> void:
	var tab := VBoxContainer.new()
	tab.name = "Particles"
	tab.add_theme_constant_override("separation", 6)
	_tabs.add_child(tab)

	_ptl_count_label = Label.new()
	_ptl_count_label.name = "ParticleCounts"
	_ptl_count_label.text = "Current Particle Count:  0 / 0"
	tab.add_child(_ptl_count_label)

	_ptl_list = ItemList.new()
	_ptl_list.name = "ParticleGroups"
	_ptl_list.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_ptl_list.focus_mode = Control.FOCUS_NONE
	tab.add_child(_ptl_list)

	_ptl_hide_check = CheckBox.new()
	_ptl_hide_check.name = "ParticlesHide"
	_ptl_hide_check.text = "Hide particles"
	_ptl_hide_check.tooltip_text = "Hide every particle effect (the retail master particle switch) — flip it to check whether an artifact is particles at all."
	_ptl_hide_check.button_pressed = false
	_ptl_hide_check.toggled.connect(_on_particles_hidden_toggled)
	tab.add_child(_ptl_hide_check)

	_ptl_boxes_check = CheckBox.new()
	_ptl_boxes_check.name = "ParticlesBoxes"
	_ptl_boxes_check.text = "Show effect boxes"
	_ptl_boxes_check.tooltip_text = "Draw a red wireframe box (retail's debug box color) + effect name over every live emitter; effects with missing textures list them on the label."
	_ptl_boxes_check.button_pressed = false
	_ptl_boxes_check.toggled.connect(_on_particle_boxes_toggled)
	tab.add_child(_ptl_boxes_check)


# The render-occlusion inspector: what the portal/section-mask engine decided
# this frame — the camera's blink state, the frame-wide latches, the batch
# split, and the per-building section masks — plus the "Show portal faces"
# world-view toggle. Row states name the culling stage: "not batched"
# (distance/frustum), "occluder-culled" (render_TOC), "drawn".
func _build_occlusion_tab() -> void:
	var tab := VBoxContainer.new()
	tab.name = "Occlusion"
	tab.add_theme_constant_override("separation", 6)
	_tabs.add_child(tab)

	_occ_status_label = Label.new()
	_occ_status_label.name = "OcclusionStatus"
	_occ_status_label.text = "No occlusion data."
	_occ_status_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	tab.add_child(_occ_status_label)

	_occ_list = ItemList.new()
	_occ_list.name = "OcclusionBuildings"
	_occ_list.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_occ_list.focus_mode = Control.FOCUS_NONE
	tab.add_child(_occ_list)

	_occ_portals_check = CheckBox.new()
	_occ_portals_check.name = "OcclusionShowPortals"
	_occ_portals_check.text = "Show portal faces"
	_occ_portals_check.tooltip_text = "Draw every nearby building's occlusion faces over the world — windows, portals and welded links as colored outlines with section labels, plain occluder faces in gray."
	_occ_portals_check.button_pressed = false
	_occ_portals_check.toggled.connect(_on_occlusion_debug_toggled)
	tab.add_child(_occ_portals_check)


# The round hit-test inspector: the RoundSim debug ring, newest first — what
# each recently resolved round actually did (which entity, which COBJ
# section/face or reaction-bone/damage-zone pair, which material -> impact tag,
# husk state), with the face-miss fly-ons called out. The "Show round trails"
# toggle draws the same ring over the world (RoundDebugView, the
# collision-view contract).
func _build_rounds_tab() -> void:
	var tab := VBoxContainer.new()
	tab.name = "Rounds"
	tab.add_theme_constant_override("separation", 6)
	_tabs.add_child(tab)

	_rnd_status_label = Label.new()
	_rnd_status_label.name = "RoundsStatus"
	_rnd_status_label.text = "No rounds resolved yet."
	_rnd_status_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	tab.add_child(_rnd_status_label)

	_rnd_list = ItemList.new()
	_rnd_list.name = "RoundEvents"
	_rnd_list.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_rnd_list.focus_mode = Control.FOCUS_NONE
	tab.add_child(_rnd_list)

	_rnd_trails_check = CheckBox.new()
	_rnd_trails_check.name = "RoundsShowTrails"
	_rnd_trails_check.text = "Show round trails"
	_rnd_trails_check.tooltip_text = "Draw the recent round outcomes over the world — flight segments and hit markers colored by result (green = face hit, amber = sphere stand-in, red ring = a graze whose face test missed and flew on)."
	_rnd_trails_check.button_pressed = false
	_rnd_trails_check.toggled.connect(_on_round_debug_toggled)
	tab.add_child(_rnd_trails_check)

	_rnd_hitbox_check = CheckBox.new()
	_rnd_hitbox_check.name = "RoundsShowHitMeshes"
	_rnd_hitbox_check.text = "Show hit meshes"
	_rnd_hitbox_check.tooltip_text = "Hit geometry is sampled at 6 Hz. Draw nearby hit geometry within 80 mission units of the local player: object bullet meshes and broad-phase spheres, plus posed person bone spheres (local player omitted; up to 96 targets). Person colors show normal-infantry damage zones: orange = x1.25 (0-4), cyan = x1.0 (5-8), lime = x0.5 (9-12/15-18), magenta = x3.0 head (13-14), dark red = masked, amber = unresolved fallback."
	_rnd_hitbox_check.button_pressed = false
	_rnd_hitbox_check.toggled.connect(_on_hitbox_debug_toggled)
	tab.add_child(_rnd_hitbox_check)


# The net-session inspector: which net role this sim runs (every session is a
# listen server — ADR 0011), the host's session row + bound port + peer count,
# and the joiner's connect phase / self wire handle / error. Read-only.
func _build_net_tab() -> void:
	var tab := VBoxContainer.new()
	tab.name = "Net"
	tab.add_theme_constant_override("separation", 6)
	_tabs.add_child(tab)

	_net_status_label = Label.new()
	_net_status_label.name = "NetStatus"
	_net_status_label.text = "No net session."
	_net_status_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	tab.add_child(_net_status_label)

	_net_list = ItemList.new()
	_net_list.name = "NetDetails"
	_net_list.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_net_list.focus_mode = Control.FOCUS_NONE
	tab.add_child(_net_list)


# Render-debug toggles. Unlike the other tabs these don't read the sim: the checkbox holds
# its own state and the host acts on the emitted signal, so it's left out of _refresh.
func _build_view_tab() -> void:
	var tab := VBoxContainer.new()
	tab.name = "View"
	tab.add_theme_constant_override("separation", 6)
	_tabs.add_child(tab)

	_skeleton_check = CheckBox.new()
	_skeleton_check.name = "ViewSkeletons"
	_skeleton_check.text = "Show skeletons"
	_skeleton_check.tooltip_text = "Draw character bones (joint-to-parent lines + axis crosses) over the world."
	_skeleton_check.button_pressed = false
	_skeleton_check.toggled.connect(_on_skeleton_toggled)
	tab.add_child(_skeleton_check)

	_user_points_check = CheckBox.new()
	_user_points_check.name = "ViewUserPoints"
	_user_points_check.text = "Show user points"
	_user_points_check.tooltip_text = "Draw every named model user point as a cyan marker + label, following live animated bones and including static-batched mission objects."
	_user_points_check.button_pressed = false
	_user_points_check.toggled.connect(_on_user_points_toggled)
	tab.add_child(_user_points_check)

	_collision_check = CheckBox.new()
	_collision_check.name = "ViewCollision"
	_collision_check.text = "Show collision"
	_collision_check.tooltip_text = "Draw object collision volumes (type-colored boxes) and the player's capsule test points over the world."
	_collision_check.button_pressed = false
	_collision_check.toggled.connect(_on_collision_toggled)
	tab.add_child(_collision_check)

	_foliage_check = CheckBox.new()
	_foliage_check.name = "ViewHideFoliage"
	_foliage_check.text = "Hide foliage"
	_foliage_check.tooltip_text = "Hide the scattered vegetation (grass / bushes / trees) to see the terrain under it."
	_foliage_check.button_pressed = false
	_foliage_check.toggled.connect(_on_foliage_toggled)
	tab.add_child(_foliage_check)

	_viewmodel_check = CheckBox.new()
	_viewmodel_check.name = "ViewForceFpArms"
	_viewmodel_check.text = "Always draw FP arms"
	_viewmodel_check.tooltip_text = "Keep the first-person arms + weapon drawn in every camera mode (debug experiment)."
	_viewmodel_check.button_pressed = false
	_viewmodel_check.toggled.connect(_on_viewmodel_forced_toggled)
	tab.add_child(_viewmodel_check)

	_body_fp_check = CheckBox.new()
	_body_fp_check.name = "ViewBodyInFirstPerson"
	_body_fp_check.text = "Show body in first person"
	_body_fp_check.tooltip_text = "Draw your own body in first person — look down to see your legs and feet (debug experiment; expect the head/shoulders to clip the camera)."
	_body_fp_check.button_pressed = false
	_body_fp_check.toggled.connect(_on_body_fp_toggled)
	tab.add_child(_body_fp_check)


func _build_entities_tab() -> void:
	var tab := VBoxContainer.new()
	tab.name = "Entities"
	tab.add_theme_constant_override("separation", 6)
	_tabs.add_child(tab)

	_entity_list = ItemList.new()
	_entity_list.name = "EntityList"
	_entity_list.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_entity_list.item_selected.connect(_on_entity_selected)
	tab.add_child(_entity_list)

	_entity_detail = Label.new()
	_entity_detail.name = "EntityDetail"
	_entity_detail.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_entity_detail.text = "Select a unit to see its details."
	tab.add_child(_entity_detail)


func _build_sim_tab() -> void:
	var tab := VBoxContainer.new()
	tab.name = "Sim"
	tab.add_theme_constant_override("separation", 6)
	_tabs.add_child(tab)

	var transport := HBoxContainer.new()
	transport.name = "SimTransport"
	transport.add_theme_constant_override("separation", 4)
	tab.add_child(transport)
	_play_button = _transport_button(transport, "SimPlay", "Play", _on_play_pressed)
	_pause_button = _transport_button(transport, "SimPause", "Pause", _on_pause_pressed)
	_step_button = _transport_button(transport, "SimStep", "Step", _on_step_pressed)
	_stop_button = _transport_button(transport, "SimStop", "Stop", _on_stop_pressed)

	_tick_label = _info_label(tab, "SimTick")
	_entities_label = _info_label(tab, "SimEntities")
	_events_label = _info_label(tab, "SimEvents")
	_wac_label = _info_label(tab, "SimWac")

	_wac_pause_check = CheckBox.new()
	_wac_pause_check.name = "SimWacPause"
	_wac_pause_check.text = "Pause scripts"
	_wac_pause_check.tooltip_text = "Stops the mission's scripts while the world keeps running."
	_wac_pause_check.toggled.connect(_on_wac_pause_toggled)
	tab.add_child(_wac_pause_check)


func _build_vars_tab() -> void:
	var tab := VBoxContainer.new()
	tab.name = "Vars"
	tab.add_theme_constant_override("separation", 6)
	_tabs.add_child(tab)

	_nonzero_check = CheckBox.new()
	_nonzero_check.name = "VarsNonzero"
	_nonzero_check.text = "Show changed values only"
	_nonzero_check.button_pressed = true
	_nonzero_check.toggled.connect(_on_vars_filter_toggled)
	tab.add_child(_nonzero_check)

	_writes_check = CheckButton.new()
	_writes_check.name = "VarsAllowWrites"
	_writes_check.text = "Allow edits"
	_writes_check.tooltip_text = "Editing changes the LIVE mission (V values only)."
	_writes_check.button_pressed = false
	_writes_check.toggled.connect(_on_vars_filter_toggled)
	tab.add_child(_writes_check)

	var scroll := ScrollContainer.new()
	scroll.name = "VarsScroll"
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	tab.add_child(scroll)

	_vars_rows = VBoxContainer.new()
	_vars_rows.name = "VarsRows"
	_vars_rows.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(_vars_rows)


func _transport_button(parent: Control, node_name: String, text: String, handler: Callable) -> Button:
	var button := Button.new()
	button.name = node_name
	button.text = text
	button.focus_mode = Control.FOCUS_NONE
	button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	button.pressed.connect(handler)
	parent.add_child(button)
	return button


func _info_label(parent: Control, node_name: String) -> Label:
	var label := Label.new()
	label.name = node_name
	parent.add_child(label)
	return label


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
	_refresh_particles()
	# The occlusion pane reads the sim's frame state and clears itself when the
	# sim (or its occlusion surface) is gone — harness sims without the debug
	# accessor just show the empty state.
	_refresh_occlusion(sim)
	# The rounds pane reads the RoundSim debug ring the same way.
	_refresh_rounds(sim)
	# The net pane reads the sim's public net accessors and self-clears.
	_refresh_net(sim)
	# The other tabs stay usable without a sim too: the panes that need one
	# clear to their empty states (their handlers already null-check).
	if not live:
		_clear_live_panes()
		return
	_refresh_entities(sim)
	_refresh_sim(runtime, sim)
	_refresh_vars(sim)
	_refresh_player(runtime, sim)


func _clear_live_panes() -> void:
	if _entity_list.item_count > 0:
		_entity_list.clear()
	_selected_entity = -1
	_entity_detail.text = "Select a unit to see its details."
	_tick_label.text = ""
	_entities_label.text = ""
	_events_label.text = ""
	_wac_label.text = ""
	_clear_player_pane()
	if _var_rows_signature != "":
		_var_rows_signature = ""
		_var_controls.clear()
		for child in _vars_rows.get_children():
			_vars_rows.remove_child(child)
			child.queue_free()


func _clear_player_pane() -> void:
	_player_context_key = ""
	_player_mission_label.text = "Mission: --"
	_player_position_label.text = "No local player."
	_player_orientation_label.text = ""
	_player_dump_button.disabled = true
	_player_dump_status.text = "Start a playable mission to capture the local player."


func _refresh_player(runtime: Object, sim: Object) -> void:
	var snapshot := _capture_local_player_pose(runtime, sim)
	if snapshot.is_empty():
		_clear_player_pane()
		return
	_sync_player_context(runtime, snapshot)
	_apply_player_pose_to_ui(snapshot)
	_player_dump_button.disabled = false


func _sync_player_context(runtime: Object, snapshot: Dictionary) -> void:
	var mission: Dictionary = snapshot.get("mission", {})
	var context_key := "%d|%s|%s" % [
		runtime.get_instance_id(),
		String(mission.get("file", "")),
		String(mission.get("name", "")),
	]
	if context_key == _player_context_key:
		return
	_player_context_key = context_key
	_player_dump_status.text = "No pose snapshot saved for this mission yet."


func _apply_player_pose_to_ui(snapshot: Dictionary) -> void:
	var mission: Dictionary = snapshot.get("mission", {})
	var mission_file := String(mission.get("file", ""))
	var mission_name := String(mission.get("name", ""))
	var mission_display := mission_file.get_file()
	if mission_display.is_empty():
		mission_display = mission_name
	if mission_display.is_empty():
		mission_display = "unknown"
	_player_mission_label.text = "Mission: %s" % mission_display

	var player: Dictionary = snapshot.get("player", {})
	var bms: Dictionary = player.get("position_bms", {})
	var godot: Dictionary = player.get("position_godot", {})
	_player_position_label.text = \
			"Position (BMS)\n  x %.3f   y %.3f   z %.3f\nPosition (Godot world)\n  x %.3f   y %.3f   z %.3f" % [
				float(bms.get("x", 0.0)), float(bms.get("y", 0.0)),
				float(bms.get("z", 0.0)), float(godot.get("x", 0.0)),
				float(godot.get("y", 0.0)), float(godot.get("z", 0.0)),
			]

	var orientation: Dictionary = player.get("orientation_mission_deg", {})
	_player_orientation_label.text = \
			"Orientation (mission degrees)\n  yaw %.3f   pitch %.3f\n  view roll %.3f" % [
				float(orientation.get("yaw", 0.0)),
				float(orientation.get("pitch", 0.0)),
				float(orientation.get("view_roll", 0.0)),
			]
	var view: Dictionary = snapshot.get("view", {})
	var camera: Dictionary = view.get("camera", {})
	var camera_mode := String(camera.get("mode", ""))
	if not camera_mode.is_empty() and camera_mode != "unknown":
		_player_orientation_label.text += "\nView camera: %s" % camera_mode.replace("_", " ")


func _capture_local_player_pose(runtime: Object, sim: Object) -> Dictionary:
	if runtime == null or sim == null or not sim.has_method("has_local_player") \
			or not bool(sim.has_local_player()):
		return {}
	if not sim.has_method("get_local_player_position") \
			or not sim.has_method("get_local_player_yaw_deg") \
			or not sim.has_method("get_local_player_pitch_deg"):
		return {}

	var position_godot: Vector3 = sim.get_local_player_position()
	var position_bms: Vector3 = MissionObjectPlacer.godot_to_bms_position(position_godot)
	var yaw_deg := float(sim.get_local_player_yaw_deg())
	var pitch_deg := float(sim.get_local_player_pitch_deg())
	var view: Dictionary = sim.get_local_player_view() \
			if sim.has_method("get_local_player_view") else {}
	var view_roll_deg := float(view.get("fp_roll_deg", 0.0))
	var yaw_rad := deg_to_rad(yaw_deg)
	var pitch_rad := deg_to_rad(pitch_deg)
	var forward_godot := Vector3(
			sin(yaw_rad) * cos(pitch_rad),
			sin(pitch_rad),
			-cos(yaw_rad) * cos(pitch_rad))
	var mission_file := String(runtime.get_mission_file()) \
			if runtime.has_method("get_mission_file") else ""
	var mission_name := String(runtime.get_mission_name()) \
			if runtime.has_method("get_mission_name") else ""

	return {
		"schema": PLAYER_POSE_SCHEMA,
		"captured_at_utc": Time.get_datetime_string_from_system(true, false) + "Z",
		"logic_tick": int(sim.get_logic_tick()) if sim.has_method("get_logic_tick") else -1,
		"mission": {
			"file": mission_file,
			"name": mission_name,
		},
		"player": {
			"position_bms": _vector3_record(position_bms),
			"position_godot": _vector3_record(position_godot),
			"orientation_mission_deg": {
				"yaw": yaw_deg,
				"pitch": pitch_deg,
				"view_roll": view_roll_deg,
			},
			"forward_godot": _vector3_record(forward_godot),
		},
		"view": {
			"fov_horizontal_deg": float(view.get(
					"fov_h_deg", DEFAULT_PLAYER_FOV_H_DEG)),
			"scope_engaged": bool(view.get("scope_engaged", false)),
			"mounted": bool(view.get("mounted", false)),
			"camera": _capture_camera_snapshot(),
		},
		"coordinate_conventions": {
			"bms": "Mission coordinates (x, y horizontal; z up)",
			"godot": "Global world coordinates (x, z horizontal; y up)",
			"yaw": "Mission yaw: 0 faces BMS +Y / Godot -Z",
			"pitch": "Positive looks up",
			"camera_rotation": "Godot world-space quaternion",
		},
	}


func _capture_camera_snapshot() -> Dictionary:
	if not _view_context_source.is_valid():
		return {}
	var context_value: Variant = _view_context_source.call()
	if not (context_value is DebugViewContext):
		return {}
	var context := context_value as DebugViewContext
	var camera := context.camera
	if camera == null or not is_instance_valid(camera):
		return {}
	var camera_transform := camera.global_transform
	var camera_basis := camera_transform.basis.orthonormalized()
	var camera_position := camera_transform.origin
	var viewport_size := Vector2.ZERO
	var viewport := camera.get_viewport()
	if viewport != null:
		viewport_size = viewport.get_visible_rect().size
	var viewport_aspect := viewport_size.x / viewport_size.y \
			if viewport_size.y > 0.0 else 0.0
	var mode := "unknown"
	if context.camera_mode_known:
		mode = "third_person" if context.third_person else "first_person"
	return {
		"mode": mode,
		"position_godot": _vector3_record(camera_position),
		"position_bms": _vector3_record(
				MissionObjectPlacer.godot_to_bms_position(camera_position)),
		"orientation_quaternion_godot": _quaternion_record(
				camera_basis.get_rotation_quaternion()),
		"right_godot": _vector3_record(camera_basis.x),
		"up_godot": _vector3_record(camera_basis.y),
		"forward_godot": _vector3_record(-camera_basis.z),
		"godot_fov_deg": camera.fov,
		"projection": int(camera.projection),
		"keep_aspect": int(camera.keep_aspect),
		"viewport_size": _vector2_record(viewport_size),
		"viewport_aspect": viewport_aspect,
		"near": camera.near,
		"far": camera.far,
	}


func _vector2_record(value: Vector2) -> Dictionary:
	return {"x": value.x, "y": value.y}


func _vector3_record(value: Vector3) -> Dictionary:
	return {"x": value.x, "y": value.y, "z": value.z}


func _quaternion_record(value: Quaternion) -> Dictionary:
	return {"x": value.x, "y": value.y, "z": value.z, "w": value.w}


## Sample the live runtime now and write one exact JSON pose snapshot. An
## optional target is useful for automation; the button uses the timestamped
## user-data location. Returns the absolute file path, or an empty string.
func dump_local_player_pose(path_override: String = "") -> String:
	var runtime := _resolve_runtime()
	var sim := _resolve_sim(runtime)
	var snapshot := _capture_local_player_pose(runtime, sim)
	if snapshot.is_empty():
		_clear_player_pane()
		return ""

	_sync_player_context(runtime, snapshot)
	var target_path := path_override
	if target_path.is_empty():
		target_path = _default_player_pose_path(snapshot)
	target_path = ProjectSettings.globalize_path(target_path)
	var directory := target_path.get_base_dir()
	var mkdir_error := DirAccess.make_dir_recursive_absolute(directory)
	if mkdir_error != OK:
		_player_dump_status.text = "Could not create dump folder: %s" % error_string(mkdir_error)
		return ""
	var file := FileAccess.open(target_path, FileAccess.WRITE)
	if file == null:
		_player_dump_status.text = "Could not write pose dump: %s" % \
				error_string(FileAccess.get_open_error())
		return ""
	file.store_string(JSON.stringify(snapshot, "\t") + "\n")
	file.flush()
	var write_error := file.get_error()
	file.close()
	if write_error != OK:
		DirAccess.remove_absolute(target_path)
		_player_dump_status.text = "Could not finish pose dump: %s" % error_string(write_error)
		return ""

	_apply_player_pose_to_ui(snapshot)
	_player_dump_button.disabled = false
	_player_dump_status.text = "Saved:\n%s" % target_path
	local_player_pose_dumped.emit(target_path)
	return target_path


func _default_player_pose_path(snapshot: Dictionary) -> String:
	var mission: Dictionary = snapshot.get("mission", {})
	var mission_stem := String(mission.get("file", "")).get_file().get_basename()
	if mission_stem.is_empty():
		mission_stem = String(mission.get("name", ""))
	if mission_stem.is_empty():
		mission_stem = "mission"
	mission_stem = mission_stem.validate_filename().replace(" ", "_")
	var stamp := String(snapshot.get("captured_at_utc", "")) \
			.replace("-", "").replace(":", "")
	var unix_usec := int(Time.get_unix_time_from_system() * MICROSECONDS_PER_SECOND)
	var target_path := ""
	while target_path.is_empty() or FileAccess.file_exists(target_path):
		_player_dump_sequence += 1
		target_path = PLAYER_POSE_DUMP_DIR.path_join("%s_%s_%d_%03d.json" % [
			mission_stem, stamp, unix_usec, _player_dump_sequence])
	return target_path


func _refresh_particles() -> void:
	var world = _effect_world_source.call() if _effect_world_source.is_valid() else null
	if world == null or not is_instance_valid(world):
		_ptl_peak = 0
		_ptl_count_label.text = "Current Particle Count:  0 / 0"
		if _ptl_list.item_count > 0:
			_ptl_list.clear()
		return
	var report: Array = world.get_debug_group_report()
	var unresolved: PackedStringArray = (world.get_unresolved_texture_names()
			if world.has_method("get_unresolved_texture_names") else PackedStringArray())
	# Retail's checked facade reports the effect world's active circular-buffer
	# entries, not the particle instances living inside each emitter.
	var current := int(world.active_entry_count())
	# The retail peak latch, quirk included: it resets to zero whenever the
	# current count is zero [orig: Debug_DrawParticleStats @ 0x44c840].
	_ptl_peak = 0 if current == 0 else maxi(_ptl_peak, current)
	_ptl_count_label.text = (
			"Current Particle Count:  %d / %d   (groups %d, effects %d, catalog missing %d)"
			% [current, _ptl_peak, report.size(), world.effect_count(), unresolved.size()])
	_ptl_list.clear()
	for miss in unresolved:
		var miss_idx := _ptl_list.add_item(
				"CATALOG missing texture: %s" % miss, null, false)
		_ptl_list.set_item_custom_fg_color(miss_idx, Color(1.0, 0.35, 0.3))
	for group_v in report:
		var group: Dictionary = group_v
		var source := String(group.get("source", ""))
		var header := "%02d   %s" % [int(group.get("id", 0)), String(group.get("name", ""))]
		if not source.is_empty():
			header += "   [%s]" % source
		_ptl_list.add_item(header, null, false)
		for emitter_v in group.get("emitters", []):
			var emitter: Dictionary = emitter_v
			_ptl_list.add_item("      %s  alive %d  drawn %d" % [
					String(emitter.get("name", "")), int(emitter.get("alive", 0)),
					int(emitter.get("rendered", 0))], null, false)


func _refresh_occlusion(sim: Object) -> void:
	if sim == null or not sim.has_method("get_occlusion_debug"):
		_clear_occlusion_pane("No occlusion data.")
		return
	var occ: Dictionary = sim.get_occlusion_debug()
	if not bool(occ.get("active", false)):
		_clear_occlusion_pane("No portal-carrying buildings in this mission.")
		return
	var counts: Dictionary = occ.get("counts", {})
	var lines := PackedStringArray()
	lines.append("Camera: %s   blink letters 0x%02X" % [
		"indoors" if bool(occ.get("camera_indoors", false)) else "outdoors",
		int(occ.get("local_blink_flags", 0))])
	lines.append("Exterior visible: %s   water visible: %s" % [
		"yes" if bool(occ.get("exterior_visible", false)) else "no",
		"yes" if bool(occ.get("water_visible", false)) else "no"])
	lines.append("Buildings: %d tracked   %d batched   %d drawn   %d occluder-culled" % [
		int(counts.get("instances", 0)), int(counts.get("batched", 0)),
		int(counts.get("visible", 0)), int(counts.get("toc_culled", 0))])
	lines.append("Portal slots %d   window wedges %d   see-through wedges %d" % [
		int(counts.get("slots", 0)), int(counts.get("window_groups", 0)),
		int(counts.get("viewthru_groups", 0))])
	lines.append("Cross-building welds %d   entities hidden %d" % [
		int(counts.get("welds", 0)), int(counts.get("culled_entities", 0))])
	_occ_status_label.text = "\n".join(lines)

	_occ_list.clear()
	for b_v in occ.get("buildings", []):
		var b: Dictionary = b_v
		var state := "drawn"
		var row_color := Color(0.55, 1.0, 0.6)
		if not bool(b.get("batched", false)):
			state = "not batched"
			row_color = Color(0.6, 0.6, 0.6)
		elif not bool(b.get("visible", false)):
			state = "occluder-culled"
			row_color = Color(1.0, 0.5, 0.35)
		var traits := ""
		if bool(b.get("has_open", false)):
			traits += "O"
		if bool(b.get("has_windows", false)):
			traits += "W"
		if bool(b.get("has_links", false)):
			traits += "L"
		var row := "#%d  mask %08X  %s" % [
			int(b.get("bms_id", 0)), int(b.get("mask", 0)) & 0xFFFFFFFF, state]
		if not traits.is_empty():
			row += "  [%s]" % traits
		var windows := int(b.get("windows", 0))
		var portals := int(b.get("portals", 0))
		var links := int(b.get("links", 0))
		if windows + portals + links > 0:
			row += "  win %d por %d link %d" % [windows, portals, links]
		var idx := _occ_list.add_item(row, null, false)
		_occ_list.set_item_custom_fg_color(idx, row_color)
	for w_v in occ.get("welds", []):
		var w: Dictionary = w_v
		var widx := _occ_list.add_item("weld  #%d s%d <-> #%d s%d" % [
			int(w.get("own_bms", 0)), int(w.get("own_section", 0)),
			int(w.get("other_bms", 0)), int(w.get("other_section", 0))], null, false)
		_occ_list.set_item_custom_fg_color(widx, Color(1.0, 0.4, 1.0))


func _clear_occlusion_pane(message: String) -> void:
	_occ_status_label.text = message
	if _occ_list.item_count > 0:
		_occ_list.clear()


# Kind colors mirror RoundDebugView.kind_color so the list rows and the world
# markers read as one system.
static func _round_kind_color(kind: int) -> Color:
	match kind:
		0:
			return Color(0.2, 0.9, 1.0)    # organic
		1:
			return Color(0.3, 1.0, 0.45)   # item FACE hit
		2:
			return Color(1.0, 0.75, 0.2)   # item sphere stand-in
		3:
			return Color(0.75, 0.6, 0.4)   # terrain
		4:
			return Color(0.55, 0.55, 0.55) # expired
		5:
			return Color(1.0, 0.25, 0.2)   # face-miss fly-on
		_:
			return Color.MAGENTA


static func _round_bone_pair(ev: Dictionary) -> String:
	var primary := int(ev.get("section", -1))
	if bool(ev.get("fallback", false)):
		return "neutral fallback sphere  reaction stand-in %d" % primary
	var secondary := int(ev.get("secondary_section", -1))
	var secondary_text := "-" if secondary < 0 else str(secondary)
	return "reaction bone %d  damage zone %s" % [primary, secondary_text]

func _refresh_rounds(sim: Object) -> void:
	if sim == null or not sim.has_method("get_round_debug"):
		_clear_rounds_pane("No round data.")
		return
	var debug: Dictionary = sim.get_round_debug()
	var events: Array = debug.get("events", [])
	if events.is_empty():
		_clear_rounds_pane("No rounds resolved yet.")
		return
	var face_hits := 0
	var person_hits := 0
	var person_fallbacks := 0
	var sphere_hits := 0
	var misses := 0
	for ev_v in events:
		match int((ev_v as Dictionary).get("kind", 4)):
			0:
				if bool((ev_v as Dictionary).get("fallback", false)):
					person_fallbacks += 1
				else:
					person_hits += 1
			1:
				face_hits += 1
			2:
				sphere_hits += 1
			5:
				misses += 1
	_rnd_status_label.text = "Last %d outcomes:  %d person bone hits   %d organic fallbacks   %d face hits   %d item sphere stand-ins   %d face-miss fly-ons" % [
		events.size(), person_hits, person_fallbacks, face_hits, sphere_hits, misses]

	_rnd_list.clear()
	# Newest first — the row you just shot is the row on top.
	for i in range(events.size() - 1, -1, -1):
		var ev: Dictionary = events[i]
		var kind := int(ev.get("kind", 4))
		var row := "t%d  %s" % [int(ev.get("tick", 0)), String(ev.get("kind_name", "?"))]
		var ent := int(ev.get("entity_handle", 0xFFFF))
		if ent != 0xFFFF:
			row += "  " + WireHandle.label(ent)
			var ent_name := String(ev.get("entity_name", ""))
			if not ent_name.is_empty():
				row += " " + ent_name
		if bool(ev.get("husk", false)):
			row += "  HUSK"
		match kind:
			0:
				row += "  %s  mat %d -> %s" % [_round_bone_pair(ev),
						int(ev.get("material", 0)),
						String(ev.get("effect_tag_name", ""))]
			1:
				row += "  sec %d face %d mat %d -> %s" % [int(ev.get("section", -1)),
						int(ev.get("face", -1)), int(ev.get("material", 0)),
						String(ev.get("effect_tag_name", ""))]
			2:
				row += "  -> %s" % String(ev.get("effect_tag_name", ""))
			5:
				row += "  graze t %.3f" % float(ev.get("t", 0.0))
			_:
				pass
		var idx := _rnd_list.add_item(row, null, false)
		_rnd_list.set_item_custom_fg_color(idx, _round_kind_color(kind))


func _clear_rounds_pane(message: String) -> void:
	_rnd_status_label.text = message
	if _rnd_list.item_count > 0:
		_rnd_list.clear()


func _refresh_net(sim: Object) -> void:
	if _net_status_label == null:
		return
	if sim == null or not sim.has_method("is_joiner"):
		_clear_net_pane("No net session.")
		return
	var rows := PackedStringArray()
	if bool(sim.is_joiner()):
		var phase := int(sim.get_joiner_phase())
		var phase_name: String = JOINER_PHASE_NAMES[phase] \
				if phase >= 0 and phase < JOINER_PHASE_NAMES.size() else str(phase)
		_net_status_label.text = "Joiner — connect phase %s" % phase_name
		rows.append("in match: %s" % ("yes" if bool(sim.is_joined_in_match()) else "no"))
		rows.append("self wire handle: %d" % int(sim.get_joiner_self_handle()))
		var join_error := String(sim.get_join_error())
		if not join_error.is_empty():
			rows.append("error: %s" % join_error)
		rows.append("server: %s" % String(sim.get_join_server_name()))
		rows.append("mission: %s (%s)" % [String(sim.get_join_mission_name()),
				String(sim.get_join_mission_file())])
	elif bool(sim.is_host_listening()):
		_net_status_label.text = "Host — listening on UDP %d, %d peer(s)" % [
				int(sim.get_host_listen_port()), int(sim.get_host_peer_count())]
		# Keys as get_host_session_config() emits them (nova_simulation_net.cpp).
		var config: Dictionary = sim.get_host_session_config()
		for key in ["server_name", "mission_name", "mission_file", "gametype",
				"max_players", "serve_and_play", "expansion", "player_name",
				"bind_port"]:
			if config.has(key):
				rows.append("%s: %s" % [key, str(config[key])])
	else:
		_net_status_label.text = "Local listen server (no bound socket) — SP session."
	# Rebuild the rows only when they change (ItemList rebuilds are the cost).
	var signature := "\n".join(rows)
	if signature != _net_rows_signature:
		_net_rows_signature = signature
		_net_list.clear()
		for row in rows:
			_net_list.add_item(row, null, false)


func _clear_net_pane(message: String) -> void:
	_net_status_label.text = message
	_net_rows_signature = ""
	if _net_list != null and _net_list.item_count > 0:
		_net_list.clear()


func _on_round_debug_toggled(pressed: bool) -> void:
	round_debug_toggled.emit(pressed)


func _on_hitbox_debug_toggled(pressed: bool) -> void:
	hitbox_debug_toggled.emit(pressed)


func _on_occlusion_debug_toggled(pressed: bool) -> void:
	occlusion_debug_toggled.emit(pressed)


func _on_particles_hidden_toggled(pressed: bool) -> void:
	particles_hidden_toggled.emit(pressed)


func _on_particle_boxes_toggled(pressed: bool) -> void:
	particle_boxes_toggled.emit(pressed)


func _refresh_entities(sim: Object) -> void:
	var snap: PackedFloat32Array = sim.get_present_snapshot()
	var stride: int = sim.get_present_stride()
	var count := 0 if stride <= 0 else snap.size() / stride
	# Rebuild only on count change; steady-state refreshes update text in place.
	if _entity_list.item_count != count:
		_entity_list.clear()
		for i in range(count):
			_entity_list.add_item("")
		if _selected_entity >= count:
			_selected_entity = -1
	for i in range(count):
		var base := i * stride
		var flags := ""
		if snap[base + NovaSimulation.PF_ALIVE] == 0.0:
			flags += "  [down]"
		if snap[base + NovaSimulation.PF_HIDDEN] != 0.0:
			flags += "  [hidden]"
		_entity_list.set_item_text(i, "#%d  ssn %d  (%.0f, %.0f)%s" % [
			i,
			int(snap[base + NovaSimulation.PF_NET_ID]),
			snap[base + NovaSimulation.PF_POS_X],
			snap[base + NovaSimulation.PF_POS_Z],
			flags,
		])
	_refresh_entity_detail(sim)


func _refresh_entity_detail(sim: Object) -> void:
	if _selected_entity < 0:
		_entity_detail.text = "Select a unit to see its details."
		return
	var card: Dictionary = sim.get_entity_debug(_selected_entity)
	if card.is_empty():
		_entity_detail.text = "Select a unit to see its details."
		return
	var name := String(card.get("name", ""))
	var pos: Vector3 = card.get("position", Vector3.ZERO)
	var lines := PackedStringArray()
	lines.append("%s  (ssn %d)" % [name if not name.is_empty() else "unnamed", int(card.get("net_id", 0))])
	lines.append("state: %s (%d)  alert %d" % [
		String(card.get("state_name", "?")), int(card.get("state", 0)), int(card.get("alert", 0))])
	lines.append("health: %d (ai %d)  team %d" % [
		int(card.get("health", 0)), int(card.get("ai_health", 0)), int(card.get("team", 0))])
	lines.append("position: (%.1f, %.1f, %.1f)  facing %.0f°" % [
		pos.x, pos.y, pos.z, float(card.get("yaw_deg", 0.0))])
	lines.append("route %d  node %d  distance %d  speed %d" % [
		int(card.get("waypoint_id", 0)), int(card.get("wp_node", 0)),
		int(card.get("wp_distance", 0)), int(card.get("out_speed", 0))])
	if bool(card.get("mounted", false)):
		var seat_local: Vector3 = card.get("mount_seat_local", Vector3.ZERO)
		lines.append("mounted: target ssn %d  seat %d/%d  %s  type %d  bone %d" % [
			int(card.get("mount_target_net_id", 0)), int(card.get("mount_seat", -1)),
			int(card.get("mount_target_seat_count", 0)),
			String(card.get("mount_seat_source_name", "")),
			int(card.get("mount_type", 0)), int(card.get("mount_seat_bone", 0))])
		lines.append("seat local: (%.2f, %.2f, %.2f)  pose %d  yaw %+d  anim %s (%d)" % [
			seat_local.x, seat_local.y, seat_local.z,
			int(card.get("mount_seat_pose_index", 0)),
			int(card.get("mount_seat_yaw_offset", 0)),
			String(card.get("anim_key", "")), int(card.get("anim_state", -1))])
	var traits := PackedStringArray()
	if bool(card.get("infantry", false)):
		traits.append("on foot")
	if bool(card.get("hidden", false)):
		traits.append("hidden")
	if bool(card.get("held", false)):
		traits.append("held")
	if bool(card.get("disabled", false)):
		traits.append("disabled")
	if not traits.is_empty():
		lines.append(", ".join(traits))
	_entity_detail.text = "\n".join(lines)


func _refresh_sim(runtime: Object, sim: Object) -> void:
	_tick_label.text = "tick %d%s" % [int(sim.get_logic_tick()),
			"" if bool(runtime.is_playing()) else "  (paused)"]
	_entities_label.text = "%d units" % int(sim.get_entity_count())
	var fired: PackedByteArray = sim.get_fired_events_snapshot()
	var fired_count := 0
	for flag in fired:
		if flag != 0:
			fired_count += 1
	_events_label.text = "events fired: %d / %d" % [fired_count, fired.size()]
	var wac: Dictionary = sim.get_wac_state()
	if bool(wac.get("loaded", false)):
		_wac_label.text = "scripts: loaded, %d run(s)%s" % [int(wac.get("runs", 0)),
				"  (paused)" if bool(wac.get("paused", false)) else ""]
	else:
		_wac_label.text = "scripts: none"
	_wac_pause_check.set_pressed_no_signal(bool(wac.get("paused", false)))


func _refresh_vars(sim: Object) -> void:
	var banks := [
		["V", sim.get_mission_variables_snapshot(), true],
		["G", sim.get_global_variables_snapshot(), false],
		["M", sim.get_music_variables_snapshot(), false],
	]
	var nonzero_only: bool = _nonzero_check.button_pressed
	var writable: bool = _writes_check.button_pressed and not _writes_locked

	# Decide what should be visible, then rebuild only when that set (or the
	# writes mode) changed; otherwise update values in place.
	var desired: Array = []
	for bank in banks:
		var values: PackedInt32Array = bank[1]
		for i in range(values.size()):
			if nonzero_only and values[i] == 0:
				continue
			desired.append([String(bank[0]), i, values[i], bool(bank[2])])
	var signature := "%d|%s|%s" % [desired.size(), str(nonzero_only), str(writable)]
	for entry in desired:
		signature += "|%s%d" % [entry[0], entry[1]]

	# Never rebuild out from under an edit in progress: with the changed-only
	# filter on a RUNNING mission, vars flip zero<->nonzero routinely, and the
	# rebuild would drop focus and in-flight text. The stale set survives one
	# refresh cycle; the rebuild lands after the field blurs.
	if signature != _var_rows_signature and _any_var_edit_focused():
		return

	if signature != _var_rows_signature:
		_var_rows_signature = signature
		_var_controls.clear()
		for child in _vars_rows.get_children():
			# Detach before queue_free so the dying rows release their names
			# immediately (replacement rows reuse them) and never shadow
			# lookups; full free stays deferred because a rebuild can be
			# triggered from a row's own LineEdit signal.
			_vars_rows.remove_child(child)
			child.queue_free()
		if desired.is_empty():
			var empty := Label.new()
			empty.name = "VarsEmpty"
			empty.text = "No values set yet."
			_vars_rows.add_child(empty)
		for entry in desired:
			_add_var_row(String(entry[0]), int(entry[1]), int(entry[2]),
					writable and bool(entry[3]))

	for entry in desired:
		var key := "%s%d" % [entry[0], entry[1]]
		var control: Control = _var_controls.get(key)
		if control is LineEdit:
			var edit := control as LineEdit
			if not edit.has_focus():
				edit.text = str(int(entry[2]))
		elif control is Label:
			(control as Label).text = str(int(entry[2]))


func _any_var_edit_focused() -> bool:
	for control in _var_controls.values():
		if control is LineEdit and (control as LineEdit).has_focus():
			return true
	return false


func _add_var_row(bank: String, index: int, value: int, writable: bool) -> void:
	var row := HBoxContainer.new()
	row.name = "VarRow_%s%d" % [bank, index]
	var name_label := Label.new()
	name_label.text = "%s%d" % [bank, index]
	name_label.custom_minimum_size = Vector2(64, 0)
	row.add_child(name_label)
	if writable:
		var edit := LineEdit.new()
		edit.name = "VarEdit_%s%d" % [bank, index]
		edit.text = str(value)
		edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		edit.text_submitted.connect(_on_var_submitted.bind(index))
		row.add_child(edit)
		_var_controls["%s%d" % [bank, index]] = edit
	else:
		var value_label := Label.new()
		value_label.name = "VarValue_%s%d" % [bank, index]
		value_label.text = str(value)
		row.add_child(value_label)
		_var_controls["%s%d" % [bank, index]] = value_label
	_vars_rows.add_child(row)


# --- Handlers ----------------------------------------------------------------

func _on_dump_player_pose_pressed() -> void:
	dump_local_player_pose()


func _on_entity_selected(index: int) -> void:
	_selected_entity = index
	var sim := _resolve_sim(_resolve_runtime())
	if sim != null:
		_refresh_entity_detail(sim)


func _on_play_pressed() -> void:
	var runtime := _resolve_runtime()
	if runtime != null:
		runtime.play()
		_refresh()
		transport_used.emit("play")


func _on_pause_pressed() -> void:
	var runtime := _resolve_runtime()
	if runtime != null:
		runtime.pause()
		_refresh()
		transport_used.emit("pause")


func _on_step_pressed() -> void:
	var runtime := _resolve_runtime()
	if runtime != null:
		runtime.step_once()
		_refresh()
		transport_used.emit("step")


func _on_stop_pressed() -> void:
	var runtime := _resolve_runtime()
	if runtime != null:
		runtime.stop()
		_refresh()
		transport_used.emit("stop")


func _on_wac_pause_toggled(pressed: bool) -> void:
	var sim := _resolve_sim(_resolve_runtime())
	if sim != null:
		sim.set_wac_paused(pressed)
		transport_used.emit("wac_pause")


func _on_vars_filter_toggled(_pressed: bool) -> void:
	_refresh()


func _on_skeleton_toggled(pressed: bool) -> void:
	skeleton_debug_toggled.emit(pressed)


func _on_user_points_toggled(pressed: bool) -> void:
	user_points_toggled.emit(pressed)


func _on_collision_toggled(pressed: bool) -> void:
	collision_debug_toggled.emit(pressed)


func _on_foliage_toggled(pressed: bool) -> void:
	foliage_hidden_toggled.emit(pressed)


func _on_viewmodel_forced_toggled(pressed: bool) -> void:
	viewmodel_forced_toggled.emit(pressed)


func _on_body_fp_toggled(pressed: bool) -> void:
	body_in_first_person_toggled.emit(pressed)


func _on_var_submitted(text: String, index: int) -> void:
	# Re-check the lock at submit time (not just at row build): rows built
	# before lock_writes() would otherwise still commit on Enter.
	if _writes_locked or not _writes_check.button_pressed:
		return
	var sim := _resolve_sim(_resolve_runtime())
	if sim != null and text.is_valid_int():
		sim.set_mission_variable(index, int(text))
		_refresh()
