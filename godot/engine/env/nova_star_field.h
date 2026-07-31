#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <env/env_celestial.h>

namespace godot {

// The 256-instance star field (env #33): the witnessed instance table +
// per-frame twinkle, generated and ticked in libs/env
// [orig: Star_GenerateInstanceTable @ 0x5ac850; render_star_field @ 0x5ad9c0].
// The binding serves GODOT-space data (the engine->render basis
// godot = (-engY, engZ, engX) / 65536, the Math_FixedPointToFloat3_YNegated
// convention); NovaCelestial owns the billboards. Like the water noise
// field, the retail table content depends on the shared PRNG's call history
// at load — the reimpl generates from a documented seed for a deterministic
// witnessed-faithful instance.
class NovaStarField : public RefCounted {
	GDCLASS(NovaStarField, RefCounted)

private:
	opennova::env::StarInstance stars[opennova::env::kStarInstanceCount];
	uint32_t prng_state = 1u;
	bool generated = false;

protected:
	static void _bind_methods();

public:
	// (Re)generate the 256-entry table [orig: sole caller
	// EffectWorld_LoadCelestialModels @ 0x5add40 — per celestial load].
	void regenerate(int p_seed);
	int get_count() const;

	// One render tick: twinkles the VISIBLE stars (the original updates the
	// accumulator only inside the visibility branch) and returns stride-6
	// floats per star: [offset_x, offset_y, offset_z (godot, world units),
	// scale (world units), brightness (0..1), visible (0/1)].
	// p_light_dir_godot = the direct near-unit active light direction.
	PackedFloat32Array tick_frame(const Vector3 &p_light_dir_godot);
};

} // namespace godot
