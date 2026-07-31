extends SimDebugView

# Draws the round hit-detection reality over the scene: nearby non-organic CFAC
# bullet-mesh wireframes (the triangles Physics_RaycastAgainstBoneCollision
# walks), colored by face material, plus their broad-phase bound spheres and
# husk state. Nearby pool-0 organic posed bone spheres share the 80-unit /
# 96-target debug budget and omit the local avatar. The 3D face of the F3
# Rounds tab's "Show hit meshes" toggle.
#
# Geometry comes from NovaSimulation.get_hitbox_debug(): triangles are
# transformed in C++ through the SAME husk-aware target_view + full-euler
# placement matrices the projectile raycast uses, so the drawn mesh IS what
# rounds resolve against. Amber spheres mark entities with NO face mesh —
# there the bound sphere alone decides hits (D-ITEM-1's stand-in). Cyan
# organic spheres show the exact per-section narrow phase and normal-infantry
# damage table: orange x1.25 (0-4), cyan x1.0 (5-8), lime x0.5
# (9-12/15-18), magenta x3.0 head (13-14), dark red masked sections, and
# amber only for the unresolved neutral-damage fallback. Built / freed by
# GameWorld on the overlay toggle, the collision-view contract.

const LABEL_NEAREST := 12       # detail labels on this many nearest entities
const SPHERE_SEGMENTS := 20

const ORGANIC_BODY_COLOR := Color(0.2, 0.9, 1.0, 0.9)
const ORGANIC_HEAVY_COLOR := Color(1.0, 0.58, 0.18, 0.95)
const ORGANIC_HEAD_COLOR := Color(1.0, 0.25, 0.85, 1.0)
const ORGANIC_LIMB_COLOR := Color(0.45, 1.0, 0.25, 0.95)
const ORGANIC_MASKED_COLOR := Color(0.45, 0.08, 0.08, 0.8)
const ORGANIC_FALLBACK_COLOR := Color(1.0, 0.65, 0.15, 0.95)

# Face flags that change the read of a triangle.
const FLAG_NEVER_HIT := 0x100   # authored never-hit — rounds ignore it
const FLAG_DOUBLE_SIDED := 0x800
const FLAG_BOTH_SIDES := 0x1

var _mesh: ImmediateMesh        # static entities: rebuilt only on set/pose/husk change
var _dyn_multimesh: MultiMesh   # posed organic spheres: retained across pose updates
var _dyn_unit_mesh: ArrayMesh   # one unit wire sphere shared by every organic section
var _labels: Array[Label3D] = []
var _signature := 0
var _organic_signature: Array = []
var _organic_signature_valid := false
var _drawable_count := 0


# Stable per-material color: golden-ratio hue walk, saturated and bright so
# adjacent material bytes read as clearly different families.
static func material_color(mat: int) -> Color:
	return Color.from_hsv(IndexHue.hue_for_index(mat), 0.75, 1.0)


## Color contract for the organic section spheres. Masked sections win so an
## authored exclusion can never be mistaken for a live hit volume.
static func organic_section_color(section: int, masked: bool, fallback: bool) -> Color:
	if masked:
		return ORGANIC_MASKED_COLOR
	if fallback:
		return ORGANIC_FALLBACK_COLOR
	if section == 13 or section == 14:
		return ORGANIC_HEAD_COLOR
	if section >= 0 and section <= 4:
		return ORGANIC_HEAVY_COLOR
	if (section >= 9 and section <= 12) or (section >= 15 and section <= 18):
		return ORGANIC_LIMB_COLOR
	return ORGANIC_BODY_COLOR


static func organic_damage_multiplier(section: int) -> float:
	if section >= 0 and section <= 4:
		return 1.25
	if (section >= 9 and section <= 12) or (section >= 15 and section <= 18):
		return 0.5
	if section == 13 or section == 14:
		return 3.0
	return 1.0


func _make_unit_wire_sphere_mesh() -> ArrayMesh:
	var vertices := PackedVector3Array()
	var colors := PackedColorArray()
	var prev_xz := Vector3(1.0, 0.0, 0.0)
	var prev_xy := Vector3(1.0, 0.0, 0.0)
	var prev_yz := Vector3(0.0, 1.0, 0.0)
	for i in range(1, SPHERE_SEGMENTS + 1):
		var t := TAU * float(i) / float(SPHERE_SEGMENTS)
		var c := cos(t)
		var s := sin(t)
		var p_xz := Vector3(c, 0.0, s)
		var p_xy := Vector3(c, s, 0.0)
		var p_yz := Vector3(0.0, c, s)
		vertices.append_array(PackedVector3Array([
			prev_xz, p_xz, prev_xy, p_xy, prev_yz, p_yz,
		]))
		for endpoint in range(6):
			colors.append(Color.WHITE)
		prev_xz = p_xz
		prev_xy = p_xy
		prev_yz = p_yz
	var arrays: Array = []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = vertices
	arrays[Mesh.ARRAY_COLOR] = colors
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_LINES, arrays)
	return mesh


