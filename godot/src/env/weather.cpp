#include "env/weather.h"

#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/core/object.hpp>

#include "env/mission_environment.h"
#include "simulation/simulation.h"

namespace godot {

namespace {

Vector3 to_vector3(const opennova::env::Rgb &rgb) {
	return Vector3(rgb.r, rgb.g, rgb.b);
}

} // namespace

void Weather::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_environment_path", "path"),
			&Weather::set_environment_path);
	ClassDB::bind_method(D_METHOD("get_environment_path"),
			&Weather::get_environment_path);
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "environment_path"),
			"set_environment_path", "get_environment_path");
	ClassDB::bind_method(D_METHOD("set_wind_strength", "value"),
			&Weather::set_wind_strength);
	ClassDB::bind_method(D_METHOD("get_wind_strength"),
			&Weather::get_wind_strength);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wind_strength",
						 PROPERTY_HINT_RANGE, "0,100,1"),
			"set_wind_strength", "get_wind_strength");
	ClassDB::bind_method(D_METHOD("set_iris_samples", "samples"),
			&Weather::set_iris_samples);
	ClassDB::bind_method(D_METHOD("get_iris_samples"),
			&Weather::get_iris_samples);
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "iris_samples"),
			"set_iris_samples", "get_iris_samples");

	ClassDB::bind_method(D_METHOD("set_world_tick_driven", "enabled"),
			&Weather::set_world_tick_driven);
	ClassDB::bind_method(D_METHOD("prepare_world_driven"),
			&Weather::prepare_world_driven);
	ClassDB::bind_method(D_METHOD("prepare_autonomous"),
			&Weather::prepare_autonomous);
	ClassDB::bind_method(D_METHOD("prewarm_mission_start"),
			&Weather::prewarm_mission_start);
	ClassDB::bind_method(D_METHOD("tick_fixed"), &Weather::tick_fixed);
	ClassDB::bind_method(D_METHOD("resync_colors"), &Weather::resync_colors);
	ClassDB::bind_method(D_METHOD("resync_colors_now"),
			&Weather::resync_colors_now);
	ClassDB::bind_method(D_METHOD("settle_exposure"),
			&Weather::settle_exposure);
	ClassDB::bind_method(D_METHOD("set_sun_veil_stopdown", "stopdown"),
			&Weather::set_sun_veil_stopdown);

	ClassDB::bind_method(D_METHOD("get_network_environment_snapshot"),
			&Weather::get_network_environment_snapshot);
	ClassDB::bind_method(D_METHOD("advance_world_driven", "delta", "sim"),
			&Weather::advance_world_driven);
	ClassDB::bind_method(D_METHOD("push_network_environment", "sim"),
			&Weather::push_network_environment);
	ClassDB::bind_method(D_METHOD("run_mission_start_boundary", "sim"),
			&Weather::run_mission_start_boundary);
	ClassDB::bind_method(D_METHOD("apply_join_network_update", "sim"),
			&Weather::apply_join_network_update);
	ClassDB::bind_method(D_METHOD("apply_network_environment_sample", "sample"),
			&Weather::apply_network_environment_sample);

	ClassDB::bind_method(D_METHOD("trigger_lightning_short"),
			&Weather::trigger_lightning_short);
	ClassDB::bind_method(D_METHOD("trigger_lightning_long"),
			&Weather::trigger_lightning_long);
	ClassDB::bind_method(D_METHOD("set_wind_duration", "seconds"),
			&Weather::set_wind_duration);
	ClassDB::bind_method(D_METHOD("get_wind_duration"),
			&Weather::get_wind_duration);

	ClassDB::bind_method(D_METHOD("get_sway_amount"), &Weather::get_sway_amount);
	ClassDB::bind_method(D_METHOD("get_sway_phase"), &Weather::get_sway_phase);
	ClassDB::bind_method(D_METHOD("get_lightning_intensity"),
			&Weather::get_lightning_intensity);

	ClassDB::bind_method(D_METHOD("get_smooth_fill"), &Weather::get_smooth_fill);
	ClassDB::bind_method(D_METHOD("get_smooth_sun"), &Weather::get_smooth_sun);
	ClassDB::bind_method(D_METHOD("get_smooth_fog"), &Weather::get_smooth_fog);
	ClassDB::bind_method(D_METHOD("get_smooth_sky"), &Weather::get_smooth_sky);
	ClassDB::bind_method(D_METHOD("get_smooth_skyfog"),
			&Weather::get_smooth_skyfog);
	ClassDB::bind_method(D_METHOD("get_smooth_ceiling"),
			&Weather::get_smooth_ceiling);
	ClassDB::bind_method(D_METHOD("get_smooth_cloud"),
			&Weather::get_smooth_cloud);
	ClassDB::bind_method(D_METHOD("get_smooth_floor"),
			&Weather::get_smooth_floor);
	ClassDB::bind_method(D_METHOD("get_smooth_sky_base"),
			&Weather::get_smooth_sky_base);
	ClassDB::bind_method(D_METHOD("get_smooth_sky_bright"),
			&Weather::get_smooth_sky_bright);
	ClassDB::bind_method(D_METHOD("get_smooth_sky_highlight"),
			&Weather::get_smooth_sky_highlight);
	ClassDB::bind_method(D_METHOD("get_smooth_cloud_base"),
			&Weather::get_smooth_cloud_base);
	ClassDB::bind_method(D_METHOD("get_smooth_cloud_highlight"),
			&Weather::get_smooth_cloud_highlight);
	ClassDB::bind_method(D_METHOD("get_smooth_cloud_edge"),
			&Weather::get_smooth_cloud_edge);

	ClassDB::bind_method(D_METHOD("get_cloud_uv_offset1", "cam_x", "cam_z"),
			&Weather::get_cloud_uv_offset1);
	ClassDB::bind_method(D_METHOD("get_cloud_uv_offset2", "cam_x", "cam_z"),
			&Weather::get_cloud_uv_offset2);
	ClassDB::bind_method(D_METHOD("get_cloud_uv_rate_per_second"),
			&Weather::get_cloud_uv_rate_per_second);
	ClassDB::bind_method(
			D_METHOD("get_water_uv_state", "cam_x", "cam_z", "fog_distance"),
			&Weather::get_water_uv_state);

	// The externally-callable render-frame drive (the _process body): a
	// GDExtension virtual override is invisible to has_method and cannot be
	// called from GDScript, and binding the `_process` name would displace
	// the engine's virtual hook — so the test harness drives frames here.
	ClassDB::bind_method(D_METHOD("advance_frame", "delta"),
			&Weather::advance_frame);

	ClassDB::bind_integer_constant(get_class_static(), "", "WEATHER_TICK_HZ",
			62);
	ClassDB::bind_integer_constant(get_class_static(), "", "MAX_CATCHUP_TICKS",
			opennova::env::WeatherRuntime::kMaxCatchupTicks);
	ClassDB::bind_integer_constant(get_class_static(), "",
			"MISSION_START_PREWARM_TICKS",
			opennova::env::WeatherRuntime::kMissionStartPrewarmTicks);
}

