class_name NovaObjectModel
extends Node3D

signal bounds_changed(bounds: AABB)

# The per-material 3DI flag byte constants live on NovaObjectShaderCache,
# single-sourced from libs/threedi (THREEDI_MATERIAL_FLAG_*) — REN-2.
const OED_UPDATE_NONE := 0
const OED_UPDATE_MTRL := 1
const OED_UPDATE_LGHT := 2
const OED_UPDATE_PANM := 4
const OED_UPDATE_ALL := OED_UPDATE_MTRL | OED_UPDATE_LGHT | OED_UPDATE_PANM

const DEFAULT_AMBIENT_COLOR := Vector3(0.35, 0.36, 0.40)
const DEFAULT_DIR_LIGHT_DIR := Vector3(-0.4082, -0.8165, -0.4082)
const DEFAULT_DIR_LIGHT_COLOR := Vector3(0.85, 0.82, 0.75)
const DEFAULT_FILL_LIGHT_COLOR := Vector3(0.18, 0.20, 0.25)
const DEFAULT_FOG_COLOR := Vector3(0.5, 0.6, 0.8)
const DEFAULT_FOG_START := 0.0
const DEFAULT_FOG_END := 1024.0
const DEFAULT_FOG_TYPE := 0

var object_data: NovaObjectData

var _material_cache: Dictionary = {}
var _material_defs: Dictionary = {}
var _robj_nodes: Dictionary = {}
var _surface_material_indices: PackedInt32Array = PackedInt32Array()
var _surface_materials: Array[ShaderMaterial] = []
var _anim_frames_by_mat: Dictionary = {}
var _ctrl_values: Dictionary = {}
var _part_anims: Dictionary = {}
var _anim_time_ms: int = 0
var _active_lod: int = 0
var _is_playing := true
var _model_bounds := AABB()
var _environment_node: Node

# Main-body skeletal animation (.bad/.adm via NovaSkeletalAnim). Distinct from PANM
# (vehicle part channels above): this drives a Skeleton3D built from the .bad skeleton,
# with the skinned meshes bound through a rest-derived Skin. _skeletal is null for static
# / non-organic models, in which case nothing here runs and the model renders as before.
var _skeletal                       # NovaSkeletalAnim, or null
var _skeleton: Skeleton3D           # built in rebuild() when skinned + _skeletal loaded
var _skeleton_skin: Skin
var _anim_key := ""                 # active clip key (ADM key, e.g. "anim_walk")
var _anim_time := 0.0               # playhead seconds into the active clip
var _anim_playing := false
var _anim_external_phase := false   # true when the sim, not _process(delta), owns _anim_time
var _body_pose_dirty := true
var _bounds_dirty := true

# Per-frame work skips. Each cached value is re-derived in rebuild() (or on the exact
# mutator), so a skip only ever omits re-pushing state that is byte-identical to what is
# already resident on the materials -- invisible in Godot's retained-mode renderer, and so
# parity-preserving against the original engine's output.
var _has_lights := false
var _material_needs_eval: Array[bool] = []          # parallel to _surface_materials
var _dynamic_material_slots: PackedInt32Array = PackedInt32Array()
var _last_env_gen := -1
var _last_env_values: Dictionary = {}


func _ready() -> void:
	set_process(true)
	if object_data != null:
		rebuild()


func set_object_data(value: NovaObjectData) -> void:
	if object_data != null and object_data.object_changed.is_connected(_on_object_changed):
		object_data.object_changed.disconnect(_on_object_changed)
	object_data = value
	_part_anims.clear()
	_active_lod = _clamp_lod_index(_active_lod)
	if object_data != null and not object_data.object_changed.is_connected(_on_object_changed):
		object_data.object_changed.connect(_on_object_changed, CONNECT_DEFERRED)
	rebuild()


func get_object_data() -> NovaObjectData:
	return object_data


func set_environment_node(value: Node) -> void:
	_environment_node = value
	_last_env_gen = -1
	_last_env_values = {}
	_apply_environment_to_materials()


func get_model_bounds() -> AABB:
	return _model_bounds


func get_render_part_nodes() -> Dictionary:
	return _robj_nodes


func get_surface_material_indices() -> PackedInt32Array:
	return _surface_material_indices


func get_surface_materials() -> Array:
	return _surface_materials


func get_material_defs() -> Dictionary:
	return _material_defs


func is_playing() -> bool:
	return _is_playing


func set_playing(value: bool) -> void:
	_is_playing = value


func reset_animation_time() -> void:
	_anim_time_ms = 0
	_anim_time = 0.0
	_anim_external_phase = false
	_apply_runtime_state(0.0)


# --- Main-body skeletal animation (.bad/.adm) -----------------------------------
# A NovaSkeletalAnim carries the parsed/sampled skeleton + clips for this model. Setting
# it (then a skinned model) makes rebuild() build the Skeleton3D + Skin; play_body_clip
# selects the active clip the per-frame pass poses. Both the object-editor preview and the
# mission present pass drive these, so organic bodies animate from one path.
func set_skeletal_anim(skeletal) -> void:
	_skeletal = skeletal
	_anim_key = ""
	_anim_time = 0.0
	_anim_playing = false
	_anim_external_phase = false
	_body_pose_dirty = true
	rebuild()


func get_skeletal_anim():
	return _skeletal


func get_skeleton() -> Skeleton3D:
	return _skeleton


func has_skeleton() -> bool:
	return _skeleton != null


## Play a main-body clip by ADM key (e.g. "anim_walk"). No-op if no skeletal set / unknown.
func play_body_clip(key: String) -> void:
	if _skeletal == null or not _skeletal.has_clip(key):
		return
	if key == _anim_key:
		_anim_external_phase = false
		_anim_playing = true
		return
	_anim_key = key
	_anim_time = 0.0
	_anim_playing = true
	_anim_external_phase = false
	_body_pose_dirty = true


