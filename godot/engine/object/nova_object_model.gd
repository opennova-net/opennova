class_name NovaObjectModel
extends Node3D

signal bounds_changed(bounds: AABB)

# The per-material 3DI flag byte constants live on NovaObjectShaderCache,
# single-sourced from libs/threedi (THREEDI_MATERIAL_FLAG_*) — REN-2.
# The OED re-export mask bits are aliases of the NovaObjectData binding,
# single-sourced from libs/oed (OED_UPDATE_*) — ENG-4.
const OED_UPDATE_NONE := NovaObjectData.UPDATE_NONE
const OED_UPDATE_MTRL := NovaObjectData.UPDATE_MTRL
const OED_UPDATE_LGHT := NovaObjectData.UPDATE_LGHT
const OED_UPDATE_PANM := NovaObjectData.UPDATE_PANM
const OED_UPDATE_ALL := NovaObjectData.UPDATE_ALL

# The witnessed lighting uniform surface (REN-5): HemiSky/HemiGround/DirLight
# + ColorSrcGlobalGain; the composer applies the fixed-function
# MODULATE2X model (docs/render/render-lighting-re.md). Defaults for un-enved
# scenes (previews, no environment node) are the RETAIL NOON register — the
# shipped full_00.env tod 1200 block bytes /255 (sun_rgb 170,170,167;
# sky_rgb 84,88,89; ground_rgb 49,55,46), so a preview lights like a JO noon
# world instead of an invented dusk. Engine-fed values come from the env
# blocks below. (Must stay equal to the composer's uniform defaults —
# libs/renderer/src/object_shader_template.cpp.)
const DEFAULT_HEMI_SKY_COLOR := Vector3(84.0 / 255.0, 88.0 / 255.0, 89.0 / 255.0)
const DEFAULT_DIR_LIGHT_DIR := Vector3(-0.4082, -0.8165, -0.4082)
const DEFAULT_DIR_LIGHT_COLOR := Vector3(170.0 / 255.0, 170.0 / 255.0, 167.0 / 255.0)
const DEFAULT_HEMI_GROUND_COLOR := Vector3(49.0 / 255.0, 55.0 / 255.0, 46.0 / 255.0)
const DEFAULT_COLOR_SRC_GAIN := Vector3.ONE
const DEFAULT_FOG_COLOR := Vector3(0.5, 0.6, 0.8)
const DEFAULT_FOG_START := 0.0
const DEFAULT_FOG_END := 1024.0
const DEFAULT_FOG_TYPE := 0
const LIGHTING_CONTEXT_ENTITY := 0
const LIGHTING_CONTEXT_INTERIOR_SECTION := 1
const SUN_SHADOW_CATCHER_SHADER := preload(
		"res://shaders/sun_shadow_catcher.gdshader")

# W4-6c split: cohesive animation, material/environment, and retained-scene
# construction clusters live in RefCounted helpers; ALL state stays here.
const NovaObjectBodyAnim := preload("res://engine/object/nova_object_body_anim.gd")
const NovaObjectMaterials := preload("res://engine/object/nova_object_materials.gd")
const NovaObjectSceneBuilder := preload("res://engine/object/nova_object_scene_builder.gd")
# The ADR 0017 typed env record moved with the materials helper; this alias
# keeps NovaObjectModel.EnvLightValues the public type (mission_object_placer,
# tests) and the owner annotations unchanged.
const EnvLightValues = NovaObjectMaterials.EnvLightValues

var object_data: NovaObjectData

var _material_cache: Dictionary = {}
# Blended (non-opaque) materials, for the water-side render-order rung (REN-3).
var _alpha_materials: Array[ShaderMaterial] = []
var _material_defs: Dictionary = {}
var _robj_nodes: Dictionary = {}
var _robj_rest_transforms: Dictionary = {}
# Per-frame hot-path caches: the object_data document-loaded gates refresh in
# rebuild(); the environment-node capability refreshes on assignment. The typed
# light-push cache holds the last-applied uniform set so identical values are
# never re-pushed without allocating a comparison Array (retained mode — a
# skipped identical push is invisible).
var _od_has_eval := false
var _od_has_frame := false
var _env_has_generation := false
var _last_light_push_valid := false
var _last_light_count := 0
var _last_light_position := Vector3.ZERO
var _last_light_color := Color.WHITE
var _last_light_intensity := 0.0
var _last_light_atten_start := 0.0
var _last_light_atten_end := 0.0
# Dense part-index -> Node3D array + the PANM revision this model last applied
# (apply_panm_to_nodes skips every node write while the shared evaluation's
# revision is unchanged; 0 forces the fresh-node full apply after rebuild).
var _robj_dense: Array = []
var _panm_applied_revision := 0
# The applied per-section render mask (-1 = everything visible); see
# set_section_visibility_mask.
var _section_visibility_mask: int = -1
var _surface_material_indices: PackedInt32Array = PackedInt32Array()
var _surface_materials: Array[ShaderMaterial] = []
var _surface_lighting_contexts: PackedByteArray = PackedByteArray()
var _anim_frames_by_mat: Dictionary = {}
# This retained model stores the latest CTRL snapshot applied to it.
var _ctrl_values: Dictionary = {}
# Presentation ownership is bookkeeping around retail's single current value,
# not a stack of values. It prevents an older presenter's teardown from
# clearing a later store, but never resurrects an overwritten value.
var _ctrl_value_owners: Dictionary = {} # register -> current presentation owner
var _ctrl_batch_depth := 0
var _ctrl_batch_dirty := false
var _part_anims: Dictionary = {}
var _part_anim_tick_accum_s := 0.0
var _anim_time_ms: int = 0
var _panm_clock
var _active_lod: int = 0
var _is_playing := true
var _model_bounds := AABB()
var _environment_node: Node
# Per-entity lighting state. Retail applies these after it has selected the
# current ENV block and before submitting each model:
#   - outdoors: directional colour * (4 - blocked across three radius casts) / 4
#   - inside a blink volume: the containing building's light_transfer lerp
# [orig: Entity_ComputeSunVisibility @0x5C6800;
#  setup_entity_lighting_and_shader_constants @0x5D98A0]
var _lighting_effect_scale := 1.0
var _interior_lerp := false
var _interior_daylight := 0.0
# Portal-building ROBJ 0 is the exterior shell. Every non-zero ROBJ is submitted
# with the interior-lighting batch bit and therefore needs a separately cached
# material even when it reuses the exterior's material index.
# [orig: object collector @0x5D9156..0x5D9170]
var _interior_section_lighting := false
var _interior_section_daylight := 0.0
# Retail keeps two explicit caster populations: people/DynamicShadow items feed
# pose-derived live projection slots, while pool-2/StaticShadow models feed the
# terrain tile cache. Marker layers let the two shadow-only Godot lights select
# those populations independently without re-lighting the fixed-function base.
# [orig: Entity_InitFromModel @0x40E1BC..0x40E1F7;
#  Terrain_CollectAndRenderTileModels @0x60D250]
var _shadow_caster_layers := 0
var _shadow_receiver_material: ShaderMaterial

