#include "env/sun_shadow.h"

#include <godot_cpp/classes/rendering_server.hpp>

#include <array>

#include <runtime/renderer/light_runtime.h>

#include "env/mission_environment.h"
#include "env/water.h"
#include "object/object_model.h"

namespace godot {

void SunShadow::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_environment_node", "environment"),
			&SunShadow::set_environment_node);
	ClassDB::bind_method(D_METHOD("advance_frame", "delta"),
			&SunShadow::advance_frame);
}

void SunShadow::set_environment_node(MissionEnvironment *p_environment) {
	environment_node_id_ = p_environment != nullptr
			? ObjectID(p_environment->get_instance_id())
			: ObjectID();
	last_emission_direction_ = Vector3(INFINITY, INFINITY, INFINITY);
	_update_direction();
}

void SunShadow::_ready() {
	// The light's OWN visual layer decides which views render it - and
	// therefore which views re-render the directional shadow
	// atlas. The beauty camera (0x18C01) and the water mirror (0x8001) need
	// it. Focused Q3 attaches resolved beauty depth and renders only typed
	// self-lit draws, so it never submits this Light3D or pays another shadow
	// atlas render. TERRAIN_SHADOW_RECEIVER (bit 15) is in exactly the beauty
	// and mirror camera masks, so the light lives there.
	set_layer_mask(Water::VISUAL_LAYER_TERRAIN_SHADOW_RECEIVER);
	// ADR 0043: one cascaded shadow map for the whole scene. Distance/splits
	// are device tuning (settings knobs later); everything with a cast-shadow
	// setting participates, so shadow_caster_mask stays wide open and the
	// per-instance cast settings (shadow twins, foliage tiers, FP body) gate
	// casting.
	set_shadow(true);
	set_param(Light3D::PARAM_SHADOW_MAX_DISTANCE, 400.0f);
	set_shadow_mode(DirectionalLight3D::SHADOW_PARALLEL_4_SPLITS);
	set_param(Light3D::PARAM_SHADOW_FADE_START, 0.9f);
	set_blend_splits(true);
	set_param(Light3D::PARAM_SHADOW_BIAS, 0.03f);
	set_param(Light3D::PARAM_SHADOW_NORMAL_BIAS, 1.5f);
	set_param(Light3D::PARAM_SIZE, 0.0f);
	// The sun is a REAL light: the env light block's dir_color at the
	// fixed-function MODULATE2X fold carried as energy, so the lit pipeline
	// reproduces the gamma-domain x2 the retired shader fold applied.
	set_param(Light3D::PARAM_ENERGY, 2.0f);
	set_param(Light3D::PARAM_SPECULAR, 0.0f);
	set_param(Light3D::PARAM_INDIRECT_ENERGY, 0.0f);
	set_param(Light3D::PARAM_VOLUMETRIC_FOG_ENERGY, 0.0f);
	set_cull_mask(0xFFFFFFFFu);
	set_shadow_caster_mask(0xFFFFFFFFu);
	set_process(true);
	_update_direction();
}

void SunShadow::_process(double p_delta) {
	advance_frame(p_delta);
}

void SunShadow::advance_frame(double p_delta) {
	_update_direction();
	_update_light_color();
	(void)p_delta;
}

// ADR 0043: the env light block's active dir color (sun by day, moon by
// night, post-modulator, NVG-rewritten — the same value the retired
// fixed-function block carried in slot 227) becomes the light's color.
void SunShadow::_update_light_color() {
	MissionEnvironment *env = Object::cast_to<MissionEnvironment>(
			ObjectDB::get_instance(environment_node_id_));
	if (env == nullptr) {
		return;
	}
	const Ref<EnvLightState> light_state = env->get_light_state();
	if (light_state.is_null() || light_state->get_values().is_null()) {
		return;
	}
	const Vector3 c = light_state->get_values()->get_dir_color();
	set_color(Color(c.x, c.y, c.z));
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
	// get_light_direction already serves the Godot-axes surface->light vector
	// (the env_axes x/z swap of the raw getter tuple IS the (g2, g1, g0)
	// reduction) [orig: Environment_GetLightDirectionFloat @0x57d870]. The
	// shadow projection clamps the vertical component to 0.25 before negating
	// into the light->surface direction, so a grazing sun never stretches a
	// shadow past 4x height — renderer::sun_shadow_direction owns that law.
	// A DirectionalLight3D emits along local -Z, so emission is the negated
	// form.
	const std::array<float, 3> shadow_dir = opennova::renderer::sun_shadow_direction(
			{ float(light_tuple.x), float(light_tuple.y), float(light_tuple.z) });
	const Vector3 emission =
			Vector3(shadow_dir[0], shadow_dir[1], shadow_dir[2]).normalized();
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
