class_name PickDebugView
extends SimDebugView

# Highlights the debug pick list over the world: per pick a bound-radius
# diamond + axis cross at the entity origin, a small cross at the exact hit
# point, and a name label, each row in its own hue (matching the overlay's
# picks section). Built / freed by the host like every SimDebugView.
#
# One deliberate deviation from the base contract: the row DATA comes from
# the injected host-owned DebugPickList, not a sim accessor — the sim's
# get_world_entity_debug is only used to re-resolve mover positions each
# refresh, so a picked vehicle keeps its highlight while driving. Statics
# (net_id 0) keep their pick-time position; they cannot move.

const LABEL_LIFT := 0.6

var _pick_list: DebugPickList = null
var _mesh: ImmediateMesh
var _labels: Array[Label3D] = []


func set_pick_list(pick_list: DebugPickList) -> void:
	_pick_list = pick_list


func _build_view() -> void:
	_mesh = ImmediateMesh.new()
	add_child(_make_lines_node("PickDebugLines", _mesh))
	for i in range(DebugPickList.MAX_PICKS):
		var lb := _make_overlay_label("PickDebugLabel%d" % i, 0.005, 40, 10)
		add_child(lb)
		_labels.append(lb)


func _refresh_from_sim(sim: Simulation) -> void:
	var live_positions := {}
	if _pick_list != null:
		for pick_v in _pick_list.get_picks():
			var net_id := int((pick_v as Dictionary).get("net_id", 0))
			if net_id <= 0 or live_positions.has(net_id):
				continue
			# Movers re-resolve so the highlight follows the live entity.
			var card: Dictionary = sim.get_world_entity_debug(net_id)
			if card.has("position"):
				live_positions[net_id] = card["position"]
	render_picks(live_positions)


## Draw the pick list; live_positions maps net_id -> live Vector3 for movers
## whose entity still resolves. Split from the sim fetch so tests and probes
## can drive the view with position data directly.
func render_picks(live_positions: Dictionary) -> void:
	_mesh.clear_surfaces()
	for lb in _labels:
		lb.visible = false
	if _pick_list == null:
		return
	var picks := _pick_list.get_picks()
	if picks.is_empty():
		return
	var segments: Array = []
	for i in range(mini(picks.size(), _labels.size())):
		var pick: Dictionary = picks[i]
		var color := Color.from_hsv(IndexHue.hue_for_index(i), 0.75, 1.0)
		var origin: Vector3 = pick.get("position_godot", Vector3.ZERO)
		var net_id := int(pick.get("net_id", 0))
		if net_id > 0 and live_positions.has(net_id):
			origin = live_positions[net_id]
		var radius := maxf(float(pick.get("bound_radius", 0.0)), 0.75)
		_diamond(segments, origin, radius, color)
		_cross(segments, origin, 0.3, color)
		_cross(segments, pick.get("hit_position_godot", Vector3.ZERO), 0.18,
				Color(color, 0.7))
		var lb := _labels[i]
		lb.visible = true
		lb.position = origin + Vector3(0.0, radius + LABEL_LIFT, 0.0)
		lb.modulate = color
		lb.text = describe_pick(pick)
	MissionOverlayUtil.emit_line_segments(_mesh, segments)


func _clear_all() -> void:
	if _mesh != null:
		_mesh.clear_surfaces()
	for lb in _labels:
		lb.visible = false


static func describe_pick(pick: Dictionary) -> String:
	var name := String(pick.get("name", ""))
	var line := name if not name.is_empty() else String(pick.get("hit_class", "entity"))
	var bms_id := int(pick.get("bms_id", 0))
	if bms_id != 0:
		line += "  #%d" % bms_id
	line += "  (%d:%d)" % [int(pick.get("kind", -1)), int(pick.get("index", -1))]
	return line
