extends Node3D

# Draws the RoundSim debug ring over the scene: every recently resolved round
# outcome as its flight segment + hit marker, color-coded by outcome kind, with
# a detail label on the newest events. The 3D face of the F3 overlay's Rounds
# tab -- a developer window into OUR hit-detection port (the item CFAC face
# narrow phase, posed person bone spheres, sphere fallbacks, terrain stops,
# and the face-miss fly-ons), not retail-mimicked UI.
#
# Data comes from NovaSimulation.get_round_debug(): positions are already in
# Godot space, sections/faces/materials are the exact values the narrow phase
# resolved (libs/world round_sim.cpp ring). Rebuilds only when the ring
# advances. Built / freed by GameWorld on the overlay's "Show round trails"
# toggle, like the collision view.

const MissionOverlayUtil := preload("res://engine/mission/mission_overlay_util.gd")

# RoundDebugEvent::Kind -> color.
static func kind_color(kind: int) -> Color:
	match kind:
		0:
			return Color(0.2, 0.9, 1.0)    # organic hit - cyan
		1:
			return Color(0.3, 1.0, 0.45)   # item FACE hit - green
		2:
			return Color(1.0, 0.75, 0.2)   # item sphere stand-in - amber
		3:
			return Color(0.75, 0.6, 0.4)   # terrain stop - tan
		4:
			return Color(0.55, 0.55, 0.55) # expired / fuze - gray
		5:
			return Color(1.0, 0.25, 0.2)   # face-miss fly-on - red (the suspects)
		_:
			return Color(1.0, 0.0, 1.0)

const LABELED_NEWEST := 6      # detail labels on this many newest events
const SEGMENT_DIM := 0.45      # segment line brightness vs the hit marker

var _world: Node               # duck-typed host (get_sim()); re-resolved every frame
var _mesh: ImmediateMesh
var _labels: Array[Label3D] = []
var _signature := 0


func setup(world: Node) -> void:
	_world = world
	_mesh = ImmediateMesh.new()
	var mi := MeshInstance3D.new()
	mi.name = "RoundDebugLines"
	mi.mesh = _mesh
	var mat := MissionOverlayUtil.line_material()
	mat.no_depth_test = true
	mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	mi.material_override = mat
	add_child(mi)
	for i in range(LABELED_NEWEST):
		var lb := Label3D.new()
		lb.name = "RoundDebugLabel%d" % i
		lb.billboard = BaseMaterial3D.BILLBOARD_ENABLED
		lb.fixed_size = false
		lb.pixel_size = 0.005
		lb.no_depth_test = true
		lb.font_size = 40
		lb.outline_size = 10
		lb.outline_modulate = Color(0.0, 0.0, 0.0, 0.85)
		lb.visible = false
		add_child(lb)
		_labels.append(lb)


func _process(_delta: float) -> void:
	refresh_now()


## Immediately refresh from the current simulation (tests and tooling can call
## this without Godot's private frame callback).
func refresh_now() -> void:
	var sim := _resolve_sim()
	if sim == null:
		_clear_all()
		return
	var debug: Dictionary = sim.get_round_debug()
	var events: Array = debug.get("events", [])
	_update(events)


func _resolve_sim() -> Object:
	if _world == null or not is_instance_valid(_world) or not _world.has_method("get_sim"):
		return null
	var sim: Variant = _world.get_sim()
	if sim == null or not is_instance_valid(sim) or not (sim as Object).has_method("get_round_debug"):
		return null
	return sim


func _clear_all() -> void:
	_mesh.clear_surfaces()
	_signature = 0
	for lb in _labels:
		lb.visible = false


