#include "env/mission_environment.h"

#include "env/env_axes.h"

#include <runtime/environment/water_frame.h>

#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace godot {

namespace {

Vector3 to_vector3(const opennova::env::Rgb &rgb) {
	return Vector3(rgb.r, rgb.g, rgb.b);
}

Vector3 to_vector3(const opennova::env::Vec3 &v) {
	return Vector3(v.x, v.y, v.z);
}

opennova::env::Rgb to_rgb(const Vector3 &v) {
	return opennova::env::Rgb{
		static_cast<float>(v.x), static_cast<float>(v.y),
		static_cast<float>(v.z)};
}

} // namespace

MissionEnvironment::MissionEnvironment() {
	light_state_.instantiate();
}

void MissionEnvironment::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_environment_data", "value"),
			&MissionEnvironment::set_environment_data);
	ClassDB::bind_method(D_METHOD("get_environment_data"),
			&MissionEnvironment::get_environment_data);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "environment_data",
						 PROPERTY_HINT_RESOURCE_TYPE, "EnvFile"),
			"set_environment_data", "get_environment_data");
	ClassDB::bind_method(D_METHOD("set_time_of_day", "value"),
			&MissionEnvironment::set_time_of_day);
	ClassDB::bind_method(D_METHOD("get_time_of_day"),
			&MissionEnvironment::get_time_of_day);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "time_of_day",
						 PROPERTY_HINT_RANGE, "0,2359,1"),
			"set_time_of_day", "get_time_of_day");
	ClassDB::bind_method(D_METHOD("is_loaded"), &MissionEnvironment::is_loaded);
	ClassDB::bind_method(D_METHOD("get_light_state"),
			&MissionEnvironment::get_light_state);

	ClassDB::bind_method(
			D_METHOD("configure_mission_clock", "start_time_q8_8", "minutes_per_day"),
			&MissionEnvironment::configure_mission_clock);
	ClassDB::bind_static_method("MissionEnvironment",
			D_METHOD("mission_start_time_hhmm", "start_time_q8_8"),
			&MissionEnvironment::mission_start_time_hhmm);
	ClassDB::bind_method(D_METHOD("advance_mission_clock", "ticks"),
			&MissionEnvironment::advance_mission_clock);
	ClassDB::bind_method(
			D_METHOD("debug_set_mission_minute_of_day", "minute_of_day"),
			&MissionEnvironment::debug_set_mission_minute_of_day);
	ClassDB::bind_method(D_METHOD("get_mission_minute_of_day"),
			&MissionEnvironment::get_mission_minute_of_day);
	ClassDB::bind_method(D_METHOD("get_mission_time_fixed24"),
			&MissionEnvironment::get_mission_time_fixed24);
	ClassDB::bind_static_method("MissionEnvironment",
			D_METHOD("minute_of_day_to_hhmm", "minute_of_day"),
			&MissionEnvironment::minute_of_day_to_hhmm);
	ClassDB::bind_static_method("MissionEnvironment",
			D_METHOD("hhmm_to_minute_of_day", "hhmm"),
			&MissionEnvironment::hhmm_to_minute_of_day);

	ClassDB::bind_method(D_METHOD("apply_network_environment_sample", "sample"),
			&MissionEnvironment::apply_network_environment_sample);
	ClassDB::bind_method(D_METHOD("get_network_quake_ticks"),
			&MissionEnvironment::get_network_quake_ticks);
	ClassDB::bind_method(D_METHOD("get_network_rain_current"),
			&MissionEnvironment::get_network_rain_current);
	ClassDB::bind_method(D_METHOD("get_overcast_blend"),
			&MissionEnvironment::get_overcast_blend);

	ClassDB::bind_method(D_METHOD("set_weather_driven", "driven"),
			&MissionEnvironment::set_weather_driven);
	ClassDB::bind_method(D_METHOD("is_weather_driven"),
			&MissionEnvironment::is_weather_driven);
	ClassDB::bind_method(D_METHOD("set_nvg_view", "active", "gain"),
			&MissionEnvironment::set_nvg_view);
	ClassDB::bind_method(D_METHOD("set_underwater_view", "underwater"),
			&MissionEnvironment::set_underwater_view);
	ClassDB::bind_method(D_METHOD("is_underwater_view"),
			&MissionEnvironment::is_underwater_view);
	ClassDB::bind_method(D_METHOD("set_underwater_overlay_view", "underwater"),
			&MissionEnvironment::set_underwater_overlay_view);
	ClassDB::bind_method(D_METHOD("apply_render_eye", "eye_y", "water_height",
			"water_active"), &MissionEnvironment::apply_render_eye);
	ClassDB::bind_method(D_METHOD("is_underwater_overlay_view"),
			&MissionEnvironment::is_underwater_overlay_view);
	ClassDB::bind_method(D_METHOD("get_underwater_overlay_color"),
			&MissionEnvironment::get_underwater_overlay_color);
	ClassDB::bind_method(D_METHOD("get_underwater_overlay_alpha_byte"),
			&MissionEnvironment::get_underwater_overlay_alpha_byte);
	ClassDB::bind_method(D_METHOD("get_scene_fog_color"),
			&MissionEnvironment::get_scene_fog_color);
	ClassDB::bind_method(D_METHOD("get_scene_fog_end"),
			&MissionEnvironment::get_scene_fog_end);

	ClassDB::bind_method(D_METHOD("get_sun_light"),
			&MissionEnvironment::get_sun_light);
	ClassDB::bind_method(D_METHOD("get_fill_light"),
			&MissionEnvironment::get_fill_light);
	ClassDB::bind_method(D_METHOD("get_sky_ambient"),
			&MissionEnvironment::get_sky_ambient);
	ClassDB::bind_method(D_METHOD("get_fog_color"),
			&MissionEnvironment::get_fog_color);
	ClassDB::bind_method(D_METHOD("get_skyfog_color"),
			&MissionEnvironment::get_skyfog_color);
	ClassDB::bind_method(D_METHOD("get_frame_clear_color"),
			&MissionEnvironment::get_frame_clear_color);
	ClassDB::bind_method(
			D_METHOD("frame_clear_color_for", "indoors", "above_water"),
			&MissionEnvironment::frame_clear_color_for);
	ClassDB::bind_method(D_METHOD("get_ceiling_color"),
			&MissionEnvironment::get_ceiling_color);
	ClassDB::bind_method(D_METHOD("get_cloud_tint"),
			&MissionEnvironment::get_cloud_tint);
	ClassDB::bind_method(D_METHOD("get_floor_color"),
			&MissionEnvironment::get_floor_color);
	ClassDB::bind_method(D_METHOD("get_sky_base"),
			&MissionEnvironment::get_sky_base);
	ClassDB::bind_method(D_METHOD("get_sky_bright"),
			&MissionEnvironment::get_sky_bright);
	ClassDB::bind_method(D_METHOD("get_sky_highlight"),
			&MissionEnvironment::get_sky_highlight);
	ClassDB::bind_method(D_METHOD("get_cloud_base"),
			&MissionEnvironment::get_cloud_base);
	ClassDB::bind_method(D_METHOD("get_cloud_highlight"),
			&MissionEnvironment::get_cloud_highlight);
	ClassDB::bind_method(D_METHOD("get_cloud_edge"),
			&MissionEnvironment::get_cloud_edge);
	ClassDB::bind_method(D_METHOD("get_color_src_gain"),
			&MissionEnvironment::get_color_src_gain);

	ClassDB::bind_method(D_METHOD("get_fill_light_target"),
			&MissionEnvironment::get_fill_light_target);
	ClassDB::bind_method(D_METHOD("get_sun_light_target"),
			&MissionEnvironment::get_sun_light_target);
	ClassDB::bind_method(D_METHOD("get_fog_color_target"),
			&MissionEnvironment::get_fog_color_target);
	ClassDB::bind_method(D_METHOD("get_sky_ambient_target"),
			&MissionEnvironment::get_sky_ambient_target);
	ClassDB::bind_method(D_METHOD("get_ceiling_color_target"),
			&MissionEnvironment::get_ceiling_color_target);
	ClassDB::bind_method(D_METHOD("get_cloud_tint_target"),
			&MissionEnvironment::get_cloud_tint_target);
	ClassDB::bind_method(D_METHOD("get_floor_color_target"),
			&MissionEnvironment::get_floor_color_target);
	ClassDB::bind_method(D_METHOD("get_lightning_color_target"),
			&MissionEnvironment::get_lightning_color_target);

	ClassDB::bind_method(D_METHOD("get_terrain_tint"),
			&MissionEnvironment::get_terrain_tint);
	ClassDB::bind_method(D_METHOD("get_terrain_lighting_attenuation"),
			&MissionEnvironment::get_terrain_lighting_attenuation);
	ClassDB::bind_method(D_METHOD("get_tile_overlay_tint"),
			&MissionEnvironment::get_tile_overlay_tint);
	ClassDB::bind_method(D_METHOD("apply_terrain_uniforms", "material"),
			&MissionEnvironment::apply_terrain_uniforms);
	ClassDB::bind_method(D_METHOD("get_water_color"),
			&MissionEnvironment::get_water_color);
	ClassDB::bind_method(D_METHOD("has_water_height"),
			&MissionEnvironment::has_water_height);
	ClassDB::bind_method(D_METHOD("get_water_height"),
			&MissionEnvironment::get_water_height);
	ClassDB::bind_method(D_METHOD("get_sun_direction"),
			&MissionEnvironment::get_sun_direction);
	ClassDB::bind_method(D_METHOD("get_moon_direction"),
			&MissionEnvironment::get_moon_direction);
	ClassDB::bind_method(D_METHOD("get_light_direction"),
			&MissionEnvironment::get_light_direction);
	ClassDB::bind_method(D_METHOD("get_light_direction_render_tuple"),
			&MissionEnvironment::get_light_direction_render_tuple);
	ClassDB::bind_method(D_METHOD("is_night_phase"),
			&MissionEnvironment::is_night_phase);
	ClassDB::bind_method(D_METHOD("get_day_phase_blend"),
			&MissionEnvironment::get_day_phase_blend);
	ClassDB::bind_method(D_METHOD("get_sun_color"),
			&MissionEnvironment::get_sun_color);
	ClassDB::bind_method(D_METHOD("get_moon_color"),
			&MissionEnvironment::get_moon_color);

	ClassDB::bind_method(D_METHOD("set_fill_light", "value"),
			&MissionEnvironment::set_fill_light);
	ClassDB::bind_method(D_METHOD("set_sun_light", "value"),
			&MissionEnvironment::set_sun_light);
	ClassDB::bind_method(D_METHOD("set_fog_color_rt", "value"),
			&MissionEnvironment::set_fog_color_rt);
	ClassDB::bind_method(D_METHOD("set_sky_ambient_rt", "value"),
			&MissionEnvironment::set_sky_ambient_rt);
	ClassDB::bind_method(
			D_METHOD("set_static_colors_rt", "ceiling", "cloud", "floor_color"),
			&MissionEnvironment::set_static_colors_rt);
	ClassDB::bind_method(
			D_METHOD("set_sky_colors_rt", "skyfog", "sky_base", "sky_bright",
					"sky_highlight", "cloud_base", "cloud_highlight", "cloud_edge"),
			&MissionEnvironment::set_sky_colors_rt);
	ClassDB::bind_method(D_METHOD("set_color_src_gain", "value"),
			&MissionEnvironment::set_color_src_gain);
	ClassDB::bind_method(D_METHOD("get_env_generation"),
			&MissionEnvironment::get_env_generation);

	ClassDB::bind_method(D_METHOD("get_fog_distance"),
			&MissionEnvironment::get_fog_distance);
	ClassDB::bind_method(D_METHOD("get_fog_level"),
			&MissionEnvironment::get_fog_level);
	ClassDB::bind_method(D_METHOD("get_fog_level_target"),
			&MissionEnvironment::get_fog_level_target);
	ClassDB::bind_method(D_METHOD("get_fog_start"),
			&MissionEnvironment::get_fog_start);
	ClassDB::bind_method(D_METHOD("get_fog_end_distance"),
			&MissionEnvironment::get_fog_end_distance);
	ClassDB::bind_method(D_METHOD("get_fog_type"),
			&MissionEnvironment::get_fog_type);
	ClassDB::bind_method(D_METHOD("get_sky_speed"),
			&MissionEnvironment::get_sky_speed);
	ClassDB::bind_method(D_METHOD("get_sky_height"),
			&MissionEnvironment::get_sky_height);
	ClassDB::bind_method(D_METHOD("get_sky_height_target"),
			&MissionEnvironment::get_sky_height_target);
	ClassDB::bind_method(
			D_METHOD("set_smoothed_scalars", "fog_distance", "sky_height",
					"sun_dim_pct", "rain_current", "overcast_blend"),
			&MissionEnvironment::set_smoothed_scalars, DEFVAL(0.0f), DEFVAL(0.0f),
			DEFVAL(0.0f));

	ClassDB::bind_method(D_METHOD("get_sky_map1_tex"),
			&MissionEnvironment::get_sky_map1_tex);
	ClassDB::bind_method(D_METHOD("get_sky_map2_tex"),
			&MissionEnvironment::get_sky_map2_tex);

	ADD_SIGNAL(MethodInfo("env_generation_changed"));
	ADD_SIGNAL(MethodInfo("underwater_overlay_changed"));

	ClassDB::bind_integer_constant(get_class_static(), "", "HOURS_PER_DAY", 24);
	ClassDB::bind_integer_constant(get_class_static(), "", "HHMM_DAY", 2400);
	ClassDB::bind_integer_constant(get_class_static(), "", "MINUTES_PER_HOUR", 60);
	ClassDB::bind_integer_constant(get_class_static(), "", "HHMM_HOUR_SCALE", 100);
	ClassDB::bind_integer_constant(get_class_static(), "",
			"CLOCK_MINUTES_PER_DAY", 1440);
	ClassDB::bind_integer_constant(get_class_static(), "", "FIXED24_ONE_HOUR",
			opennova::env::EnvironmentState::kFixed24OneHour);
	ClassDB::bind_integer_constant(get_class_static(), "", "TOD_DAY_FIXED24",
			opennova::env::EnvironmentState::kTodDayFixed24);
	ClassDB::bind_integer_constant(get_class_static(), "",
			"DEFAULT_MINUTES_PER_DAY", 1440);
	ClassDB::bind_integer_constant(get_class_static(), "", "DEFAULT_START_HOUR",
			12);
}

