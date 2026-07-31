extends Node3D

# Draws the live particle-effect world over the scene: one wireframe AABB per
# active emitter plus a billboarded effect-name label per spawn group. The 3D
# face of the F3 overlay's "Show effect boxes" toggle, mimicking the retail
# particle debug boxes: retail draws every emitter's accumulated AABB in
# opaque red inside the particle render pass [orig: CParticleManager_RenderBatch
# @ 0x5e9890 gate mgr[182] -> Render_DrawDebugBoundingBox @ 0x5e06e0, color
# 0xFFFF0000; ptl-format-re.md §11]. Bounds come from draw-packet values (the
# same rendered coverage F3 displays, without walking renderer Nodes). Labels
# are our diagnostic extension; catalog-wide missing textures stay in the F3
# list instead of being assigned to arbitrary live groups. Retail's boxes carry
# no text.

const MissionOverlayUtil := preload("res://engine/mission/mission_overlay_util.gd")

# Retail's box color [orig: 0xFFFF0000 at both RenderBatch call sites].
const COLOR_BOX := Color(1.0, 0.0, 0.0)
const LABEL_FONT_SIZE := 40
const LABEL_OUTLINE_SIZE := 10

const BOX_EDGES := [
	[0, 1], [2, 3], [4, 5], [6, 7],
	[0, 2], [1, 3], [4, 6], [5, 7],
	[0, 4], [1, 5], [2, 6], [3, 7],
]

var _effect_world_source := Callable()
var _lines: ImmediateMesh
var _labels: Dictionary = {}  # group id -> Label3D, reused across refreshes
var _drawable_count := 0      # live emitters with non-empty bounds


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
	_drawable_count = 0
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
		var group_color := COLOR_BOX
		var group_center := Vector3.ZERO
		var group_boxes := 0
		for emitter_v in group.get("emitters", []):
			var bounds: AABB = (emitter_v as Dictionary).get("bounds", AABB())
			if bounds.size == Vector3.ZERO:
				continue
			_append_box(segments, bounds, group_color)
			group_center += bounds.get_center()
			group_boxes += 1
			_drawable_count += 1
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


## Number of live emitter bounds currently contributing geometry.
func get_debug_drawable_count() -> int:
	return _drawable_count


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
		label.font_size = LABEL_FONT_SIZE
		label.outline_size = LABEL_OUTLINE_SIZE
		label.outline_modulate = Color(0.0, 0.0, 0.0, 0.85)
		add_child(label)
		_labels[id] = label
	var text := "%02d  %s" % [id, String(group.get("name", ""))]
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