void Weather::set_environment_path(const NodePath &p_path) {
	environment_path_ = p_path;
	_resolve_environment();
}

void Weather::_resolve_environment() {
	// Relative paths resolve off-tree too (sibling wiring under a detached
	// mount, the harness pattern); absolute paths need the tree and re-resolve
	// in _ready.
	MissionEnvironment *env = nullptr;
	if (!environment_path_.is_empty() &&
			(is_inside_tree() || !environment_path_.is_absolute())) {
		env = Object::cast_to<MissionEnvironment>(
				get_node_or_null(environment_path_));
	}
	env_node_id_ = env != nullptr ? ObjectID(env->get_instance_id())
								  : ObjectID();
}

MissionEnvironment *Weather::_env_node() const {
	if (env_node_id_.is_valid()) {
		return Object::cast_to<MissionEnvironment>(
				ObjectDB::get_instance(env_node_id_));
	}
	return nullptr;
}

void Weather::_post_runtime(MissionEnvironment *p_env) {
	if (p_env == nullptr || !p_env->state().is_loaded()) {
		return;
	}
	p_env->flush_publication();
	const opennova::env::WeatherShaderGlobals globals =
			opennova::env::build_weather_shader_globals(p_env->state(),
					runtime_, p_env->is_underwater_view());
	RenderingServer *rs = RenderingServer::get_singleton();
	rs->global_shader_parameter_set("opennova_sun_light",
			to_vector3(globals.base.sun_light));
	rs->global_shader_parameter_set("opennova_sky_ambient",
			to_vector3(globals.base.sky_ambient));
	rs->global_shader_parameter_set("opennova_fog_color",
			to_vector3(globals.base.fog_color));
	rs->global_shader_parameter_set("opennova_sun_direction",
			Vector3(globals.base.sun_direction.x, globals.base.sun_direction.y,
					globals.base.sun_direction.z));
	rs->global_shader_parameter_set("opennova_fog_end", globals.base.fog_end);
	rs->global_shader_parameter_set("opennova_fog_start",
			globals.base.fog_start);
	rs->global_shader_parameter_set("opennova_fog_type", globals.base.fog_type);
}

