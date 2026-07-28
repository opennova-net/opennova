class_name DebugStatsPane
extends VBoxContainer
## The debug overlay's Stats tab: one row per major runtime system with
## window-averaged per-frame milliseconds (avg + worst frame in the window)
## and live counters beside them. The numbers come from the shared
## FrameStatsBoard the hosts feed; this pane only opens/closes the capture
## window and formats what accumulated.
##
## Capture is edge-gated: the board's `enabled` flips true only while this tab
## is the visible overlay tab, so a closed overlay costs the hosts nothing.
## Rows read from a fixed table; the Tree is built once and text is updated
## in place. Every read/format runs at the (divided) overlay refresh cadence,
## never per frame.

const _KIND_SPAN := 0    # one board slot: avg + max ms
const _KIND_GROUP := 1   # sum of several slots: avg ms only (maxes don't add)
const _KIND_HEADER := 2  # label + info only (no time)
const _KIND_RESIDUAL := 3 # base slot minus a slot list: avg ms only

# Read the board every Nth overlay refresh: at the overlay's 0.25 s cadence
# this makes 0.5 s windows — wide enough that two consecutive readings of a
# steady scene agree.
const _REFRESH_DIVIDER := 2

# The fixed row table. depth parents each row under the nearest shallower row,
# the span-tree convention the Perf tab uses.
const _ROWS := [
	{"id": "frame", "label": "Frame (process)", "depth": 0, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.FRAME_PROCESS},
	{"id": "before", "label": "Player host (pre)", "depth": 1, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.FRAME_PLAYER_BEFORE},
	{"id": "world", "label": "World tick", "depth": 1, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.FRAME_WORLD},
	{"id": "foliage", "label": "Foliage", "depth": 2, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.WORLD_FOLIAGE},
	{"id": "sim", "label": "Sim step", "depth": 2, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.SIM_STEP},
	{"id": "net", "label": "Net wire leg", "depth": 3, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.SIM_NET},
	{"id": "effects", "label": "Effects tick", "depth": 2, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.EFFECTS_TICK},
	{"id": "present", "label": "Present", "depth": 2, "kind": _KIND_GROUP,
			"slots": [FrameStatsBoard.PRESENT_SNAPSHOT, FrameStatsBoard.PRESENT_MISSION,
					FrameStatsBoard.PRESENT_WIRE, FrameStatsBoard.PRESENT_FIRE,
					FrameStatsBoard.PRESENT_DESTRUCTION, FrameStatsBoard.PRESENT_THROWABLE]},
	{"id": "snapshot", "label": "Row snapshot", "depth": 3, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.PRESENT_SNAPSHOT},
	{"id": "mission_rows", "label": "Mission rows", "depth": 3, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.PRESENT_MISSION},
	{"id": "wire_rows", "label": "Wire rows", "depth": 3, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.PRESENT_WIRE},
	{"id": "fire", "label": "Fire", "depth": 3, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.PRESENT_FIRE},
	{"id": "destruction", "label": "Destruction", "depth": 3, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.PRESENT_DESTRUCTION},
	{"id": "throwable", "label": "Throwable", "depth": 3, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.PRESENT_THROWABLE},
	{"id": "occl", "label": "Occlusion", "depth": 2, "kind": _KIND_GROUP,
			"slots": [FrameStatsBoard.OCCL_BUILD, FrameStatsBoard.OCCL_PROBE,
					FrameStatsBoard.OCCL_APPLY, FrameStatsBoard.OCCL_GLUE]},
	{"id": "occl_build", "label": "Build (native)", "depth": 3, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.OCCL_BUILD},
	{"id": "occl_probe", "label": "Probe (native)", "depth": 3, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.OCCL_PROBE},
	{"id": "occl_apply", "label": "Apply (nodes)", "depth": 3, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.OCCL_APPLY},
	{"id": "env", "label": "Env (weather/blink/iris)", "depth": 2, "kind": _KIND_GROUP,
			"slots": [FrameStatsBoard.WORLD_WEATHER, FrameStatsBoard.WORLD_BLINK,
					FrameStatsBoard.WORLD_IRIS]},
	{"id": "audio", "label": "Audio", "depth": 2, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.WORLD_AUDIO},
	{"id": "after", "label": "Player host (post)", "depth": 1, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.FRAME_PLAYER_AFTER},
	{"id": "hud", "label": "HUD tick", "depth": 1, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.FRAME_HUD},
	{"id": "hud_scalars", "label": "Scalars", "depth": 2, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.HUD_SCALARS},
	{"id": "hud_attach", "label": "Attach labels", "depth": 2, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.HUD_ATTACH},
	{"id": "hud_waypoint", "label": "Waypoint", "depth": 2, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.HUD_WAYPOINT},
	{"id": "hud_info", "label": "Info build", "depth": 2, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.HUD_INFO},
	{"id": "hud_flush", "label": "Flush", "depth": 2, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.HUD_FLUSH},
	# The process step minus every measured shell leg: deferred/off-span work
	# the frame induces (skeleton updates, queued frees, other nodes' _process).
	# When this row is large, the next slice hides here, not in the spans above.
	# Caveat: with vsync ON the engine's swap wait can land inside the process
	# step, inflating this row — read it vsync-off (the A/B probes do), or
	# treat only growth beyond the vsync period as real work.
	{"id": "shell_residual", "label": "Outside shell spans", "depth": 1,
			"kind": _KIND_RESIDUAL, "base": FrameStatsBoard.FRAME_PROCESS,
			"minus": [FrameStatsBoard.FRAME_PLAYER_BEFORE, FrameStatsBoard.FRAME_WORLD,
					FrameStatsBoard.FRAME_PLAYER_AFTER, FrameStatsBoard.FRAME_HUD]},
	{"id": "render", "label": "Render", "depth": 0, "kind": _KIND_HEADER},
	{"id": "render_root_cpu", "label": "Viewport CPU", "depth": 1, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.RENDER_ROOT_CPU},
	{"id": "render_root_gpu", "label": "Viewport GPU", "depth": 1, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.RENDER_ROOT_GPU},
	{"id": "render_water_cpu", "label": "Water RTT CPU", "depth": 1, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.RENDER_WATER_CPU},
	{"id": "render_water_gpu", "label": "Water RTT GPU", "depth": 1, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.RENDER_WATER_GPU},
]