## Pose a main-body clip at the authoritative infantry motor playhead. IDA's
## AnimMap phase advances in half-frame ticks, so seconds = ticks / (2 * clip_fps).
## The model does not free-run this clip between sim snapshots.
func play_body_clip_at(key: String, phase_ticks: int) -> void:
	if _skeletal == null or not _skeletal.has_clip(key):
		return
	var previous_key := _anim_key
	var previous_time := _anim_time
	var previous_external := _anim_external_phase
	var fps: float = _skeletal.get_clip_fps(key)
	var seconds := 0.0
	if fps > 0.0:
		seconds = float(maxi(phase_ticks, 0)) / (2.0 * fps)
	var same_external := previous_external and key == previous_key and is_equal_approx(previous_time, seconds)
	_anim_key = key
	_set_body_playhead(seconds)
	_anim_playing = false
	_anim_external_phase = true
	if same_external and not _body_pose_dirty:
		return
	_body_pose_dirty = true
	_advance_body_anim(0.0)


func stop_body_clip() -> void:
	_anim_playing = false
	_anim_external_phase = false


func get_active_body_clip() -> String:
	return _anim_key


## Play a main-body animation by canonical AI slot (opennova::world::BodyAnim). Resolves the
## slot to a clip key via the loaded NovaSkeletalAnim (with idle/reset fallback) and plays it.
## Idempotent: a repeated same-slot call (every present tick) does not restart a playing loop.
## No-op without a skeletal set or for slot < 0. This is the present pass's body-anim entry point.
func play_body_anim(slot: int) -> void:
	if _skeletal == null or slot < 0:
		return
	var key: String = _skeletal.slot_to_key(slot)
	if key.is_empty():
		return
	if key == _anim_key and not _anim_external_phase:
		return
	play_body_clip(key)


## Pose a main-body animation slot at the authoritative infantry motor playhead.
func play_body_anim_at(slot: int, phase_ticks: int) -> void:
	if _skeletal == null or slot < 0:
		return
	var key: String = _skeletal.slot_to_key(slot)
	if key.is_empty():
		return
	play_body_clip_at(key, phase_ticks)


func get_animation_time_ms() -> int:
	return _anim_time_ms


## Scrub the active body clip's playhead to `seconds` and pose IMMEDIATELY,
## even while paused (paused scrubbing is the point; while playing the
## zero-delta advance adds nothing). Mirrors eval_pose's own time handling so
## the stored playhead and the rendered pose can never disagree: looping clips
## wrap over the clip length, one-shots clamp to it. No-op without a skeletal
## set / active clip. Distinct from the _anim_time_ms material/PANM clock.
func set_animation_time(seconds: float) -> void:
	if _skeletal == null or _anim_key.is_empty():
		return
	_anim_external_phase = false
	_set_body_playhead(seconds)
	_body_pose_dirty = true
	_advance_body_anim(0.0)


func _set_body_playhead(seconds: float) -> void:
	var length: float = _skeletal.get_clip_length(_anim_key)
	if length <= 0.0:
		_anim_time = 0.0
	elif _skeletal.is_clip_looping(_anim_key):
		_anim_time = fposmod(seconds, length)
	else:
		_anim_time = clampf(seconds, 0.0, length)


## The active body clip's playhead in seconds, loop-wrapped (one-shots clamp),
## so a scrub slider binds to it directly.
func get_animation_time() -> float:
	if _skeletal == null or _anim_key.is_empty():
		return 0.0
	var length: float = _skeletal.get_clip_length(_anim_key)
	if length <= 0.0:
		return 0.0
	if _skeletal.is_clip_looping(_anim_key):
		return fposmod(_anim_time, length)
	return clampf(_anim_time, 0.0, length)


func set_active_lod(lod_index: int) -> void:
	var next_lod := _clamp_lod_index(lod_index)
	if _active_lod == next_lod:
		return
	_active_lod = next_lod
	rebuild()


func get_active_lod() -> int:
	return _active_lod


func set_ctrl_value(name: String, value: int) -> void:
	if name.is_empty():
		return
	var next_value := clampi(value, 0, 65535)
	if int(_ctrl_values.get(name, -1)) == next_value:
		return
	_ctrl_values[name] = next_value
	_bounds_dirty = true
	_apply_runtime_state(0.0)


func clear_ctrl_value(name: String) -> void:
	if not _ctrl_values.has(name):
		return
	_ctrl_values.erase(name)
	_bounds_dirty = true
	_apply_runtime_state(0.0)


func clear_ctrl_values() -> void:
	if _ctrl_values.is_empty():
		return
	_ctrl_values.clear()
	_bounds_dirty = true
	_apply_runtime_state(0.0)


func get_ctrl_values() -> Dictionary:
	return _ctrl_values.duplicate(true)


# --- Part-animation channels (PLAYPARTANIM mission action) ---
# [orig: Jointops Entity_ApplyCommand @0x43ab60 case 0x22] PLAYPARTANIM(channel, play_type, time):
# ANIMNUM (channel) in {1,2} selects one of two part-anim channels (slot = channel-1); ANIMPLAYTYPE
# +1/0/-1 = forward/stop/reverse; ANIMTIME seconds = how long the part takes to cross its full range.
# The original stores a per-channel direction + per-tick rate on the AI struct and a per-frame consumer
# sweeps a 16.16 phase (0..65536, full range in ANIMTIME at 62.5Hz), clamping at the ends; it is
# velocity-from-current (it does NOT reset the phase). We drive part channel `slot` through the model's
# PANM control register at index `slot`, value 0..65535 == that phase, fed to evaluate_panm() each frame.
# See notes/mission/anim-ai-grill-2026-06-07.md.