void Weather::_ready() {
	set_process_priority(-10);
	set_process(true);
	_resolve_environment();
}

void Weather::_process(double p_delta) {
	advance_frame(p_delta);
}

void Weather::advance_frame(double p_delta) {
	MissionEnvironment *env = _env_node();
	runtime_.process_delta(env != nullptr ? &env->state() : nullptr, p_delta);
	_post_runtime(env);
}

void Weather::set_wind_strength(float p_value) {
	runtime_.set_wind_strength_pct(p_value);
}

float Weather::get_wind_strength() const {
	return runtime_.wind_strength_pct();
}

void Weather::set_iris_samples(const PackedInt32Array &p_samples) {
	iris_samples_ = p_samples;
	runtime_.set_iris_samples(p_samples.ptr(),
			static_cast<int>(p_samples.size()));
}

PackedInt32Array Weather::get_iris_samples() const {
	return iris_samples_;
}

void Weather::set_world_tick_driven(bool p_enabled) {
	runtime_.set_world_tick_driven(p_enabled);
}

void Weather::prepare_world_driven() {
	MissionEnvironment *env = _env_node();
	runtime_.prepare_world_driven(env != nullptr ? &env->state() : nullptr);
	iris_samples_ = PackedInt32Array();
	_post_runtime(env);
}

void Weather::prepare_autonomous() {
	MissionEnvironment *env = _env_node();
	runtime_.prepare_autonomous(env != nullptr ? &env->state() : nullptr);
	iris_samples_ = PackedInt32Array();
	_post_runtime(env);
}

void Weather::prewarm_mission_start() {
	MissionEnvironment *env = _env_node();
	if (env == nullptr) {
		return;
	}
	runtime_.prewarm_mission_start(&env->state());
	_post_runtime(env);
}

void Weather::tick_fixed() {
	MissionEnvironment *env = _env_node();
	runtime_.tick_fixed(env != nullptr ? &env->state() : nullptr);
	_post_runtime(env);
}

void Weather::resync_colors() {
	runtime_.resync_colors();
}

void Weather::resync_colors_now() {
	MissionEnvironment *env = _env_node();
	runtime_.resync_colors_now(env != nullptr ? &env->state() : nullptr);
	_post_runtime(env);
}

void Weather::set_sun_veil_stopdown(int p_stopdown) {
	runtime_.set_sun_veil_stopdown(p_stopdown);
}

void Weather::settle_exposure() {
	MissionEnvironment *env = _env_node();
	runtime_.settle_exposure(env != nullptr ? &env->state() : nullptr);
	_post_runtime(env);
}