var status_label: Label
var stats_tree: Tree

var _board: FrameStatsBoard = null
var _world_source := Callable()
var _host_visible := false
var _capture_active := false
var _refresh_count := 0
var _items: Dictionary = {}  # row id -> TreeItem


func _init() -> void:
	add_theme_constant_override("separation", 6)

	status_label = Label.new()
	status_label.name = "StatsStatus"
	status_label.text = "No frame stats source."
	status_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	add_child(status_label)

	stats_tree = Tree.new()
	stats_tree.name = "StatsRows"
	stats_tree.columns = 4
	stats_tree.column_titles_visible = true
	stats_tree.set_column_title(0, "System")
	stats_tree.set_column_title(1, "avg")
	stats_tree.set_column_title(2, "max")
	stats_tree.set_column_title(3, "info")
	stats_tree.set_column_expand(0, true)
	stats_tree.set_column_custom_minimum_width(0, 118)
	for column in [1, 2]:
		stats_tree.set_column_expand(column, false)
		stats_tree.set_column_custom_minimum_width(column, 46)
	stats_tree.set_column_expand(3, true)
	stats_tree.set_column_custom_minimum_width(3, 60)
	stats_tree.hide_root = true
	stats_tree.focus_mode = Control.FOCUS_NONE
	stats_tree.size_flags_vertical = Control.SIZE_EXPAND_FILL
	add_child(stats_tree)
	_build_rows()


func set_frame_stats_board(board: FrameStatsBoard) -> void:
	_board = board
	_sync_capture()


## Supplier of the world host (GameWorld or null) for the counter pulls.
func set_world_source(source: Callable) -> void:
	_world_source = source


## The overlay's explicit visibility edge: Controls under a hidden CanvasLayer
## don't all observe the layer hide, so the overlay tells us on toggle.
func set_capture_active(host_visible: bool) -> void:
	_host_visible = host_visible
	_sync_capture()