# NATIVE-frame build: meshes emitted without the (-x,y,z) import flip (winding re-reversed),
# for the first-person viewmodel rigs whose skeletal runtime (model_bind) poses in the native
# model frame; the owner maps the whole rig to the camera in one container transform. Set by
# the placer BEFORE set_object_data. World models keep the default flipped frame.
var native_frame := false

# Main-body skeletal animation (.bad/.adm via NovaSkeletalAnim). Distinct from PANM
# (vehicle part channels above): this drives a Skeleton3D built from the .bad skeleton,
# with the skinned meshes bound through a rest-derived Skin. _skeletal is null for static
# / non-organic models, in which case nothing here runs and the model renders as before.
var _skeletal                       # NovaSkeletalAnim, or null
var _skeleton: Skeleton3D           # built in rebuild() when skinned + _skeletal loaded
var _skeleton_skin: Skin
# The gun-flash muzzle userpoint, resolved for the AI fire-origin seam (D-AI-6):
# bone = the userpoint's subobject row (the rig is index-driven: row i is bone i),
# position = the authored model-space point. [orig: the anim-event fire transforms
# the fire-bone userpoint by the live pose — Entity_GetAttachmentWorldPosition
# @0x4b2670 over the model userpoint table @model+0xC0]
var _muzzle_bone := -1
var _muzzle_model_pos := Vector3.ZERO
var _anim_key := ""                 # active clip key (ADM key, e.g. "anim_walk")
var _anim_variant := 0              # same-key variant index (multi-clip .adm rows; the
                                    # sim's ring latch picks it for viewmodel plays
                                    # [orig: AnimMap_PlayAnimBySlot @0x40bda0 latch +68])
var _anim_time := 0.0               # playhead seconds into the active clip
var _anim_playing := false
var _anim_external_phase := false   # true when the sim, not _process(delta), owns _anim_time
# Retail remote-body request channel. The wire may advertise a pending state
# while the current clip's flags defer acceptance, so current/pending ownership
# belongs beside this model's playhead and completion clock.
var _remote_state := -1
var _remote_flags := 0
var _remote_pending_state := -1
var _remote_pending_key := ""
var _remote_pending_flags := 0
var _remote_pending_end_time := INF
# Receive-side fixed-tick blend reconstruction retains the outgoing channel
# for 10/15 ticks; WirePresentPass, never _process, advances these clocks.
var _remote_blend_active := false
var _remote_blend_source_key := ""
var _remote_blend_source_phase_ticks := 0
var _remote_blend_source_time := 0.0
var _remote_blend_target_phase_ticks := 0
var _remote_blend_weight := 1.0
var _remote_blend_step := 0.0
var _body_pose_dirty := true
var _bounds_dirty := true
# Applied-phase stamp for the external-phase body path: play_body_clip_at() can
# prove a repeat call identical (same key, same tick, still external, pose clean)
# before paying clip lookups or a playhead scrub. Any other writer of the
# key/playhead/external state drops the stamp (directly or via _set_body_playhead).
var _body_phase_stamp_valid := false
var _body_phase_ticks_applied := 0
# Empty source / weight 1 selects the steady-state single-channel path.
var _body_blend_source_key := ""
var _body_blend_source_time := 0.0
var _body_blend_weight := 1.0
# Single-entry slot->key cache for the per-frame play_body_anim_at() path; a
# skeletal swap invalidates it.
var _last_slot_resolved := -1
var _last_slot_key := ""

# Per-frame work skips. Each cached value is re-derived in rebuild() (or on the exact
# mutator), so a skip only ever omits re-pushing state that is byte-identical to what is
# already resident on the materials -- invisible in Godot's retained-mode renderer, and so
# parity-preserving against the original engine's output.
var _has_lights := false
var _has_live_panm := false
var _material_needs_eval: Array[bool] = []          # parallel to _surface_materials
var _dynamic_material_slots: PackedInt32Array = PackedInt32Array()
var _last_env_gen := -1
var _last_env_values: EnvLightValues = null
var _last_section_env_values: EnvLightValues = null
# Retail loads LGHT records with the model resource but never reads that field
# on the gameplay render path. Keep them available only to the explicit object
# editor preview; ordinary runtime models retain u_local_light_count = 0.
# [orig: parse_lights_chunk @0x5B47B0; model field +0xCC has no post-load
#  renderer read in Jointops.exe]
var _model_light_preview_enabled := false