func _update(events: Array) -> void:
	# The ring only ever advances; the newest event's tick + count is a cheap
	# change signature.
	var sig_parts := [events.size()]
	if not events.is_empty():
		var newest: Dictionary = events[events.size() - 1]
		sig_parts.append(newest.get("tick", 0))
		sig_parts.append(newest.get("hit", Vector3.ZERO))
	var sig := hash(sig_parts)
	if sig == _signature:
		return
	_signature = sig

	_mesh.clear_surfaces()
	for lb in _labels:
		lb.visible = false
	if events.is_empty():
		return

	var segments: Array = []
	for i in range(events.size()):
		var ev: Dictionary = events[i]
		var kind := int(ev.get("kind", 4))
		var color := kind_color(kind)
		var p0: Vector3 = ev.get("p0", Vector3.ZERO)
		var p1: Vector3 = ev.get("p1", Vector3.ZERO)
		var hit: Vector3 = ev.get("hit", Vector3.ZERO)
		var seg_color := Color(color, SEGMENT_DIM)
		# The tick's flight segment, then the resolved stop (or graze point for
		# a face miss -- where the sphere said "maybe" and the faces said no).
		segments.append({ "a": p0, "b": p1, "color": seg_color })
		_cross(segments, hit, 0.18, color)
		if kind == 5:
			# Face misses ring the graze so they read at a glance.
			_diamond(segments, hit, 0.35, color)
	MissionOverlayUtil.emit_line_segments(_mesh, segments)

	# Detail labels, newest last (events arrive oldest -> newest).
	var label_i := 0
	var i2 := events.size() - 1
	while i2 >= 0 and label_i < _labels.size():
		var ev2: Dictionary = events[i2]
		var lb := _labels[label_i]
		lb.visible = true
		lb.position = (ev2.get("hit", Vector3.ZERO) as Vector3) + Vector3(0.0, 0.4, 0.0)
		lb.modulate = kind_color(int(ev2.get("kind", 4)))
		lb.text = _describe(ev2)
		label_i += 1
		i2 -= 1


static func _describe(ev: Dictionary) -> String:
	var kind := int(ev.get("kind", 4))
	var line := String(ev.get("kind_name", "?"))
	var ent := int(ev.get("entity_handle", 0xFFFF))
	if ent != 0xFFFF:
		var name := String(ev.get("entity_name", ""))
		line += "  ent %d/%d%s" % [(ent >> 12) & 0xF, ent & 0xFFF,
				("  " + name) if not name.is_empty() else ""]
	if bool(ev.get("husk", false)):
		line += "  HUSK"
	match kind:
		0:
			var primary := int(ev.get("section", -1))
			var secondary := int(ev.get("secondary_section", -1))
			if bool(ev.get("fallback", false)):
				line += "\nneutral fallback sphere  reaction stand-in %d  mat %d -> %s" % [
						primary, int(ev.get("material", 0)),
						String(ev.get("effect_tag_name", ""))]
			else:
				var secondary_text := "-" if secondary < 0 else str(secondary)
				line += "\nreaction bone %d  damage zone %s  mat %d -> %s" % [
						primary, secondary_text, int(ev.get("material", 0)),
						String(ev.get("effect_tag_name", ""))]
		1:
			line += "\nsec %d face %d  mat %d -> %s" % [int(ev.get("section", -1)),
					int(ev.get("face", -1)), int(ev.get("material", 0)),
					String(ev.get("effect_tag_name", ""))]
		2:
			line += "\nsphere stand-in -> %s" % String(ev.get("effect_tag_name", ""))
		5:
			line += "\ngraze t %.3f -- no face crossed" % float(ev.get("t", 0.0))
		_:
			pass
	return line


func _cross(segments: Array, at: Vector3, arm: float, color: Color) -> void:
	segments.append({ "a": at - Vector3(arm, 0, 0), "b": at + Vector3(arm, 0, 0), "color": color })
	segments.append({ "a": at - Vector3(0, arm, 0), "b": at + Vector3(0, arm, 0), "color": color })
	segments.append({ "a": at - Vector3(0, 0, arm), "b": at + Vector3(0, 0, arm), "color": color })


func _diamond(segments: Array, at: Vector3, r: float, color: Color) -> void:
	var px := at + Vector3(r, 0, 0)
	var nx := at + Vector3(-r, 0, 0)
	var pz := at + Vector3(0, 0, r)
	var nz := at + Vector3(0, 0, -r)
	segments.append({ "a": px, "b": pz, "color": color })
	segments.append({ "a": pz, "b": nx, "color": color })
	segments.append({ "a": nx, "b": nz, "color": color })
	segments.append({ "a": nz, "b": px, "color": color })