// --- document wiring --------------------------------------------------------

void MissionEnvironment::set_environment_data(const Ref<EnvFile> &p_value) {
	const Callable changed_cb =
			callable_mp(this, &MissionEnvironment::_on_environment_changed);
	if (environment_data_.is_valid() &&
			environment_data_->is_connected("environment_changed", changed_cb)) {
		environment_data_->disconnect("environment_changed", changed_cb);
	}
	environment_data_ = p_value;
	if (environment_data_.is_valid() &&
			!environment_data_->is_connected("environment_changed", changed_cb)) {
		environment_data_->connect("environment_changed", changed_cb);
	}
	// The engine attach clears every remote network override — a replacement
	// .env resets remote state (the shell setter's _clear_network_ behavior).
	_attach_config();
	if (is_inside_tree()) {
		_ensure_loaded();
		state_.set_loaded(environment_data_.is_valid() &&
				environment_data_->is_loaded());
		if (is_loaded()) {
			set_time_of_day(
					static_cast<float>(environment_data_->get_curtime()));
		} else {
			// The not-loaded update path still refreshes the globals.
			write_shader_globals();
		}
	}
}

void MissionEnvironment::_attach_config() {
	state_.set_config(
			environment_data_.is_valid() ? &environment_data_->native_config()
										 : nullptr,
			environment_data_.is_valid() && environment_data_->is_loaded());
}

