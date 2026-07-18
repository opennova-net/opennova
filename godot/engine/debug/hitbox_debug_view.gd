extends Node3D

# Draws the round hit-detection reality over the scene: every nearby entity's
# CFAC bullet-mesh wireframe (the triangles Physics_RaycastAgainstBoneCollision
# walks), colored by face material, plus the broad-phase bound sphere, the
# husk state, and the pool-0 organic stand-in spheres. The 3D face of the F3
# Rounds tab's "Show hit meshes" toggle.
#
# Geometry comes from NovaSimulation.get_hitbox_debug(): triangles are
# transformed in C++ through the SAME husk-aware target_view + full-euler
# placement matrices the projectile raycast uses, so the drawn mesh IS what
# rounds resolve against. Amber spheres mark entities with NO face mesh —
# there the bound sphere alone decides hits (D-ITEM-1's stand-in). Cyan
# spheres are the organic torso stand-in (D-ITEM-13c). Built / freed by
# GameWorld on the overlay toggle, the collision-view contract.

const MissionOverlayUtil := preload("res://engine/mission/mission_overlay_util.gd")

const LABEL_NEAREST := 12       # detail labels on this many nearest entities
const SPHERE_SEGMENTS := 20

# Face flags that change the read of a triangle.
const FLAG_NEVER_HIT := 0x100   # authored never-hit — rounds ignore it
const FLAG_DOUBLE_SIDED := 0x800
const FLAG_BOTH_SIDES := 0x1

var _world: Node                # duck-typed host (get_sim()); re-resolved every frame
var _mesh: ImmediateMesh        # static entities: rebuilt only on set/pose/husk change
var _dyn_mesh: ImmediateMesh    # organic stand-ins: tiny, rebuilt every refresh
var _labels: Array[Label3D] = []
var _signature := 0


# Stable per-material color: golden-ratio hue walk, saturated and bright so
# adjacent material bytes read as clearly different families.
static func material_color(mat: int) -> Color:
	return Color.from_hsv(fposmod(float(mat) * 0.618033988749895, 1.0), 0.75, 1.0)


func setup(world: Node) -> void:
	_world = world
	_mesh = ImmediateMesh.new()
	var mi := MeshInstance3D.new()
	mi.name = "HitboxLines"
	mi.mesh = _mesh
	var mat := MissionOverlayUtil.line_material()
	mat.no_depth_test = false  # depth-tested: the mesh should hug the visual model
	mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	mi.material_override = mat
	add_child(mi)
	_dyn_mesh = ImmediateMesh.new()
	var dyn := MeshInstance3D.new()
	dyn.name = "HitboxOrganicLines"
	dyn.mesh = _dyn_mesh
	var dyn_mat := MissionOverlayUtil.line_material()
	dyn_mat.no_depth_test = true  # people read through cover
	dyn.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	dyn.material_override = dyn_mat
	add_child(dyn)
	for i in range(LABEL_NEAREST):
		var lb := Label3D.new()
		lb.name = "HitboxLabel%d" % i
		lb.billboard = BaseMaterial3D.BILLBOARD_ENABLED
		lb.fixed_size = false
		lb.pixel_size = 0.005
		lb.no_depth_test = true
		lb.font_size = 36
		lb.outline_size = 10
		lb.outline_modulate = Color(0.0, 0.0, 0.0, 0.85)
		lb.visible = false
		add_child(lb)
		_labels.append(lb)


const REFRESH_EVERY := 10  # frames — the native fetch marshals up to 24k tris

var _frame := 0


func _process(_delta: float) -> void:
	_frame += 1
	if _frame % REFRESH_EVERY == 0:
		refresh_now()


## Immediately refresh from the current simulation.
func refresh_now() -> void:
	var sim := _resolve_sim()
	if sim == null:
		_clear_all()
		return
	var debug: Dictionary = sim.get_hitbox_debug()
	_update(debug.get("entities", []), debug.get("organics", []))


func _resolve_sim() -> Object:
	if _world == null or not is_instance_valid(_world) or not _world.has_method("get_sim"):
		return null
	var sim: Variant = _world.get_sim()
	if sim == null or not is_instance_valid(sim) or not (sim as Object).has_method("get_hitbox_debug"):
		return null
	return sim


func _clear_all() -> void:
	_mesh.clear_surfaces()
	_dyn_mesh.clear_surfaces()
	_signature = 0
	for lb in _labels:
		lb.visible = false


func _update(entities: Array, organics: Array) -> void:
	# The organic stand-ins are tiny and move constantly — their own mesh,
	# rebuilt every refresh.
	_dyn_mesh.clear_surfaces()
	var dyn_segments: Array = []
	for o_v in organics:
		var o: Dictionary = o_v
		_wire_sphere(dyn_segments, o.get("pos", Vector3.ZERO), float(o.get("radius", 0.6)),
				Color(0.2, 0.9, 1.0, 0.9))
	if not dyn_segments.is_empty():
		MissionOverlayUtil.emit_line_segments(_dyn_mesh, dyn_segments)

	# Statics only change on set membership, pose, or husk swap.
	var sig_parts := []
	for e_v in entities:
		var e: Dictionary = e_v
		sig_parts.append(e.get("entity_handle", -1))
		sig_parts.append(e.get("pos", Vector3.ZERO))
		sig_parts.append(e.get("husk", false))
	var sig := hash(sig_parts)
	if sig == _signature:
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

	# Labels on the nearest entities to the camera.
	var cam := get_viewport().get_camera_3d() if get_viewport() != null else null
	var cam_pos := cam.global_position if cam != null else Vector3.ZERO
	var order: Array = []
	for e_v in entities:
		var e2: Dictionary = e_v
		order.append([cam_pos.distance_to(e2.get("pos", Vector3.ZERO)), e2])
	order.sort_custom(func(x, y): return x[0] < y[0])
	for i in range(mini(order.size(), _labels.size())):
		var e3: Dictionary = order[i][1]
		var lb := _labels[i]
		var ent := int(e3.get("entity_handle", 0xFFFF))
		var text := "%d/%d" % [(ent >> 12) & 0xF, ent & 0xFFF]
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
		lb.text = text
		lb.position = (e3.get("pos", Vector3.ZERO) as Vector3) \
				+ Vector3(0.0, float(e3.get("bound_radius", 1.0)) + 0.3, 0.0)
		lb.modulate = Color(1.0, 0.9, 0.5) if bool(e3.get("husk", false)) else Color(0.85, 0.95, 1.0)
		lb.visible = true


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