# The W4-6c section helpers (RefCounted; a plain back-ref cannot cycle --
# the owner is a manually-managed Node3D). Constructed in _init so they
# exist before any pre-_ready set_object_data/rebuild.
var _body_anim: NovaObjectBodyAnim
var _materials: NovaObjectMaterials
var _scene_builder: NovaObjectSceneBuilder


func _init() -> void:
	_body_anim = NovaObjectBodyAnim.new(self)
	_materials = NovaObjectMaterials.new(self)
	_scene_builder = NovaObjectSceneBuilder.new(self)


func _ready() -> void:
	set_process(true)
	if object_data != null:
		rebuild()


func set_object_data(value: NovaObjectData) -> void:
	if object_data != null and object_data.object_changed.is_connected(_on_object_changed):
		object_data.object_changed.disconnect(_on_object_changed)
	object_data = value
	reset_remote_body_state()
	_part_anims.clear()
	_part_anim_tick_accum_s = 0.0
	_active_lod = _clamp_lod_index(_active_lod)
	if object_data != null and not object_data.object_changed.is_connected(_on_object_changed):
		object_data.object_changed.connect(_on_object_changed, CONNECT_DEFERRED)
	rebuild()


func get_object_data() -> NovaObjectData:
	return object_data


func set_model_light_preview_enabled(enabled: bool) -> void:
	if _model_light_preview_enabled == enabled:
		return
	_model_light_preview_enabled = enabled
	_last_light_push_valid = false
	if enabled:
		_apply_lights()
		return
	for material in _surface_materials:
		if material != null:
			material.set_shader_parameter("u_local_light_count", 0)


func set_shadow_caster_enabled(enabled: bool) -> void:
	_set_shadow_caster_layer_enabled(
			NovaWater.VISUAL_LAYER_DYNAMIC_SHADOW_CASTER, enabled)


func is_shadow_caster_enabled() -> bool:
	return (_shadow_caster_layers \
			& NovaWater.VISUAL_LAYER_DYNAMIC_SHADOW_CASTER) != 0


func set_static_shadow_caster_enabled(enabled: bool) -> void:
	_set_shadow_caster_layer_enabled(
			NovaWater.VISUAL_LAYER_STATIC_SHADOW_CASTER, enabled)


func is_static_shadow_caster_enabled() -> bool:
	return (_shadow_caster_layers \
			& NovaWater.VISUAL_LAYER_STATIC_SHADOW_CASTER) != 0


func _set_shadow_caster_layer_enabled(layer: int, enabled: bool) -> void:
	var next_layers := _shadow_caster_layers | layer \
			if enabled else _shadow_caster_layers & ~layer
	if next_layers == _shadow_caster_layers:
		return
	_shadow_caster_layers = next_layers
	_apply_shadow_casting_below(self)


func _apply_shadow_casting_below(root: Node) -> void:
	var setting := GeometryInstance3D.SHADOW_CASTING_SETTING_ON \
			if _shadow_caster_layers != 0 \
			else GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	for child in root.get_children():
		if child is GeometryInstance3D:
			var geometry := child as GeometryInstance3D
			geometry.cast_shadow = setting
			geometry.layers = (
					geometry.layers
					& ~NovaWater.VISUAL_LAYER_SHADOW_CASTER_MASK
					) | _shadow_caster_layers
		_apply_shadow_casting_below(child)


func set_environment_node(value: Node) -> void:
	_environment_node = value
	_env_has_generation = value != null and value.has_method("get_env_generation")
	_last_env_gen = -1
	_last_env_values = null
	_last_section_env_values = null
	_apply_environment_to_materials()


func set_entity_lighting_context(effect_scale: float, interior_lerp: bool,
		interior_daylight: float) -> void:
	var next_effect := clampf(effect_scale, 0.0, 1.0)
	var next_daylight := clampf(interior_daylight, 0.0, 1.0)
	if is_equal_approx(_lighting_effect_scale, next_effect) \
			and _interior_lerp == interior_lerp \
			and is_equal_approx(_interior_daylight, next_daylight):
		return
	_lighting_effect_scale = next_effect
	_interior_lerp = interior_lerp
	_interior_daylight = next_daylight
	_last_env_gen = -1
	_last_env_values = null
	_last_section_env_values = null
	_apply_environment_to_materials()


func set_interior_section_light_transfer(daylight: float) -> void:
	var next_daylight := clampf(daylight, 0.0, 1.0)
	if _interior_section_lighting \
			and is_equal_approx(_interior_section_daylight, next_daylight):
		return
	_interior_section_lighting = true
	_interior_section_daylight = next_daylight
	_last_env_gen = -1
	_last_env_values = null
	_last_section_env_values = null
	# The exterior/interior split is part of the material-cache key. Owners
	# normally configure it before set_object_data(), but preserve correctness
	# for a live reconfiguration too.
	if object_data != null and object_data.has_document():
		rebuild()
	else:
		_apply_environment_to_materials()


func _lighting_context_for_robj(robj_index: int) -> int:
	if _interior_section_lighting and robj_index != 0:
		return LIGHTING_CONTEXT_INTERIOR_SECTION
	return LIGHTING_CONTEXT_ENTITY


