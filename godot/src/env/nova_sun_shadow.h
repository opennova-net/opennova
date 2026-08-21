#pragma once

#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/core/class_db.hpp>

namespace godot {

class MissionEnvironment;

// Native directional-light device leg. It reads the active ENV sun/moon
// direction and color, supplies direct lighting to the facelift materials,
// and casts cascaded Godot shadow maps. The existing static and dynamic caster
// layers both feed that one native sun; ordinary world and viewmodel layers
// receive it. Each frame it reads the environment's
// surface-to-light direction (the engine light chain already computes the
// negated emission form: renderer::WorldLightingBlock.dir), orients via a
// look-at basis with a degenerate-up fallback, and hides itself when the
// environment is unloaded or the direction is near zero. Ported from
// nova_sun_shadow.gd (2026-08-10 de-scripting).
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
	void _apply_native_masks();
	void _update_direction();

	ObjectID environment_node_id_;
	Vector3 last_emission_direction_ = Vector3(INFINITY, INFINITY, INFINITY);
};

} // namespace godot
