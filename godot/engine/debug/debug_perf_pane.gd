class_name DebugPerfPane
extends VBoxContainer
## The debug overlay's Perf tab: the live per-frame budget (the world's tick
## counters), the PerfTimeline ring (recent mission loads as a span tree with
## per-stage milliseconds), plus a small set of live Performance monitors. A
## separate script from the overlay so tests drive it directly with fabricated
## timelines/counters. Host-neutral: engine deps only.

const _MONITORS := [
	["fps", "FPS", Performance.TIME_FPS, false],
	["frame_ms", "Frame time", Performance.TIME_PROCESS, true],
	["objects", "Objects", Performance.OBJECT_COUNT, false],
	["nodes", "Nodes", Performance.OBJECT_NODE_COUNT, false],
	["draw_calls", "Draw calls", Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME, false],
	["video_mem", "Video memory", Performance.RENDER_VIDEO_MEM_USED, false],
]

# The per-frame budget rows (GameWorld.get_runtime_perf_counters). Key -> label;
# the indented rows break the mission-runtime slice down (its sim/net/snapshot
# numbers ride the nested runtime/sim dictionaries).
const _FRAME_ROWS := [
	["tick_us", "World tick"],
	["foliage_us", "Foliage"],
	["runtime_us", "Mission runtime"],
	["sim_tick_us", "    Simulation"],
	["net_tick_us", "    Networking"],
	["present_snapshot_us", "    Entity snapshot"],
	["present_us", "    Scene update"],
	["audio_us", "Audio"],
]

var history_option: OptionButton
var span_tree: Tree
var monitor_labels: Dictionary = {}
var frame_labels: Dictionary = {}
var frame_status: Label

var _history: Array = []
# The timeline instance the tree currently shows: the span tree only rebuilds
# when this changes (a new load finished or the user picked another entry),
# never on the steady-state refresh.
var _shown: PerfTimeline = null


func _init() -> void:
	add_theme_constant_override("separation", 6)

	var frame_label := Label.new()
	frame_label.name = "PerfFrameLabel"
	frame_label.text = "This frame"
	add_child(frame_label)

	frame_status = Label.new()
	frame_status.name = "PerfFrameStatus"
	frame_status.text = "No world running."
	add_child(frame_status)

	for row in _FRAME_ROWS:
		var hbox := HBoxContainer.new()
		hbox.name = "PerfFrame_%s" % row[0]
		var name_label := Label.new()
		name_label.text = String(row[1])
		name_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		hbox.add_child(name_label)
		var value_label := Label.new()
		value_label.name = "Value"
		value_label.text = "—"
		hbox.add_child(value_label)
		add_child(hbox)
		frame_labels[row[0]] = value_label

	var loads_label := Label.new()
	loads_label.name = "PerfLoadsLabel"
	loads_label.text = "Recent loads"
	add_child(loads_label)

	history_option = OptionButton.new()
	history_option.name = "PerfHistory"
	history_option.focus_mode = Control.FOCUS_NONE
	history_option.item_selected.connect(_on_history_selected)
	add_child(history_option)

	span_tree = Tree.new()
	span_tree.name = "PerfSpans"
	span_tree.columns = 2
	span_tree.column_titles_visible = true
	span_tree.set_column_title(0, "Stage")
	span_tree.set_column_title(1, "Time")
	span_tree.set_column_expand(0, true)
	span_tree.set_column_expand(1, false)
	span_tree.set_column_custom_minimum_width(1, 84)
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
		name_label.text = String(monitor[1])
		name_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		row.add_child(name_label)
		var value_label := Label.new()
		value_label.name = "Value"
		row.add_child(value_label)
		add_child(row)
		monitor_labels[monitor[0]] = value_label


## One overlay-cadence refresh: monitors always, the ring + tree only when the
## retained history actually changed. The frame budget is pushed separately
## (render_frame_budget) by whoever holds the world.
func refresh() -> void:
	refresh_monitors()
	render_history(PerfTimeline.history())


## The live per-frame budget from GameWorld.get_runtime_perf_counters(). An
## empty Dictionary (no world wired / none running) dashes the rows out and
## shows the status line instead.
func render_frame_budget(counters: Dictionary) -> void:
	frame_status.visible = counters.is_empty()
	var runtime_v: Variant = counters.get("runtime", {})
	var runtime: Dictionary = runtime_v if runtime_v is Dictionary else {}
	var sim_v: Variant = runtime.get("sim", {})
	var sim: Dictionary = sim_v if sim_v is Dictionary else {}
	var values := {
		"tick_us": counters.get("tick_us"),
		"foliage_us": counters.get("foliage_us"),
		"runtime_us": counters.get("runtime_us"),
		"audio_us": counters.get("audio_us"),
		"present_us": runtime.get("present_us"),
		"sim_tick_us": sim.get("sim_tick_us"),
		"net_tick_us": sim.get("net_tick_us"),
		"present_snapshot_us": sim.get("present_snapshot_us"),
	}
	for key in frame_labels:
		var label: Label = frame_labels[key]
		var v: Variant = values.get(key)
		label.text = _fmt_us(int(v)) if v != null else "—"


# Microseconds for the sub-millisecond slices, milliseconds above — a 62.5 Hz
# frame budget is 16000 µs, so both scales appear side by side.
static func _fmt_us(us: int) -> String:
	return ("%.2f ms" % (us / 1000.0)) if us >= 1000 else ("%d µs" % us)


func refresh_monitors() -> void:
	for monitor in _MONITORS:
		var value := Performance.get_monitor(int(monitor[2]))
		var label: Label = monitor_labels[monitor[0]]
		if String(monitor[0]) == "video_mem":
			label.text = "%.1f MB" % (value / (1024.0 * 1024.0))
		elif bool(monitor[3]):
			label.text = "%.1f ms" % (value * 1000.0)
		else:
			label.text = str(int(value))


## history: most-recent-first PerfTimeline array (the ring's shape). Keeps the
## current selection when its timeline is still retained; defaults to newest.
func render_history(history: Array) -> void:
	if _same_history(history):
		_render_selected()
		return
	_history = history.duplicate()
	history_option.clear()
	for timeline in _history:
		history_option.add_item((timeline as PerfTimeline).summary(2))
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
	total.set_text(0, timeline.label if not timeline.label.is_empty() else "total")
	total.set_text(1, _fmt_duration(timeline.total_ms()))
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
		item.set_text(0, String(span["name"]))
		var end_us := int(span["end_us"])
		if end_us > 0:
			item.set_text(1, _fmt_duration(float(end_us - int(span["start_us"])) / 1000.0))
		else:
			item.set_text(1, "…")
		stack.push_back(item)
	# Collapsing is the reader's choice; start fully expanded.
	total.set_collapsed_recursive(false)


# Seconds for the big stages, milliseconds below - the same convention the
# ring's summary labels use, so the tree and the dropdown agree (a 50s load's
# stages would otherwise clip the fixed time column).
static func _fmt_duration(ms: float) -> String:
	return ("%.1f s" % (ms / 1000.0)) if ms >= 1000.0 else ("%.1f ms" % ms)


func _render_selected() -> void:
	var index := history_option.selected
	if index < 0 or index >= _history.size():
		render_timeline(null)
		return
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