void MissionEnvironment::_ensure_loaded() {
	if (environment_data_.is_valid() && !environment_data_->is_loaded()) {
		const Error err = environment_data_->load();
		if (err != OK) {
			UtilityFunctions::push_warning(
					"MissionEnvironment: failed to load .env: ", err);
		}
	}
}

bool MissionEnvironment::is_loaded() const {
	return environment_data_.is_valid() && environment_data_->is_loaded();
}

void MissionEnvironment::_on_environment_changed() {
	state_.set_loaded(environment_data_.is_valid() &&
			environment_data_->is_loaded());
	if (is_loaded()) {
		state_.update_tod();
		_after_tod_update();
	}
}

void MissionEnvironment::_ready() {
	_ensure_loaded();
	_attach_config();
	if (is_loaded()) {
		set_time_of_day(static_cast<float>(environment_data_->get_curtime()));
	}
	if (state_.is_loaded()) {
		state_.update_tod();
		_after_tod_update();
	} else {
		write_shader_globals();
	}
}

// --- device tails -----------------------------------------------------------

void MissionEnvironment::set_time_of_day(double p_value) {
	state_.set_time_of_day(p_value);
	if (state_.is_loaded()) {
		_after_tod_update();
	}
}

void MissionEnvironment::_after_tod_update() {
	flush_publication();
	if (!state_.is_weather_driven()) {
		write_shader_globals();
	}
}

