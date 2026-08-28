#pragma once

#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/core/class_db.hpp>

namespace godot {

class MissionEnvironment;

// The sun shadow-direction device leg: a DirectionalLight3D that contributes
// no color (the fixed-function terrain/object shaders own all color) and
// models retail's two independent shadow projection lists
// (docs/render/render-lighting-re.md: the static collector admits pool-2
// unless NoShadow and pool-1 only with attrib2 StaticShadow; dynamic slots
// ride entity init) via a projection_mode enum. PROJECTION_STATIC_TERRAIN
// casts the Godot shadow map the static-terrain bake reads (static casters
// onto terrain receivers); PROJECTION_DYNAMIC keeps the shadow map OFF — the
// SlotShadow capture pipeline renders the live entity ground shadows — and
// stays the direction-law reference (opennova::renderer::slot_projection_direction:
// the 0.25 vertical clamp, then negate). Both pair light_cull_mask
// (receivers) with shadow_caster_mask (casters) over the Water visual-layer
// bits. Each frame it reads the environment's surface-to-light direction,
// orients via a look-at basis with a degenerate-up fallback, and hides
// itself when the environment is unloaded or the direction is near zero.
class SunShadow : public DirectionalLight3D {
	GDCLASS(SunShadow, DirectionalLight3D)

public:
	enum ProjectionMode {
		PROJECTION_DYNAMIC = 0,
		PROJECTION_STATIC_TERRAIN = 1,
	};

	void set_projection_mode(int p_mode);
	int get_projection_mode() const { return projection_mode_; }
	void set_environment_node(MissionEnvironment *p_environment);

	// One render-frame advance (the _process body) — the externally-callable
	// drive the test harness uses; the engine's virtual delegates here.
	void advance_frame(double p_delta);

	void _ready() override;
	void _process(double p_delta) override;

protected:
	static void _bind_methods();

private:
	void _apply_projection_masks();
	void _update_direction();

	int projection_mode_ = PROJECTION_DYNAMIC;
	ObjectID environment_node_id_;
	Vector3 last_emission_direction_ = Vector3(INFINITY, INFINITY, INFINITY);
};

} // namespace godot
