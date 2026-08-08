class_name DebugPerfPage
extends NovaDebugPage
## The debug overlay's Perf page: renders the PerfTimeline ring (recent mission
## loads as a span tree with per-stage milliseconds) plus a small set of live
## Performance monitors. A separate script from the overlay so tests drive it
## directly with fabricated timelines. Shell-neutral: engine deps only.

const MONITOR_VALUE_WIDTH := 96.0

const _MONITORS := [
	["fps", "FPS", Performance.TIME_FPS, false],
	["frame_ms", "Frame time", Performance.TIME_PROCESS, true],
	["physics_ms", "Physics time", Performance.TIME_PHYSICS_PROCESS, true],
	["navigation_ms", "Navigation time", Performance.TIME_NAVIGATION_PROCESS, true],
	["audio_latency", "Audio latency", Performance.AUDIO_OUTPUT_LATENCY, true],
	["objects", "Objects", Performance.OBJECT_COUNT, false],
	["nodes", "Nodes", Performance.OBJECT_NODE_COUNT, false],
	["resources", "Resources", Performance.OBJECT_RESOURCE_COUNT, false],
	["orphans", "Orphan nodes", Performance.OBJECT_ORPHAN_NODE_COUNT, false],
	["render_objects", "Rendered objects", Performance.RENDER_TOTAL_OBJECTS_IN_FRAME, false],
	["primitives", "Primitives", Performance.RENDER_TOTAL_PRIMITIVES_IN_FRAME, false],
	["draw_calls", "Draw calls", Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME, false],
	["physics_objects", "Physics 3D objects", Performance.PHYSICS_3D_ACTIVE_OBJECTS, false],
	["physics_pairs", "Physics 3D pairs", Performance.PHYSICS_3D_COLLISION_PAIRS, false],
	["nav_maps", "Navigation maps", Performance.NAVIGATION_ACTIVE_MAPS, false],
	["nav_regions", "Navigation regions", Performance.NAVIGATION_REGION_COUNT, false],
	["nav_agents", "Navigation agents", Performance.NAVIGATION_AGENT_COUNT, false],
	["memory_static", "Static memory", Performance.MEMORY_STATIC, false],
	["video_mem", "Video memory", Performance.RENDER_VIDEO_MEM_USED, false],
	["texture_mem", "Texture memory", Performance.RENDER_TEXTURE_MEM_USED, false],
	["buffer_mem", "Buffer memory", Performance.RENDER_BUFFER_MEM_USED, false],
]

var history_option: OptionButton
var span_tree: Tree
var monitor_labels: Dictionary = {}

var _history: Array = []
# The timeline instance the tree currently shows: the span tree only rebuilds
# when this changes (a new load finished or the user picked another entry),
# never on the steady-state refresh.
var _shown: PerfTimeline = null


func page_id() -> StringName:
	return &"Perf"


func page_category() -> StringName:
	return CATEGORY_DIAGNOSTICS


func _build() -> void:
	add_theme_constant_override("separation", 6)

	var loads_label := Label.new()
	loads_label.name = "PerfLoadsLabel"
	loads_label.text = "Recent loads"
	add_child(loads_label)

	history_option = OptionButton.new()
	history_option.name = "PerfHistory"
	history_option.fit_to_longest_item = false
	history_option.clip_text = true
	history_option.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS
	history_option.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	history_option.focus_mode = Control.FOCUS_NONE
	history_option.item_selected.connect(_on_history_selected)
	add_child(history_option)

	span_tree = Tree.new()
	span_tree.name = "PerfSpans"
	span_tree.columns = 2
	span_tree.column_titles_visible = true
	span_tree.set_column_title(0, "Stage")
	span_tree.set_column_title(1, "Time")
	span_tree.set_column_title_tooltip_text(0, "Mission-load stage")
	span_tree.set_column_title_tooltip_text(1, "Elapsed stage time")
	span_tree.scroll_horizontal_enabled = false
	for column in range(span_tree.columns):
		span_tree.set_column_clip_content(column, true)
	span_tree.set_column_expand(0, true)
	span_tree.set_column_expand(1, false)
	span_tree.set_column_custom_minimum_width(1, 92)
	span_tree.set_column_title_alignment(1, HORIZONTAL_ALIGNMENT_RIGHT)
	span_tree.hide_root = true
	span_tree.size_flags_vertical = Control.SIZE_EXPAND_FILL
	add_child(span_tree)

	var monitors_label := Label.new()
	monitors_label.name = "PerfMonitorsLabel"
	monitors_label.text = "Right now"
	add_child(monitors_label)

	for monitor in _MONITORS:
		var row := HBoxContainer.new()
		row.name = "PerfMonitor_%s" % monitor[0]
		var name_label := Label.new()
		name_label.name = "Name"
		name_label.text = String(monitor[1])
		name_label.tooltip_text = name_label.text
		name_label.clip_text = true
		name_label.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS
		name_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		row.add_child(name_label)
		var value_label := Label.new()
		value_label.name = "Value"
		value_label.custom_minimum_size.x = MONITOR_VALUE_WIDTH
		value_label.clip_text = true
		value_label.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS
		value_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
		row.add_child(value_label)
		add_child(row)
		monitor_labels[monitor[0]] = value_label