## Play a model part animation, mirroring the runtime PLAYPARTANIM action so editor preview and host
## playback share one path. channel: 1 or 2. play_type: 1 play / 0 stop / -1 reverse. time_s: seconds
## for the part to traverse its full range (ANIMTIME).
func play_part_anim(channel: int, play_type: int, time_s: float) -> void:
	var slot := channel - 1
	if slot < 0 or slot > 1:
		return  # the original validates channel in {1,2}; anything else is ignored
	var register := _resolve_anim_channel_register(slot)
	if register.is_empty():
		return
	if play_type == 0:
		_part_anims.erase(register)  # Stop: freeze the part at its current value
		return
	var dir := 1 if play_type > 0 else -1
	var speed := 65535.0 / time_s if time_s > 0.0 else 1.0e9  # full range crossed in time_s seconds
	_part_anims[register] = {
		"register": register,
		"dir": dir,
		"speed": speed,
		"value": float(int(_ctrl_values.get(register, 0))),  # velocity from current (no reset)
	}


## Editor-preview convenience: seed the channel at its rest start (0 forward / max reverse) then play,
## so a preview always shows the full motion from rest. The runtime uses play_part_anim directly
## (velocity-from-current, faithful to the action); only the editor preview restarts.
func restart_part_anim(channel: int, play_type: int, time_s: float) -> void:
	var slot := channel - 1
	if slot < 0 or slot > 1:
		return
	var register := _resolve_anim_channel_register(slot)
	# Stop (play_type == 0) must freeze the part where it is, so do NOT reseed the register: reseeding
	# to 0 would jump the part to its 0 pose before play_part_anim's stop erases the channel. Only the
	# forward/reverse previews seed a rest start (0 forward / max reverse).
	if not register.is_empty() and play_type != 0:
		_ctrl_values[register] = 0 if play_type >= 0 else 65535
	play_part_anim(channel, play_type, time_s)


## Pose a part channel directly to an engine-computed phase (0..65535 == 0..1 over the part's range).
## The faithful runtime path: NovaSimulation/the AI brain integrates the PLAYPARTANIM phase in-engine
## (Entity_ApplyCommand @0x43ab60 + the per-frame consumer), and the host just writes it to the PANM
## control register here. Distinct from play_part_anim (the editor/object-preview host-side integrator).
func set_part_phase(channel: int, phase: int) -> void:
	var register := _resolve_anim_channel_register(channel - 1)
	if register.is_empty():
		return
	var next_phase := clampi(phase, 0, 65535)
	if int(_ctrl_values.get(register, -1)) == next_phase and not _part_anims.has(register):
		return
	_part_anims.erase(register)  # the engine owns this channel's phase; no host integrator on it
	_ctrl_values[register] = next_phase
	_bounds_dirty = true


## Stop a single channel's part animation (freeze in place); no-op if the channel is not animating.
func stop_part_anim(channel: int) -> void:
	var register := _resolve_anim_channel_register(channel - 1)
	if not register.is_empty():
		_part_anims.erase(register)


func clear_part_anims() -> void:
	_part_anims.clear()


func get_active_part_anims() -> Dictionary:
	return _part_anims.duplicate(true)


# [orig: ANIMNUM channel (1/2) -> part-anim slot 0/1 -> the model's PANM control register at index `slot`.]
func _resolve_anim_channel_register(slot: int) -> String:
	if object_data == null or not object_data.has_method("get_control_registers"):
		return ""
	var regs: Array = object_data.get_control_registers()
	if slot < 0 or slot >= regs.size():
		return ""
	return String((regs[slot] as Dictionary).get("name", ""))


# Advance each active channel's phase toward its endpoint at the authored speed, clamping at [0,65535].
# Writes straight into _ctrl_values (NOT set_ctrl_value, which would eagerly re-evaluate per channel);
# the enclosing _apply_runtime_state applies the result once, in the same frame, to materials + PANM.
func _advance_part_anims(delta: float) -> bool:
	if _part_anims.is_empty() or delta <= 0.0:
		return false
	var finished: Array = []
	var changed := false
	for register in _part_anims.keys():
		var anim: Dictionary = _part_anims[register]
		var old_value := int(_ctrl_values.get(register, 0))
		var value := clampf(float(anim["value"]) + float(anim["speed"]) * float(anim["dir"]) * delta, 0.0, 65535.0)
		anim["value"] = value
		var next_value := int(round(value))
		_ctrl_values[register] = next_value
		changed = changed or old_value != next_value
		if (int(anim["dir"]) > 0 and value >= 65535.0) or (int(anim["dir"]) < 0 and value <= 0.0):
			finished.append(register)  # reached the clamp endpoint; the part holds there
	for register in finished:
		_part_anims.erase(register)
	_bounds_dirty = _bounds_dirty or changed
	return changed


# Pose the Skeleton3D from the active main-body clip. Advances the playhead while playing,
# evaluates the parent-local pose per bone (NovaSkeletalAnim, Godot space) and writes it as
# the bone pose. With no active clip the bones stay at their reset (== bind) pose.
func _advance_body_anim(delta: float) -> void:
	if _skeleton == null or _skeletal == null or _anim_key.is_empty():
		return
	if _is_playing and _anim_playing and not _anim_external_phase and delta != 0.0:
		_anim_time += delta
		_body_pose_dirty = true
	if not _body_pose_dirty:
		return
	var pose: Array = _skeletal.eval_pose(_anim_key, _anim_time)
	var count: int = mini(pose.size(), _skeleton.get_bone_count())
	for i in range(count):
		var t: Transform3D = pose[i]
		_skeleton.set_bone_pose_position(i, t.origin)
		_skeleton.set_bone_pose_rotation(i, t.basis.get_rotation_quaternion())
	_body_pose_dirty = false