func _sim_debug_method() -> String:
	return "get_hitbox_debug"


func _build_view() -> void:
	_mesh = ImmediateMesh.new()
	# Depth-tested: the static wireframe should hug the visual model.
	add_child(_make_lines_node("HitboxLines", _mesh, false))
	_dyn_unit_mesh = _make_unit_wire_sphere_mesh()
	_dyn_multimesh = MultiMesh.new()
	_dyn_multimesh.instance_count = 0
	_dyn_multimesh.transform_format = MultiMesh.TRANSFORM_3D
	_dyn_multimesh.use_colors = true
	_dyn_multimesh.use_custom_data = false
	_dyn_multimesh.mesh = _dyn_unit_mesh
	var dyn := MultiMeshInstance3D.new()
	dyn.name = "HitboxOrganicLines"
	dyn.multimesh = _dyn_multimesh
	var dyn_mat := MissionOverlayUtil.line_material()
	dyn_mat.no_depth_test = true  # people read through cover
	dyn_mat.albedo_color = Color.WHITE
	dyn_mat.vertex_color_use_as_albedo = true
	dyn.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	dyn.material_override = dyn_mat
	add_child(dyn)
	for i in range(LABEL_NEAREST):
		var lb := _make_overlay_label("HitboxLabel%d" % i, 0.005, 8, 2)
		add_child(lb)
		_labels.append(lb)


const REFRESH_INTERVAL_SECONDS := 1.0 / 6.0  # six Hz; native fetch marshals up to 24k tris
const REFRESH_EPSILON := 0.000000001

var _refresh_elapsed := 0.0


func _process(delta: float) -> void:
	advance_refresh(delta)


## Advance the fixed debug-snapshot cadence by one rendered-frame delta.
## Public so hosts and tests drive the same bounded scheduling behavior.
func advance_refresh(delta: float) -> void:
	if delta <= 0.0:
		return
	_refresh_elapsed += delta
	if _refresh_elapsed + REFRESH_EPSILON >= REFRESH_INTERVAL_SECONDS:
		_refresh_elapsed = fposmod(_refresh_elapsed, REFRESH_INTERVAL_SECONDS)
		refresh_now()


func _refresh_from_sim(sim: Object) -> void:
	var debug: Dictionary = sim.get_hitbox_debug()
	_update(debug.get("entities", []), debug.get("organics", []))


func _clear_all() -> void:
	_drawable_count = 0
	_mesh.clear_surfaces()
	if _dyn_multimesh.instance_count != 0:
		_dyn_multimesh.instance_count = 0
	if _dyn_multimesh.custom_aabb != AABB():
		_dyn_multimesh.custom_aabb = AABB()
	_signature = 0
	_organic_signature = []
	_organic_signature_valid = false
	for lb in _labels:
		lb.visible = false


## Number of static entities or organic section spheres currently contributing
## hit-detection geometry.
func get_debug_drawable_count() -> int:
	return _drawable_count


