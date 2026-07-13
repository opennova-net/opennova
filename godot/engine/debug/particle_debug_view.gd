extends Node3D

# Draws the live particle-effect world over the scene: one wireframe AABB per
# active emitter plus a billboarded effect-name label per spawn group. The 3D
# face of the F3 overlay's "Show effect boxes" toggle, mimicking the retail
# particle debug overlay's in-world boxes (the 2D pages are witnessed —
# ptl-format-re.md §11; retail's own box drawer is still an open witness, so
# the box GEOMETRY here is a diagnostics aid built from our emitters' render
# bounds, not engine-witnessed behavior).
#
# Colors: green = emitting, yellow = draining (finite window closing), red =
# the emitter carries authored-but-unresolved texture names (the D-PTL-14
# misses — exactly the layers that render nothing in-game).

const MissionOverlayUtil := preload("res://engine/mission/mission_overlay_util.gd")

const COLOR_EMITTING := Color(0.3, 1.0, 0.45)
const COLOR_DRAINING := Color(1.0, 0.9, 0.25)
const COLOR_UNRESOLVED := Color(1.0, 0.25, 0.2)

const BOX_EDGES := [
	[0, 1], [2, 3], [4, 5], [6, 7],
	[0, 2], [1, 3], [4, 6], [5, 7],
	[0, 4], [1, 5], [2, 6], [3, 7],
]

var _effect_world_source := Callable()
var _lines: ImmediateMesh
var _labels: Dictionary = {}  # group id -> Label3D, reused across refreshes


## `source` resolves the live NovaEffectWorld (or null) on every refresh —
## mission reloads free and rebuild the effect world, so a held reference
## would go stale (the collision-view contract).
func setup(source: Callable) -> void:
	_effect_world_source = source
	_lines = ImmediateMesh.new()
	var mi := MeshInstance3D.new()
	mi.name = "ParticleBoxLines"
	mi.mesh = _lines
	var mat := MissionOverlayUtil.line_material()
	mat.no_depth_test = true
	mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	mi.material_override = mat
	add_child(mi)


func _process(_delta: float) -> void:
	refresh_now()


func refresh_now() -> void:
	_lines.clear_surfaces()
	if not _effect_world_source.is_valid():
		_hide_stale_labels({})
		return
	var world = _effect_world_source.call()
	if world == null or not is_instance_valid(world):
		_hide_stale_labels({})
		return
	var seen := {}
	var segments: Array = []
	for group_v in world.get_debug_group_report():
		var group: Dictionary = group_v
		var unresolved: PackedStringArray = group.get("unresolved", PackedStringArray())
		var group_color := COLOR_UNRESOLVED if not unresolved.is_empty() \
				else (COLOR_EMITTING if bool(group.get("forever", false)) else COLOR_DRAINING)
		var group_center := Vector3.ZERO
		var group_boxes := 0
		for emitter_v in group.get("emitters", []):
			var emitter := (emitter_v as Dictionary).get("node") as Node3D
			if emitter == null or not is_instance_valid(emitter):
				continue
			var bounds := _emitter_world_bounds(emitter)
			if bounds.size == Vector3.ZERO:
				continue
			_append_box(segments, bounds, group_color)
			group_center += bounds.get_center()
			group_boxes += 1
		if group_boxes > 0:
			var id := int(group.get("id", 0))
			seen[id] = true
			_place_label(id, group, group_center / float(group_boxes), group_color)
	_hide_stale_labels(seen)
	if segments.is_empty():
		return
	_lines.surface_begin(Mesh.PRIMITIVE_LINES)
	for seg in segments:
		_lines.surface_set_color(seg[2])
		_lines.surface_add_vertex(seg[0])
		_lines.surface_set_color(seg[2])
		_lines.surface_add_vertex(seg[1])
	_lines.surface_end()


## Union of the emitter's per-layer render meshes in world space (the quads
## are built world-space at the top level, so the mesh AABBs ARE the bounds).
func _emitter_world_bounds(emitter: Node3D) -> AABB:
	var united := AABB()
	var first := true
	for child in emitter.get_children():
		var mi := child as MeshInstance3D
		if mi == null or mi.mesh == null:
			continue
		var local := mi.get_aabb()
		if local.size == Vector3.ZERO:
			continue
		var world_box := mi.global_transform * local
		if first:
			united = world_box
			first = false
		else:
			united = united.merge(world_box)
	return united


func _append_box(segments: Array, box: AABB, color: Color) -> void:
	var corners: Array = []
	for i in range(8):
		corners.append(box.position + Vector3(
				box.size.x if (i & 1) != 0 else 0.0,
				box.size.y if (i & 2) != 0 else 0.0,
				box.size.z if (i & 4) != 0 else 0.0))
	for edge in BOX_EDGES:
		segments.append([corners[edge[0]], corners[edge[1]], color])


func _place_label(id: int, group: Dictionary, at: Vector3, color: Color) -> void:
	var label: Label3D = _labels.get(id)
	if label == null:
		label = Label3D.new()
		label.billboard = BaseMaterial3D.BILLBOARD_ENABLED
		label.pixel_size = 0.004
		label.no_depth_test = true
		label.font_size = 40
		label.outline_size = 10
		label.outline_modulate = Color(0.0, 0.0, 0.0, 0.85)
		add_child(label)
		_labels[id] = label
	var text := "%02d  %s" % [id, String(group.get("name", ""))]
	var unresolved: PackedStringArray = group.get("unresolved", PackedStringArray())
	if not unresolved.is_empty():
		text += "\nmissing: " + ", ".join(unresolved)
	label.text = text
	label.modulate = color
	label.global_position = at
	label.visible = true


func _hide_stale_labels(seen: Dictionary) -> void:
	for id in _labels.keys():
		if not seen.has(id):
			var label: Label3D = _labels[id]
			if is_instance_valid(label):
				label.queue_free()
			_labels.erase(id)