func get_model_bounds() -> AABB:
	return _model_bounds


func get_render_part_nodes() -> Dictionary:
	return _robj_nodes


# Per-section render mask: bit N visible = render part (COBJ section) N draws.
# The render-occlusion frame drives this on portal-carrying buildings — bit 0 is
# the exterior, interior sections occupy the low part indices, and the sim has
# already merged the forced-visible def bits. -1 restores everything.
# [orig: g_HiddenSectionMask @ 0xB7965C consumption in Terrain_RenderSectorModels
# @ 0x5c5d30 — the per-draw hidden mask is ~mask; the two-pass open-building
# draw order and the per-light section scoping are renderer-specific legs the
# Godot depth buffer / light model replace (D-OCC-13)]
func set_section_visibility_mask(mask: int) -> void:
	if _section_visibility_mask == mask:
		return
	_section_visibility_mask = mask
	for robj_index in _robj_nodes:
		var node: Node3D = _robj_nodes[robj_index]
		if node != null:
			node.visible = mask == -1 or (mask >> int(robj_index)) & 1 == 1


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


func set_panm_clock(value) -> void:
	_panm_clock = value
	if _panm_clock != null:
		_anim_time_ms = int(_panm_clock.get("time_ms")) & 0xffffffff
	_apply_runtime_state(0.0)


func reset_animation_time() -> void:
	_anim_time_ms = 0
	_anim_time = 0.0
	_anim_external_phase = false
	_body_phase_stamp_valid = false
	reset_remote_body_state()
	_apply_runtime_state(0.0)


# --- W4-6c: the main-body skeletal-animation cluster (clip play/variant/
# seeded selection, remote body-state arbitration, muzzle userpoint,
# playhead scrub, PLAYPARTANIM part-anim channels, aim overlay, pose
# evaluation) moved verbatim to nova_object_body_anim.gd. ALL state stays
# on this owner; the delegates below preserve the external surface and
# test-subclass override dispatch.

func set_skeletal_anim(skeletal) -> void:
	_body_anim.set_skeletal_anim(skeletal)


func get_skeletal_anim():
	return _body_anim.get_skeletal_anim()


func get_skeleton() -> Skeleton3D:
	return _body_anim.get_skeleton()


func has_skeleton() -> bool:
	return _body_anim.has_skeleton()


func has_muzzle() -> bool:
	return _body_anim.has_muzzle()


func get_muzzle_world_position() -> Vector3:
	return _body_anim.get_muzzle_world_position()


func _resolve_muzzle_userpoint() -> void:
	_body_anim._resolve_muzzle_userpoint()


func play_body_clip(key: String) -> void:
	_body_anim.play_body_clip(key)


func play_body_clip_variant(key: String, variant: int) -> void:
	_body_anim.play_body_clip_variant(key, variant)


func play_body_clip_variant_at_time(key: String, variant: int, seconds: float) -> void:
	_body_anim.play_body_clip_variant_at_time(key, variant, seconds)


func play_body_clip_at(key: String, phase_ticks: int) -> void:
	_body_anim.play_body_clip_at(key, phase_ticks)


func play_body_blend_at(source_key: String, source_phase_ticks: int,
		target_key: String, target_phase_ticks: int,
		weight: float) -> void:
	_body_anim.play_body_blend_at(source_key, source_phase_ticks,
			target_key, target_phase_ticks, weight)


func play_body_clip_seeded(key: String, phase_ticks: int) -> void:
	_body_anim.play_body_clip_seeded(key, phase_ticks)


func _select_body_clip_seeded(key: String, phase_ticks: int) -> bool:
	return _body_anim._select_body_clip_seeded(key, phase_ticks)


func apply_remote_body_state(state_id: int, key: String, flags: int,
		phase_ticks: int = -1) -> bool:
	return _body_anim.apply_remote_body_state(state_id, key, flags, phase_ticks)


func reset_remote_body_state() -> void:
	_body_anim.reset_remote_body_state()


func _accept_remote_body_state(state_id: int, key: String, flags: int,
		phase_ticks: int) -> void:
	_body_anim._accept_remote_body_state(state_id, key, flags, phase_ticks)


func _queue_remote_body_state(state_id: int, key: String, flags: int) -> void:
	_body_anim._queue_remote_body_state(state_id, key, flags)


func _clear_remote_body_pending() -> void:
	_body_anim._clear_remote_body_pending()


func advance_remote_body_blend_tick(state_id: int) -> bool:
	return _body_anim.advance_remote_body_blend_tick(state_id)


func remote_body_needs_fixed_tick() -> bool:
	return _body_anim.remote_body_needs_fixed_tick()


func _promote_remote_body_pending_if_due() -> bool:
	return _body_anim._promote_remote_body_pending_if_due()


func stop_body_clip() -> void:
	_body_anim.stop_body_clip()


func get_active_body_clip() -> String:
	return _body_anim.get_active_body_clip()


func play_body_anim(slot: int) -> void:
	_body_anim.play_body_anim(slot)


func play_body_anim_at(slot: int, phase_ticks: int) -> void:
	_body_anim.play_body_anim_at(slot, phase_ticks)


func get_animation_time_ms() -> int:
	return _anim_time_ms


func set_animation_time(seconds: float) -> void:
	_body_anim.set_animation_time(seconds)


func _set_body_playhead(seconds: float) -> void:
	_body_anim._set_body_playhead(seconds)


func get_animation_time() -> float:
	return _body_anim.get_animation_time()