Dictionary Weather::get_network_environment_snapshot() {
	Dictionary result;
	MissionEnvironment *env = _env_node();
	opennova::env::NetEnvSnapshot snapshot;
	if (env == nullptr ||
			!runtime_.network_snapshot(&env->state(), snapshot)) {
		return result;
	}
	result["fog_target_q16"] = snapshot.fog_target_q16;
	result["fog_current_q16"] = snapshot.fog_current_q16;
	result["fog_accel_clamp"] = snapshot.fog_accel_clamp;
	result["tod_fixed24"] = snapshot.tod_fixed24;
	result["tod_advance_per_tick"] = snapshot.tod_advance_per_tick;
	result["quake_ticks"] = snapshot.quake_ticks;
	result["cloud_scroll_rate_target"] = snapshot.cloud_scroll_rate_target;
	result["rain_pct_current_q16"] = snapshot.rain_pct_current_q16;
	result["overcast_blend_q16"] = snapshot.overcast_blend_q16;
	result["precipitation_kind"] = snapshot.precipitation_kind;
	return result;
}

void Weather::advance_world_driven(double p_delta, Object *p_sim) {
	MissionEnvironment *env = _env_node();
	if (env == nullptr) {
		return;
	}
	const int tick_count = runtime_.consume_world_tick_credits(p_delta);
	if (tick_count <= 0) {
		return;
	}
	Simulation *sim = Object::cast_to<Simulation>(p_sim);
	const bool authority = sim != nullptr && !sim->is_joiner();
	for (int i = 0; i < tick_count; ++i) {
		env->advance_mission_clock(1);
		tick_fixed();
		// The per-tick advance of the world's network sample rides the engine
		// tick (listen_host::frame / MissionKernel::tick_no_net); the device
		// side only pushes its settled sample.
		if (authority) push_network_environment(sim);
	}
}

void Weather::push_network_environment(Object *p_sim) {
	Simulation *sim = Object::cast_to<Simulation>(p_sim);
	if (sim == nullptr || sim->is_joiner()) {
		return;
	}
	MissionEnvironment *env = _env_node();
	opennova::env::NetEnvSnapshot snapshot;
	if (env == nullptr ||
			!runtime_.network_snapshot(&env->state(), snapshot)) {
		return;
	}
	sim->set_network_environment(snapshot.fog_target_q16,
			snapshot.fog_current_q16, snapshot.fog_accel_clamp,
			snapshot.tod_fixed24, snapshot.tod_advance_per_tick,
			snapshot.quake_ticks, snapshot.cloud_scroll_rate_target,
			snapshot.rain_pct_current_q16, snapshot.overcast_blend_q16,
			snapshot.precipitation_kind);
}

void Weather::run_mission_start_boundary(Object *p_sim) {
	Simulation *sim = Object::cast_to<Simulation>(p_sim);
	const bool authority = sim != nullptr && !sim->is_joiner();
	// The first publication seeds native retail units at the authored T0. It
	// is local state only; no host pump or phase-2 packet runs inside this
	// boundary.
	if (authority) {
		push_network_environment(sim);
		sim->run_mission_start_wac();
		sim->initialize_network_environment_mission_start();
	}
	// WAC direct execution precedes the complete-weather-update settle.
	// Joiners settle their local render owner but never run authority WAC.
	prewarm_mission_start();
	if (authority) {
		for (int i = 0;
				i < opennova::env::WeatherRuntime::kMissionStartPrewarmTicks;
				++i) {
			sim->advance_network_environment_tick();
		}
		// Non-scripted values adopt the settled resource sample; WAC-owned
		// channels survive through the ownership masks.
		push_network_environment(sim);
		sim->seal_mission_start_baseline();
	}
}

void Weather::apply_join_network_update(Object *p_sim) {
	Simulation *sim = Object::cast_to<Simulation>(p_sim);
	if (sim == nullptr || !sim->is_joiner()) {
		return;
	}
	Dictionary sample = sim->take_join_environment_update();
	if (sample.is_empty()) {
		return;
	}
	apply_network_environment_sample(sample);
}

