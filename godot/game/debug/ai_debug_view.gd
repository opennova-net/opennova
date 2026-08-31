extends SimDebugView

# Draws the AI system's live decision state over the world: a state/alert
# label above every nearby brain, the nav-channel routes with each follower's
# current node highlighted, the AI -> combat-target and aim-direction lines,
# and the perception rings (sight + attack range) for the selected and
# engaged brains. The 3D face of the F3 AI window -- a developer window into
# OUR AI port, not retail-mimicked UI.
#
# Data comes from Simulation.get_ai_debug(): positions and aim directions are
# already in Godot space (the engine join world::inspect::ai_debug_report).
# Element flags arrive from DebugViewSet (retained across reloads); the
# selection arrives through an injected provider Callable so tests drive it
# with a lambda. Built / freed by GameWorld on the "Show AI overlay" master
# toggle, like the collision view.

const LABEL_MAX := 64          # pooled labels; nearest brains win
const LABEL_RANGE := 150.0     # meters from the camera a label stays readable
const RING_SEGMENTS := 32
const RING_MAX := 24           # perception rings beyond the selection
const LABEL_LIFT := 2.2        # meters above the entity origin
const AIM_RAY_LENGTH := 10.0
const SEGMENT_DIM := 0.45      # route line brightness vs the node markers

const TARGET_COLOR := Color(1.0, 0.3, 0.25)
const AIM_COLOR := Color(0.3, 0.9, 1.0)
const DEAD_COLOR := Color(0.55, 0.55, 0.55)

var _route_mesh: ImmediateMesh
var _target_mesh: ImmediateMesh
var _ring_mesh: ImmediateMesh
var _labels: Array[Label3D] = []
var _drawable_count := 0

# Element flags (the F3 strip / debug-control rows; master build/free lives in
# DebugViewSet).
var _labels_on := true
var _routes_on := true
var _targets_on := true
var _rings_on := true
var _selection_provider := Callable()  # () -> int packed handle; -1 = none


static func alert_color(alert: int) -> Color:
	match alert:
		1:
			return Color(1.0, 0.85, 0.25)  # yellow
		2:
			return Color(1.0, 0.3, 0.25)   # red
		_:
			return Color(0.35, 1.0, 0.45)  # green


func _build_view() -> void:
	_route_mesh = ImmediateMesh.new()
	add_child(_make_lines_node("AiDebugRoutes", _route_mesh))
	_target_mesh = ImmediateMesh.new()
	add_child(_make_lines_node("AiDebugTargets", _target_mesh))
	_ring_mesh = ImmediateMesh.new()
	add_child(_make_lines_node("AiDebugRings", _ring_mesh))
	for i in range(LABEL_MAX):
		var lb := _make_overlay_label("AiDebugLabel%d" % i, 0.005, 40, 10)
		add_child(lb)
		_labels.append(lb)


## Element visibility (labels / routes / targets / rings), retained by
## DebugViewSet and forwarded here on every change.
func set_elements(labels_on: bool, routes_on: bool, targets_on: bool,
		rings_on: bool) -> void:
	_labels_on = labels_on
	_routes_on = routes_on
	_targets_on = targets_on
	_rings_on = rings_on


## The F3 selection, lent as a Callable (() -> packed wire handle, -1 = none)
## so the view never reaches into the dev tools.
func set_selection_provider(provider: Callable) -> void:
	_selection_provider = provider


func _refresh_from_sim(sim: Simulation) -> void:
	render_report(sim.get_ai_debug())


func _clear_all() -> void:
	_drawable_count = 0
	if _route_mesh != null:
		_route_mesh.clear_surfaces()
	if _target_mesh != null:
		_target_mesh.clear_surfaces()
	if _ring_mesh != null:
		_ring_mesh.clear_surfaces()
	for lb in _labels:
		lb.visible = false


## Number of brains + routes currently drawn.
func get_debug_drawable_count() -> int:
	return _drawable_count


func _selected_handle() -> int:
	if not _selection_provider.is_valid():
		return -1
	var value: Variant = _selection_provider.call()
	return int(value) if value is int else -1