void MissionEnvironment::flush_publication(bool p_pass_changed) {
	if (state_.env_generation() == last_published_generation_) {
		return;
	}
	last_published_generation_ = state_.env_generation();
	const Ref<EnvLightValues> values = _build_light_values();
	light_state_->publish(values, p_pass_changed);
	_write_lighting_block_globals(values);
	emit_signal("env_generation_changed");
}

MissionEnvironment *MissionEnvironment::lighting_block_writer_ = nullptr;

void MissionEnvironment::_write_lighting_block_globals(
		const Ref<EnvLightValues> &p_values) {
	if (p_values.is_null()) {
		return;
	}
	const EnvLightValues &v = **p_values;
	RenderingServer *rs = RenderingServer::get_singleton();
	rs->global_shader_parameter_set("opennova_light_block_dir", v.dir);
	rs->global_shader_parameter_set("opennova_light_block_dir_color",
			v.dir_color);
	rs->global_shader_parameter_set("opennova_light_block_hemi_sky",
			v.hemi_sky);
	rs->global_shader_parameter_set("opennova_light_block_hemi_ground",
			v.hemi_ground);
	rs->global_shader_parameter_set("opennova_light_block_ceiling", v.ceiling);
	rs->global_shader_parameter_set("opennova_light_block_floor",
			v.floor_color);
	rs->global_shader_parameter_set("opennova_light_block_gain", v.gain);
	// The scene fog block itself (color/start/end/type) is the pass state
	// write_shader_globals / the weather tick / the pass switch already
	// publish for every fogged consumer; the object family only needs the
	// enable, which a loaded world always carries.
	rs->global_shader_parameter_set("opennova_fog_enabled", v.fog_enabled);
	lighting_block_writer_ = this;
}

void MissionEnvironment::_release_lighting_block() {
	if (lighting_block_writer_ != this) {
		return;
	}
	// Globals are process-wide: leave the shipped noon defaults behind so a
	// later preview or mission does not inherit this world's block.
	_write_lighting_block_globals(EnvLightValues::retail_noon_defaults());
	lighting_block_writer_ = nullptr;
	// The block the world carries is no longer published anywhere: the next
	// flush must write it again even though the generation did not move.
	last_published_generation_ = -1;
}