func rebuild() -> void:
	for child in get_children():
		remove_child(child)
		child.queue_free()
	_robj_nodes.clear()
	_skeleton = null
	_skeleton_skin = null
	_surface_material_indices.clear()
	_surface_materials.clear()
	_anim_frames_by_mat.clear()
	_material_cache.clear()
	_material_defs.clear()
	_body_pose_dirty = true
	_bounds_dirty = true
	_has_lights = false
	_material_needs_eval.clear()
	_dynamic_material_slots = PackedInt32Array()
	_last_env_gen = -1
	_last_env_values = {}
	if object_data == null or not object_data.has_document():
		_set_model_bounds(AABB())
		return

	_material_defs = _build_material_defs()
	_active_lod = _clamp_lod_index(_active_lod)
	# A loaded .adm drives the model: build a Skeleton3D from its .bad skeleton. This applies to
	# BOTH per-vertex skinned models (organic bodies/arms) AND rigid models (first-person weapons) --
	# rigid parts ride a bone via "fake skinning" (build_lod_submeshes(skeletal=true)). Without a
	# .adm, no skeleton is built and the model renders static exactly as before.
	var skeletal_mode: bool = _skeletal != null and _skeletal.is_loaded()
	if skeletal_mode:
		_build_skeleton()
	var bone_count: int = _skeleton.get_bone_count() if skeletal_mode and _skeleton != null else 0
	var submeshes: Array = object_data.build_lod_submeshes(_active_lod, skeletal_mode, bone_count) if object_data.has_method("build_lod_submeshes") else []
	if submeshes.is_empty():
		submeshes = _legacy_submeshes_from_surfaces(_active_lod)
	for entry in submeshes:
		var submesh: Dictionary = entry
		var mesh := submesh.get("mesh") as ArrayMesh
		if mesh == null:
			continue
		var robj_index := int(submesh.get("robj_index", submesh.get("part_index", 0)))
		var material_index := int(submesh.get("material_index", 0))
		var instance := MeshInstance3D.new()
		instance.mesh = mesh
		var material := _material_for_index(material_index)
		instance.material_override = material
		# Skinned + rigid-fake-skinned submeshes bind to the shared Skeleton3D; everything else
		# stays under its render-object (Robj) part node so PANM part transforms keep working.
		if skeletal_mode and _skeleton != null and bool(submesh.get("is_skinned", false)):
			_skeleton.add_child(instance)
			instance.skin = _skeleton_skin
			instance.skeleton = instance.get_path_to(_skeleton)
		else:
			var node := _get_or_create_robj_node(robj_index)
			node.add_child(instance)
		_surface_material_indices.append(material_index)
		_surface_materials.append(material)
		_collect_anim_frames(material_index)

	_classify_materials()
	_has_lights = object_data.has_method("get_light_count") and int(object_data.get_light_count()) > 0
	_apply_robj_transforms()
	_apply_runtime_state(0.0)


# Build the Skeleton3D + rest-derived Skin from the loaded NovaSkeletalAnim. Bones come from
# the .bad skeleton (names/parents/parent-local bind rest). The Skin binds each bone with its
# global-rest inverse (create_skin_from_rest_transforms), so a skinned mesh renders exactly at
# rest when the pose equals the rest -- making the rest render independent of the (animated)
# coordinate convention. [orig: BoneFile_Load @0x40fff0 builds the runtime skeleton.]
func _build_skeleton() -> void:
	_skeleton = Skeleton3D.new()
	_skeleton.name = "Skeleton3D"
	add_child(_skeleton)
	var bones: Array = _skeletal.get_skeleton_bones()
	for b in bones:
		_skeleton.add_bone(String((b as Dictionary).get("name", "bone")))
	for i in range(bones.size()):
		var bd: Dictionary = bones[i]
		var parent := int(bd.get("parent_index", -1))
		if parent >= 0 and parent < _skeleton.get_bone_count() and parent != i:
			_skeleton.set_bone_parent(i, parent)
		_skeleton.set_bone_rest(i, bd.get("rest", Transform3D()))
	for i in range(_skeleton.get_bone_count()):
		_skeleton.reset_bone_pose(i)
	_skeleton_skin = _skeleton.create_skin_from_rest_transforms()


func _on_object_changed() -> void:
	var update_mask := _last_object_update_mask()
	if update_mask == OED_UPDATE_PANM or update_mask == OED_UPDATE_LGHT or update_mask == (OED_UPDATE_PANM | OED_UPDATE_LGHT):
		# get_last_oed_update_mask() reports only the final mask of a deferred-flush window,
		# so a coalesced batch could read LGHT/PANM even when a material's generator style
		# also changed. Reclassify here (cheap, idempotent) so the dynamic-material set can
		# never go stale relative to the current IR -- otherwise a newly-animated material
		# would stay frozen on this no-rebuild fast path.
		_classify_materials()
		_apply_runtime_state(0.0)
		return
	rebuild()


func _last_object_update_mask() -> int:
	if object_data != null and object_data.has_method("get_last_oed_update_mask"):
		return int(object_data.get_last_oed_update_mask()) & OED_UPDATE_ALL
	return OED_UPDATE_ALL


func _process(delta: float) -> void:
	_apply_runtime_state(delta)


func _clamp_lod_index(lod_index: int) -> int:
	if object_data == null or not object_data.has_document():
		return 0
	var summary: Dictionary = object_data.get_summary()
	var lod_count := int(summary.get("lod_count", 1))
	return clampi(lod_index, 0, maxi(lod_count - 1, 0))


func _build_material_defs() -> Dictionary:
	var result := {}
	for material in object_data.get_materials():
		var material_index := int(material.get("material_index", material.get("index", 0)))
		result[material_index] = material
		var array_index := int(material.get("index", material_index))
		if not result.has(array_index):
			result[array_index] = material
	return result


