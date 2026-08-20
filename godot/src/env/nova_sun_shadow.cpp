#include "env/nova_sun_shadow.h"

#include <godot_cpp/classes/rendering_server.hpp>

#include <algorithm>

#include "env/nova_mission_environment.h"
#include "env/nova_water.h"

namespace godot {

void SunShadow::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_projection_mode", "mode"),
			&SunShadow::set_projection_mode);
	ClassDB::bind_method(D_METHOD("get_projection_mode"),
			&SunShadow::get_projection_mode);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "projection_mode",
						 PROPERTY_HINT_ENUM, "Dynamic,Static Terrain"),
			"set_projection_mode", "get_projection_mode");
	ClassDB::bind_method(D_METHOD("set_environment_node", "environment"),
			&SunShadow::set_environment_node);
	ClassDB::bind_method(D_METHOD("advance_frame", "delta"),
			&SunShadow::advance_frame);

	ClassDB::bind_integer_constant(get_class_static(), "",
			"PROJECTION_DYNAMIC", PROJECTION_DYNAMIC);
	ClassDB::bind_integer_constant(get_class_static(), "",
			"PROJECTION_STATIC_TERRAIN", PROJECTION_STATIC_TERRAIN);
}

void SunShadow::set_projection_mode(int p_mode) {
	projection_mode_ = p_mode;
	_apply_projection_masks();
}

void SunShadow::set_environment_node(MissionEnvironment *p_environment) {
	environment_node_id_ = p_environment != nullptr
			? ObjectID(p_environment->get_instance_id())
			: ObjectID();
	last_emission_direction_ = Vector3(INFINITY, INFINITY, INFINITY);
	_update_direction();
}

void SunShadow::_ready() {
	// One-time Godot shadow-map quality tuning (device knobs, not witnessed
	// retail constants).
	set_shadow(true);
	set_param(Light3D::PARAM_SHADOW_MAX_DISTANCE, 192.0f);
	set_shadow_mode(DirectionalLight3D::SHADOW_PARALLEL_4_SPLITS);
	set_param(Light3D::PARAM_SHADOW_FADE_START, 0.95f);
	set_blend_splits(true);
	set_param(Light3D::PARAM_SHADOW_BIAS, 0.02f);
	set_param(Light3D::PARAM_SHADOW_NORMAL_BIAS, 0.2f);
	set_param(Light3D::PARAM_SIZE, 0.0f);
	set_param(Light3D::PARAM_ENERGY, 1.0f);
	set_param(Light3D::PARAM_SPECULAR, 0.0f);
	set_param(Light3D::PARAM_INDIRECT_ENERGY, 0.0f);
	set_param(Light3D::PARAM_VOLUMETRIC_FOG_ENERGY, 0.0f);
	_apply_projection_masks();
	set_process(true);
	_update_direction();
}

// The retail two-list split as Godot mask pairs: receivers via
// light_cull_mask, casters via shadow_caster_mask, both over the Water
// visual-layer allocation. The mask VALUES are device plumbing; the pairing
// policy they encode is the witnessed list taxonomy
// (docs/render/render-lighting-re.md).
void SunShadow::_apply_projection_masks() {
	if (projection_mode_ == PROJECTION_STATIC_TERRAIN) {
		set_cull_mask(Water::VISUAL_LAYER_TERRAIN_SHADOW_RECEIVER);
		set_shadow_caster_mask(Water::VISUAL_LAYER_STATIC_SHADOW_CASTER);
		set_shadow(true);
	} else {
		// Receivers: both world-entity layers plus the hidden FP body (its
		// silhouette must land on the world the player sees).
		set_cull_mask(Water::VISUAL_LAYER_WORLD |
				Water::VISUAL_LAYER_WORLD_NO_MIRROR |
				Water::VISUAL_LAYER_FP_BODY_SHADOW_ONLY);
		set_shadow_caster_mask(Water::VISUAL_LAYER_DYNAMIC_SHADOW_CASTER);
		// The SlotShadow capture pipeline renders the live entity ground
		// shadows (retail's per-slot RT + terrain drape, see
		// docs/render/render-lighting-re.md); the dynamic light keeps the
		// direction law but no longer needs its own shadow map.
		set_shadow(false);
	}
}

void SunShadow::_process(double p_delta) {
	advance_frame(p_delta);
}

void SunShadow::advance_frame(double p_delta) {
	_update_direction();
	(void)p_delta;
}

void SunShadow::_update_direction() {
	MissionEnvironment *env = Object::cast_to<MissionEnvironment>(
			ObjectDB::get_instance(environment_node_id_));
	if (env == nullptr || !env->is_loaded()) {
		set_visible(false);
		return;
	}
	const Vector3 light_tuple = env->get_light_direction();
	if (light_tuple.length_squared() <= 1.0e-6) {
		set_visible(false);
		return;
	}
	set_visible(true);
	// The environment serves the DIRECT retail getter tuple g = (-Y_bms,
	// Z_bms, X_bms); presentation-world surface->light is the (g2, g1, g0)
	// reduction (retail: Environment_GetLightDirectionFloat @0x57d870;
	// Math_BuildFixedPointToFloatMatrix4x4 @0x612402..0x612457). Retail's
	// entity shadow projection additionally clamps the vertical component to
	// 0.25 before negating into the slot's light->surface direction, so a
	// grazing sun never stretches an entity silhouette past 4x height
	// (retail: render_shadow_pass @0x5d7b70 — GetLightDirectionFloat into
	// RenderSlot_DefaultLightDir*, `if (y < 0.25) y = 0.25`, then negate all
	// three; the law lives portable in renderer::slot_projection_direction —
	// see docs/render/render-lighting-re.md). A DirectionalLight3D emits
	// along local -Z, so emission is that negated form. The SlotShadow
	// capture pipeline owns the entity ground shadows; this light remains
	// the direction-law reference and the static-terrain bake device.
	const Vector3 surface_to_light(light_tuple.z,
			std::max(light_tuple.y, real_t(0.25)), light_tuple.x);
	const Vector3 emission = -surface_to_light.normalized();
	if (emission.is_equal_approx(last_emission_direction_)) {
		return;
	}
	last_emission_direction_ = emission;
	const Vector3 up = Math::abs(emission.dot(Vector3(0, 1, 0))) > 0.99
			? Vector3(0, 0, 1)
			: Vector3(0, 1, 0);
	set_basis(Basis::looking_at(emission, up));
}

} // namespace godot