void MissionEnvironment::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_EXIT_TREE:
		case NOTIFICATION_PREDELETE:
			_release_lighting_block();
			break;
		case NOTIFICATION_ENTER_TREE:
			// A re-entered environment publishes its live block again (the
			// exit above handed the globals back to noon).
			if (state_.is_loaded() && last_published_generation_ < 0) {
				flush_publication();
			}
			break;
		default:
			break;
	}
}

Ref<EnvLightValues> MissionEnvironment::_build_light_values() const {
	Ref<EnvLightValues> defaults = EnvLightValues::retail_noon_defaults();
	opennova::env::WorldLightValues out;
	// The engine computes in the render-float axes; the published defaults
	// are a Godot-world vector, so they cross the env_axes.h swap on the way
	// in, and the built direction crosses back on the way out (the
	// 2026-08-20 celestial-axis correction: the identity mapping lit every
	// object from a direction 90 degrees off in yaw and mirrored — the
	// 03tr-sun-sky "front-lit where retail backlights" half).
	const opennova::env::Vec3 default_dir = godot_to_render_float(defaults->dir);
	if (!state_.build_light_values(default_dir, out, underwater_view_)) {
		return defaults;
	}
	Ref<EnvLightValues> v;
	v.instantiate();
	v->hemi_sky = to_vector3(out.hemi_sky);
	v->dir = render_float_to_godot(out.dir);
	v->dir_color = to_vector3(out.dir_color);
	v->hemi_ground = to_vector3(out.hemi_ground);
	v->ceiling = to_vector3(out.ceiling);
	v->floor_color = to_vector3(out.floor_color);
	v->gain = to_vector3(out.gain);
	v->fog_enabled = out.fog_enabled;
	v->fog_color = to_vector3(out.fog_color);
	v->fog_start = out.fog_start;
	v->fog_end = out.fog_end;
	v->fog_type = out.fog_type;
	return v;
}

void MissionEnvironment::write_shader_globals() {
	RenderingServer *rs = RenderingServer::get_singleton();
	const opennova::env::EnvShaderGlobals globals =
			state_.build_shader_globals(underwater_view_);
	rs->global_shader_parameter_set("opennova_fill_light",
			to_vector3(globals.fill_light));
	rs->global_shader_parameter_set("opennova_sun_light",
			to_vector3(globals.sun_light));
	rs->global_shader_parameter_set("opennova_sky_ambient",
			to_vector3(globals.sky_ambient));
	rs->global_shader_parameter_set("opennova_sun_direction",
			to_vector3(globals.sun_direction));
	rs->global_shader_parameter_set("opennova_fog_color",
			to_vector3(globals.fog_color));
	rs->global_shader_parameter_set("opennova_fog_end", globals.fog_end);
	rs->global_shader_parameter_set("opennova_fog_start", globals.fog_start);
	rs->global_shader_parameter_set("opennova_fog_type", globals.fog_type);
	rs->global_shader_parameter_set("opennova_wind_sway_amount",
			globals.wind_sway_amount);
	rs->global_shader_parameter_set("opennova_wind_sway_phase",
			globals.wind_sway_phase);
}

void MissionEnvironment::_write_scene_fog_globals() {
	const opennova::env::SceneFogValues fog =
			state_.build_scene_fog(underwater_view_);
	RenderingServer *rs = RenderingServer::get_singleton();
	rs->global_shader_parameter_set("opennova_fog_color",
			to_vector3(fog.color));
	rs->global_shader_parameter_set("opennova_fog_end", fog.end);
	rs->global_shader_parameter_set("opennova_fog_start", fog.start);
	rs->global_shader_parameter_set("opennova_fog_type", fog.type);
}

// --- mission clock ----------------------------------------------------------

void MissionEnvironment::configure_mission_clock(int p_start_time_q8_8,
		int p_minutes_per_day) {
	state_.configure_mission_clock(p_start_time_q8_8, p_minutes_per_day);
	if (state_.is_loaded()) {
		_after_tod_update();
	}
}

double MissionEnvironment::mission_start_time_hhmm(int p_start_time_q8_8) {
	return opennova::env::EnvironmentState::mission_start_time_hhmm(
			p_start_time_q8_8);
}

void MissionEnvironment::advance_mission_clock(int p_ticks) {
	state_.advance_mission_clock(p_ticks);
	if (p_ticks > 0 && state_.is_loaded()) {
		_after_tod_update();
	}
}

Error MissionEnvironment::debug_set_mission_minute_of_day(
		double p_minute_of_day) {
	if (!state_.debug_set_mission_minute_of_day(p_minute_of_day)) {
		return ERR_INVALID_PARAMETER;
	}
	if (state_.is_loaded()) {
		_after_tod_update();
	}
	return OK;
}