func _legacy_submeshes_from_surfaces(lod_index: int) -> Array:
	var result := []
	if object_data == null:
		return result
	for surface in object_data.get_lod_surfaces(lod_index):
		var mesh := ArrayMesh.new()
		var arrays := []
		arrays.resize(Mesh.ARRAY_MAX)
		arrays[Mesh.ARRAY_VERTEX] = surface.get("vertices", PackedVector3Array())
		arrays[Mesh.ARRAY_NORMAL] = surface.get("normals", PackedVector3Array())
		arrays[Mesh.ARRAY_TEX_UV] = surface.get("uvs", PackedVector2Array())
		arrays[Mesh.ARRAY_TEX_UV2] = surface.get("uvs2", PackedVector2Array())
		var tangents: PackedFloat32Array = surface.get("tangents", PackedFloat32Array())
		if tangents.size() == arrays[Mesh.ARRAY_VERTEX].size() * 4:
			arrays[Mesh.ARRAY_TANGENT] = tangents
		arrays[Mesh.ARRAY_INDEX] = surface.get("indices", PackedInt32Array())
		if arrays[Mesh.ARRAY_VERTEX].is_empty():
			continue
		mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
		result.append({
			"robj_index": int(surface.get("part_index", 0)),
			"material_index": int(surface.get("material_array_index", surface.get("material_index", 0))),
			"mesh": mesh,
		})
	return result


func _get_or_create_robj_node(robj_index: int) -> Node3D:
	if _robj_nodes.has(robj_index):
		return _robj_nodes[robj_index]
	var node := Node3D.new()
	node.name = "Robj_%d" % robj_index
	add_child(node)
	_robj_nodes[robj_index] = node
	return node


func _compute_transformed_mesh_bounds() -> AABB:
	var bounds := AABB()
	var has_bounds := false
	var model_inverse := global_transform.affine_inverse()
	# Static / rigid submeshes hang under their Robj part nodes; skinned (and rigid-fake-skinned)
	# submeshes hang under the shared Skeleton3D (see rebuild()). Walk both, so a fully-skinned
	# model (e.g. an avatar body) still reports real bounds -- get_model_bounds drives consumers
	# like the menu-portrait framing, which would otherwise see an empty AABB and never frame.
	var roots: Array = _robj_nodes.values()
	if _skeleton != null:
		roots.append(_skeleton)
	for root_node in roots:
		var node := root_node as Node3D
		if node == null:
			continue
		for child in node.get_children():
			if child is MeshInstance3D:
				var instance := child as MeshInstance3D
				if instance.mesh == null:
					continue
				var mesh_aabb := instance.mesh.get_aabb()
				if mesh_aabb.size == Vector3.ZERO:
					continue
				var local_aabb: AABB = model_inverse * (instance.global_transform * mesh_aabb)
				bounds = local_aabb if not has_bounds else bounds.merge(local_aabb)
				has_bounds = true
	return bounds


func _apply_runtime_state(delta: float) -> void:
	if object_data == null or not object_data.has_document():
		return
	if _is_playing:
		_anim_time_ms = (_anim_time_ms + int(delta * 1000.0)) & 0x7fffffff
	var part_changed := _advance_part_anims(delta)
	_advance_body_anim(delta)
	# Only materials whose UV/RGB/alpha generators animate (or whose texture flip-book
	# advances) need a per-frame push; a fully-static material already carries its identity
	# values from _create_material, so re-evaluating it each frame just re-writes identical
	# bytes. _dynamic_material_slots holds exactly the slots that can change (built in
	# _classify_materials); _material_needs_eval[i] distinguishes the eval path from the
	# texture-flip-book-only path.
	var has_eval := object_data.has_method("eval_material_runtime")
	var has_frame := object_data.has_method("compute_anim_frame")
	for i in _dynamic_material_slots:
		var material := _surface_materials[i]
		if material == null:
			continue
		var material_index := int(_surface_material_indices[i])
		if _material_needs_eval[i] and has_eval:
			var runtime: Dictionary = object_data.eval_material_runtime(material_index, _anim_time_ms, _ctrl_values)
			if not runtime.is_empty():
				material.set_shader_parameter("u_uv_offset", runtime.get("uv_offset", Vector2.ZERO))
				material.set_shader_parameter("u_uv_scale", runtime.get("uv_scale", Vector2.ONE))
				material.set_shader_parameter("u_uv_rotation", runtime.get("uv_rotation", 0.0))
				var rgb: Vector3 = runtime.get("rgb_mod", Vector3.ONE)
				material.set_shader_parameter("u_rgb_mod", rgb)
				material.set_shader_parameter("u_alpha_mod", runtime.get("alpha_mod", 1.0))
		var frames: Array = _anim_frames_by_mat.get(material_index, [])
		if frames.size() > 1 and has_frame:
			var frame_index := int(object_data.compute_anim_frame(material_index, _anim_time_ms, _ctrl_values))
			if frame_index >= 0 and frame_index < frames.size() and frames[frame_index] is Texture2D:
				material.set_shader_parameter("u_diffuse", frames[frame_index])
	var robj_changed := _apply_robj_transforms()
	_apply_lights()
	_apply_environment_to_materials()
	if _bounds_dirty or part_changed or robj_changed:
		_set_model_bounds(_compute_transformed_mesh_bounds())
		_bounds_dirty = false


func _apply_robj_transforms() -> bool:
	if object_data == null or not object_data.has_method("evaluate_panm") or _robj_nodes.is_empty():
		return false
	var transforms: Dictionary = object_data.evaluate_panm(_active_lod, _anim_time_ms, _ctrl_values)
	var changed := false
	for key in transforms.keys():
		var robj_index := int(key)
		if _robj_nodes.has(robj_index):
			var node := _robj_nodes[robj_index] as Node3D
			var next_transform: Transform3D = transforms[key]
			if node.transform != next_transform:
				node.transform = next_transform
				changed = true
	return changed