void Weather::apply_network_environment_sample(const Dictionary &p_sample) {
	MissionEnvironment *env = _env_node();
	if (env == nullptr) {
		return;
	}
	opennova::env::NetEnvSample sample;
	sample.fog_dist = static_cast<int>(p_sample.get("fog_dist", 0));
	sample.fog_accel = static_cast<int>(p_sample.get("fog_accel", 0));
	sample.rain_pct = static_cast<int>(p_sample.get("rain_pct", 0));
	sample.overcast = static_cast<int>(p_sample.get("overcast", 0));
	sample.cloud_scroll = static_cast<int>(p_sample.get("cloud_scroll", 0));
	sample.quake_ticks = static_cast<int>(p_sample.get("quake_ticks", 0));
	sample.tod_fixed = static_cast<int>(p_sample.get("tod_fixed", 0));
	sample.precipitation_kind =
			static_cast<int>(p_sample.get("precipitation_kind", 0));
	runtime_.apply_network_sample(&env->state(), sample);
	_post_runtime(env);
}

void Weather::trigger_lightning_short() {
	runtime_.trigger_lightning_short();
}

void Weather::trigger_lightning_long() {
	runtime_.trigger_lightning_long();
}

void Weather::set_wind_duration(int p_seconds) {
	runtime_.set_wind_duration_seconds(p_seconds);
}

int Weather::get_wind_duration() const {
	return runtime_.wind_duration_seconds();
}

float Weather::get_sway_amount() const {
	return runtime_.sway_amount();
}

float Weather::get_sway_phase() const {
	return runtime_.sway_phase();
}

float Weather::get_lightning_intensity() const {
	return runtime_.lightning_intensity();
}

Vector3 Weather::get_smooth_fill() const {
	return to_vector3(runtime_.smooth_fill());
}

Vector3 Weather::get_smooth_sun() const {
	return to_vector3(runtime_.smooth_sun());
}

Vector3 Weather::get_smooth_fog() const {
	return to_vector3(runtime_.smooth_fog());
}

Vector3 Weather::get_smooth_sky() const {
	return to_vector3(runtime_.smooth_sky());
}

Vector3 Weather::get_smooth_skyfog() const {
	return to_vector3(runtime_.smooth_skyfog());
}

Vector3 Weather::get_smooth_ceiling() const {
	return to_vector3(runtime_.smooth_ceiling());
}

Vector3 Weather::get_smooth_cloud() const {
	return to_vector3(runtime_.smooth_cloud());
}

Vector3 Weather::get_smooth_floor() const {
	return to_vector3(runtime_.smooth_floor());
}

Vector3 Weather::get_smooth_sky_base() const {
	return to_vector3(runtime_.smooth_sky_base());
}

Vector3 Weather::get_smooth_sky_bright() const {
	return to_vector3(runtime_.smooth_sky_bright());
}

Vector3 Weather::get_smooth_sky_highlight() const {
	return to_vector3(runtime_.smooth_sky_highlight());
}

Vector3 Weather::get_smooth_cloud_base() const {
	return to_vector3(runtime_.smooth_cloud_base());
}

Vector3 Weather::get_smooth_cloud_highlight() const {
	return to_vector3(runtime_.smooth_cloud_highlight());
}

Vector3 Weather::get_smooth_cloud_edge() const {
	return to_vector3(runtime_.smooth_cloud_edge());
}

Vector2 Weather::get_cloud_uv_offset1(float p_cam_x, float p_cam_z) const {
	const opennova::env::CloudUvOffsets offsets =
			runtime_.cloud_uv_offsets(p_cam_x, p_cam_z);
	return Vector2(offsets.u1, offsets.v1);
}

Vector2 Weather::get_cloud_uv_offset2(float p_cam_x, float p_cam_z) const {
	const opennova::env::CloudUvOffsets offsets =
			runtime_.cloud_uv_offsets(p_cam_x, p_cam_z);
	return Vector2(offsets.u2, offsets.v2);
}

float Weather::get_cloud_uv_rate_per_second() const {
	return runtime_.cloud_uv_rate_per_second();
}

Vector4 Weather::get_water_uv_state(float p_cam_x, float p_cam_z,
		float p_fog_distance) const {
	const opennova::env::WaterUvState state =
			runtime_.water_uv_state(p_cam_x, p_cam_z, p_fog_distance);
	return Vector4(state.scale, state.bias, state.offset_u, state.offset_v);
}

} // namespace godot
