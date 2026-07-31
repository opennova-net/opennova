class_name NovaSunShadow
extends DirectionalLight3D

# Shadow-only shell adapter. The fixed-function terrain/object shaders keep
# owning all color lighting; these lights are visible only to their black
# ATTENUATION catcher pass. Retail has two independent projection lists:
# pose-derived live entity silhouettes can reach world models, while pool-2 /
# StaticShadow silhouettes are composed into terrain tiles and foliage only.
# The reimpl static adapter exposes terrain only; alpha-tested foliage waits for
# an alpha-aware tile-cache compositor.

enum {
	PROJECTION_DYNAMIC,
	PROJECTION_STATIC_TERRAIN,
}

var projection_mode := PROJECTION_DYNAMIC
var _environment_node: Node
var _last_emission_direction := Vector3.INF


func _ready() -> void:
	_apply_projection_masks()
	shadow_enabled = true
	directional_shadow_mode = DirectionalLight3D.SHADOW_PARALLEL_4_SPLITS
	directional_shadow_max_distance = 192.0
	directional_shadow_fade_start = 0.95
	directional_shadow_blend_splits = true
	shadow_bias = 0.02
	shadow_normal_bias = 0.2
	light_angular_distance = 0.0
	light_energy = 1.0
	light_specular = 0.0
	light_indirect_energy = 0.0
	light_volumetric_fog_energy = 0.0
	set_process(true)
	_update_direction()


func _apply_projection_masks() -> void:
	if projection_mode == PROJECTION_STATIC_TERRAIN:
		light_cull_mask = NovaWater.VISUAL_LAYER_TERRAIN_SHADOW_RECEIVER
		shadow_caster_mask = NovaWater.VISUAL_LAYER_STATIC_SHADOW_CASTER
		return
	light_cull_mask = \
			NovaWater.VISUAL_LAYER_WORLD \
			| NovaWater.VISUAL_LAYER_BODY_REFLECTION_ONLY
	shadow_caster_mask = NovaWater.VISUAL_LAYER_DYNAMIC_SHADOW_CASTER


func set_environment_node(value: Node) -> void:
	_environment_node = value
	_last_emission_direction = Vector3.INF
	_update_direction()


func _process(_delta: float) -> void:
	_update_direction()


func _update_direction() -> void:
	if _environment_node == null \
			or not bool(_environment_node.is_loaded()):
		visible = false
		return
	var light_direction: Vector3 = _environment_node.get_light_direction()
	if light_direction.length_squared() <= 0.000001:
		visible = false
		return
	visible = true
	# Environment_GetLightDirectionFloat is surface -> light. Godot emits a
	# DirectionalLight3D along local -Z, so its ray direction is the negative.
	var emission_direction := -light_direction.normalized()
	if emission_direction.is_equal_approx(_last_emission_direction):
		return
	var up := Vector3.UP
	if absf(emission_direction.dot(up)) > 0.99:
		up = Vector3.FORWARD
	basis = Basis.looking_at(emission_direction, up)
	_last_emission_direction = emission_direction