func _apply_lights() -> void:
	# A model with no .3di lights keeps the count-0 light defaults written at material
	# creation (_create_material); evaluate_lights would return empty and this loop would
	# only re-write those same defaults every frame. _has_lights is recomputed in rebuild(),
	# the only path that can change the (immutable, IR-backed) light count.
	if not _has_lights:
		return
	if object_data == null or not object_data.has_method("evaluate_lights"):
		return
	var lights: Array = object_data.evaluate_lights(_anim_time_ms, _ctrl_values)
	var dominant := {}
	var best_intensity := -1.0
	for light in lights:
		var info: Dictionary = light
		var intensity := float(info.get("intensity", 1.0))
		if intensity > best_intensity:
			best_intensity = intensity
			dominant = info
	var count := 0 if dominant.is_empty() else 1
	var position: Vector3 = dominant.get("position", Vector3.ZERO)
	var color: Color = dominant.get("color", Color.WHITE)
	var atten_start := float(dominant.get("atten_start", 0.0))
	var atten_end := float(dominant.get("atten_end", 5.0))
	var subobject := int(dominant.get("subobject", -1))
	if subobject >= 0 and _robj_nodes.has(subobject):
		var node := _robj_nodes[subobject] as Node3D
		position = node.global_transform * position
	for material in _surface_materials:
		if material == null:
			continue
		material.set_shader_parameter("u_local_light_count", count)
		material.set_shader_parameter("u_local_light_position", position)
		material.set_shader_parameter("u_local_light_color", Vector3(color.r, color.g, color.b))
		material.set_shader_parameter("u_local_light_intensity", best_intensity if best_intensity > 0.0 else 1.0)
		material.set_shader_parameter("u_local_light_atten_start", atten_start)
		material.set_shader_parameter("u_local_light_atten_end", atten_end)


func _material_for_index(material_array_index: int) -> ShaderMaterial:
	if _material_cache.has(material_array_index):
		return _material_cache[material_array_index]
	var material_def: Dictionary = _material_defs.get(material_array_index, {})
	var material := _create_material(material_array_index, material_def)
	_material_cache[material_array_index] = material
	return material


func _create_material(index: int, material_def: Dictionary) -> ShaderMaterial:
	var material := ShaderMaterial.new()
	var material_index := int(material_def.get("index", index))
	var info := object_data.get_material_info(material_index) if object_data != null and material_index >= 0 and material_index < object_data.get_material_count() else {}
	var shader_tag := String(info.get("shader_tag", material_def.get("shader", "FF_ST_OP")))
	if shader_tag.is_empty():
		shader_tag = "FF_ST_OP"
	var material_flags := 0
	if bool(info.get("alpha_test_enabled", (int(material_def.get("flags", 0)) & NovaObjectShaderCache.MATERIAL_FLAG_ALPHA_TEST) != 0)):
		material_flags |= NovaObjectShaderCache.MATERIAL_FLAG_ALPHA_TEST
	if bool(info.get("alpha_invert", (int(material_def.get("flags", 0)) & NovaObjectShaderCache.MATERIAL_FLAG_ALPHA_INVERT) != 0)):
		material_flags |= NovaObjectShaderCache.MATERIAL_FLAG_ALPHA_INVERT
	if bool(info.get("two_sided", (int(material_def.get("flags", 0)) & NovaObjectShaderCache.MATERIAL_FLAG_TWO_SIDED) != 0)):
		material_flags |= NovaObjectShaderCache.MATERIAL_FLAG_TWO_SIDED
	var emissive_type := 2 if bool(info.get("emissive", false)) else int(material_def.get("emissive_type", 0))
	var is_glass_flag := 1 if bool(info.get("is_glass", material_def.get("is_glass", false))) else 0
	var alpha_test_byte := int(info.get("alpha_test", roundi(float(material_def.get("alpha_threshold", 0.0)) * 255.0)))
	var shader_cache := NovaObjectShaderCache.get_singleton()
	var key := shader_cache.classify(shader_tag, material_flags, emissive_type, is_glass_flag, alpha_test_byte)
	material.shader = shader_cache.get_shader_for_key(key)

	var diffuse := _load_texture_for_slot(material_def, 1)
	var detail := _load_texture_for_slot(material_def, 2)
	var normal := _load_texture_for_slot(material_def, 3)
	if normal == null:
		normal = _load_texture_for_slot(material_def, 4)
	if diffuse == null and detail != null:
		diffuse = detail
		detail = null
	if diffuse != null:
		material.set_shader_parameter("u_diffuse", diffuse)
	else:
		material.set_shader_parameter("u_diffuse", _solid_colour_texture(_hash_color_for_index(index)))
	if detail != null:
		material.set_shader_parameter("u_detail", detail)
	else:
		material.set_shader_parameter("u_detail", _solid_colour_texture(Color.WHITE))
	if normal != null:
		material.set_shader_parameter("u_normal_map", normal)
	else:
		material.set_shader_parameter("u_normal_map", _solid_colour_texture(Color(0.5, 0.5, 1.0, 1.0)))
	if (material_flags & NovaObjectShaderCache.MATERIAL_FLAG_ALPHA_TEST) != 0:
		## The ref byte feeds the compare exactly; the shader keeps a > ref
		## (invert: a <= ref), so no epsilon fudge is needed for ref 0.
		## [orig: CGfxDevice_SetAlphaTestRef @ 0x6770a0]
		material.set_shader_parameter("u_alpha_test_threshold", float(alpha_test_byte) / 255.0)
		material.set_shader_parameter("u_alpha_test_invert", 1.0 if (material_flags & NovaObjectShaderCache.MATERIAL_FLAG_ALPHA_INVERT) != 0 else 0.0)
	else:
		material.set_shader_parameter("u_alpha_test_threshold", 0.0)
		material.set_shader_parameter("u_alpha_test_invert", 0.0)
	var reflect: Color = info.get("reflect_color", Color(0.7, 0.8, 0.9, 0.35))
	material.set_shader_parameter("u_reflect_color", reflect)
	material.set_shader_parameter("u_uv_offset", Vector2.ZERO)
	material.set_shader_parameter("u_uv_scale", Vector2.ONE)
	material.set_shader_parameter("u_uv_rotation", 0.0)
	material.set_shader_parameter("u_rgb_mod", Vector3.ONE)
	material.set_shader_parameter("u_alpha_mod", 1.0)
	material.set_shader_parameter("u_emissive", 1.0 if bool(info.get("emissive", false)) else 0.0)
	material.set_shader_parameter("u_local_light_count", 0)
	material.set_shader_parameter("u_local_light_position", Vector3.ZERO)
	material.set_shader_parameter("u_local_light_color", Vector3.ONE)
	material.set_shader_parameter("u_local_light_intensity", 1.0)
	material.set_shader_parameter("u_local_light_atten_start", 0.0)
	material.set_shader_parameter("u_local_light_atten_end", 5.0)
	_apply_default_environment_to_material(material)
	return material