double MissionEnvironment::get_mission_minute_of_day() const {
	return state_.mission_minute_of_day();
}

int MissionEnvironment::get_mission_time_fixed24() const {
	return state_.mission_time_fixed24();
}

double MissionEnvironment::minute_of_day_to_hhmm(double p_minute_of_day) {
	return opennova::env::EnvironmentState::minute_of_day_to_hhmm(
			p_minute_of_day);
}

double MissionEnvironment::hhmm_to_minute_of_day(double p_hhmm) {
	return opennova::env::EnvironmentState::hhmm_to_minute_of_day(p_hhmm);
}

// --- network phase-2 --------------------------------------------------------

void MissionEnvironment::apply_network_environment_sample(
		const Dictionary &p_sample) {
	opennova::env::NetEnvSample sample;
	sample.fog_dist = static_cast<int>(p_sample.get("fog_dist", 0));
	sample.cloud_scroll = static_cast<int>(p_sample.get("cloud_scroll", 0));
	sample.quake_ticks = static_cast<int>(p_sample.get("quake_ticks", 0));
	sample.tod_fixed = static_cast<int>(p_sample.get("tod_fixed", 0));
	sample.precipitation_kind =
			static_cast<int>(p_sample.get("precipitation_kind", 0));
	state_.apply_network_sample(sample);
	if (state_.is_loaded()) {
		_after_tod_update();
	}
}

int MissionEnvironment::get_network_quake_ticks() const {
	return state_.network_quake_ticks();
}

float MissionEnvironment::get_network_rain_current() const {
	return state_.network_rain_current();
}

float MissionEnvironment::get_overcast_blend() const {
	return state_.overcast_blend();
}

// --- weather split / NVG ----------------------------------------------------

void MissionEnvironment::set_weather_driven(bool p_driven) {
	state_.set_weather_driven(p_driven);
}

bool MissionEnvironment::is_weather_driven() const {
	return state_.is_weather_driven();
}

void MissionEnvironment::set_nvg_view(bool p_active, int p_gain) {
	if (!state_.set_nvg_view(p_active, p_gain)) {
		return;
	}
	flush_publication();
	// The weather owns the full per-frame global write while present. Refresh
	// only the two affected channels immediately, and let its next tick
	// publish the same getter-derived values again without disturbing
	// wind/fog state.
	RenderingServer *rs = RenderingServer::get_singleton();
	rs->global_shader_parameter_set("opennova_fill_light",
			to_vector3(state_.fill_light()));
	rs->global_shader_parameter_set("opennova_sky_ambient",
			to_vector3(state_.sky_ambient()));
}

void MissionEnvironment::set_underwater_view(bool p_underwater) {
	if (underwater_view_ == p_underwater) {
		return;
	}
	underwater_view_ = p_underwater;
	// The authored/weather state did not change, but every retained material
	// and immutable particle snapshot must observe this pass transition.
	last_published_generation_ = -1;
	flush_publication(true);
	// Foliage submits before Weather's frame tail, so commit the selected fog
	// immediately. Weather is pass-aware too and preserves it on later writes.
	_write_scene_fog_globals();
}

void MissionEnvironment::apply_render_eye(float p_eye_y,
		float p_water_height, bool p_water_active) {
	// One sampled eye decides both flags; the strict-vs-inclusive comparison
	// semantics live in the engine (see env::EnvironmentState::
	// classify_render_eye — the fog selector is strict, the murk scissor
	// includes exact waterline equality).
	const opennova::env::EnvironmentState::RenderEyeClassification eye =
			opennova::env::EnvironmentState::classify_render_eye(
					p_eye_y, p_water_height, p_water_active);
	set_underwater_view(eye.underwater_view);
	set_underwater_overlay_view(eye.underwater_overlay_view);
}

void MissionEnvironment::set_underwater_overlay_view(bool p_underwater) {
	if (underwater_overlay_view_ == p_underwater) {
		return;
	}
	underwater_overlay_view_ = p_underwater;
	emit_signal("underwater_overlay_changed");
}

Vector3 MissionEnvironment::get_underwater_overlay_color() const {
	// The overlay always uses Env_WaterColorLit, including exact waterline
	// equality where the independently selected device-fog pass remains dry.
	return to_vector3(state_.build_scene_fog(true).color);
}

int MissionEnvironment::get_underwater_overlay_alpha_byte() const {
	return static_cast<int>(opennova::env::underwater_murk_overlay_alpha_byte(
			state_.water_murk()));
}

Vector3 MissionEnvironment::get_scene_fog_color() const {
	return to_vector3(state_.build_scene_fog(underwater_view_).color);
}

float MissionEnvironment::get_scene_fog_start() const {
	return state_.build_scene_fog(underwater_view_).start;
}

float MissionEnvironment::get_scene_fog_end() const {
	return state_.build_scene_fog(underwater_view_).end;
}

