#include <runtime/environment/environment_state.h>

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

// The same world-units -> 16.16 clamp the EnvFile static used.
uint32_t to_fixed_16_16(float units) {
	if (units <= 0.0f) {
		return 0u;
	}
	return static_cast<uint32_t>(units * 65536.0f);
}

} // namespace

void EnvironmentState::set_config(const Config *config, bool loaded) {
	clear_network_state();
	config_ = config;
	loaded_ = loaded;
}

void EnvironmentState::set_time_of_day(double hhmm) {
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
	is_night_ = phase.is_night;
	day_phase_blend_ = phase.blend;
	light_dir_ = is_night_ ? moon_dir_ : sun_dir_;
	tod_valid_ = !config_->keyframes.empty();
	if (tod_valid_) {
		tod_ = interpolate_tod(config_->keyframes,
				static_cast<float>(time_of_day_), config_->envscale);
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
	if (!weather_driven_) {
		fog_distance_ = config_->fog_level;
	}
	// A TOD recompute can move any object-consumed value (weather-driven, the
	// moving targets flow through the smoothers instead); the bump keeps
	// material restamps tracking either way.
	bump_env_generation();
}

// --- mission clock ---------------------------------------------------------

void EnvironmentState::configure_mission_clock(int start_time_q8_8,
		int minutes_per_day) {
	clear_network_state();
	mission_time_fixed24_ = tod_start_fixed24(start_time_q8_8);
	mission_advance_per_tick_ = tod_advance_per_tick(minutes_per_day);
	set_time_of_day(mission_start_time_hhmm(start_time_q8_8));
}

double EnvironmentState::mission_start_time_hhmm(int start_time_q8_8) {
	return fixed24_to_hhmm(tod_start_fixed24(start_time_q8_8));
}

void EnvironmentState::advance_mission_clock(int ticks) {
	if (ticks <= 0) {
		return;
	}
	// Net receive writes Env_QuakeTicks, but retail's shared 62 Hz environment
	// tick still owns the countdown between phase-2 samples.
	if (network_environment_active_ && network_quake_ticks_ > 0) {
		network_quake_ticks_ = std::max(0, network_quake_ticks_ - ticks);
	}
	mission_time_fixed24_ = tod_advance(
			mission_time_fixed24_, ticks, mission_advance_per_tick_);
	set_time_of_day(fixed24_to_hhmm(mission_time_fixed24_));
}

bool EnvironmentState::debug_set_mission_minute_of_day(double minute_of_day) {
	if (!std::isfinite(minute_of_day) || minute_of_day < 0.0 ||
			minute_of_day >= kClockMinutesPerDay) {
		return false;
	}
	mission_time_fixed24_ = static_cast<int>(std::lround(
			minute_of_day * static_cast<double>(kFixed24OneHour) /
			kMinutesPerHour)) % kTodDayFixed24;
	set_time_of_day(minute_of_day_to_hhmm(minute_of_day));
	return true;
}

double EnvironmentState::mission_minute_of_day() const {
	return hhmm_to_minute_of_day(time_of_day_);
}

// --- network phase-2 overrides ---------------------------------------------

void EnvironmentState::apply_network_sample(const NetEnvSample &sample) {
	network_environment_active_ = true;
	network_fog_target_ = static_cast<float>(
			std::clamp(sample.fog_dist, 0, 0xFFFF));
	network_sky_speed_ = static_cast<float>(
			std::clamp(sample.cloud_scroll, 0, 0xFF));
	network_quake_ticks_ = std::clamp(sample.quake_ticks, 0, 0xFF);
	network_precipitation_kind_ = std::clamp(sample.precipitation_kind, 0, 0xFF);
	mission_time_fixed24_ =
			(std::clamp(sample.tod_fixed, 0, 0xFFFF) << 13) % kTodDayFixed24;
	set_time_of_day(fixed24_to_hhmm(mission_time_fixed24_));
}

void EnvironmentState::clear_network_state() {
	network_environment_active_ = false;
	network_fog_target_ = 0.0f;
	network_sky_speed_ = 0.0f;
	network_quake_ticks_ = 0;
	network_rain_current_ = 0.0f;
	network_overcast_blend_ = 0.0f;
	network_precipitation_kind_ = 0;
}

int EnvironmentState::network_quake_ticks() const {
	return network_environment_active_ ? network_quake_ticks_ : 0;
}

float EnvironmentState::network_rain_current() const {
	return network_environment_active_ ? network_rain_current_ : 0.0f;
}

float EnvironmentState::overcast_blend() const {
	return network_environment_active_ ? network_overcast_blend_ : 0.0f;
}

int EnvironmentState::network_precipitation_kind() const {
	return network_environment_active_ ? network_precipitation_kind_ : 0;
}

// --- HHMM conversions ------------------------------------------------------

double EnvironmentState::minute_of_day_to_hhmm(double minute_of_day) {
	const double wrapped = fposmod(minute_of_day, kClockMinutesPerDay);
	const double hour = std::floor(wrapped / kMinutesPerHour);
	return hour * kHhmmHourScale + fposmod(wrapped, kMinutesPerHour);
}

double EnvironmentState::hhmm_to_minute_of_day(double hhmm) {
	const double wrapped = fposmod(hhmm, kHhmmDay);
	const double hour = std::floor(wrapped / kHhmmHourScale);
	return hour * kMinutesPerHour + (wrapped - hour * kHhmmHourScale);
}

double EnvironmentState::fixed24_to_hhmm(int value) {
	return hours_to_hhmm(
			static_cast<double>(value) / static_cast<double>(kFixed24OneHour));
}

double EnvironmentState::hours_to_hhmm(double hours) {
	const double wrapped = fposmod(hours, static_cast<double>(kHoursPerDay));
	const double hour = std::floor(wrapped);
	return hour * kHhmmHourScale + (wrapped - hour) * kMinutesPerHour;
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

Rgb EnvironmentState::fill_light() const {
	return nvg_view_active_ ? apply_nvg_hemi_gain(fill_light_) : fill_light_;
}

// The SMOOTHED sky block when the weather tick drives it — written back per
// tick like fill/sun/fog, so object hemi_sky serves the post-modulator block
// [orig: CTerrainRenderer_BuildLightingShaderConstants @ 0x5c8090 reads
// Env_SkyBlock[0]; the blocks smooth + modulate in the weather tick
// @ 0x57ef97..0x57f03c]. Discrete TOD recomputes re-seed it from the keyframe
// (like fill); the chase target stays sky_ambient_target().
Rgb EnvironmentState::sky_ambient() const {
	return nvg_view_active_ ? apply_nvg_hemi_gain(sky_ambient_rt_)
							: sky_ambient_rt_;
}

// --- keyframe targets ------------------------------------------------------

Rgb EnvironmentState::fill_light_target() const {
	return tod_valid_ ? tod_.ground : fill_light_;
}

Rgb EnvironmentState::sun_light_target() const {
	if (!tod_valid_) {
		return sun_light_;
	}
	return is_night_ ? tod_.moon : tod_.sun;
}

Rgb EnvironmentState::fog_color_target() const {
	return double_rgb(fog_color_base_target());
}

Rgb EnvironmentState::fog_color_base_target() const {
	return tod_valid_ ? tod_.fog : rgb_scale(fog_color_rt_, 0.5f);
}

Rgb EnvironmentState::sky_ambient_target() const {
	return tod_valid_ ? tod_.sky : Rgb{0.3f, 0.4f, 0.6f};
}

Rgb EnvironmentState::skyfog_color_target() const {
	return tod_valid_ ? tod_.skyfog : Rgb{};
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
	return tod_valid_ ? tod_.skybase : sky_base_rt_;
}

Rgb EnvironmentState::sky_bright_target() const {
	return tod_valid_ ? tod_.skybright : sky_bright_rt_;
}

Rgb EnvironmentState::sky_highlight_target() const {
	return tod_valid_ ? tod_.skyhighlight : sky_highlight_rt_;
}

Rgb EnvironmentState::cloud_base_target() const {
	return tod_valid_ ? tod_.cloudbase : cloud_base_rt_;
}

Rgb EnvironmentState::cloud_highlight_target() const {
	return tod_valid_ ? tod_.cloudhighlight : cloud_highlight_rt_;
}

Rgb EnvironmentState::cloud_edge_target() const {
	return tod_valid_ ? tod_.cloudedge : cloud_edge_rt_;
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

Rgb EnvironmentState::frame_clear_color_for(bool indoors,
		bool above_water) const {
	if (indoors) {
		return Rgb{0.0f, 0.0f, 0.0f};
	}
	if (above_water) {
		return frame_clear_color();
	}
	const Rgb combined = combine_terrain_light(sun_light(), sky_ambient());
	return lit_water_color(water_color(), combined);
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

// --- env #27 smoothed scalars ---------------------------------------------

float EnvironmentState::fog_level() const {
	if (fog_dist_smoothed_ >= 0.0f) {
		return fog_dist_smoothed_;
	}
	return config_ != nullptr ? config_->fog_level : 1000.0f;
}

float EnvironmentState::fog_level_target() const {
	if (network_environment_active_) {
		return network_fog_target_;
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
	return config_ != nullptr ? config_->fog_type : 2;
}

float EnvironmentState::sky_speed() const {
	if (network_environment_active_) {
		return network_sky_speed_;
	}
	return config_ != nullptr ? config_->sky_speed : 15.0f;
}

float EnvironmentState::sky_height() const {
	if (sky_height_smoothed_ >= 0.0f) {
		return sky_height_smoothed_;
	}
	return config_ != nullptr ? config_->sky_height : 175.0f;
}

float EnvironmentState::sky_height_target() const {
	// The parsed .env value — the eighth-snap target.
	return config_ != nullptr ? config_->sky_height : 175.0f;
}

void EnvironmentState::set_smoothed_scalars(float fog_distance,
		float sky_height, float sun_dim_pct, float rain_current,
		float overcast_blend) {
	const bool fog_changed = fog_dist_smoothed_ != fog_distance;
	fog_dist_smoothed_ = fog_distance;
	sky_height_smoothed_ = sky_height;
	sun_dim_smoothed_ = sun_dim_pct;
	network_rain_current_ = rain_current;
	network_overcast_blend_ = overcast_blend;
	if (fog_changed) {
		bump_env_generation();
	}
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
	globals.sun_light = sun_light_;
	globals.sky_ambient = sky_ambient();
	globals.sun_direction = light_dir_;
	const SceneFogValues fog = build_scene_fog(underwater_view);
	globals.fog_color = fog.color;
	globals.fog_end = fog.end;
	globals.fog_start = fog.start;
	globals.fog_type = fog.type;
	globals.wind_sway_amount = 1.0f;
	globals.wind_sway_phase = 0.0f;
	return globals;
}

TerrainEnvUniforms EnvironmentState::build_terrain_uniforms(
		bool underwater_view) const {
	TerrainEnvUniforms uniforms;
	uniforms.sun_light = sun_light();
	uniforms.sky_ambient = sky_ambient();
	uniforms.sun_direction = light_direction();
	uniforms.tile_overlay_tint = tile_overlay_tint();
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

SceneFogValues EnvironmentState::build_scene_fog(
		bool underwater_view) const {
	SceneFogValues fog;
	if (!underwater_view) {
		fog.color = fog_color();
		fog.start = fog_start();
		fog.end = fog_end_distance();
		fog.type = fog_type();
		return fog;
	}

	// The underwater pass replaces the weather fog block with lit water and
	// the murk visibility curve. Type 1 is linear and its retail caller start
	// is the fixed 0.5 world units encoded by compute_fog_params.
	// [orig: Environment_ApplyFogAndAmbient @ 0x57E471..0x57E4AD — device fog
	// color <- Env_WaterColorLit and fog type <- 1 when underwater]
	const Rgb combined = combine_terrain_light(sun_light(), sky_ambient());
	fog.color = lit_water_color(water_color(), combined);
	fog.end = fog_end_underwater(water_murk());
	fog.type = 1;
	fog.start = compute_fog_params(
			fog.type, fog.end, overcast_blend()).start;
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
			to_fixed_16_16(fog_distance), to_fixed_16_16(1024.0f));
	return double_rgb(blended);
}

} // namespace opennova::env
