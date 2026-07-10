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

const REFRESH_INTERVAL := 0.25
const PANEL_WIDTH := 380.0

var _runtime_source := Callable()
var _world_source := Callable()
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

# Perf pane (C11): the PerfTimeline ring + live monitors + the per-frame budget.
var _perf_pane: DebugPerfPane

# Foliage pane: the world's live vegetation counters, one label row per stat.
var _foliage_status: Label
var _foliage_stat_labels: Dictionary = {}

# Player pane: the local player's live state + the network one-liner.
var _player_label: Label

# View pane: render-debug toggles the host acts on (skeleton bone overlay, foliage, ...).
var _skeleton_check: CheckBox
var _collision_check: CheckBox
var _foliage_check: CheckBox
var _viewmodel_check: CheckBox
var _body_fp_check: CheckBox


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


## Convenience for hosts holding one runtime instance directly.
func set_runtime(runtime) -> void:
	var ref: WeakRef = weakref(runtime)
	set_runtime_source(func(): return ref.get_ref())


## Optional world supplier: a Callable returning the host's current GameWorld (or
## null), re-resolved every refresh like the runtime source. Feeds the Perf tab's
## per-frame budget and the Foliage tab; hosts that never call this simply leave
## those panes in their empty states.
func set_world_source(source: Callable) -> void:
	_world_source = source
	if visible:
		_refresh()


## Convenience for hosts holding one world instance directly.
func set_world(world) -> void:
	var ref: WeakRef = weakref(world)
	set_world_source(func(): return ref.get_ref())


func toggle() -> void:
	visible = not visible
	_sync_timer()
	if visible:
		_refresh()


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
	_build_player_tab()
	_build_vars_tab()
	_build_perf_tab()
	_build_foliage_tab()
	_build_view_tab()


func _build_perf_tab() -> void:
	_perf_pane = DebugPerfPane.new()
	_perf_pane.name = "Perf"
	_tabs.add_child(_perf_pane)


# The local player's live state: where they are, health/side, the equipped weapon,
# the zone flags the collision resolver maintains, and the network one-liner.
func _build_player_tab() -> void:
	var tab := VBoxContainer.new()
	tab.name = "Player"
	tab.add_theme_constant_override("separation", 6)
	_tabs.add_child(tab)

	_player_label = Label.new()
	_player_label.name = "PlayerState"
	_player_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_player_label.text = "No mission running."
	tab.add_child(_player_label)


# Foliage stat rows. A fixed row set (built once) fed from the world's dispatch
# counters each refresh; label copy stays artist-facing (the far tier is the
# scattered ground cover, the model tier the bush/tree clusters around objects).
const _FOLIAGE_ROWS := [
	["_header_far", "Ground cover (scattered)"],
	["far_cells_visible", "Patches visible"],
	["cached_cells", "Patches kept"],
	["total_instances", "Plants kept"],
	["far_pool_hits", "Cache hits"],
	["far_pool_misses", "Cache misses"],
	["far_cells_baked", "Patches built"],
	["far_instances_baked", "Plants built"],
	["_header_model", "Bushes & trees (around objects)"],
	["model_anchors_in_range", "Objects in view"],
	["model_tiles_emitted", "Clusters drawn"],
	["model_instances", "Plants drawn"],
	["model_cached_tiles", "Clusters kept"],
	["model_cache_hits", "Cache hits"],
	["model_cache_misses", "Cache misses"],
	["model_regenerations", "Clusters rebuilt"],
	["model_uploads", "Mesh uploads"],
]


func _build_foliage_tab() -> void:
	var tab := VBoxContainer.new()
	tab.name = "Foliage"
	tab.add_theme_constant_override("separation", 4)
	_tabs.add_child(tab)

	_foliage_status = Label.new()
	_foliage_status.name = "FoliageStatus"
	_foliage_status.text = "No world running."
	_foliage_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	tab.add_child(_foliage_status)

	var scroll := ScrollContainer.new()
	scroll.name = "FoliageScroll"
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	tab.add_child(scroll)

	var rows := VBoxContainer.new()
	rows.name = "FoliageRows"
	rows.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(rows)

	for entry in _FOLIAGE_ROWS:
		var key: String = entry[0]
		if key.begins_with("_header"):
			var header := Label.new()
			header.name = "FoliageHeader%s" % key.trim_prefix("_header")
			header.text = String(entry[1])
			rows.add_child(header)
			continue
		var row := HBoxContainer.new()
		row.name = "FoliageRow_%s" % key
		var name_label := Label.new()
		name_label.text = "  %s" % String(entry[1])
		name_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		row.add_child(name_label)
		var value_label := Label.new()
		value_label.name = "Value"
		value_label.text = "—"
		row.add_child(value_label)
		rows.add_child(row)
		_foliage_stat_labels[key] = value_label


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

	_collision_check = CheckBox.new()
	_collision_check.name = "ViewCollision"
	_collision_check.text = "Show collision"
	_collision_check.tooltip_text = "Draw the shapes objects block movement with (walls gray, platforms blue, armory zones green, indoor boxes yellow, hurt zones red, player-only walls purple) plus your soldier's collision capsule and ground gap."
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


func _resolve_world() -> Object:
	if not _world_source.is_valid():
		return null
	var world: Variant = _world_source.call()
	if world == null or not is_instance_valid(world):
		return null
	return world


# --- Refresh ---------------------------------------------------------------