func _load_texture_for_slot(material_def: Dictionary, slot: int) -> Texture2D:
	if object_data == null or material_def.is_empty():
		return null
	var textures: Array = material_def.get("textures", [])
	if textures.is_empty():
		return null

	var material_index := int(material_def.get("index", -1))
	if material_index < 0:
		return null

	for i in range(textures.size()):
		var texture: Dictionary = textures[i]
		if int(texture.get("slot", 0)) == slot:
			var loaded: Texture2D = object_data.load_material_texture(material_index, i)
			if loaded != null:
				return loaded
	return null


func _collect_anim_frames(material_index: int) -> void:
	if _anim_frames_by_mat.has(material_index) or object_data == null or not object_data.has_method("get_material_anim_frames"):
		return
	var frame_names: PackedStringArray = object_data.get_material_anim_frames(material_index, 1)
	if frame_names.size() <= 1:
		return
	var frames: Array = []
	for frame_name in frame_names:
		frames.append(_load_texture_name(frame_name))
	_anim_frames_by_mat[material_index] = frames


func _load_texture_name(texture_name: String) -> Texture2D:
	if object_data == null or texture_name.is_empty():
		return null
	return object_data.load_texture_name(texture_name)


func _hash_color_for_index(idx: int) -> Color:
	var h := fposmod(float(idx) * 0.61803398, 1.0)
	return Color.from_hsv(h, 0.35, 0.85)


func _solid_colour_texture(color: Color) -> ImageTexture:
	var image := Image.create(1, 1, false, Image.FORMAT_RGBA8)
	image.set_pixel(0, 0, color)
	return ImageTexture.create_from_image(image)


func _apply_default_environment_to_material(material: ShaderMaterial) -> void:
	material.set_shader_parameter("u_ambient_color", DEFAULT_AMBIENT_COLOR)
	material.set_shader_parameter("u_dir_light_dir", DEFAULT_DIR_LIGHT_DIR)
	material.set_shader_parameter("u_dir_light_color", DEFAULT_DIR_LIGHT_COLOR)
	material.set_shader_parameter("u_fill_light_color", DEFAULT_FILL_LIGHT_COLOR)
	material.set_shader_parameter("u_fog_enabled", false)
	material.set_shader_parameter("u_fog_color", DEFAULT_FOG_COLOR)
	material.set_shader_parameter("u_fog_start", DEFAULT_FOG_START)
	material.set_shader_parameter("u_fog_end", DEFAULT_FOG_END)
	material.set_shader_parameter("u_fog_type", DEFAULT_FOG_TYPE)


# A surface material needs per-frame UV/RGB/alpha evaluation only if one of its generators
# animates. The native get_material_runtime_kind (faithful style taxonomy, single-sourced in
# libs/renderer) is preferred when present; until it is built, the conservative fallback
# treats any non-zero generator style as dynamic -- it can only over-evaluate, never freeze
# an animation (a fully-static material's eval is the identity that _create_material already set).
func _material_runtime_is_dynamic(material_index: int) -> bool:
	if object_data == null:
		return true
	if object_data.has_method("get_material_runtime_kind"):
		return int(object_data.get_material_runtime_kind(material_index)) != 0  # 0 == STATIC
	if not object_data.has_method("get_material_info"):
		return true
	var info: Dictionary = object_data.get_material_info(material_index)
	if info.is_empty():
		return true
	return int(info.get("uv_u_style", 0)) != 0 \
		or int(info.get("uv_v_style", 0)) != 0 \
		or int(info.get("rgb_gen_style", 0)) != 0 \
		or int(info.get("alpha_gen_style", 0)) != 0


# Partition the surface materials into those that change at runtime (UV/RGB/alpha generators
# or a multi-frame texture animation) and the static remainder. Only the dynamic slots are
# visited per frame; static slots keep the identity values written at material creation.
func _classify_materials() -> void:
	_material_needs_eval.clear()
	_dynamic_material_slots = PackedInt32Array()
	var kind_cache: Dictionary = {}
	for i in range(_surface_materials.size()):
		var material_index := int(_surface_material_indices[i])
		var needs_eval: bool
		if kind_cache.has(material_index):
			needs_eval = bool(kind_cache[material_index])
		else:
			needs_eval = _material_runtime_is_dynamic(material_index)
			kind_cache[material_index] = needs_eval
		_material_needs_eval.append(needs_eval)
		var frames: Array = _anim_frames_by_mat.get(material_index, [])
		if needs_eval or frames.size() > 1:
			_dynamic_material_slots.append(i)