func _notification(what: int) -> void:
	# Tab switches flip this Control's own visibility.
	if what == NOTIFICATION_VISIBILITY_CHANGED:
		_sync_capture()


func is_capturing() -> bool:
	return _capture_active


func _sync_capture() -> void:
	var want := _board != null and _host_visible and visible and is_inside_tree()
	if want == _capture_active:
		return
	_capture_active = want
	_refresh_count = 0
	if _board != null:
		_board.enabled = want
		_board.reset_window()


## One overlay-cadence refresh. Reads the window only every _REFRESH_DIVIDER
## calls so displayed means cover ~0.5 s of frames.
func refresh(runtime: Object, sim: Object) -> void:
	_sync_capture()
	if _board == null:
		status_label.text = "No frame stats source."
		return
	if not _capture_active:
		return
	_refresh_count += 1
	if _refresh_count % _REFRESH_DIVIDER != 0:
		return
	var frames := _board.window_frames()
	var sums := _board.window_sums()
	var maxes := _board.window_maxes()
	var counts := _board.window_counts()
	_board.reset_window()
	if frames <= 0:
		return
	status_label.text = "Per-frame means over the last %d frames." % frames
	render_window(frames, sums, maxes, counts, runtime, sim)


## Format one drained window into the rows. Split from refresh() so tests can
## drive the pane with fabricated windows.
func render_window(frames: int, sums: PackedInt64Array, maxes: PackedInt64Array,
		counts: PackedInt32Array, runtime: Object, sim: Object) -> void:
	for row_v in _ROWS:
		var row: Dictionary = row_v
		var item: TreeItem = _items[row["id"]]
		match int(row["kind"]):
			_KIND_SPAN:
				var slot := int(row["slot"])
				if counts[slot] <= 0:
					item.set_text(1, "-")
					item.set_text(2, "-")
				else:
					item.set_text(1, "%.2f" % (float(sums[slot]) / 1000.0 / frames))
					item.set_text(2, "%.2f" % (float(maxes[slot]) / 1000.0))
			_KIND_GROUP:
				var total := 0
				var seen := false
				for slot_v in row["slots"]:
					var slot := int(slot_v)
					total += sums[slot]
					seen = seen or counts[slot] > 0
				item.set_text(1, "%.2f" % (float(total) / 1000.0 / frames) if seen else "-")
				item.set_text(2, "")
			_KIND_RESIDUAL:
				var base_slot := int(row["base"])
				if counts[base_slot] <= 0:
					item.set_text(1, "-")
				else:
					var residual := int(sums[base_slot])
					for slot_v in row["minus"]:
						residual -= sums[int(slot_v)]
					item.set_text(1, "%.2f" %
							(float(maxi(residual, 0)) / 1000.0 / frames))
				item.set_text(2, "")
			_:
				pass
	_refresh_info(sums, counts, frames, runtime, sim)


func _build_rows() -> void:
	stats_tree.clear()
	_items.clear()
	var root := stats_tree.create_item()
	var stack: Array = [root]
	for row_v in _ROWS:
		var row: Dictionary = row_v
		var depth := int(row["depth"])
		while stack.size() > depth + 1:
			stack.pop_back()
		var item := stats_tree.create_item(stack.back())
		item.set_text(0, String(row["label"]))
		item.set_text(1, "-")
		for column in [1, 2]:
			item.set_text_alignment(column, HORIZONTAL_ALIGNMENT_RIGHT)
		_items[row["id"]] = item
		stack.push_back(item)


func _set_info(id: String, text: String) -> void:
	(_items[id] as TreeItem).set_text(3, text)