## Render one AI snapshot (Simulation.get_ai_debug's shape). Split from the
## sim fetch so tests and probes can drive the view with report data directly.
func render_report(debug: Dictionary) -> void:
	_clear_all()
	if not bool(debug.get("valid", false)):
		return
	var rows: Array = debug.get("rows", [])
	var channels: Array = debug.get("channels", [])
	_drawable_count = rows.size() + channels.size()

	var camera: Camera3D = null
	if is_inside_tree():
		camera = get_viewport().get_camera_3d()
	var cam_pos := camera.global_position if camera != null else Vector3.ZERO
	var selected := _selected_handle()

	if _labels_on:
		_render_labels(rows, camera, cam_pos)
	if _routes_on:
		_render_routes(rows, channels)
	if _targets_on:
		_render_targets(rows)
	if _rings_on:
		_render_rings(rows, camera, cam_pos, selected)


func _render_labels(rows: Array, camera: Camera3D, cam_pos: Vector3) -> void:
	var label_i := 0
	for raw in rows:
		if label_i >= _labels.size():
			break
		var row: Dictionary = raw
		var pos: Vector3 = row.get("pos", Vector3.ZERO)
		# Distance-gate only when a camera exists (headless tests have none).
		if camera != null and cam_pos.distance_to(pos) > LABEL_RANGE:
			continue
		var lb := _labels[label_i]
		label_i += 1
		lb.visible = true
		lb.position = pos + Vector3(0.0, LABEL_LIFT, 0.0)
		var alive := bool(row.get("alive", false))
		lb.modulate = alert_color(int(row.get("alert", 0))) if alive else DEAD_COLOR
		lb.text = _label_text(row)


static func _label_text(row: Dictionary) -> String:
	var row_name := String(row.get("name", ""))
	var text := row_name if not row_name.is_empty() \
			else "ai %d" % int(row.get("ai_index", -1))
	if not bool(row.get("alive", false)):
		return text + "\nDEAD"
	# ai_state_name is "?" for the unnamed gaps (state 0 = the infantry
	# motor's SM default); the number reads better than a bare "?".
	var state_name := String(row.get("state_name", ""))
	if state_name.is_empty() or state_name == "?":
		state_name = "state %d" % int(row.get("state", 0))
	text += "\n%s" % state_name
	if bool(row.get("infantry", false)):
		text += "  m%d" % int(row.get("move_mode", 0))
	var speed := int(row.get("out_speed", 0))
	if speed != 0:
		text += "  spd %d" % speed
	var channel := int(row.get("wp_channel", 0))
	if channel > 0:
		text += "\nch %d node %d" % [channel, int(row.get("wp_node", 0))]
	if bool(row.get("target_valid", false)):
		var target := String(row.get("target_name", ""))
		text += "\n-> %s" % (target if not target.is_empty() else "?")
		var fire_delay := int(row.get("fire_delay", 0))
		if fire_delay > 0:
			text += "  fd %d" % fire_delay
	return text