func set_active_lod(lod_index: int) -> void:
	var next_lod := _clamp_lod_index(lod_index)
	if _active_lod == next_lod:
		return
	_active_lod = next_lod
	rebuild()


func get_active_lod() -> int:
	return _active_lod


func _ctrl_dword(value: int) -> int:
	# Retail's global CTRL bus stores signed dwords. Keep exact 0x10000
	# endpoints and negative angular controls instead of narrowing to uint16.
	var next_value := value & 0xFFFFFFFF
	if next_value >= 0x80000000:
		next_value -= 0x100000000
	return next_value


func _finish_ctrl_change(_register: String, apply_now: bool) -> void:
	_bounds_dirty = true
	if apply_now:
		if _ctrl_batch_depth > 0:
			_ctrl_batch_dirty = true
		else:
			_apply_runtime_state(0.0)


## Batch the ordered register stores that precede one retained-model sample.
## Retail writes every relevant global slot and then consumes the model once;
## this avoids advancing shared waveform/random consumers once per individual
## store. Nested callers are supported.
func begin_ctrl_update() -> void:
	_ctrl_batch_depth += 1


func end_ctrl_update() -> void:
	if _ctrl_batch_depth <= 0:
		return
	_ctrl_batch_depth -= 1
	if _ctrl_batch_depth == 0 and _ctrl_batch_dirty:
		_ctrl_batch_dirty = false
		_apply_runtime_state(0.0)


func set_ctrl_value(name: String, value: int) -> void:
	var register := NovaObjectData.canonical_control_register_name(name)
	if register.is_empty():
		return
	var next_value := _ctrl_dword(value)
	if (_ctrl_values.has(register)
			and int(_ctrl_values[register]) == next_value
			and not _ctrl_value_owners.has(register)):
		return
	_ctrl_values[register] = next_value
	_ctrl_value_owners.erase(register)
	_finish_ctrl_change(register, true)


func clear_ctrl_value(name: String) -> void:
	var register := NovaObjectData.canonical_control_register_name(name)
	if register.is_empty() or not _ctrl_values.has(register):
		return
	_ctrl_values.erase(register)
	_ctrl_value_owners.erase(register)
	_finish_ctrl_change(register, true)


## Publish one dedicated retail writer into the register's single current
## value. The owner tag is lifecycle bookkeeping only: a stale teardown cannot
## clear a later writer's store, and overwritten values are never stacked or
## restored. [orig: global CTRL value slots @0x83FCE8, stride 8]
func set_ctrl_override(owner: String, name: String, value: int) -> void:
	var register := NovaObjectData.canonical_control_register_name(name)
	if owner.is_empty() or register.is_empty():
		return
	var next_value := _ctrl_dword(value)
	if (_ctrl_values.has(register)
			and int(_ctrl_values[register]) == next_value
			and String(_ctrl_value_owners.get(register, "")) == owner):
		return
	_ctrl_values[register] = next_value
	_ctrl_value_owners[register] = owner
	_finish_ctrl_change(register, true)


func clear_ctrl_override(owner: String, name: String) -> void:
	var register := NovaObjectData.canonical_control_register_name(name)
	if (owner.is_empty() or register.is_empty()
			or String(_ctrl_value_owners.get(register, "")) != owner):
		return
	_ctrl_value_owners.erase(register)
	_ctrl_values.erase(register)
	_finish_ctrl_change(register, true)


func clear_ctrl_values() -> void:
	if _ctrl_values.is_empty() and _ctrl_value_owners.is_empty():
		return
	_ctrl_values.clear()
	_ctrl_value_owners.clear()
	_finish_ctrl_change("", true)


func get_ctrl_values() -> Dictionary:
	return _ctrl_values.duplicate(true)


# PLAYPARTANIM and its fixed VEHICLE_SPECIAL1/2 CTRL publication live in the
# body-animation helper. These delegates retain the NovaEntityVisual surface.
func play_part_anim(channel: int, play_type: int, time_s: float) -> void:
	_body_anim.play_part_anim(channel, play_type, time_s)


func restart_part_anim(channel: int, play_type: int, time_s: float) -> void:
	_body_anim.restart_part_anim(channel, play_type, time_s)


func set_part_phase(channel: int, phase: int) -> void:
	_body_anim.set_part_phase(channel, phase)


func clear_part_phase(channel: int) -> void:
	_body_anim.clear_part_phase(channel)


func clear_part_anims() -> void:
	_body_anim.clear_part_anims()


func get_active_part_anims() -> Dictionary:
	return _body_anim.get_active_part_anims()


func _resolve_anim_channel_register(slot: int) -> String:
	return _body_anim._resolve_anim_channel_register(slot)


func _advance_part_anims(delta: float) -> bool:
	return _body_anim._advance_part_anims(delta)


# --- third-person aim overlay (the torso bend) ----------------------------------
# One node-frame rotation Basis per anim overlay class; empty disables. The owner that
# owns the aim state (LocalPlayerPresenter) sets this each frame from
# NovaSimulation.get_local_player_aim_overlay(); the pose write then routes through
# NovaSkeletalAnim.eval_pose_overlay so each skeleton segment gets its witnessed
# aim/body blend. [orig: Entity_BuildBoneTransformMatrices @0x4b1290;
# docs/world/world-wac-ai-re.md §14 (D-INF-11)]
var _aim_overlay_deltas: Array = []
var _aim_overlay_classes := PackedInt32Array()
# Retail clips the baked personal weapon for a non-player organic attached to a
# controller/gunner/driver mount slot by collapsing model bone 16 (BN17 R Hand)
# at its animated joint. Presentation supplies the authoritative derived verdict;
# this model carries it into the shared native evaluator. [orig: special row in
# Entity_BuildBoneTransformMatrices @0x4b1290; world-wac-ai-re.md section 14.1]
var _collapse_right_hand := false

