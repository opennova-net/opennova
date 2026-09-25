#include "env/sun_shadow.h"

#include <godot_cpp/classes/rendering_server.hpp>

#include <array>

#include <runtime/renderer/render_slot_shadow.h>

#include "env/mission_environment.h"
#include "env/water.h"

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
	// The light's OWN visual layer decides which views render it: the beauty
	// camera (0x78C01) and the water mirror (0x8001). Focused Q3 attaches
	// resolved beauty depth and renders only typed self-lit draws, so it never
	// submits this Light3D. TERRAIN_SHADOW_RECEIVER (bit 15) is in exactly the
	// beauty and mirror camera masks, so the light lives there.
	set_layer_mask(Water::VISUAL_LAYER_TERRAIN_SHADOW_RECEIVER);
	set_param(Light3D::PARAM_SIZE, 0.0f);
	set_param(Light3D::PARAM_ENERGY, 1.0f);
	set_param(Light3D::PARAM_SPECULAR, 0.0f);
	set_param(Light3D::PARAM_INDIRECT_ENERGY, 0.0f);
	set_param(Light3D::PARAM_VOLUMETRIC_FOG_ENERGY, 0.0f);
	// The dynamic receiver/caster mask pair over the Water visual-layer
	// allocation (the mask VALUES are device plumbing; the pairing is the
	// witnessed list taxonomy, docs/render/render-lighting-re.md): receivers
	// on both world-entity layers plus the hidden FP body, casters on the
	// dynamic layer. No shadow map: the SlotShadow capture pipeline renders
	// the live entity ground shadows (retail's per-slot RT + terrain drape)
	// and the tile composer the static ones.
	set_cull_mask(Water::VISUAL_LAYER_WORLD |
			Water::VISUAL_LAYER_WORLD_NO_MIRROR |
			Water::VISUAL_LAYER_FP_BODY_SHADOW_ONLY);
	set_shadow_caster_mask(Water::VISUAL_LAYER_DYNAMIC_SHADOW_CASTER);
	set_shadow(false);
	_update_direction();
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
	// get_light_direction serves the Godot-axes vector: the util/axes.h x/z
	// swap of the raw getter tuple g = (-Y_bms, Z_bms, X_bms) IS the
	// (g2, g1, g0) surface->light reduction, applied once at the getter
	// [orig: Environment_GetLightDirectionFloat @0x57d870;
	// Math_BuildFixedPointToFloatMatrix4x4 @0x612402..0x612457]. Retail's
	// entity shadow projection additionally clamps the vertical component to
	// 0.25 before negating into the slot's light->surface direction, so a
	// grazing sun never stretches an entity silhouette past 4x height
	// [orig: render_shadow_pass @0x5d7b70 — GetLightDirectionFloat into
	// RenderSlot_DefaultLightDir*, `if (y < 0.25) y = 0.25`, then negate all
	// three; opennova::renderer::slot_projection_direction is the one owner of that
	// law — see docs/render/render-lighting-re.md]. A DirectionalLight3D
	// emits along local -Z, so emission is that negated form. The SlotShadow
	// capture pipeline owns the entity ground shadows and the tile composer
	// the static ones; this light remains the direction-law reference.
	const std::array<float, 3> slot_dir = opennova::renderer::slot_projection_direction(
			{ float(light_tuple.x), float(light_tuple.y), float(light_tuple.z) });
	const Vector3 emission =
			Vector3(slot_dir[0], slot_dir[1], slot_dir[2]).normalized();
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
