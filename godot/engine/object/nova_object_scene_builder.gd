extends RefCounted

# NovaObjectModel's retained-scene construction module. Its single rebuild()
# interface hides child teardown, cache reset, Skeleton3D/Skin construction,
# mesh/material assembly, and the initial runtime-state application. All state
# stays on the composing NovaObjectModel, reached through `_m`; the owner owns
# this helper for its full Node lifetime and the plain Node back-reference
# cannot form a RefCounted ownership cycle.

var _m


func _init(model) -> void:
	_m = model


func rebuild() -> void:
	for child in _m.get_children():
		_m.remove_child(child)
		child.queue_free()
	_m._robj_nodes.clear()
	_m._robj_rest_transforms.clear()
	_m._skeleton = null
	_m._skeleton_skin = null
	_m._muzzle_bone = -1
	_m._surface_material_indices.clear()
	_m._surface_materials.clear()
	_m._surface_lighting_contexts.clear()
	_m._alpha_materials.clear()
	_m._anim_frames_by_mat.clear()
	_m._material_cache.clear()
	_m._material_defs.clear()
	_m._body_pose_dirty = true
	_m._bounds_dirty = true
	_m._has_lights = false
	_m._has_live_panm = false
	_m._material_needs_eval.clear()
	_m._dynamic_material_slots = PackedInt32Array()
	_m._last_env_gen = -1
	_m._last_env_values = null
	_m._last_section_env_values = null
	_m._last_light_push_valid = false
	_m._robj_dense = []
	_m._panm_applied_revision = 0
	if _m.object_data == null or not _m.object_data.has_document():
		_m._od_has_eval = false
		_m._od_has_frame = false
		_m._set_model_bounds(AABB())
		return
	_m._od_has_eval = true
	_m._od_has_frame = true

	_m._material_defs = _m._build_material_defs()
	_m._active_lod = _m._clamp_lod_index(_m._active_lod)
	_m._refresh_live_panm_classification()
	# A loaded .adm drives the model: build a Skeleton3D from its .bad skeleton. This applies to
	# BOTH per-vertex skinned models (organic bodies/arms) AND rigid models (first-person weapons) --
	# rigid parts ride a bone via "fake skinning" (build_lod_submeshes(skeletal=true)). Without a
	# .adm, no skeleton is built and the model renders static exactly as before.
	var skeletal_mode: bool = _m._skeletal != null and _m._skeletal.is_loaded()
	if skeletal_mode:
		_build_skeleton()
		_m._resolve_muzzle_userpoint()
	var bone_count: int = (
			_m._skeleton.get_bone_count()
			if skeletal_mode and _m._skeleton != null
			else 0)
	var submeshes: Array = _m.object_data.build_lod_submeshes(
			_m._active_lod, skeletal_mode, bone_count, _m.native_frame)
	if submeshes.is_empty():
		submeshes = _m._legacy_submeshes_from_surfaces(_m._active_lod)
	for entry in submeshes:
		var submesh: Dictionary = entry
		var mesh := submesh.get("mesh") as ArrayMesh
		if mesh == null:
			continue
		var robj_index := int(submesh.get(
				"robj_index", submesh.get("part_index", 0)))
		var material_index := int(submesh.get("material_index", 0))
		if not _m._robj_rest_transforms.has(robj_index):
			_m._robj_rest_transforms[robj_index] = Transform3D(
					Basis.IDENTITY, submesh.get("abs", Vector3.ZERO))
		var lighting_context: int = _m._lighting_context_for_robj(robj_index)
		var instance := MeshInstance3D.new()
		instance.mesh = mesh
		instance.cast_shadow = (
				GeometryInstance3D.SHADOW_CASTING_SETTING_ON
				if _m._shadow_caster_layers != 0
				else GeometryInstance3D.SHADOW_CASTING_SETTING_OFF)
		instance.layers = (
				instance.layers
				& ~NovaWater.VISUAL_LAYER_SHADOW_CASTER_MASK
				) | _m._shadow_caster_layers
		var material: ShaderMaterial = _m._material_for_index(
				material_index, lighting_context)
		instance.material_override = material
		# Skinned + rigid-fake-skinned submeshes bind to the shared Skeleton3D; everything else
		# stays under its render-object (Robj) part node so PANM part transforms keep working.
		if (skeletal_mode and _m._skeleton != null
				and bool(submesh.get("is_skinned", false))):
			_m._skeleton.add_child(instance)
			instance.skin = _m._skeleton_skin
			instance.skeleton = instance.get_path_to(_m._skeleton)
		else:
			var node: Node3D = _m._get_or_create_robj_node(robj_index)
			node.add_child(instance)
		_m._surface_material_indices.append(material_index)
		_m._surface_materials.append(material)
		_m._surface_lighting_contexts.append(lighting_context)
		_m._collect_anim_frames(material_index)

	_m._classify_materials()
	_m._has_lights = int(_m.object_data.get_light_count()) > 0
	_m._apply_runtime_state(0.0)
	_m.refresh_render_order()


# Build the Skeleton3D + rest-derived Skin from the loaded NovaSkeletalAnim.
# Bones come from the .bad skeleton (names/parents/parent-local bind rest). The
# Skin binds each bone with its global-rest inverse, so a skinned mesh renders
# exactly at rest when the pose equals the rest.
# [orig: BoneFile_Load @0x40fff0 builds the runtime skeleton.]
func _build_skeleton() -> void:
	_m._skeleton = Skeleton3D.new()
	_m._skeleton.name = "Skeleton3D"
	# This model writes final bone poses directly and only parents skinned
	# MeshInstance3D nodes below the skeleton; it never installs a
	# SkeletonModifier3D, BoneAttachment3D, or physical-bone simulator. Godot's
	# default IDLE modifier mode registers an internal process callback anyway.
	# MANUAL removes that empty per-frame modifier pass; pose setters still queue
	# the independent deferred skeleton/skin update.
	_m._skeleton.modifier_callback_mode_process = (
			Skeleton3D.MODIFIER_CALLBACK_MODE_PROCESS_MANUAL)
	_m.add_child(_m._skeleton)
	var bones: Array = _m._skeletal.get_skeleton_bones()
	# The rig is index-driven (the model bone table pairs channels/parts by row —
	# net-re §5.40; empty or duplicate row names are legal in shipped models,
	# e.g. REVX M82_1st carries unnamed rows). Godot refuses empty, duplicate,
	# ':', and '/' names; unique placeholders preserve row i as bone i instead
	# of shifting every later binding.
	var used := {}
	for i in range(bones.size()):
		var n := String((bones[i] as Dictionary).get("name", "")).strip_edges()
		n = n.replace(":", "_").replace("/", "_")
		if n.is_empty():
			n = "bone_%d" % i
		if used.has(n):
			n = "%s_%d" % [n, i]
		used[n] = true
		_m._skeleton.add_bone(n)
	for i in range(bones.size()):
		var bd: Dictionary = bones[i]
		var parent := int(bd.get("parent_index", -1))
		if (parent >= 0 and parent < _m._skeleton.get_bone_count()
				and parent != i):
			_m._skeleton.set_bone_parent(i, parent)
		_m._skeleton.set_bone_rest(i, bd.get("rest", Transform3D()))
	for i in range(_m._skeleton.get_bone_count()):
		_m._skeleton.reset_bone_pose(i)
	_m._skeleton_skin = _m._skeleton.create_skin_from_rest_transforms()
