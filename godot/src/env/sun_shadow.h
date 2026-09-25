#pragma once

#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/core/class_db.hpp>

namespace godot {

class MissionEnvironment;

// The sun shadow-direction device leg: a DirectionalLight3D that contributes
// no color (the fixed-function terrain/object shaders own all color) and
// casts no shadow map — the SlotShadow capture pipeline renders the live
// entity ground shadows and the terrain tile composer rasterizes the static
// ones into page alpha. It stays the direction-law reference
// (opennova::renderer::slot_projection_direction: the 0.25 vertical clamp,
// then negate) with the dynamic receiver/caster masks over the Water
// visual-layer bits. Each frame it reads the environment's surface-to-light
// direction, orients via a look-at basis with a degenerate-up fallback, and
// hides itself when the environment is unloaded or the direction is near
// zero.
class SunShadow : public DirectionalLight3D {
	GDCLASS(SunShadow, DirectionalLight3D)

public:
	void set_environment_node(MissionEnvironment *p_environment);

	// One render-frame advance — the externally-callable
	// drive the test harness uses; the engine's virtual delegates here.
	void advance_frame(double p_delta);

	void _ready() override;

protected:
	static void _bind_methods();

private:
	void _update_direction();

	ObjectID environment_node_id_;
	Vector3 last_emission_direction_ = Vector3(INFINITY, INFINITY, INFINITY);
};

} // namespace godot
