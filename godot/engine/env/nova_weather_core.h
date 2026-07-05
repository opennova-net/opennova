#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/color.hpp>

#include <env/env_render.h>

namespace godot {

// The per-tick weather state cluster of [orig: Environment_UpdateWeatherTick
// @ 0x57e9b0]: the wind-sway oscillator, both lightning flash sequencers,
// rain fade, and the four color blocks the host smooths (ground/fill,
// directional light, fog, sky ambient). All math lives in libs/env
// (env/env_render.h); this binding owns state and the witnessed
// order-of-operations only. NovaWeather (the node) drives one tick() per
// 62 Hz frame and reads the results back. RE record: docs/env/env-tod-re.md.
class NovaWeatherCore : public RefCounted {
	GDCLASS(NovaWeatherCore, RefCounted)

private:
	opennova::env::WeatherOscillator oscillator;
	opennova::env::LightningSequencers lightning;
	opennova::env::RainState rain;
	opennova::env::WeatherColorBlock fill_block; // ground light
	opennova::env::WeatherColorBlock sun_block;  // directional light (never flashed)
	opennova::env::WeatherColorBlock fog_block;
	opennova::env::WeatherColorBlock sky_block;
	// OpenNova authoring extension: an ARMED wind timer whose expiry decays
	// the oscillator intensity (x31 >> 5 per tick) back to zero. Unarmed
	// wind never decays — retail's Env_WindScale is a constant
	// (Environment_InitDefaults @ 0x57c1d1 is its only writer), so a set
	// strength holds until the author changes it. Marked as an extension in
	// docs/env/env-tod-re.md.
	int wind_duration_ticks = 0;
	bool wind_armed = false;

protected:
	static void _bind_methods();

public:
	// Snaps all four block states (and their packed colors) to the given
	// colors — the discrete-scrub resync path.
	void snap_colors(const Color &p_fill, const Color &p_sun, const Color &p_fog, const Color &p_sky);

	// One 62 Hz weather tick in the witnessed order: oscillator, wind-decay
	// extension, rain fade, lightning sequencers (additive slot rewrites on
	// epoch), then the four block pipelines.
	void tick(const Color &p_fill_target, const Color &p_sun_target,
			const Color &p_fog_target, const Color &p_sky_target,
			const Color &p_lightning_color);

	// Raw Env_WindScale units. The witnessed retail value is 256; the
	// oscillator's 15*prev feedback is stable only for intensity <= 273.
	void set_wind_intensity(int p_value);
	int get_wind_intensity() const;
	void set_wind_duration_ticks(int p_ticks);
	int get_wind_duration_ticks() const;

	void trigger_lightning_short();
	void trigger_lightning_long();

	Color get_fill() const;
	Color get_sun() const;
	Color get_fog() const;
	Color get_sky() const;

	// (smoothed - 0x8000) / 0x8000 * 2 — the shader-facing sway scalar.
	float get_sway_amount() const;
	// ring_index * (TAU / 256) — the shader-facing sway phase.
	float get_sway_phase() const;
	// Last SET flash level / 255.
	float get_lightning_intensity() const;
};

} // namespace godot