int MissionEnvironment::get_scene_fog_type() const {
	return state_.build_scene_fog(underwater_view_).type;
}

// --- reads ------------------------------------------------------------------

Vector3 MissionEnvironment::get_sun_light() const {
	return to_vector3(state_.sun_light());
}

Vector3 MissionEnvironment::get_fill_light() const {
	return to_vector3(state_.fill_light());
}

Vector3 MissionEnvironment::get_sky_ambient() const {
	return to_vector3(state_.sky_ambient());
}

Vector3 MissionEnvironment::get_fog_color() const {
	return to_vector3(state_.fog_color());
}

Vector3 MissionEnvironment::get_skyfog_color() const {
	return to_vector3(state_.skyfog_color());
}

Vector3 MissionEnvironment::get_frame_clear_color() const {
	return to_vector3(state_.frame_clear_color());
}

Color MissionEnvironment::frame_clear_color_for(bool p_indoors,
		bool p_above_water) const {
	const opennova::env::Rgb rgb =
			state_.frame_clear_color_for(p_indoors, p_above_water);
	return Color(rgb.r, rgb.g, rgb.b);
}

Vector3 MissionEnvironment::get_ceiling_color() const {
	return to_vector3(state_.ceiling_color());
}

Vector3 MissionEnvironment::get_cloud_tint() const {
	return to_vector3(state_.cloud_tint());
}

Vector3 MissionEnvironment::get_floor_color() const {
	return to_vector3(state_.floor_color());
}

Vector3 MissionEnvironment::get_sky_base() const {
	return to_vector3(state_.sky_base());
}

Vector3 MissionEnvironment::get_sky_bright() const {
	return to_vector3(state_.sky_bright());
}

Vector3 MissionEnvironment::get_sky_highlight() const {
	return to_vector3(state_.sky_highlight());
}

Vector3 MissionEnvironment::get_cloud_base() const {
	return to_vector3(state_.cloud_base());
}

Vector3 MissionEnvironment::get_cloud_highlight() const {
	return to_vector3(state_.cloud_highlight());
}

Vector3 MissionEnvironment::get_cloud_edge() const {
	return to_vector3(state_.cloud_edge());
}

Vector3 MissionEnvironment::get_color_src_gain() const {
	return to_vector3(state_.color_src_gain());
}

Vector3 MissionEnvironment::get_fill_light_target() const {
	return to_vector3(state_.fill_light_target());
}

Vector3 MissionEnvironment::get_sun_light_target() const {
	return to_vector3(state_.sun_light_target());
}

Vector3 MissionEnvironment::get_fog_color_target() const {
	return to_vector3(state_.fog_color_target());
}

Vector3 MissionEnvironment::get_sky_ambient_target() const {
	return to_vector3(state_.sky_ambient_target());
}

Vector3 MissionEnvironment::get_ceiling_color_target() const {
	return to_vector3(state_.ceiling_color_target());
}

Vector3 MissionEnvironment::get_cloud_tint_target() const {
	return to_vector3(state_.cloud_tint_target());
}

Vector3 MissionEnvironment::get_floor_color_target() const {
	return to_vector3(state_.floor_color_target());
}

Vector3 MissionEnvironment::get_lightning_color_target() const {
	return to_vector3(state_.lightning_color_target());
}

Vector3 MissionEnvironment::get_terrain_tint() const {
	return to_vector3(state_.terrain_tint());
}

Vector3 MissionEnvironment::get_terrain_lighting_attenuation() const {
	return to_vector3(state_.terrain_lighting_attenuation());
}

Vector3 MissionEnvironment::get_tile_overlay_tint() const {
	return to_vector3(state_.tile_overlay_tint());
}

void MissionEnvironment::apply_terrain_uniforms(
		const Ref<ShaderMaterial> &p_material) {
	if (p_material.is_null()) {
		return;
	}
	const opennova::env::TerrainEnvUniforms uniforms =
			state_.build_terrain_uniforms(underwater_view_);
	Ref<ShaderMaterial> material = p_material;
	material->set_shader_parameter("u_sun_light",
			to_vector3(uniforms.sun_light));
	material->set_shader_parameter("u_sky_ambient",
			to_vector3(uniforms.sky_ambient));
	material->set_shader_parameter("u_sun_direction",
			to_vector3(uniforms.sun_direction));
	material->set_shader_parameter("u_tile_overlay_tint",
			to_vector3(uniforms.tile_overlay_tint));
	material->set_shader_parameter("u_fog_color",
			to_vector3(uniforms.fog_color));
	material->set_shader_parameter("u_fog_end", uniforms.fog_end);
	material->set_shader_parameter("u_fog_start", uniforms.fog_start);
	material->set_shader_parameter("u_fog_type", uniforms.fog_type);
}

Vector3 MissionEnvironment::get_water_color() const {
	return to_vector3(state_.water_color());
}

bool MissionEnvironment::has_water_height() const {
	return state_.has_water_height();
}

