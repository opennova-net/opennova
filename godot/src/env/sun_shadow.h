#pragma once

#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/core/class_db.hpp>

namespace godot {

class MissionEnvironment;

// The scene sun (ADR 0043): a real DirectionalLight3D carrying the env light
// block's active color (sun by day, moon by night, post-modulator,
// NVG-rewritten) at MODULATE2X energy, casting the one cascaded shadow map
// every lit receiver takes — entities, vehicles, foliage, terrain and static
// models alike. The direction keeps the witnessed shadow-projection law
// (renderer::sun_shadow_direction — the 0.25 vertical clamp, then negate), so
// a grazing sun never stretches a shadow past 4x height. Each frame it reads
// the environment's surface-to-light direction, orients via a look-at basis
// with a degenerate-up fallback, and hides itself when the environment is
// unloaded or the direction is near zero.
class SunShadow : public DirectionalLight3D {
	GDCLASS(SunShadow, DirectionalLight3D)

public:
	void set_environment_node(MissionEnvironment *p_environment);

	// One render-frame advance (the _process body) — the externally-callable
	// drive the test harness uses; the engine's virtual delegates here.
	void advance_frame(double p_delta);

	void _ready() override;
	void _process(double p_delta) override;

protected:
	static void _bind_methods();

private:
	void _update_direction();
	void _update_light_color();

	ObjectID environment_node_id_;
	Vector3 last_emission_direction_ = Vector3(INFINITY, INFINITY, INFINITY);
};

} // namespace godot