# The upper-body weapon channel: a second clip posed at its OWN playhead onto the mask
# bones (clavicles/arms/forearms/neck/head/hands) before the aim overlay composes. The
# owner that owns the sim state (LocalPlayerPresenter) feeds it each frame from
# PlayerWeaponView.body_anim_key/body_anim_phase; empty key disables. Unknown keys
# no-op inside the native splice. [orig: the mask override @0x4b14db/@0x4b16a7 in
# Entity_BuildBoneTransformMatrices; docs/world/world-wac-ai-re.md §14.8]
var _wpn_key := ""
var _wpn_phase_ticks := 0


func set_weapon_channel(key: String, phase_ticks: int) -> void:
	_body_anim.set_weapon_channel(key, phase_ticks)


func set_aim_overlay(deltas: Array) -> void:
	_body_anim.set_aim_overlay(deltas)


func set_right_hand_collapsed(collapsed: bool) -> void:
	_body_anim.set_right_hand_collapsed(collapsed)


func advance_body_animation(delta: float, write_pose := true) -> void:
	_body_anim.advance_body_animation(delta, write_pose)


func rebuild() -> void:
	_scene_builder.rebuild()


# Blended materials take their water-side transparency rung from the witnessed
# frame ladder (maturity REN-3, docs/render/render-order-re.md): below-water
# alpha draws before the water surface, above-water after [orig: the Q1/Q2
# split @ 0x5d932e..0x5d9354 + the flush bracket @ 0x5c9596 / @ 0x5c967a].
# Retail bins per STRIP per frame; we bin per MODEL from its placed height
# (D-RORD-3). With no water in the session this is the default rung (0).
# Owners that move a model across the water plane re-call this.
func refresh_render_order() -> void:
	if _alpha_materials.is_empty() or not is_inside_tree():
		return
	var shader_cache := NovaObjectShaderCache.get_singleton()
	var rung := shader_cache.alpha_rung_for_height(global_position.y)
	for material in _alpha_materials:
		if material != null:
			material.render_priority = rung


func _on_object_changed() -> void:
	var update_mask := _last_object_update_mask()
	if update_mask == OED_UPDATE_PANM or update_mask == OED_UPDATE_LGHT or update_mask == (OED_UPDATE_PANM | OED_UPDATE_LGHT):
		# get_last_oed_update_mask() reports only the final mask of a deferred-flush window,
		# so a coalesced batch could read LGHT/PANM even when a material's generator style
		# also changed. Reclassify here (cheap, idempotent) so the dynamic-material set can
		# never go stale relative to the current IR -- otherwise a newly-animated material
		# would stay frozen on this no-rebuild fast path.
		_classify_materials()
		_refresh_live_panm_classification()
		_bounds_dirty = true
		_apply_runtime_state(0.0)
		return
	rebuild()


func _last_object_update_mask() -> int:
	if object_data != null:
		return int(object_data.get_last_oed_update_mask()) & OED_UPDATE_ALL
	return OED_UPDATE_ALL


func _process(delta: float) -> void:
	advance_runtime_frame(delta)


## Advance the model's render-time state once. This is the public equivalent
## of the engine process callback for deterministic owners and tests: clocks
## continue while hidden, while render-derived work waits until the model can
## be submitted again.
func advance_runtime_frame(delta: float) -> void:
	if object_data == null or not object_data.has_document():
		return
	# Retail evaluates material constants / PANM transforms / light state per
	# SUBMITTED model [orig: Terrain_RenderSectorModels @ 0x5c5d30 — the batch
	# computes constants for the models it draws]. is_visible_in_tree() is only
	# the retained owner's hierarchy-visibility gate: it skips explicitly hidden
	# props/buildings, but it does not prove camera/frustum submission. Exact
	# noise-call cadence therefore remains a renderer-scheduling gap (D-3DI-2),
	# not something this SceneTree callback can reconstruct. Time-ACCUMULATING
	# state (commanded part anims, an internally-timed skeletal clip, the private
	# preview clock) still advances inside _apply_runtime_state — a door
	# commanded open while hidden is open when next seen.
	var renderable := is_visible_in_tree()
	if not _needs_runtime_frame_work():
		# Keep the private preview clock continuous even while the model has no
		# time-driven consumer. A later OED edit can make a material/PANM track
		# live and must observe the same model age it did before this fast path.
		# Shared-clock mission models simply sample that clock when work resumes.
		if _panm_clock == null and _is_playing:
			_anim_time_ms = (_anim_time_ms + int(delta * 1000.0)) & 0xffffffff
		# Lighting/fog is the one retained-state input that can change without a
		# model mutator. Its generation gate makes this an integer comparison in
		# the steady state while avoiding all other per-model runtime work.
		if renderable:
			_apply_environment_to_materials()
		return
	_apply_runtime_state(delta, renderable)


func _needs_runtime_frame_work() -> bool:
	if (_bounds_dirty
			or _has_live_panm
			or not _dynamic_material_slots.is_empty()
			or (_model_light_preview_enabled and _has_lights)
			or not _part_anims.is_empty()):
		return true
	if _skeleton == null or _skeletal == null or _anim_key.is_empty():
		return false
	return (_body_pose_dirty
			or (_is_playing and _anim_playing and not _anim_external_phase)
			or _remote_pending_state >= 0)


