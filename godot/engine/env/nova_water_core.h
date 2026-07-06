#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include <env/env_render.h>

namespace godot {

// The per-frame water noise texture pair of [orig: render_water_surface
// @ 0x5c32c0 -> Water_GenerateNoiseTextures @ 0x5c0360]: the animated
// 128x128 ridge color/alpha texture and its DuDv/normal derivative. All math
// lives in libs/env (env/env_render.h); this binding owns the static tables
// (built once with the witnessed init, from the boot PRNG state) and the
// frame buffers. NovaWater updates once per frame and blits the results into
// ImageTextures. RE record: docs/env/env-tod-re.md "Water surface".
class NovaWaterCore : public RefCounted {
	GDCLASS(NovaWaterCore, RefCounted)

private:
	opennova::env::WaterNoiseTables tables = opennova::env::water_init_noise_tables();
	uint32_t color_pixels[opennova::env::kWaterNoiseSize * opennova::env::kWaterNoiseSize] = {};
	uint32_t normal_pixels[opennova::env::kWaterNoiseSize * opennova::env::kWaterNoiseSize] = {};

protected:
	static void _bind_methods();

public:
	// Regenerates both textures for the given 62 Hz frame counter
	// [orig: called per frame from render_water_surface @ 0x5c3326].
	void update(int p_frame_counter);

	// RGBA8 bytes (128x128) for Image::create_from_data - the ridge
	// color/alpha texture and the DuDv/normal map from the last update().
	PackedByteArray get_color_rgba8() const;
	PackedByteArray get_normal_rgba8() const;

	int get_texture_size() const;
};

} // namespace godot