func _apply_environment_to_materials() -> void:
	# The environment is shared and changes slowly (time-of-day) or not at all. NovaWeather
	# re-stamps it every frame, but the smoothed colours quantise to identical bytes once
	# settled, so the 9 values these materials consume are byte-stable in steady state. Skip
	# the 9 cross-language reads + 9-uniform-per-material push when nothing changed since the
	# last push: a NovaEnvironment generation makes the steady-state check a single int
	# compare; the value cache is the fallback for env nodes without one. Either way the skip
	# only ever omits re-pushing identical uniforms (retained mode -> invisible), so the
	# rendered lighting/fog is byte-identical to pushing every frame.
	var gen := -1
	var has_gen: bool = _environment_node != null and _environment_node.has_method("get_env_generation")
	if has_gen:
		gen = int(_environment_node.get_env_generation())
		if gen == _last_env_gen and not _last_env_values.is_empty():
			return
	var values := _environment_values()
	if _env_values_equal(values, _last_env_values):
		_last_env_gen = gen
		return
	_last_env_values = values
	_last_env_gen = gen
	for material in _surface_materials:
		if material == null:
			continue
		material.set_shader_parameter("u_ambient_color", values.get("ambient", DEFAULT_AMBIENT_COLOR))
		material.set_shader_parameter("u_dir_light_dir", values.get("dir", DEFAULT_DIR_LIGHT_DIR))
		material.set_shader_parameter("u_dir_light_color", values.get("dir_color", DEFAULT_DIR_LIGHT_COLOR))
		material.set_shader_parameter("u_fill_light_color", values.get("fill", DEFAULT_FILL_LIGHT_COLOR))
		material.set_shader_parameter("u_fog_enabled", bool(values.get("fog_enabled", false)))
		material.set_shader_parameter("u_fog_color", values.get("fog_color", DEFAULT_FOG_COLOR))
		material.set_shader_parameter("u_fog_start", float(values.get("fog_start", DEFAULT_FOG_START)))
		material.set_shader_parameter("u_fog_end", float(values.get("fog_end", DEFAULT_FOG_END)))
		material.set_shader_parameter("u_fog_type", int(values.get("fog_type", DEFAULT_FOG_TYPE)))


func _environment_values() -> Dictionary:
	if _environment_node == null or not _environment_node.has_method("is_loaded") or not _environment_node.call("is_loaded"):
		return {
			"ambient": DEFAULT_AMBIENT_COLOR,
			"dir": DEFAULT_DIR_LIGHT_DIR,
			"dir_color": DEFAULT_DIR_LIGHT_COLOR,
			"fill": DEFAULT_FILL_LIGHT_COLOR,
			"fog_enabled": false,
			"fog_color": DEFAULT_FOG_COLOR,
			"fog_start": DEFAULT_FOG_START,
			"fog_end": DEFAULT_FOG_END,
			"fog_type": DEFAULT_FOG_TYPE,
		}
	var sun_dir: Vector3 = _environment_node.call("get_sun_direction")
	if sun_dir.length() <= 0.001:
		sun_dir = -DEFAULT_DIR_LIGHT_DIR
	return {
		"ambient": _environment_node.call("get_sky_ambient"),
		"dir": -sun_dir.normalized(),
		"dir_color": _environment_node.call("get_sun_light"),
		"fill": _environment_node.call("get_fill_light"),
		"fog_enabled": true,
		"fog_color": _environment_node.call("get_fog_color"),
		"fog_start": _environment_node.call("get_fog_start"),
		"fog_end": _environment_node.call("get_fog_level"),
		"fog_type": _environment_node.call("get_fog_type"),
	}


func _set_model_bounds(bounds: AABB) -> void:
	if _aabb_equal_approx(_model_bounds, bounds):
		return
	_model_bounds = bounds
	bounds_changed.emit(_model_bounds)


func _aabb_equal_approx(a: AABB, b: AABB) -> bool:
	return a.position.is_equal_approx(b.position) and a.size.is_equal_approx(b.size)


# True when two _environment_values() dicts carry the same lighting/fog the shaders consume.
# Colours are compared with is_equal_approx (the weather smoother quantises to 8-bit, so real
# changes are >= 1/255, far above epsilon); an empty cache (first push after rebuild) is never
# equal, forcing the initial push.
func _env_values_equal(a: Dictionary, b: Dictionary) -> bool:
	if a.is_empty() or b.is_empty():
		return false
	return (a.get("ambient", Vector3.ZERO) as Vector3).is_equal_approx(b.get("ambient", Vector3.ONE)) \
		and (a.get("dir", Vector3.ZERO) as Vector3).is_equal_approx(b.get("dir", Vector3.ONE)) \
		and (a.get("dir_color", Vector3.ZERO) as Vector3).is_equal_approx(b.get("dir_color", Vector3.ONE)) \
		and (a.get("fill", Vector3.ZERO) as Vector3).is_equal_approx(b.get("fill", Vector3.ONE)) \
		and bool(a.get("fog_enabled", false)) == bool(b.get("fog_enabled", true)) \
		and (a.get("fog_color", Vector3.ZERO) as Vector3).is_equal_approx(b.get("fog_color", Vector3.ONE)) \
		and is_equal_approx(float(a.get("fog_start", 0.0)), float(b.get("fog_start", -1.0))) \
		and is_equal_approx(float(a.get("fog_end", 0.0)), float(b.get("fog_end", -1.0))) \
		and int(a.get("fog_type", 0)) == int(b.get("fog_type", -1))