func _render_routes(rows: Array, channels: Array) -> void:
	var segments: Array = []
	# Current node per follower needs the channel's node run by channel index.
	var nodes_by_channel: Dictionary = {}
	for raw in channels:
		var channel: Dictionary = raw
		var index := int(channel.get("index", 0))
		var nodes: PackedVector3Array = channel.get("nodes", PackedVector3Array())
		nodes_by_channel[index] = nodes
		if nodes.size() == 0:
			continue
		# Only routes something is walking: a mission authors far more
		# channels than its brains use (the F3 AI window's table lists all).
		if int(channel.get("followers", 0)) <= 0:
			continue
		var color := Color.from_hsv(IndexHue.hue_for_index(index), 0.75, 1.0)
		var line_color := Color(color, SEGMENT_DIM)
		for k in range(nodes.size() - 1):
			segments.append({ "a": nodes[k], "b": nodes[k + 1], "color": line_color })
		if not bool(channel.get("once", false)) and nodes.size() > 2:
			# A looping path closes back on node 0.
			segments.append({ "a": nodes[nodes.size() - 1], "b": nodes[0],
					"color": line_color })
		var radii: PackedFloat32Array = channel.get("radii", PackedFloat32Array())
		for k2 in range(nodes.size()):
			var r := radii[k2] if k2 < radii.size() else 0.5
			_diamond(segments, nodes[k2], maxf(r, 0.35), color)
	# Each follower's current node: a bright marker + a line from the brain.
	for raw2 in rows:
		var row: Dictionary = raw2
		var channel_id := int(row.get("wp_channel", 0))
		if channel_id <= 0 or not nodes_by_channel.has(channel_id):
			continue
		var run: PackedVector3Array = nodes_by_channel[channel_id]
		var node_i := int(row.get("wp_node", 0))
		if node_i < 0 or node_i >= run.size():
			continue
		var color2 := Color.from_hsv(IndexHue.hue_for_index(channel_id), 0.75, 1.0)
		segments.append({ "a": row.get("pos", Vector3.ZERO), "b": run[node_i],
				"color": Color(color2, 0.8) })
		_cross(segments, run[node_i], 0.5, color2)
	MissionOverlayUtil.emit_line_segments(_route_mesh, segments)


func _render_targets(rows: Array) -> void:
	var segments: Array = []
	for raw in rows:
		var row: Dictionary = raw
		if not bool(row.get("alive", false)):
			continue
		var pos: Vector3 = row.get("pos", Vector3.ZERO)
		var eye := pos + Vector3(0.0, 1.5, 0.0)
		if bool(row.get("target_valid", false)):
			segments.append({ "a": eye, "b": row.get("target_pos", Vector3.ZERO),
					"color": Color(TARGET_COLOR, 0.6) })
		if bool(row.get("muzzle_valid", false)):
			_cross(segments, row.get("muzzle", Vector3.ZERO), 0.15, AIM_COLOR)
		if bool(row.get("aim_valid", false)):
			var origin: Vector3 = row.get("muzzle", Vector3.ZERO) \
					if bool(row.get("muzzle_valid", false)) else eye
			var dir: Vector3 = row.get("aim_dir", Vector3.ZERO)
			if dir != Vector3.ZERO:
				segments.append({ "a": origin, "b": origin + dir * AIM_RAY_LENGTH,
						"color": AIM_COLOR })
	MissionOverlayUtil.emit_line_segments(_target_mesh, segments)


func _render_rings(rows: Array, camera: Camera3D, cam_pos: Vector3,
		selected: int) -> void:
	var segments: Array = []
	var ring_count := 0
	for raw in rows:
		var row: Dictionary = raw
		if not bool(row.get("alive", false)):
			continue
		var handle := int(row.get("handle", -1))
		var is_selected := selected >= 0 and handle == selected
		# Rings for the selection always; otherwise only engaged brains near
		# the camera, capped -- a full battlefield of circles is noise.
		if not is_selected:
			if not bool(row.get("target_valid", false)):
				continue
			if ring_count >= RING_MAX:
				continue
			var pos_check: Vector3 = row.get("pos", Vector3.ZERO)
			if camera != null and cam_pos.distance_to(pos_check) > LABEL_RANGE:
				continue
		ring_count += 1
		var pos: Vector3 = row.get("pos", Vector3.ZERO)
		var color := alert_color(int(row.get("alert", 0)))
		var sight := float(row.get("sight_range", 0.0))
		var attack := float(row.get("attack_range", 0.0))
		if sight > 0.0:
			_ring(segments, pos, sight, Color(color, 0.25))
		if attack > 0.0:
			_ring(segments, pos, attack, Color(color, 0.7))
	MissionOverlayUtil.emit_line_segments(_ring_mesh, segments)


static func _ring(segments: Array, at: Vector3, radius: float, color: Color) -> void:
	var prev := at + Vector3(radius, 0.0, 0.0)
	for k in range(1, RING_SEGMENTS + 1):
		var angle := TAU * float(k) / float(RING_SEGMENTS)
		var next := at + Vector3(cos(angle) * radius, 0.0, sin(angle) * radius)
		segments.append({ "a": prev, "b": next, "color": color })
		prev = next
