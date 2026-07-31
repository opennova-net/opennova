class_name DebugStatsPage
extends NovaDebugPage
## The debug overlay's Stats page: one row per major runtime system with
## window-averaged per-frame milliseconds (avg + worst frame in the window)
## and live counters beside them. The numbers come from the shared
## FrameStatsBoard the hosts feed; this pane only opens/closes the capture
## window and formats what accumulated.
##
## Capture is edge-gated: the board is active only while this tab is the visible
## overlay tab, so a closed overlay costs the hosts nothing.
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
	{"id": "frame", "label": "Frame (wall)", "depth": 0, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.FRAME_WALL},
	{"id": "before", "label": "Player presenter (pre)", "depth": 1, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.FRAME_PLAYER_BEFORE},
	{"id": "world", "label": "World tick", "depth": 1, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.FRAME_WORLD},
	{"id": "foliage", "label": "Foliage", "depth": 2, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.WORLD_FOLIAGE},
	{"id": "runtime", "label": "Mission runtime", "depth": 2, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.WORLD_RUNTIME},
	{"id": "sim", "label": "Sim step", "depth": 3, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.SIM_STEP},
	{"id": "net", "label": "Net wire leg", "depth": 4, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.SIM_NET},
	{"id": "trace", "label": "Projectile trace (attributed)", "depth": 4,
			"kind": _KIND_GROUP,
			"slots": [FrameStatsBoard.TRACE_TERRAIN, FrameStatsBoard.TRACE_STATIC,
					FrameStatsBoard.TRACE_DYNAMIC, FrameStatsBoard.TRACE_PERSON]},
	{"id": "trace_terrain", "label": "Terrain", "depth": 5, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.TRACE_TERRAIN},
	{"id": "trace_static", "label": "Static", "depth": 5, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.TRACE_STATIC},
	{"id": "trace_dynamic", "label": "Dynamic", "depth": 5, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.TRACE_DYNAMIC},
	{"id": "trace_person", "label": "Person", "depth": 5, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.TRACE_PERSON},
	{"id": "effects_drain", "label": "Effects drain", "depth": 3, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.EFFECTS_DRAIN},
	{"id": "effects", "label": "Effects tick", "depth": 3, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.EFFECTS_TICK},
	{"id": "present", "label": "Present", "depth": 3, "kind": _KIND_GROUP,
			"slots": [FrameStatsBoard.PRESENT_SNAPSHOT, FrameStatsBoard.PRESENT_MISSION,
					FrameStatsBoard.PRESENT_WIRE, FrameStatsBoard.PRESENT_FIRE,
					FrameStatsBoard.PRESENT_DESTRUCTION, FrameStatsBoard.PRESENT_THROWABLE]},
	{"id": "snapshot", "label": "Row snapshot", "depth": 4, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.PRESENT_SNAPSHOT},
	{"id": "mission_rows", "label": "Mission rows", "depth": 4, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.PRESENT_MISSION},
	{"id": "wire_rows", "label": "Wire rows", "depth": 4, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.PRESENT_WIRE},
	{"id": "fire", "label": "Fire", "depth": 4, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.PRESENT_FIRE},
	{"id": "destruction", "label": "Destruction", "depth": 4, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.PRESENT_DESTRUCTION},
	{"id": "throwable", "label": "Throwable", "depth": 4, "kind": _KIND_SPAN,
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
	{"id": "occl_glue", "label": "Binding/glue", "depth": 3, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.OCCL_GLUE},
	{"id": "env", "label": "Env (weather/blink/iris)", "depth": 2, "kind": _KIND_GROUP,
			"slots": [FrameStatsBoard.WORLD_WEATHER, FrameStatsBoard.WORLD_BLINK,
					FrameStatsBoard.WORLD_IRIS]},
	{"id": "audio", "label": "Audio", "depth": 2, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.WORLD_AUDIO},
	{"id": "after", "label": "Player presenter (post)", "depth": 1, "kind": _KIND_SPAN,
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
	# The wall frame minus every measured shell leg: everything OUTSIDE our
	# spans — other nodes' _process, engine internals, render/present time on
	# this thread. When this row is large, the next slice hides here, not in
	# the spans above (this is the row that exposed the per-model _process
	# residual). Under an fps cap or vsync-enabled host the pacing wait also
	# lands here; the project runs uncapped with vsync off.
	{"id": "shell_residual", "label": "Outside shell spans", "depth": 1,
			"kind": _KIND_RESIDUAL, "base": FrameStatsBoard.FRAME_WALL,
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
var _overlay_visible := false
var _capture_active := false
var _refresh_count := 0
var _items: Dictionary = {}  # row id -> TreeItem


func page_id() -> StringName:
	return &"Stats"


func page_category() -> StringName:
	return CATEGORY_DIAGNOSTICS


func _build() -> void:
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
	stats_tree.set_column_title(1, "Avg")
	stats_tree.set_column_title(2, "Peak")
	stats_tree.set_column_title(3, "Info")
	stats_tree.set_column_title_tooltip_text(0, "Runtime system or subsystem")
	stats_tree.set_column_title_tooltip_text(1, "Average milliseconds per frame")
	stats_tree.set_column_title_tooltip_text(2, "Peak milliseconds in one frame")
	stats_tree.set_column_title_tooltip_text(3, "Live counters and runtime context")
	# Keep deep span nesting useful in the narrow dock: every cell clips with an
	# ellipsis, while row tooltips below retain the complete label and info.
	stats_tree.scroll_horizontal_enabled = false
	stats_tree.add_theme_constant_override("item_margin", 10)
	for column in range(stats_tree.columns):
		stats_tree.set_column_clip_content(column, true)
	# The 232 px floor leaves room for the Tree frame inside the 252 px page.
	# System and Info share wider docks; the numeric columns stay compact.
	stats_tree.set_column_expand(0, true)
	stats_tree.set_column_expand_ratio(0, 3)
	stats_tree.set_column_custom_minimum_width(0, 92)
	for column in [1, 2]:
		stats_tree.set_column_expand(column, false)
		stats_tree.set_column_custom_minimum_width(column, 42)
		stats_tree.set_column_title_alignment(column, HORIZONTAL_ALIGNMENT_RIGHT)
	stats_tree.set_column_expand(3, true)
	stats_tree.set_column_expand_ratio(3, 2)
	stats_tree.set_column_custom_minimum_width(3, 56)
	stats_tree.hide_root = true
	stats_tree.focus_mode = Control.FOCUS_ALL
	stats_tree.size_flags_vertical = Control.SIZE_EXPAND_FILL
	add_child(stats_tree)
	_build_rows()


func set_frame_stats_board(board: FrameStatsBoard) -> void:
	if board == _board:
		return
	if _board != null:
		_board.set_capture_active(false)
	_board = board
	_capture_active = false
	_refresh_count = 0
	status_label.text = "No frame stats source." if board == null else "Stats capture paused."
	_sync_capture()


## The overlay's explicit visibility edge: Controls under a hidden CanvasLayer
## don't all observe the layer hide, so the overlay tells us on toggle.
func set_capture_active(overlay_visible: bool) -> void:
	_overlay_visible = overlay_visible
	_sync_capture()


func _notification(what: int) -> void:
	if what == NOTIFICATION_EXIT_TREE:
		_overlay_visible = false
		if _board != null:
			_board.set_capture_active(false)
		_capture_active = false
		return
	# Tab switches flip this Control's own visibility.
	if what == NOTIFICATION_VISIBILITY_CHANGED:
		_sync_capture()


func is_capturing() -> bool:
	return _capture_active


func _sync_capture() -> void:
	var want := _board != null and _overlay_visible and visible and is_inside_tree()
	if want == _capture_active \
			and (_board == null or _board.is_capture_active() == want):
		return
	_capture_active = want
	_refresh_count = 0
	if _board != null:
		_board.set_capture_active(want)
	if want:
		status_label.text = "Capturing…"
		_clear_display_values()
	elif _board != null:
		status_label.text = "Stats capture paused."


## One overlay-cadence refresh. Reads the window only every _REFRESH_DIVIDER
## calls so displayed means cover ~0.5 s of frames.
func refresh() -> void:
	_sync_capture()
	if _board == null:
		status_label.text = "No frame stats source."
		return
	if not _capture_active:
		return
	_refresh_count += 1
	if _refresh_count % _REFRESH_DIVIDER != 0:
		return
	var window := _board.drain()
	if window.frames <= 0:
		return
	status_label.text = "Captured %d render frames; overlay cost is included." % window.frames
	var runtime: Object = _ctx.runtime() if _ctx != null else null
	var sim: Object = _ctx.sim() if _ctx != null else null
	render_window(window.frames, window.sums, window.peaks,
			window.sample_frames, runtime, sim)


## Stable, value-only observation seam for tests and automated probes.
func get_display_snapshot() -> Array[DebugStatsDisplayRow]:
	var out: Array[DebugStatsDisplayRow] = []
	for row_v in _ROWS:
		var row := row_v as Dictionary
		var item := _items[row["id"]] as TreeItem
		out.append(DebugStatsDisplayRow.new(
				StringName(row["id"]), String(row["label"]),
				item.get_text(1), item.get_text(2), item.get_text(3)))
	return out


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
					_set_metric(item, 1, "-")
					_set_metric(item, 2, "-")
				else:
					_set_metric(item, 1,
							"%.2f" % (float(sums[slot]) / 1000.0 / frames))
					_set_metric(item, 2,
							"%.2f" % (float(maxes[slot]) / 1000.0))
			_KIND_GROUP:
				var total := 0
				var seen := false
				for slot_v in row["slots"]:
					var slot := int(slot_v)
					total += sums[slot]
					seen = seen or counts[slot] > 0
				_set_metric(item, 1,
						"%.2f" % (float(total) / 1000.0 / frames) if seen else "-")
				_set_metric(item, 2, "")
			_KIND_RESIDUAL:
				var base_slot := int(row["base"])
				if counts[base_slot] <= 0:
					_set_metric(item, 1, "-")
				else:
					var residual := int(sums[base_slot])
					for slot_v in row["minus"]:
						residual -= sums[int(slot_v)]
					_set_metric(item, 1, "%.2f" %
							(float(maxi(residual, 0)) / 1000.0 / frames))
				_set_metric(item, 2, "")
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
		var label := String(row["label"])
		item.set_text(0, label)
		item.set_tooltip_text(0, label)
		item.set_text_overrun_behavior(0, TextServer.OVERRUN_TRIM_ELLIPSIS)
		item.set_text_overrun_behavior(3, TextServer.OVERRUN_TRIM_ELLIPSIS)
		_set_metric(item, 1, "-")
		_set_metric(item, 2, "-")
		for column in [1, 2]:
			item.set_text_alignment(column, HORIZONTAL_ALIGNMENT_RIGHT)
		_items[row["id"]] = item
		stack.push_back(item)


func _clear_display_values() -> void:
	for item_v in _items.values():
		var item := item_v as TreeItem
		_set_metric(item, 1, "-")
		_set_metric(item, 2, "-")
		item.set_text(3, "")
		item.set_tooltip_text(3, "")


func _set_metric(item: TreeItem, column: int, text: String) -> void:
	item.set_text(column, text)
	item.set_tooltip_text(column, text)


func _set_info(id: String, text: String) -> void:
	var item := _items[id] as TreeItem
	item.set_text(3, text)
	item.set_tooltip_text(3, text)


# The counter pulls: live Dictionaries/typed stats read at refresh cadence
# only, every source duck-typed and optional so SP, listen-host, joiner and
# harness stubs all render what they have.
func _refresh_info(sums: PackedInt64Array, counts: PackedInt32Array, frames: int,
		runtime: Object, sim: Object) -> void:
	# Sources are mission-scoped and can disappear between divided refreshes.
	# Clear every conditional cell first so reload/menu transitions cannot retain
	# counters from the previous world.
	for id in ["sim", "net", "trace", "effects", "fire", "destruction",
			"throwable", "wire_rows", "occl"]:
		_set_info(id, "")
	_set_info("frame", "%d fps" % int(Performance.get_monitor(Performance.TIME_FPS)))
	_set_info("render", "%d draws · %d objs · %s prims · %d nodes" % [
		int(Performance.get_monitor(Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME)),
		int(Performance.get_monitor(Performance.RENDER_TOTAL_OBJECTS_IN_FRAME)),
		_compact_count(int(Performance.get_monitor(
				Performance.RENDER_TOTAL_PRIMITIVES_IN_FRAME))),
		int(Performance.get_monitor(Performance.OBJECT_NODE_COUNT)),
	])

	var world: Object = _ctx.world() if _ctx != null else null

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

	if counts[FrameStatsBoard.TRACE_CALLS] > 0:
		_set_info("trace",
				"%.1f calls/f · S %.1f D %.1f P %.1f surv/f · %.1f/%.1f faces/f" % [
					float(sums[FrameStatsBoard.TRACE_CALLS]) / frames,
					float(sums[FrameStatsBoard.TRACE_STATIC_SURVIVORS]) / frames,
					float(sums[FrameStatsBoard.TRACE_DYNAMIC_SURVIVORS]) / frames,
					float(sums[FrameStatsBoard.TRACE_PERSON_SURVIVORS]) / frames,
					float(sums[FrameStatsBoard.TRACE_STATIC_FACES]) / frames,
					float(sums[FrameStatsBoard.TRACE_DYNAMIC_FACES]) / frames,
				])

	if world != null and world.has_method("get_effect_world"):
		var fx = world.get_effect_world()
		if fx != null and is_instance_valid(fx) and fx.has_method("active_entry_count"):
			var drain_ms := float(sums[FrameStatsBoard.EFFECTS_DRAIN]) / 1000.0 / frames
			_set_info("effects", "%d live · drain %.2f ms/f" % [
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
		var wire: WirePresentStats = runtime.get_wire_present_stats()
		if wire != null and (wire.live > 0 or wire.unresolved > 0):
			_set_info("wire_rows", "%d live · %d unresolved" % [
					wire.live, wire.unresolved])

	if sim != null and sim.has_method("get_occlusion_debug"):
		var occ: Dictionary = sim.get_occlusion_debug()
		if bool(occ.get("active", false)):
			var occ_counts: Dictionary = occ.get("counts", {})
			_set_info("occl", "%d bld · %d drawn · %d ent culled" % [
					int(occ_counts.get("instances", 0)),
					int(occ_counts.get("visible", 0)),
					int(occ_counts.get("culled_entities", 0))])


static func _compact_count(value: int) -> String:
	if value >= 1_000_000:
		return "%.1fM" % (value / 1_000_000.0)
	if value >= 10_000:
		return "%dk" % int(value / 1000.0)
	return str(value)