## One overlay-cadence refresh: monitors always, the ring + tree only when the
## retained history actually changed.
func refresh() -> void:
	refresh_monitors()
	render_history(PerfTimeline.history())


func refresh_monitors() -> void:
	for monitor in _MONITORS:
		var value := Performance.get_monitor(int(monitor[2]))
		var label: Label = monitor_labels[monitor[0]]
		if String(monitor[0]).ends_with("_mem") \
				or String(monitor[0]) == "memory_static":
			label.text = "%.1f MB" % (value / (1024.0 * 1024.0))
		elif bool(monitor[3]):
			# The live gauge keeps a FIXED unit (a 1.4 s headless frame still
			# reads "1400.0 ms"): a gauge that switches units mid-watch is
			# harder to track than one long number. Span DURATIONS render via
			# PerfTimeline.format_ms; this row is a different concern.
			label.text = "%.1f ms" % (value * 1000.0)
		else:
			label.text = str(int(value))
		label.tooltip_text = label.text


## history: most-recent-first PerfTimeline array (the ring's shape). Keeps the
## current selection when its timeline is still retained; defaults to newest.
func render_history(history: Array) -> void:
	if _same_history(history):
		_render_selected()
		return
	_history = history.duplicate()
	history_option.clear()
	for timeline in _history:
		var summary := (timeline as PerfTimeline).summary(2)
		history_option.add_item(summary)
		history_option.set_item_tooltip(history_option.item_count - 1, summary)
	var keep := _history.find(_shown)
	if keep >= 0:
		history_option.select(keep)
	elif not _history.is_empty():
		history_option.select(0)
	_render_selected()


func render_timeline(timeline: PerfTimeline) -> void:
	if timeline == _shown:
		return
	_shown = timeline
	span_tree.clear()
	if timeline == null:
		return
	var root := span_tree.create_item()
	var total := span_tree.create_item(root)
	_set_span_row(
			total,
			timeline.label if not timeline.label.is_empty() else "total",
			PerfTimeline.format_ms(timeline.total_ms()))
	# Parent each span by walking the depth stack: a span of depth d nests
	# under the most recent span of depth d-1.
	var stack: Array = [total]
	for span_value in timeline.spans():
		var span := span_value as Dictionary
		var depth := int(span["depth"])
		while stack.size() > depth + 1:
			stack.pop_back()
		var parent: TreeItem = stack.back()
		var item := span_tree.create_item(parent)
		var end_us := int(span["end_us"])
		var duration := "…"
		if end_us > 0:
			duration = PerfTimeline.format_ms(
					float(end_us - int(span["start_us"])) / 1000.0)
		_set_span_row(item, String(span["name"]), duration)
		stack.push_back(item)
	# Collapsing is the reader's choice; start fully expanded.
	total.set_collapsed_recursive(false)


func _set_span_row(item: TreeItem, stage: String, duration: String) -> void:
	item.set_text(0, stage)
	item.set_tooltip_text(0, stage)
	item.set_text_overrun_behavior(0, TextServer.OVERRUN_TRIM_ELLIPSIS)
	item.set_text(1, duration)
	item.set_tooltip_text(1, duration)
	item.set_text_alignment(1, HORIZONTAL_ALIGNMENT_RIGHT)
	item.set_text_overrun_behavior(1, TextServer.OVERRUN_TRIM_ELLIPSIS)


func _render_selected() -> void:
	var index := history_option.selected
	if index < 0 or index >= _history.size():
		history_option.tooltip_text = ""
		render_timeline(null)
		return
	history_option.tooltip_text = history_option.get_item_text(index)
	render_timeline(_history[index])


func _on_history_selected(_index: int) -> void:
	_render_selected()


func _same_history(history: Array) -> bool:
	if history.size() != _history.size():
		return false
	for i in range(history.size()):
		if history[i] != _history[i]:
			return false
	return true