func _refresh_live_panm_classification() -> void:
	if object_data == null:
		_has_live_panm = false
	else:
		_has_live_panm = bool(object_data.has_live_panm_for_lod(_active_lod))


func _clamp_lod_index(lod_index: int) -> int:
	if object_data == null or not object_data.has_document():
		return 0
	var summary: Dictionary = object_data.get_summary()
	var lod_count := int(summary.get("lod_count", 1))
	return clampi(lod_index, 0, maxi(lod_count - 1, 0))


# --- W4-6c: the material factory/classification + environment-lighting
# cluster moved verbatim to nova_object_materials.gd; delegates below.

func _build_material_defs() -> Dictionary:
	return _materials._build_material_defs()


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
	# Rebuilds honor the applied section mask (see set_section_visibility_mask).
	if _section_visibility_mask != -1:
		node.visible = (_section_visibility_mask >> robj_index) & 1 == 1
	add_child(node)
	_robj_nodes[robj_index] = node
	# The dense part-index -> node array apply_panm_to_nodes writes through
	# (nulls for parts without a node).
	while _robj_dense.size() <= robj_index:
		_robj_dense.append(null)
	_robj_dense[robj_index] = node
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


func _apply_runtime_state(delta: float, renderable := true) -> void:
	if object_data == null or not object_data.has_document():
		return
	if _panm_clock != null:
		_anim_time_ms = int(_panm_clock.get("time_ms")) & 0xffffffff
	elif _is_playing:
		_anim_time_ms = (_anim_time_ms + int(delta * 1000.0)) & 0xffffffff
	var part_changed := _advance_part_anims(delta)
	advance_body_animation(delta, renderable)
	if not renderable:
		# Everything below derives from the absolute clock + the register/pose
		# state advanced above; it re-derives on the next visible frame, with
		# the pending dirt (_bounds_dirty, _body_pose_dirty) staying latched.
		return
	# Retail poses PANM during entity submission before the later render-batch
	# flush evaluates material generators. Preserve that order because noise
	# waveforms share one random stream.
	# [orig: Render_SubmitEntity @0x5DAD80 -> Model_TransformBoneMatrices
	#  @0x58E390; CRenderBatchQueue_SortAndFlush @0x5DAE40 ->
	#  apply_shader_parameters @0x58DB80]
	var robj_changed := false
	if _has_live_panm or _bounds_dirty:
		robj_changed = _apply_robj_transforms()
	# Only materials whose UV/RGB/alpha generators animate (or whose texture flip-book
	# advances) need a per-frame push; a fully-static material already carries its identity
	# values from _create_material, so re-evaluating it each frame just re-writes identical
	# bytes. _dynamic_material_slots holds exactly the slots that can change (built in
	# _classify_materials); _material_needs_eval[i] distinguishes the eval path from the
	# texture-flip-book-only path.
	for i in _dynamic_material_slots:
		var material := _surface_materials[i]
		if material == null:
			continue
		var material_index := int(_surface_material_indices[i])
		if _material_needs_eval[i] and _od_has_eval:
			var runtime: Dictionary = object_data.eval_material_runtime(
					material_index, _anim_time_ms, _ctrl_values)
			if not runtime.is_empty():
				material.set_shader_parameter("u_uv_transform_u",
						runtime.get("uv_transform_u", Vector3(1.0, 0.0, 0.0)))
				material.set_shader_parameter("u_uv_transform_v",
						runtime.get("uv_transform_v", Vector3(0.0, 1.0, 0.0)))
				var rgb: Vector3 = runtime.get("rgb_mod", Vector3.ONE)
				material.set_shader_parameter("u_rgb_mod", rgb)
				material.set_shader_parameter("u_alpha_mod", runtime.get("alpha_mod", 1.0))
		var frames: Array = _anim_frames_by_mat.get(material_index, [])
		if frames.size() > 1 and _od_has_frame:
			var frame_index := int(object_data.compute_anim_frame(
					material_index, _anim_time_ms, _ctrl_values))
			if frame_index >= 0 and frame_index < frames.size() and frames[frame_index] is Texture2D:
				material.set_shader_parameter("u_diffuse", frames[frame_index])
	_apply_lights()
	_apply_environment_to_materials()
	if _bounds_dirty or part_changed or robj_changed:
		_set_model_bounds(_compute_transformed_mesh_bounds())
		_bounds_dirty = false


func _apply_robj_transforms() -> bool:
	if object_data == null or _robj_nodes.is_empty():
		return false
	# The shared-evaluation hot path: one native call evaluates PANM at most
	# once per graphic per frame (the placer shares one NovaObjectData across
	# every instance of a graphic, all riding one presentation clock) and
	# writes only the parts whose transforms changed since this model last
	# applied. The common empty-control path allocates nothing and no output
	# Dictionary is boxed.
	var revision := int(object_data.apply_panm_to_nodes(
			_active_lod, _anim_time_ms, _ctrl_values, _robj_dense,
			_panm_applied_revision))
	var changed := revision != _panm_applied_revision
	_panm_applied_revision = revision
	return changed


