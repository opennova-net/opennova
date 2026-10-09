#include <runtime/environment/environment_state.h>
#include <base/vfs/file_source.h>
#include <base/io/rotating_prng.h>
#include <base/io/fixed.h>

#include <formats/env/tod_clock.h>
#include <formats/mission/bms.h>
#include <runtime/environment/weather_seed.h>

#include <formats/env/env_weather.h>

#include <algorithm>
#include <cmath>

namespace opennova::env {

namespace {

// Godot fposmod semantics: the result carries the divisor's sign.
double fposmod(double x, double y) {
	double value = std::fmod(x, y);
	if ((value < 0.0f && y > 0.0f) || (value > 0.0f && y < 0.0f)) {
		value += y;
	}
	return value;
}

bool rgb_equal(const Rgb &a, const Rgb &b) {
	return a.r == b.r && a.g == b.g && a.b == b.b;
}

Rgb rgb_scale(const Rgb &value, float factor) {
	return Rgb{value.r * factor, value.g * factor, value.b * factor};
}

float vec3_length(const Vec3 &v) {
	return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

// The thermal view's flat colours: the world block's 0.5 hemispheres
// (flt_7C3B94) and the unk_808080 device fog / frame clear, both in the
// render space the modulate2x path consumes verbatim; the terrain ramp pair
// 0x101010 (c1 light) / 0xF0F0F0 (c0 sky).
// [orig: CTerrainRenderer_BuildLightingShaderConstants @ 0x5c8396 (0.5);
//  Environment_ApplyFogAndAmbient @ 0x57e499 and Render_ProcessMainSceneFrame
//  @ 0x5ca778 (unk_808080); Render_TerrainScene @ 0x610e65]
constexpr Rgb kThermalHemi{0.5f, 0.5f, 0.5f};
constexpr Rgb kThermalGrey{128.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f};
constexpr Rgb kThermalTerrainLight{16.0f / 255.0f, 16.0f / 255.0f, 16.0f / 255.0f};
constexpr Rgb kThermalTerrainSky{240.0f / 255.0f, 240.0f / 255.0f, 240.0f / 255.0f};

} // namespace

void EnvironmentState::set_config(const Config *config, bool loaded) {
	config_ = config;
	loaded_ = loaded;
}

void EnvironmentState::set_overcast_config(const Config *overcast) {
	overcast_config_ = overcast;
	if (is_loaded()) {
		update_tod();
	}
}

void EnvironmentState::set_time_of_day(double hhmm) {
	time_of_day_ = fposmod(hhmm, kHhmmDay);
	// The standalone home is the clock the tick publishes back: an owner
	// setting the TOD (the authored curtime, a preview scrub) moves it too.
	if (weather_is_standalone()) {
		standalone_weather_.tod_fixed24 = hhmm_to_fixed24(time_of_day_);
	}
	if (is_loaded()) {
		update_tod();
	}
}

void EnvironmentState::set_render_time_of_day(double hhmm) {
	time_of_day_ = fposmod(hhmm, kHhmmDay);
	if (is_loaded()) {
		update_tod();
	}
}

void EnvironmentState::update_tod() {
	if (!is_loaded()) {
		return;
	}
	sun_dir_ = compute_sun_direction(static_cast<float>(time_of_day_));
	moon_dir_ = compute_moon_direction(static_cast<float>(time_of_day_));
	// The light source switches sun<->moon at the hardcoded 06:00/18:45
	// boundaries [orig: Environment_GetLightDirectionFloat @ 0x57d870].
	const DayPhase phase = compute_day_phase(static_cast<float>(time_of_day_));
    weather_->tod_keyframed = !config_->keyframes.empty();
    if (weather_->tod_keyframed) weather_->night_phase = phase.is_night ? 1 : 0;
    is_night_ = weather_->is_night_phase();
	day_phase_blend_ = phase.blend;
	light_dir_ = is_night_ ? moon_dir_ : sun_dir_;
	tod_valid_ = !config_->keyframes.empty();
	if (tod_valid_) {
		// The overcast cross-fade (env #16): both tables interpolate, then the
		// weather's overcast blend (read BEFORE its spring stepped this tick)
		// lerps the .env colors toward the overcast table's, toward black with
		// none (env #41) [orig: Environment_ComputeTimeOfDayColors @ 0x57de40 ->
		//  Environment_LerpKeyframeSet @ 0x57c3b0]. No live weather, no blend.
		tod_ = tod_colors(*config_, overcast_config_, static_cast<float>(time_of_day_),
				weather_live() ? weather_->overcast_for_tod_q16 : 0);
	}
	if (!weather_driven_ && tod_valid_) {
		// Standalone (no weather tick): this state owns the current render
		// colors directly. Weather-driven, these fields belong to the smoothed
		// writeback — writing raw keyframes here would fight it (see
		// set_weather_driven).
		sun_light_ = is_night_ ? tod_.moon : tod_.sun;
		fill_light_ = tod_.ground;
		sky_ambient_rt_ = tod_.sky;
		sky_base_rt_ = tod_.skybase;
		sky_bright_rt_ = tod_.skybright;
		sky_highlight_rt_ = tod_.skyhighlight;
		cloud_base_rt_ = tod_.cloudbase;
		cloud_highlight_rt_ = tod_.cloudhighlight;
		cloud_edge_rt_ = tod_.cloudedge;
		ceiling_color_rt_ = ceiling_color_target();
		cloud_tint_rt_ = cloud_tint_target();
		floor_color_rt_ = floor_color_target();
		// Fog/skyfog blocks operate in undoubled authored bytes. Standalone
		// owners derive the same post-blend doubled colors that the weather
		// tick writes after its block tick.
		const Rgb fog_raw = tod_.fog;
		fog_color_rt_ = double_rgb(fog_raw);
		skyfog_color_rt_ = derive_skyfog_render_color(
				fog_raw, tod_.skyfog, fog_level());
	}
	// A TOD recompute can move any object-consumed value (weather-driven, the
	// moving targets flow through the smoothers instead); the bump keeps
	// material restamps tracking either way.
	bump_env_generation();
}

// --- the mission clock ------------------------------------------------------

void EnvironmentState::reset_standalone_weather(int wind_scale) {
	bms::Header header{};
	header.start_time = clock_start_q8_8_;
	header.minutes_per_day = clock_minutes_per_day_;
	world::WeatherSeed seed = config_ != nullptr
			? weather_seed_from_config(*config_, header)
			: weather_seed_from_config(Config{}, header);
	seed.wind_scale = wind_scale;
	standalone_weather_.seed(seed);
	uint32_t precipitation_seed = io::kPrng16BSeed;
	standalone_weather_.precipitation.reset(&io::rotating_prng_callback, &precipitation_seed);
	if (!clock_configured_) {
		// No mission clock configured: a preview holds its render TOD (the
		// authored curtime or the embedder's scrub) and does not run it.
		standalone_weather_.tod_fixed24 = hhmm_to_fixed24(time_of_day_);
		standalone_weather_.tod_advance_per_tick = 0;
	}
}

void EnvironmentState::configure_mission_clock(int start_time_q8_8,
		int minutes_per_day) {
	clock_start_q8_8_ = start_time_q8_8;
	clock_minutes_per_day_ = minutes_per_day;
	clock_configured_ = true;
	// The standalone home takes the clock now (its other channels keep their
	// state; a later reset re-seeds everything from the same numbers).
	standalone_weather_.tod_fixed24 = static_cast<uint32_t>(tod_start_fixed24(start_time_q8_8)) %
			world::WeatherState::kTodDayFixed24;
	standalone_weather_.tod_advance_per_tick =
			static_cast<uint32_t>(tod_advance_per_tick(minutes_per_day));
	standalone_weather_.tod_epoch_tickdown = world::WeatherState::kTodEpochTicks;
	if (weather_is_standalone()) {
		sync_clock_from_weather();
	}
}

void EnvironmentState::advance_mission_clock(int ticks) {
	if (ticks <= 0 || !weather_is_standalone()) {
		return;
	}
	world::WeatherTickEvents events;
	for (int i = 0; i < ticks; ++i) {
		standalone_weather_.tick_sim(nullptr, events);
	}
	sync_clock_from_weather();
}

bool EnvironmentState::debug_set_mission_minute_of_day(double minute_of_day) {
	if (!std::isfinite(minute_of_day) || minute_of_day < 0.0 ||
			minute_of_day >= kClockMinutesPerDay) {
		return false;
	}
	weather_->debug_set_time_of_day_minutes(minute_of_day);
	sync_clock_from_weather();
	return true;
}

double EnvironmentState::mission_minute_of_day() const {
	return hhmm_to_minute_of_day(time_of_day_);
}

int EnvironmentState::mission_time_fixed24() const {
	return static_cast<int>(weather_->tod_fixed24);
}

// --- the weather-home reads --------------------------------------------------

void EnvironmentState::sync_clock_from_weather() {
	// A World home bound before its seed has no clock to publish (it must
	// not replace the configured clock with 00:00); the standalone home's
	// clock is always the owner's (configure_mission_clock / the seed).
	if (!weather_is_standalone() && !weather_live()) {
		return;
	}
	set_render_time_of_day(weather_->tod_hhmm());
}

int EnvironmentState::quake_ticks() const {
	return weather_live() ? static_cast<int>(weather_->quake_ticks) : 0;
}

float EnvironmentState::rain_current() const {
	return weather_live()
			? static_cast<float>(weather_->core.scalar_channels.rain_pct_fp) / 65536.0f
			: 0.0f;
}

float EnvironmentState::overcast_blend() const {
	return weather_live()
			? static_cast<float>(weather_->core.scalar_channels.overcast_fp) / 65536.0f
			: 0.0f;
}

int EnvironmentState::precipitation_kind() const {
	return weather_live() ? static_cast<int>(weather_->precipitation_kind) : 0;
}

bool EnvironmentState::raining() const {
	return weather_live() && weather_->raining();
}

// --- HHMM conversions ------------------------------------------------------

double EnvironmentState::hhmm_to_minute_of_day(double hhmm) {
	const double wrapped = fposmod(hhmm, kHhmmDay);
	const double hour = std::floor(wrapped / kHhmmHourScale);
	return hour * kMinutesPerHour + (wrapped - hour * kHhmmHourScale);
}

// The curtime parse: the truncating 16.16 hours, widened by a shift. The
// minute part truncates (`(m << 16) / 60`), so every minute that is not a
// quarter hour lands below the exact 8.24 hour; the hour and minute clamps
// keep the result under one day.
// [orig: TimeOfDay_ParseProperty curtime arm @0x57d0b6..0x57d0d2,
//  g_EnvCurTimeFixed24 = Environment_ParseTimeString(token) << 8;
//  Environment_ParseTimeString @0x57c500 (clamps @0x57c552/@0x57c55c,
//  the truncating divide @0x57c566..0x57c57c)]
uint32_t EnvironmentState::hhmm_to_fixed24(double hhmm) {
	return static_cast<uint32_t>(hhmm_to_hours_fp(static_cast<float>(hhmm))) << 8;
}

void EnvironmentState::ensure_standalone_weather_seeded(int wind_scale) {
	if (!is_loaded() || standalone_weather_.valid) {
		return;
	}
	reset_standalone_weather(wind_scale);
}

// --- NVG -------------------------------------------------------------------

bool EnvironmentState::set_nvg_view(bool active, int gain) {
	const int clamped_gain = std::clamp(gain, 0, 4);
	if (active == nvg_view_active_ && clamped_gain == nvg_gain_) {
		return false;
	}
	nvg_view_active_ = active;
	nvg_gain_ = clamped_gain;
	bump_env_generation();
	return true;
}

Rgb EnvironmentState::apply_nvg_hemi_gain(const Rgb &color) const {
	const float f = static_cast<float>(nvg_gain_ + 1) * 0.2f;
	const Rgb scaled = rgb_scale(color, 0.25f * f);
	const Rgb gain = rgb_scale(color_src_gain_, f / 10.0f);
	return Rgb{scaled.r + gain.r, scaled.g + gain.g, scaled.b + gain.b};
}

// The interior pair takes the modulator's R term on all three channels: the
// same st(0) term is reused by each of the six `fld st(1); fmul; fadd
// st,st(1); fstp` sequences, while sky/ground use the per-byte terms
// [orig: CTerrainRenderer_BuildLightingShaderConstants @ 0x5c82a5..0x5c82e9
//  vs @ 0x5c8258..0x5c82a1].
Rgb EnvironmentState::apply_nvg_hemi_gain_r(const Rgb &color) const {
	const float f = static_cast<float>(nvg_gain_ + 1) * 0.2f;
	const Rgb scaled = rgb_scale(color, 0.25f * f);
	const float gain_r = color_src_gain_.r * (f / 10.0f);
	return Rgb{scaled.r + gain_r, scaled.g + gain_r, scaled.b + gain_r};
}

// The terrain rebuilds the NVG sky as a packed colour: per channel
// trunc(sky_byte * (0.25 * f) + modulator_byte * (f * 0.0015625 * 255)) with
// f = (level + 1) * 0.2, the x87 control word forced to chop for each fistp,
// and only the stored low byte kept. The alpha byte stays the sky block's.
// sky_ambient_rt_ carries the block bytes / 255 and color_src_gain_ the
// modulator bytes / 64, so both round back to their exact bytes.
// [orig: Render_TerrainScene @ 0x610d16..0x610e22: flt_7C3340 = 0.2,
//  flt_7C333C = 0.25, flt_7D00A8 = 0.0015625, flt_7CA29C = 255, the
//  `or eax, 0C00h; fldcw; fistp` sequences]
Rgb EnvironmentState::nvg_terrain_sky() const {
	const float f = static_cast<float>(nvg_gain_ + 1) * 0.2f;
	const float quarter = 0.25f * f;
	const float k = f * 0.0015625f * 255.0f;
	const auto channel = [&](float sky, float gain) {
		const int sky_byte = std::clamp(static_cast<int>(std::lround(sky * 255.0f)), 0, 255);
		const int mod_byte = std::clamp(static_cast<int>(std::lround(gain * 64.0f)), 0, 255);
		const float sum = static_cast<float>(sky_byte) * quarter +
				static_cast<float>(mod_byte) * k;
		const int byte = static_cast<int>(std::trunc(sum)) & 0xFF;
		return static_cast<float>(byte) / 255.0f;
	};
	return Rgb{channel(sky_ambient_rt_.r, color_src_gain_.r),
			channel(sky_ambient_rt_.g, color_src_gain_.g),
			channel(sky_ambient_rt_.b, color_src_gain_.b)};
}

// --- the thermal view --------------------------------------------------------

bool EnvironmentState::set_thermal_view(bool world, bool terrain) {
	if (world == thermal_view_ && terrain == thermal_terrain_view_) {
		return false;
	}
	thermal_view_ = world;
	thermal_terrain_view_ = terrain;
	bump_env_generation();
	return true;
}

// --- keyframe targets ------------------------------------------------------

// With no keyframe table the per-tick compute writes no color block [orig:
// Environment_ComputeTimeOfDayColors @ 0x57de40, the gate @ 0x57de8a]: the eleven
// keyframed blocks keep the parsed targets the load left them, the .env's scratch
// keyframe [orig: Environment_LoadTimeOfDayConfig @ 0x57dce0..0x57dd57] or, the .env
// skipped, Environment_InitDefaults' values [orig: @ 0x57c03a..0x57c17d] (the same seed,
// formats/env/env.h scratch_keyframe_defaults). `sun` is the light block, sun or moon
// alike: nothing switches it without a table.
const Keyframe &EnvironmentState::untimed_targets() const {
	static const Keyframe kSkipped = scratch_keyframe_defaults();
	return config_ != nullptr ? config_->scratch : kSkipped;
}

Rgb EnvironmentState::fill_light_target() const {
	return tod_valid_ ? tod_.ground : untimed_targets().ground;
}

Rgb EnvironmentState::sun_light_target() const {
	if (!tod_valid_) {
		return untimed_targets().sun;
	}
	return is_night_phase() ? tod_.moon : tod_.sun;
}

Rgb EnvironmentState::fog_color_target() const {
	return double_rgb(fog_color_base_target());
}

Rgb EnvironmentState::fog_color_base_target() const {
	return tod_valid_ ? tod_.fog : untimed_targets().fog;
}

Rgb EnvironmentState::sky_ambient_target() const {
	return tod_valid_ ? tod_.sky : untimed_targets().sky;
}

Rgb EnvironmentState::skyfog_color_target() const {
	return tod_valid_ ? tod_.skyfog : untimed_targets().skyfog;
}

Rgb EnvironmentState::ceiling_color_target() const {
	if (config_ == nullptr) {
		return ceiling_color_rt_;
	}
	return global_color_target(config_->ceiling_rgb);
}

Rgb EnvironmentState::cloud_tint_target() const {
	if (config_ == nullptr) {
		return cloud_tint_rt_;
	}
	return global_color_target(config_->cloud_rgb);
}

Rgb EnvironmentState::floor_color_target() const {
	if (config_ == nullptr) {
		return floor_color_rt_;
	}
	return global_color_target(config_->floor_rgb);
}

Rgb EnvironmentState::lightning_color_target() const {
	if (config_ == nullptr) {
		return Rgb{1.0f, 1.0f, 1.0f};
	}
	return global_color_target(config_->lightning_rgb);
}

Rgb EnvironmentState::sky_base_target() const {
	return tod_valid_ ? tod_.skybase : untimed_targets().skybase;
}

Rgb EnvironmentState::sky_bright_target() const {
	return tod_valid_ ? tod_.skybright : untimed_targets().skybright;
}

Rgb EnvironmentState::sky_highlight_target() const {
	return tod_valid_ ? tod_.skyhighlight : untimed_targets().skyhighlight;
}

Rgb EnvironmentState::cloud_base_target() const {
	return tod_valid_ ? tod_.cloudbase : untimed_targets().cloudbase;
}

Rgb EnvironmentState::cloud_highlight_target() const {
	return tod_valid_ ? tod_.cloudhighlight : untimed_targets().cloudhighlight;
}

Rgb EnvironmentState::cloud_edge_target() const {
	return tod_valid_ ? tod_.cloudedge : untimed_targets().cloudedge;
}

// --- terrain / water -------------------------------------------------------

Rgb EnvironmentState::terrain_tint() const {
	if (config_ == nullptr) {
		return Rgb{1.0f, 1.0f, 1.0f};
	}
	return config_->terrain_rgb;
}

Rgb EnvironmentState::tile_overlay_tint() const {
	Rgb tint{1.0f, 1.0f, 1.0f};
	if (config_ != nullptr) {
		tint = config_->terrain_rgb;
	}
	return tile_overlay_tint_factor(terrain_tint_from_rgb(tint));
}

uint32_t EnvironmentState::terrain_color_recip_packed() const {
	if (config_ == nullptr) {
		return kTerrainColorRecipDefaultPacked; // [orig: @ 0x57c065]
	}
	return terrain_color_recip_from_rgb(config_->terrain_rgb);
}

Rgb EnvironmentState::water_color() const {
	if (config_ == nullptr) {
		return Rgb{0.408f, 0.314f, 0.224f};
	}
	return global_color_target(config_->water_rgb);
}

bool EnvironmentState::has_water_height() const {
	return config_ != nullptr && config_->water_height_set;
}

Rgb EnvironmentState::frame_clear_color_for(bool eye_above_water) const {
	// The thermal frame clears to unk_808080 before the water test
	// [orig: Render_ProcessMainSceneFrame @ 0x5ca771..0x5ca778].
	if (thermal_view_) {
		return kThermalGrey;
	}
	return nvg_scene_clear_color(eye_above_water);
}

Rgb EnvironmentState::nvg_scene_clear_color(bool eye_above_water) const {
	if (eye_above_water) {
		return frame_clear_color();
	}
	const Rgb combined = combine_terrain_light(sun_light(), sky_ambient());
	return lit_water_color(water_color(), combined);
}

Rgb EnvironmentState::water_mirror_clear_color(bool mirror_outdoors) const {
	// [orig: Render_MainScene @ 0x5c1474 xor esi, esi; @ 0x5c1597 mov esi,
	//  g_EnvSkyfogBlock on the outdoors path]
	return mirror_outdoors ? frame_clear_color() : Rgb{};
}

float EnvironmentState::water_height() const {
	return has_water_height() ? config_->water_height : 0.0f;
}

Rgb EnvironmentState::sun_color() const {
	return tod_valid_ ? tod_.sun : Rgb{};
}

Rgb EnvironmentState::moon_color() const {
	return tod_valid_ ? tod_.moon : Rgb{};
}

// --- weather writeback -----------------------------------------------------

void EnvironmentState::set_fill_light(const Rgb &value) {
	if (!rgb_equal(value, fill_light_)) {
		fill_light_ = value;
		bump_env_generation();
	}
}

void EnvironmentState::set_sun_light(const Rgb &value) {
	if (!rgb_equal(value, sun_light_)) {
		sun_light_ = value;
		bump_env_generation();
	}
}

void EnvironmentState::set_fog_color_rt(const Rgb &value) {
	if (!rgb_equal(value, fog_color_rt_)) {
		fog_color_rt_ = value;
		bump_env_generation();
	}
}

void EnvironmentState::set_sky_ambient_rt(const Rgb &value) {
	if (!rgb_equal(value, sky_ambient_rt_)) {
		sky_ambient_rt_ = value;
		bump_env_generation();
	}
}

void EnvironmentState::set_static_colors_rt(const Rgb &ceiling,
		const Rgb &cloud, const Rgb &floor_color) {
	const bool changed = !rgb_equal(ceiling_color_rt_, ceiling) ||
			!rgb_equal(cloud_tint_rt_, cloud) ||
			!rgb_equal(floor_color_rt_, floor_color);
	ceiling_color_rt_ = ceiling;
	cloud_tint_rt_ = cloud;
	floor_color_rt_ = floor_color;
	// cloud_rgb is sampled by the flat dome every frame; ceiling/floor are
	// live indoor-light inputs. Keep the shared material-generation seam
	// honest for any additional consumers that cache environment values.
	if (changed) {
		bump_env_generation();
	}
}

void EnvironmentState::set_sky_colors_rt(const Rgb &skyfog,
		const Rgb &sky_base, const Rgb &sky_bright, const Rgb &sky_highlight,
		const Rgb &cloud_base, const Rgb &cloud_highlight,
		const Rgb &cloud_edge) {
	const bool skyfog_changed = !rgb_equal(skyfog_color_rt_, skyfog);
	skyfog_color_rt_ = skyfog;
	sky_base_rt_ = sky_base;
	sky_bright_rt_ = sky_bright;
	sky_highlight_rt_ = sky_highlight;
	cloud_base_rt_ = cloud_base;
	cloud_highlight_rt_ = cloud_highlight;
	cloud_edge_rt_ = cloud_edge;
	// The world composer generation-gates the clear-color push; the six dome
	// ramps are sampled directly every frame, but the shared skyfog/clear
	// value must wake that gate when its weather block moves.
	if (skyfog_changed) {
		bump_env_generation();
	}
}

void EnvironmentState::set_color_src_gain(const Rgb &value) {
	if (!rgb_equal(value, color_src_gain_)) {
		color_src_gain_ = value;
		bump_env_generation();
	}
}

// --- the smoothed scalars (env #27) ------------------------------------------

float EnvironmentState::fog_level() const {
	if (weather_live()) {
		return static_cast<float>(weather_->core.scalar_channels.fog_dist_fp) / 65536.0f;
	}
	return config_ != nullptr ? config_->fog_level : 1000.0f;
}

float EnvironmentState::fog_level_target() const {
	if (weather_live()) {
		return static_cast<float>(weather_->core.scalar_channels.fog_dist_target_fp) / 65536.0f;
	}
	// The parsed .env value — the spring target the weather tick chases.
	return config_ != nullptr ? config_->fog_level : 1000.0f;
}

float EnvironmentState::fog_start() const {
	return compute_fog_params(fog_type(), fog_end_distance(), overcast_blend())
			.start;
}

float EnvironmentState::fog_end_distance() const {
	return fog_level() * (1.0f - overcast_blend() * 0.5f);
}

int EnvironmentState::fog_type() const {
	// g_EnvFogType: the .env value at the seed, the `fogtype` WAC after
	// [orig: @ 0x26c6808].
	if (weather_live()) {
		return weather_->fog_type;
	}
	return config_ != nullptr ? config_->fog_type : 2;
}

float EnvironmentState::sky_speed() const {
	if (weather_live()) {
		return static_cast<float>(weather_->cloud_scroll_rate_target >> 10);
	}
	return config_ != nullptr ? config_->sky_speed : 15.0f;
}

float EnvironmentState::sky_height() const {
	if (weather_live()) {
		return static_cast<float>(weather_->core.scalar_channels.sky_height_fp) / 65536.0f;
	}
	return config_ != nullptr ? config_->sky_height : 175.0f;
}

float EnvironmentState::sky_height_target() const {
	if (weather_live()) {
		return static_cast<float>(weather_->core.scalar_channels.sky_height_target_fp) / 65536.0f;
	}
	return config_ != nullptr ? config_->sky_height : 175.0f;
}

float EnvironmentState::sun_dim_pct() const {
	return weather_live()
			? static_cast<float>(weather_->core.scalar_channels.sun_dim_fp) / 65536.0f
			: 0.0f;
}

// --- typed value builders --------------------------------------------------

bool EnvironmentState::build_light_values(const Vec3 &default_dir,
		WorldLightValues &out, bool underwater_view) const {
	if (!is_loaded()) {
		return false;
	}
	const Vec3 light_dir = light_direction();
	const float length = vec3_length(light_dir);
	if (length <= 0.001f) {
		out.dir = default_dir;
	} else {
		out.dir = Vec3{-light_dir.x / length, -light_dir.y / length,
				-light_dir.z / length};
	}
	out.hemi_sky = sky_ambient();
	out.dir_color = sun_light();
	out.hemi_ground = fill_light();
	out.ceiling = ceiling_color();
	out.floor_color = floor_color();
	// The first-person NVG rewrite of the block's four hemisphere colours:
	// sky/ground per channel, ceiling/floor on the modulator's R term. It lives
	// in this block alone; the colour getters stay raw
	// [orig: CTerrainRenderer_BuildLightingShaderConstants @ 0x5c820b..0x5c82e9].
	if (nvg_view_active_) {
		out.hemi_sky = apply_nvg_hemi_gain(out.hemi_sky);
		out.hemi_ground = apply_nvg_hemi_gain(out.hemi_ground);
		out.ceiling = apply_nvg_hemi_gain_r(out.ceiling);
		out.floor_color = apply_nvg_hemi_gain_r(out.floor_color);
	}
	// The thermal grey override, after the NVG rewrite:
	// constant [0] (the dir-enable flag) clears — the ctx store then zeroes
	// the dir colour, so the 0.1 written into [1..4] never reaches a draw —
	// and every sky/ground/ceiling/floor channel becomes 0.5; the gain is
	// bound separately and untouched
	// [orig: CTerrainRenderer_BuildLightingShaderConstants @ 0x5c837c
	//  (Player_IsOpticalViewVisible) && @ 0x5c8389 (EquippedSlot->Def flags2 & 4)
	//  -> @ 0x5c839c..0x5c843c; RenderBatchCtx_StoreLightingConstants
	//  @ 0x5d8b06..0x5d8b33].
	if (thermal_view_) {
		out.dir_color = Rgb{0.0f, 0.0f, 0.0f};
		out.hemi_sky = kThermalHemi;
		out.hemi_ground = kThermalHemi;
		out.ceiling = kThermalHemi;
		out.floor_color = kThermalHemi;
	}
	out.gain = color_src_gain();
	out.fog_enabled = true;
	out.fog_color = fog_color();
	// The primary device fog block as Environment_ApplyFogAndAmbient sets it:
	// the end is Environment_GetFogEndDistance's overcast-scaled distance
	// (fog_end_distance, the same end build_scene_fog publishes to the shader
	// globals) and the start is what Render_SetFogState resolved from that
	// end for the fog type. The corona fog-to-black fold and the Q3 copies
	// consume this pair, so both must fog over the one device range.
	// [orig: Environment_ApplyFogAndAmbient @ 0x57e44c (GetFogEndDistance)
	//  .. 0x57e4db (Render_SetFogState(0.5, fogEnd, type, overcast))]
	out.fog_start = fog_start();
	out.fog_end = fog_end_distance();
	out.fog_type = fog_type();
	if (underwater_view) {
		const SceneFogValues fog = build_scene_fog(true);
		out.fog_color = fog.color;
		out.fog_start = fog.start;
		out.fog_end = fog.end;
		out.fog_type = fog.type;
	}
	return true;
}

EnvShaderGlobals EnvironmentState::build_shader_globals(
		bool underwater_view) const {
	EnvShaderGlobals globals;
	globals.fill_light = fill_light();
	// The sun/sky globals are the terrain's c1/c0 pair (foliage inherits the
	// same two device constants), so the thermal ramps select once, below.
	const TerrainEnvUniforms terrain = build_terrain_uniforms(underwater_view);
	globals.sun_light = terrain.sun_light;
	globals.sky_ambient = terrain.sky_ambient;
	globals.light_block = sun_light();
	globals.sun_direction = light_dir_;
	const SceneFogValues fog = build_scene_fog(underwater_view);
	globals.fog_color = fog.color;
	globals.fog_end = fog.end;
	globals.fog_start = fog.start;
	globals.fog_type = fog.type;
	return globals;
}

TerrainEnvUniforms EnvironmentState::build_terrain_uniforms(
		bool underwater_view) const {
	TerrainEnvUniforms uniforms;
	// c1 <- the light block, c0 <- the sky block. NVG in first person takes
	// its branch first with the byte-rebuilt sky (nvg_terrain_sky); else the
	// thermal view in first person swaps in the flat 0x101010 / 0xF0F0F0 pair
	// [orig: Render_TerrainScene @ 0x610d10 (NVG gate) ->
	//  Terrain_InitLightingColorRamps(g_EnvLightBlock, NVG sky) @ 0x610e36;
	//  @ 0x610e51 (flags2 & Thermal && camera mode 0) ->
	//  Terrain_InitLightingColorRamps (0x101010, 0xF0F0F0) @ 0x610e65;
	//  else (g_EnvLightBlock, g_EnvSkyBlock) @ 0x610ea1].
	if (nvg_view_active_) {
		uniforms.sun_light = sun_light();
		uniforms.sky_ambient = nvg_terrain_sky();
	} else if (thermal_terrain_view_) {
		uniforms.sun_light = kThermalTerrainLight;
		uniforms.sky_ambient = kThermalTerrainSky;
	} else {
		uniforms.sun_light = sun_light();
		uniforms.sky_ambient = sky_ambient();
	}
	const SceneFogValues fog = build_scene_fog(underwater_view);
	uniforms.fog_color = fog.color;
	uniforms.fog_end = fog.end;
	uniforms.fog_start = fog.start;
	uniforms.fog_type = fog.type;
	return uniforms;
}

float EnvironmentState::water_murk() const {
	return config_ != nullptr ? config_->water_murk : 0.8f;
}

SceneFogValues EnvironmentState::build_water_mirror_fog() const {
	// Environment_ApplyFogAndAmbient(0, 0): g_EnvFogBlock under the dry range
	// and type [orig: @ 0x57e49b..0x57e4db]; the reflected pass never selects
	// the alternate (thermal) or underwater branch.
	SceneFogValues fog;
	fog.color = fog_color();
	fog.start = fog_start();
	fog.end = fog_end_distance();
	fog.type = fog_type();
	return fog;
}

SceneFogValues EnvironmentState::build_inset_scene_fog(bool underwater_view) const {
	// Environment_ApplyFogAndAmbient(0, underwater): the lit water below the
	// plane, else g_EnvFogBlock under the dry range and type [orig: @ 0x57e46c
	// (the underwater branch), @ 0x57e48c..0x57e4a1 (no alternate)].
	return underwater_view ? build_scene_fog(true) : build_water_mirror_fog();
}

SceneFogValues EnvironmentState::build_scene_fog(
		bool underwater_view) const {
	SceneFogValues fog;
	if (!underwater_view) {
		// The dry pass: the thermal view's unk_808080, else the weather fog
		// block; the range and type stay the weather's either way
		// [orig: Environment_ApplyFogAndAmbient @ 0x57e491..0x57e4ad].
		fog.color = thermal_view_ ? kThermalGrey : fog_color();
		fog.start = fog_start();
		fog.end = fog_end_distance();
		fog.type = fog_type();
		return fog;
	}

	// The underwater pass replaces the weather fog block with lit water and
	// the murk visibility curve. Type 1 is linear and its retail caller start
	// is the fixed 0.5 world units encoded by compute_fog_params.
	// [orig: Environment_ApplyFogAndAmbient @ 0x57E471..0x57E4AD — device fog
	// color <- g_EnvWaterColorLit and fog type <- 1 when underwater]
	const Rgb combined = combine_terrain_light(sun_light(), sky_ambient());
	fog.color = lit_water_color(water_color(), combined);
	fog.end = fog_end_underwater(water_murk());
	fog.type = 1;
	fog.start = compute_fog_params(
			fog.type, fog.end, overcast_blend()).start;
	return fog;
}

SceneFogValues EnvironmentState::build_viewmodel_fog(bool sky_dome_drawn) const {
	// [orig: ApplyFogAndAmbient(0, thermal) @ 0x5ca3bf..0x5ca3ce; the dome
	//  wrapper's g_EnvFogBlock restore @ 0x579ce6..0x579cf6]
	SceneFogValues fog = build_scene_fog(false);
	if (thermal_view_ && sky_dome_drawn) {
		fog.color = fog_color();
	}
	return fog;
}

// --- color math ------------------------------------------------------------

float EnvironmentState::scale_global_channel(float channel, float envscale) {
	const int authored_byte = std::clamp(
			static_cast<int>(channel * 255.0f + 0.5f), 0, 255);
	const int scaled_byte = std::clamp(
			static_cast<int>(static_cast<float>(authored_byte) * envscale), 0,
			255);
	return static_cast<float>(scaled_byte) / 255.0f;
}

Rgb EnvironmentState::scale_global_color(const Rgb &color, float envscale) {
	return Rgb{
		scale_global_channel(color.r, envscale),
		scale_global_channel(color.g, envscale),
		scale_global_channel(color.b, envscale),
	};
}

Rgb EnvironmentState::global_color_target(const Rgb &color) const {
	const float envscale = config_ != nullptr ? config_->envscale : 1.0f;
	return scale_global_color(color, envscale);
}

Rgb EnvironmentState::double_rgb(const Rgb &value) {
	return double_saturate(value);
}

Rgb EnvironmentState::derive_skyfog_render_color(const Rgb &fog_raw,
		const Rgb &skyfog_raw, float fog_distance) {
	const Rgb blended = horizon_blend_skyfog(fog_raw, skyfog_raw,
			io::float_to_fp16_16_nonneg(fog_distance), io::float_to_fp16_16_nonneg(1024.0f));
	return double_rgb(blended);
}

namespace {

EnvTextReader text_reader(const FileSource &files) {
	return [&files](const std::string &name, std::string &text) {
		std::vector<uint8_t> bytes;
		if (!files.read(name, bytes)) return false;
		text.assign(bytes.begin(), bytes.end());
		return true;
	};
}

} // namespace

bool read_mission_env(const FileSource &files, const std::string &terrain_file,
		const std::string &environment_file, MissionEnv &out) {
	return read_mission_env(text_reader(files), terrain_file, environment_file, out);
}

bool load_mission_env_config(const FileSource &files, const std::string &terrain,
		const std::string &environment, const BmsEnvOverrides &overrides, MissionEnv &out) {
	return load_mission_env_config(text_reader(files), terrain, environment, overrides, out);
}

} // namespace opennova::env