func _update(entities: Array, organics: Array) -> void:
	_drawable_count = 0
	for e_v in entities:
		var e: Dictionary = e_v
		var materials: PackedByteArray = e.get("materials", PackedByteArray())
		if not materials.is_empty() or float(e.get("bound_radius", 0.0)) > 0.0:
			_drawable_count += 1
	for o_v in organics:
		if float((o_v as Dictionary).get("radius", 0.0)) > 0.0:
			_drawable_count += 1

	# Native debug snapshots allocate fresh containers at the fixed cadence, but
	# the posed values often stay identical across several samples. Cache only
	# the fields that affect emitted sphere geometry; label-only fields still
	# flow through _update_labels below.
	var organic_signature: Array = []
	for o_v in organics:
		var o: Dictionary = o_v
		var radius := float(o.get('radius', 0.0))
		if radius <= 0.0:
			continue
		organic_signature.append(o.get('pos', Vector3.ZERO))
		organic_signature.append(radius)
		organic_signature.append(int(o.get('section', -1)))
		organic_signature.append(bool(o.get('masked', false)))
		organic_signature.append(bool(o.get('fallback', false)))
	var organic_geometry_changed := (
			not _organic_signature_valid or organic_signature != _organic_signature)
	if organic_geometry_changed:
		_organic_signature = organic_signature
		_organic_signature_valid = true

	if organic_geometry_changed:
		var instance_count := 0
		for o_v in organics:
			var o: Dictionary = o_v
			if float(o.get("radius", 0.0)) > 0.0:
				instance_count += 1
		if _dyn_multimesh.instance_count != instance_count:
			_dyn_multimesh.instance_count = instance_count
		if instance_count > 0:
			var packed := PackedFloat32Array()
			packed.resize(instance_count * 16)
			var packed_index := 0
			var bounds := AABB()
			var has_bounds := false
			for o_v in organics:
				var o: Dictionary = o_v
				var radius := float(o.get("radius", 0.0))
				if radius <= 0.0:
					continue
				var pos: Vector3 = o.get("pos", Vector3.ZERO)
				var color := organic_section_color(int(o.get("section", -1)),
						bool(o.get("masked", false)), bool(o.get("fallback", false)))
				var offset := packed_index * 16
				packed[offset] = radius
				packed[offset + 1] = 0.0
				packed[offset + 2] = 0.0
				packed[offset + 3] = pos.x
				packed[offset + 4] = 0.0
				packed[offset + 5] = radius
				packed[offset + 6] = 0.0
				packed[offset + 7] = pos.y
				packed[offset + 8] = 0.0
				packed[offset + 9] = 0.0
				packed[offset + 10] = radius
				packed[offset + 11] = pos.z
				packed[offset + 12] = color.r
				packed[offset + 13] = color.g
				packed[offset + 14] = color.b
				packed[offset + 15] = color.a
				var extent := Vector3.ONE * radius
				var row_bounds := AABB(pos - extent, extent * 2.0)
				bounds = bounds.merge(row_bounds) if has_bounds else row_bounds
				has_bounds = true
				packed_index += 1
			_dyn_multimesh.set_buffer(packed)
			_dyn_multimesh.custom_aabb = bounds
		else:
			_dyn_multimesh.custom_aabb = AABB()

	# Statics only change on set membership, pose, or husk swap.
	var sig_parts := []
	for e_v in entities:
		var e: Dictionary = e_v
		sig_parts.append(e.get("entity_handle", -1))
		sig_parts.append(e.get("pos", Vector3.ZERO))
		sig_parts.append(e.get("husk", false))
		sig_parts.append(e.get("bound_radius", 0.0))
		sig_parts.append(e.get("has_faces", true))
		# Generic/PANM section matrices can move the transformed CFAC triangles
		# while the entity origin and husk state stay unchanged. Model swaps can
		# also change face style without moving vertices. Hash every emitted mesh
		# input so the cached F3 view follows the authoritative query payload.
		sig_parts.append(hash(e.get("tris", PackedVector3Array())))
		sig_parts.append(hash(e.get("materials", PackedByteArray())))
		sig_parts.append(hash(e.get("flags", PackedInt32Array())))
	var sig := hash(sig_parts)
	if sig == _signature:
		_update_labels(entities, organics)
		return
	_signature = sig

	_mesh.clear_surfaces()
	for lb in _labels:
		lb.visible = false

	var segments: Array = []
	for e_v in entities:
		var e: Dictionary = e_v
		var tris: PackedVector3Array = e.get("tris", PackedVector3Array())
		var mats: PackedByteArray = e.get("materials", PackedByteArray())
		var flags: PackedInt32Array = e.get("flags", PackedInt32Array())
		for f in range(mats.size()):
			var a := tris[f * 3]
			var b := tris[f * 3 + 1]
			var c := tris[f * 3 + 2]
			var fl := flags[f]
			var color := material_color(mats[f])
			if fl & FLAG_NEVER_HIT:
				color = Color(0.45, 0.08, 0.08)  # authored never-hit: dark red
			elif fl & (FLAG_DOUBLE_SIDED | FLAG_BOTH_SIDES):
				color = color.lightened(0.25)    # hits from both sides
			segments.append({ "a": a, "b": b, "color": color })
			segments.append({ "a": b, "b": c, "color": color })
			segments.append({ "a": c, "b": a, "color": color })
		# The broad-phase bound sphere: dim white when the face mesh decides,
		# AMBER when there is no face mesh (the sphere IS the hitbox).
		var pos: Vector3 = e.get("pos", Vector3.ZERO)
		var r := float(e.get("bound_radius", 0.0))
		if r > 0.0:
			var sphere_color := Color(1.0, 1.0, 1.0, 0.22)
			if not bool(e.get("has_faces", true)):
				sphere_color = Color(1.0, 0.75, 0.2, 0.9)
			_wire_sphere(segments, pos, r, sphere_color)
	if not segments.is_empty():
		MissionOverlayUtil.emit_line_segments(_mesh, segments)
	_update_labels(entities, organics)