float MissionEnvironment::get_water_height() const {
	return state_.water_height();
}

// The engine state serves the render-float tuple; these Godot-facing getters
// serve the GODOT-world direction through the env_axes.h swap (2026-08-20).
// Raw-tuple consumers (the opennova_sun_direction global and the terrain
// u_sun_direction uniform, whose shaders re-swizzle into the engine texture
// basis; the star-field cull) read state_ directly and never route here.
Vector3 MissionEnvironment::get_sun_direction() const {
	return render_float_to_godot(state_.sun_direction());
}

Vector3 MissionEnvironment::get_moon_direction() const {
	return render_float_to_godot(state_.moon_direction());
}

Vector3 MissionEnvironment::get_light_direction() const {
	return render_float_to_godot(state_.light_direction());
}

Vector3 MissionEnvironment::get_light_direction_render_tuple() const {
	return to_vector3(state_.light_direction());
}

bool MissionEnvironment::is_night_phase() const {
	return state_.is_night_phase();
}

float MissionEnvironment::get_day_phase_blend() const {
	return state_.day_phase_blend();
}

Vector3 MissionEnvironment::get_sun_color() const {
	return to_vector3(state_.sun_color());
}

Vector3 MissionEnvironment::get_moon_color() const {
	return to_vector3(state_.moon_color());
}

// --- weather writeback ------------------------------------------------------

void MissionEnvironment::set_fill_light(const Vector3 &p_value) {
	state_.set_fill_light(to_rgb(p_value));
	flush_publication();
}

void MissionEnvironment::set_sun_light(const Vector3 &p_value) {
	state_.set_sun_light(to_rgb(p_value));
	flush_publication();
}

void MissionEnvironment::set_fog_color_rt(const Vector3 &p_value) {
	state_.set_fog_color_rt(to_rgb(p_value));
	flush_publication();
}

void MissionEnvironment::set_sky_ambient_rt(const Vector3 &p_value) {
	state_.set_sky_ambient_rt(to_rgb(p_value));
	flush_publication();
}

void MissionEnvironment::set_static_colors_rt(const Vector3 &p_ceiling,
		const Vector3 &p_cloud, const Vector3 &p_floor_color) {
	state_.set_static_colors_rt(to_rgb(p_ceiling), to_rgb(p_cloud),
			to_rgb(p_floor_color));
	flush_publication();
}

void MissionEnvironment::set_sky_colors_rt(const Vector3 &p_skyfog,
		const Vector3 &p_sky_base, const Vector3 &p_sky_bright,
		const Vector3 &p_sky_highlight, const Vector3 &p_cloud_base,
		const Vector3 &p_cloud_highlight, const Vector3 &p_cloud_edge) {
	state_.set_sky_colors_rt(to_rgb(p_skyfog), to_rgb(p_sky_base),
			to_rgb(p_sky_bright), to_rgb(p_sky_highlight), to_rgb(p_cloud_base),
			to_rgb(p_cloud_highlight), to_rgb(p_cloud_edge));
	flush_publication();
}

void MissionEnvironment::set_color_src_gain(const Vector3 &p_value) {
	state_.set_color_src_gain(to_rgb(p_value));
	flush_publication();
}

int64_t MissionEnvironment::get_env_generation() const {
	return state_.env_generation();
}

// --- env #27 scalars --------------------------------------------------------

float MissionEnvironment::get_fog_distance() const {
	return state_.fog_distance();
}

float MissionEnvironment::get_fog_level() const {
	return state_.fog_level();
}

float MissionEnvironment::get_fog_level_target() const {
	return state_.fog_level_target();
}

float MissionEnvironment::get_fog_start() const {
	return state_.fog_start();
}

float MissionEnvironment::get_fog_end_distance() const {
	return state_.fog_end_distance();
}

int MissionEnvironment::get_fog_type() const {
	return state_.fog_type();
}

float MissionEnvironment::get_sky_speed() const {
	return state_.sky_speed();
}

float MissionEnvironment::get_sky_height() const {
	return state_.sky_height();
}

float MissionEnvironment::get_sky_height_target() const {
	return state_.sky_height_target();
}

void MissionEnvironment::set_smoothed_scalars(float p_fog_distance,
		float p_sky_height, float p_sun_dim_pct, float p_rain_current,
		float p_overcast_blend) {
	state_.set_smoothed_scalars(p_fog_distance, p_sky_height, p_sun_dim_pct,
			p_rain_current, p_overcast_blend);
	flush_publication();
}

Ref<Texture2D> MissionEnvironment::get_sky_map1_tex() const {
	return environment_data_.is_valid() ? environment_data_->get_sky_map1_tex()
										: Ref<Texture2D>();
}

Ref<Texture2D> MissionEnvironment::get_sky_map2_tex() const {
	return environment_data_.is_valid() ? environment_data_->get_sky_map2_tex()
										: Ref<Texture2D>();
}

} // namespace godot