func _refresh() -> void:
	var runtime := _resolve_runtime()
	var sim := _resolve_sim(runtime)
	var live := sim != null
	_status_label.visible = not live
	# The perf + foliage panes are fed by HOST-WIDE state (the PerfTimeline ring,
	# live monitors, and the world's frame counters), not the sim — they refresh
	# regardless, so "that load was slow, let me look" works from the menu after
	# returning from a mission.
	var world := _resolve_world()
	var world_counters: Dictionary = {}
	if world != null and world.has_method("get_runtime_perf_counters"):
		world_counters = world.get_runtime_perf_counters()
	_perf_pane.render_frame_budget(world_counters)
	_perf_pane.refresh()
	_refresh_foliage(world_counters)
	# The other tabs stay usable without a sim too: the panes that need one
	# clear to their empty states (their handlers already null-check).
	if not live:
		_clear_live_panes()
		return
	_refresh_entities(sim)
	_refresh_sim(runtime, sim)
	_refresh_vars(sim)
	_refresh_player(sim)


func _clear_live_panes() -> void:
	if _entity_list.item_count > 0:
		_entity_list.clear()
	_selected_entity = -1
	_entity_detail.text = "Select a unit to see its details."
	_tick_label.text = ""
	_entities_label.text = ""
	_events_label.text = ""
	_wac_label.text = ""
	_player_label.text = "No mission running."
	if _var_rows_signature != "":
		_var_rows_signature = ""
		_var_controls.clear()
		for child in _vars_rows.get_children():
			_vars_rows.remove_child(child)
			child.queue_free()


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


# The Player tab: every line reads a sim seam guarded by has_method, so the pane
# stays resilient across sim variants (net spectators, dedicated hosts, tests).
func _refresh_player(sim: Object) -> void:
	var lines := PackedStringArray()
	var has_player: bool = sim.has_method("has_local_player") and bool(sim.has_local_player())
	if has_player:
		var pos: Vector3 = sim.get_local_player_position() if sim.has_method("get_local_player_position") else Vector3.ZERO
		var yaw := float(sim.get_local_player_yaw_deg()) if sim.has_method("get_local_player_yaw_deg") else 0.0
		lines.append("position: (%.1f, %.1f, %.1f)  facing %.0f°" % [pos.x, pos.y, pos.z, yaw])
		if sim.has_method("get_local_player_health"):
			lines.append("health: %d / %d   side %d" % [int(sim.get_local_player_health()),
					int(sim.get_local_player_max_health()), int(sim.get_local_player_team())])
		lines.append(_weapon_line(sim))
		var indoors: bool = sim.has_method("local_player_indoors") and bool(sim.local_player_indoors())
		var blink := int(sim.local_player_blink_flags()) if sim.has_method("local_player_blink_flags") else 0
		lines.append("indoors: %s   (blink flags 0x%X)" % ["yes" if indoors else "no", blink])
		var in_armory: bool = sim.has_method("local_player_in_armory_zone") and bool(sim.local_player_in_armory_zone())
		lines.append("in armory zone: %s" % ("yes" if in_armory else "no"))
	else:
		lines.append("No player in the world.")
	lines.append(_net_line(sim))
	_player_label.text = "\n".join(lines)


func _weapon_line(sim: Object) -> String:
	if not sim.has_method("get_local_player_weapon_state"):
		return "weapon: none"
	var ws: Dictionary = sim.get_local_player_weapon_state()
	if not bool(ws.get("active", false)):
		return "weapon: none"
	var anim := String(ws.get("anim_key", ""))
	var clip := int(ws.get("clip", -1))
	var reserve := int(ws.get("reserve", -1))
	return "weapon: %s   magazine %s   spare %s" % [
		anim if not anim.is_empty() else "(idle)",
		str(clip) if clip >= 0 else "∞",
		str(reserve) if reserve >= 0 else "∞",
	]


# The network one-liner: hosting (port + joined count), joining (phase), or
# single player. Reads the same seams the shell's net flows use.
func _net_line(sim: Object) -> String:
	if sim.has_method("is_host_listening") and bool(sim.is_host_listening()):
		var port := int(sim.get_host_listen_port()) if sim.has_method("get_host_listen_port") else 0
		var peers := int(sim.get_host_peer_count()) if sim.has_method("get_host_peer_count") else 0
		return "network: hosting on port %d, %d joined" % [port, peers]
	if sim.has_method("is_joiner") and bool(sim.is_joiner()):
		if sim.has_method("is_joined_in_match") and bool(sim.is_joined_in_match()):
			return "network: joined a host (in match)"
		var phase := int(sim.get_joiner_phase()) if sim.has_method("get_joiner_phase") else -1
		return "network: joining a host (step %d)" % phase
	return "network: single player"


# The Foliage tab: the world's live dispatch counters as label rows. Fed the
# same Dictionary the Perf tab's frame budget reads; empty (no world / no
# terrain) leaves the rows dashed with the status line explaining why.
func _refresh_foliage(world_counters: Dictionary) -> void:
	var stats_v: Variant = world_counters.get("foliage", {})
	var stats: Dictionary = stats_v if stats_v is Dictionary else {}
	_foliage_status.visible = stats.is_empty()
	if stats.is_empty():
		_foliage_status.text = "No world running." if world_counters.is_empty() else "No foliage in this world."
	for key in _foliage_stat_labels:
		var label: Label = _foliage_stat_labels[key]
		label.text = str(int(stats[key])) if stats.has(key) else "—"


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