func _update_labels(entities: Array, organics: Array) -> void:
	# Labels on the nearest static entities and person bone spheres to the camera.
	var cam := get_viewport().get_camera_3d() if get_viewport() != null else null
	var cam_pos := cam.global_position if cam != null else Vector3.ZERO
	var order: Array = []
	for e_v in entities:
		var e2: Dictionary = e_v
		order.append([cam_pos.distance_to(e2.get("pos", Vector3.ZERO)), false, e2])
	for o_v in organics:
		var o: Dictionary = o_v
		if float(o.get("radius", 0.0)) <= 0.0:
			continue
		order.append([cam_pos.distance_to(o.get("pos", Vector3.ZERO)), true, o])
	order.sort_custom(func(x, y): return x[0] < y[0])
	var visible_count := mini(order.size(), _labels.size())
	for i in range(visible_count):
		var lb := _labels[i]
		var is_organic: bool = order[i][1]
		var e3: Dictionary = order[i][2]
		var ent := int(e3.get("entity_handle", 0xFFFF))
		var text := WireHandle.label(ent)
		var label_position: Vector3
		var label_modulate: Color
		if is_organic:
			var section := int(e3.get("section", -1))
			var radius := float(e3.get("radius", 0.0))
			text += "  bone %d" % section
			if bool(e3.get("fallback", false)):
				text += "  damage neutral"
			else:
				text += "  damage x%.2f" % organic_damage_multiplier(section)
			if section == 13 or section == 14:
				text += " HEAD"
			elif (section >= 9 and section <= 12) or (section >= 15 and section <= 18):
				text += " LIMB"
			text += "  r %.2f" % radius
			if bool(e3.get("masked", false)):
				text += "  MASKED"
			elif bool(e3.get("fallback", false)):
				text += "  FALLBACK"
			else:
				var authored := float(e3.get("authored_radius", radius))
				text += "  authored %.2f" % authored
			label_position = (e3.get("pos", Vector3.ZERO) as Vector3) \
					+ Vector3(0.0, radius + 0.08, 0.0)
			label_modulate = organic_section_color(section, bool(e3.get("masked", false)),
					bool(e3.get("fallback", false)))
		else:
			var total := int(e3.get("face_total", 0))
			var drawn: int = (e3.get("materials", PackedByteArray()) as PackedByteArray).size()
			if not bool(e3.get("has_faces", true)):
				text += "  SPHERE STAND-IN"
			else:
				text += "  %d faces" % total
				if drawn < total:
					text += " (drawn %d)" % drawn
			if bool(e3.get("husk", false)):
				text += "  HUSK"
			label_position = (e3.get("pos", Vector3.ZERO) as Vector3) \
					+ Vector3(0.0, float(e3.get("bound_radius", 1.0)) + 0.3, 0.0)
			label_modulate = Color(1.0, 0.9, 0.5) \
					if bool(e3.get("husk", false)) else Color(0.85, 0.95, 1.0)
		if lb.text != text:
			lb.text = text
		if lb.position != label_position:
			lb.position = label_position
		if lb.modulate != label_modulate:
			lb.modulate = label_modulate
		if not lb.visible:
			lb.visible = true
	for i in range(visible_count, _labels.size()):
		var lb := _labels[i]
		if lb.visible:
			lb.visible = false


# Three axis-aligned wireframe circles reading as a sphere.
func _wire_sphere(segments: Array, center: Vector3, r: float, color: Color) -> void:
	var prev_xz := center + Vector3(r, 0, 0)
	var prev_xy := center + Vector3(r, 0, 0)
	var prev_yz := center + Vector3(0, r, 0)
	for i in range(1, SPHERE_SEGMENTS + 1):
		var t := TAU * float(i) / float(SPHERE_SEGMENTS)
		var c := cos(t)
		var s := sin(t)
		var p_xz := center + Vector3(r * c, 0, r * s)
		var p_xy := center + Vector3(r * c, r * s, 0)
		var p_yz := center + Vector3(0, r * c, r * s)
		segments.append({ "a": prev_xz, "b": p_xz, "color": color })
		segments.append({ "a": prev_xy, "b": p_xy, "color": color })
		segments.append({ "a": prev_yz, "b": p_yz, "color": color })
		prev_xz = p_xz
		prev_xy = p_xy
		prev_yz = p_yz