func _apply_lights() -> void:
	# Gameplay deliberately leaves these parsed records inactive: retail never
	# submits model-authored LGHT. The opt-in editor preview is useful for
	# authoring/inspection without changing shipped rendering.
	if not _model_light_preview_enabled or not _has_lights:
		return
	if object_data == null:
		return
	var lights: Array = object_data.evaluate_lights(
			_anim_time_ms, _ctrl_values)
	var dominant := {}
	var best_intensity := -1.0
	for light in lights:
		var info: Dictionary = light
		var intensity := float(info.get("intensity", 1.0))
		if intensity > best_intensity:
			best_intensity = intensity
			dominant = info
	var count := 0 if dominant.is_empty() else 1
	var model_position: Vector3 = dominant.get("position", Vector3.ZERO)
	var position := global_transform * model_position
	var color: Color = dominant.get("color", Color.WHITE)
	var atten_start := float(dominant.get("atten_start", 0.0))
	var atten_end := float(dominant.get("atten_end", 5.0))
	var subobject := int(dominant.get("subobject", -1))
	if _skeleton != null and subobject >= 0 \
			and subobject < _skeleton.get_bone_count():
		# LGHT positions are authored in model space. Move through the same
		# rest-to-live bone transform as user points and muzzle attachments.
		position = (_skeleton.global_transform
				* _skeleton.get_bone_global_pose(subobject)
				* _skeleton.get_bone_global_rest(subobject).affine_inverse()) \
				* model_position
	elif subobject >= 0 and _robj_nodes.has(subobject) \
			and _robj_rest_transforms.has(subobject):
		var node := _robj_nodes[subobject] as Node3D
		var rest: Transform3D = _robj_rest_transforms[subobject]
		position = node.global_transform * (rest.affine_inverse() * model_position)
	# A static light evaluates to the same values every frame; re-pushing them
	# re-writes identical uniforms across every material. Push only on change
	# (retained mode — the skip is invisible); an animated/PANM-carried light
	# changes the compare key and pushes normally.
	var intensity := best_intensity if best_intensity > 0.0 else 1.0
	if _last_light_push_valid \
			and count == _last_light_count \
			and position == _last_light_position \
			and color == _last_light_color \
			and intensity == _last_light_intensity \
			and atten_start == _last_light_atten_start \
			and atten_end == _last_light_atten_end:
		return
	_last_light_push_valid = true
	_last_light_count = count
	_last_light_position = position
	_last_light_color = color
	_last_light_intensity = intensity
	_last_light_atten_start = atten_start
	_last_light_atten_end = atten_end
	for material in _surface_materials:
		if material == null:
			continue
		material.set_shader_parameter("u_local_light_count", count)
		material.set_shader_parameter("u_local_light_position", position)
		material.set_shader_parameter("u_local_light_color", Vector3(color.r, color.g, color.b))
		material.set_shader_parameter("u_local_light_intensity", intensity)
		material.set_shader_parameter("u_local_light_atten_start", atten_start)
		material.set_shader_parameter("u_local_light_atten_end", atten_end)


func _material_for_index(material_array_index: int,
		lighting_context: int = LIGHTING_CONTEXT_ENTITY) -> ShaderMaterial:
	var cache_key := Vector2i(material_array_index, lighting_context)
	if _material_cache.has(cache_key):
		return _material_cache[cache_key]
	var material_def: Dictionary = _material_defs.get(material_array_index, {})
	var material := _create_material(material_array_index, material_def)
	_material_cache[cache_key] = material
	# A newly created context-specific material still carries only the defaults
	# from _create_material; force the next environment push to stamp it.
	_last_env_gen = -1
	_last_env_values = null
	_last_section_env_values = null
	return material


static func material_supports_projected_shadow_receiver(
		blend_mode: int, material_flags: int) -> bool:
	return NovaObjectMaterials.material_supports_projected_shadow_receiver(blend_mode, material_flags)


func _create_material(index: int, material_def: Dictionary) -> ShaderMaterial:
	return _materials._create_material(index, material_def)


func _get_shadow_receiver_material() -> ShaderMaterial:
	return _materials._get_shadow_receiver_material()


func _load_texture_for_slot(material_def: Dictionary, slot: int) -> Texture2D:
	return _materials._load_texture_for_slot(material_def, slot)


func _collect_anim_frames(material_index: int) -> void:
	_materials._collect_anim_frames(material_index)


func _load_texture_name(texture_name: String) -> Texture2D:
	return _materials._load_texture_name(texture_name)


func _hash_color_for_index(idx: int) -> Color:
	return _materials._hash_color_for_index(idx)


func _solid_colour_texture(color: Color) -> ImageTexture:
	return _materials._solid_colour_texture(color)


func _apply_default_environment_to_material(material: ShaderMaterial) -> void:
	_materials._apply_default_environment_to_material(material)


func _material_runtime_is_dynamic(material_index: int) -> bool:
	return _materials._material_runtime_is_dynamic(material_index)


func _classify_materials() -> void:
	_materials._classify_materials()


static func environment_values_from(env_node: Node) -> EnvLightValues:
	return NovaObjectMaterials.environment_values_from(env_node)


static func entity_lighting_values(world_values: EnvLightValues, effect_scale: float,
		interior_lerp: bool, interior_daylight: float) -> EnvLightValues:
	return NovaObjectMaterials.entity_lighting_values(world_values, effect_scale, interior_lerp, interior_daylight)


static func apply_environment_values(material: ShaderMaterial, values: EnvLightValues) -> void:
	NovaObjectMaterials.apply_environment_values(material, values)


func _apply_environment_to_materials() -> void:
	_materials._apply_environment_to_materials()


func _environment_values() -> EnvLightValues:
	return _materials._environment_values()


func _set_model_bounds(bounds: AABB) -> void:
	if _aabb_equal_approx(_model_bounds, bounds):
		return
	_model_bounds = bounds
	bounds_changed.emit(_model_bounds)


func _aabb_equal_approx(a: AABB, b: AABB) -> bool:
	return a.position.is_equal_approx(b.position) and a.size.is_equal_approx(b.size)