# The counter pulls: live Dictionaries/typed stats read at refresh cadence
# only, every source duck-typed and optional so SP, listen-host, joiner and
# harness stubs all render what they have.
func _refresh_info(sums: PackedInt64Array, counts: PackedInt32Array, frames: int,
		runtime: Object, sim: Object) -> void:
	_set_info("frame", "%d fps" % int(Performance.get_monitor(Performance.TIME_FPS)))
	_set_info("render", "%d draws · %d objs · %s prims · %d nodes" % [
		int(Performance.get_monitor(Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME)),
		int(Performance.get_monitor(Performance.RENDER_TOTAL_OBJECTS_IN_FRAME)),
		_compact_count(int(Performance.get_monitor(
				Performance.RENDER_TOTAL_PRIMITIVES_IN_FRAME))),
		int(Performance.get_monitor(Performance.OBJECT_NODE_COUNT)),
	])

	var world: Object = null
	if _world_source.is_valid():
		var world_v: Variant = _world_source.call()
		if world_v is Object and is_instance_valid(world_v):
			world = world_v

	# Sim row: ticks/frame from the value slot + entity/role counters.
	var sim_info := ""
	if counts[FrameStatsBoard.SIM_TICKS] > 0:
		sim_info = "%.1f t/f" % (float(sums[FrameStatsBoard.SIM_TICKS]) / frames)
	if world != null and world.has_method("get_runtime_perf_counters"):
		var wc: Dictionary = world.get_runtime_perf_counters()
		var simc: Dictionary = (wc.get("runtime", {}) as Dictionary).get("sim", {})
		if not simc.is_empty():
			var role := "local"
			if bool(simc.get("listen_server", false)):
				role = "listen"
			elif sim != null and sim.has_method("is_joiner") and bool(sim.is_joiner()):
				role = "joiner"
			sim_info += "%s%d ents · %s" % [
				"" if sim_info.is_empty() else " · ",
				int(simc.get("present_entity_count", 0)), role]
	_set_info("sim", sim_info)

	var net_info := ""
	if sim != null and sim.has_method("get_host_peer_count"):
		var peers := int(sim.get_host_peer_count())
		if peers > 0:
			net_info = "%d peer(s)" % peers
	_set_info("net", net_info)

	if world != null and world.has_method("get_effect_world"):
		var fx = world.get_effect_world()
		if fx != null and is_instance_valid(fx) and fx.has_method("active_entry_count"):
			var drain_ms := float(sums[FrameStatsBoard.EFFECTS_DRAIN]) / 1000.0 / frames
			_set_info("effects", "%d live · drain %.2f" % [
					int(fx.active_entry_count()), drain_ms])

	if world != null and world.has_method("get_fire_present_stats"):
		var fire: Dictionary = world.get_fire_present_stats()
		if not fire.is_empty():
			_set_info("fire", "%d fires · %d snd" % [
					int(fire.get("fires", 0)), int(fire.get("sounds", 0))])

	if world != null and world.has_method("get_destruction_present_stats"):
		var destruction = world.get_destruction_present_stats()
		if destruction != null:
			_set_info("destruction", "%d husks · %d bursts" % [
					int(destruction.husk_swaps), int(destruction.bursts)])

	if runtime != null and runtime.has_method("get_throwable_present_stats"):
		var throwable = runtime.get_throwable_present_stats()
		if throwable != null:
			_set_info("throwable", "%d live" % int(throwable.live))

	if runtime != null and runtime.has_method("get_wire_present_stats"):
		var wire: Dictionary = runtime.get_wire_present_stats()
		if not wire.is_empty():
			_set_info("wire_rows", "%d live · %d unresolved" % [
					int(wire.get("live", 0)), int(wire.get("unresolved", 0))])

	if sim != null and sim.has_method("get_occlusion_debug"):
		var occ: Dictionary = sim.get_occlusion_debug()
		if bool(occ.get("active", false)):
			var occ_counts: Dictionary = occ.get("counts", {})
			_set_info("occl", "%d bld · %d drawn · %d ent culled" % [
					int(occ_counts.get("instances", 0)),
					int(occ_counts.get("visible", 0)),
					int(occ_counts.get("culled_entities", 0))])
		else:
			_set_info("occl", "")


static func _compact_count(value: int) -> String:
	if value >= 1_000_000:
		return "%.1fM" % (value / 1_000_000.0)
	if value >= 10_000:
		return "%dk" % int(value / 1000.0)
	return str(value)
