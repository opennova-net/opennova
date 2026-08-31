#include "env/weather.h"

#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/core/object.hpp>

#include "env/env_file.h"
#include "env/mission_environment.h"
#include "object/object_data.h"
#include "simulation/simulation.h"

#include <formats/mission/bms.h>
#include <runtime/environment/weather_seed.h>

namespace godot {

namespace {

Vector3 to_vector3(const opennova::env::Rgb &rgb) {
	return Vector3(rgb.r, rgb.g, rgb.b);
}

// The color seam (ADR 0043 linear-scene amendment): witnessed gamma bytes
// convert to the linear scene domain once, at the global write.
Vector3 to_linear(const Vector3 &srgb) {
	const Color c = Color(srgb.x, srgb.y, srgb.z).srgb_to_linear();
	return Vector3(c.r, c.g, c.b);
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

	ClassDB::bind_method(D_METHOD("bind_simulation", "sim"), &Weather::bind_simulation);
	ClassDB::bind_method(D_METHOD("run_mission_start_boundary", "sim",
			"start_time_q8_8", "minutes_per_day"),
			&Weather::run_mission_start_boundary);
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

	ClassDB::bind_method(D_METHOD("get_weather_snapshot"),
			&Weather::get_weather_snapshot);
	ClassDB::bind_method(D_METHOD("apply_wire_sample", "sample"),
			&Weather::apply_wire_sample);

	ClassDB::bind_method(D_METHOD("trigger_lightning_short"),
			&Weather::trigger_lightning_short);
	ClassDB::bind_method(D_METHOD("trigger_lightning_long"),
			&Weather::trigger_lightning_long);
	ClassDB::bind_method(D_METHOD("command_fog_distance", "metres"), &Weather::command_fog_distance);
	ClassDB::bind_method(D_METHOD("command_move_fog", "metres", "seconds"), &Weather::command_move_fog);
	ClassDB::bind_method(D_METHOD("command_rain", "percent", "seconds"), &Weather::command_rain);
	ClassDB::bind_method(D_METHOD("command_snow", "percent", "seconds"), &Weather::command_snow);
	ClassDB::bind_method(D_METHOD("command_overcast", "percent", "seconds"), &Weather::command_overcast);
	ClassDB::bind_method(D_METHOD("command_sky_speed", "rate"), &Weather::command_sky_speed);
	ClassDB::bind_method(D_METHOD("command_quake", "seconds"), &Weather::command_quake);
	ClassDB::bind_method(D_METHOD("command_time_of_day_minutes", "minute_of_day"),
			&Weather::command_time_of_day_minutes);
	ClassDB::bind_method(D_METHOD("command_fog_type", "type"), &Weather::command_fog_type);
	ClassDB::bind_method(D_METHOD("set_wind_duration", "seconds"),
			&Weather::set_wind_duration);
	ClassDB::bind_method(D_METHOD("get_wind_duration"),
			&Weather::get_wind_duration);

	ClassDB::bind_method(D_METHOD("get_sway_amount"), &Weather::get_sway_amount);
	ClassDB::bind_method(D_METHOD("get_sway_phase"), &Weather::get_sway_phase);
	ClassDB::bind_method(D_METHOD("get_lightning_intensity"),
			&Weather::get_lightning_intensity);
	ClassDB::bind_method(D_METHOD("get_terrain_light_combined_rgb"),
			&Weather::get_terrain_light_combined_rgb);

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
	if (!_bound_sim()) {
		// No env resolves: fall back to the runtime's own idle home so the
		// runtime never keeps pointing into a freed MissionEnvironment.
		runtime_.attach_state(nullptr, env != nullptr ? &env->state() : nullptr);
	}
}

MissionEnvironment *Weather::_env_node() const {
	if (env_node_id_.is_valid()) {
		return Object::cast_to<MissionEnvironment>(
				ObjectDB::get_instance(env_node_id_));
	}
	return nullptr;
}

Simulation *Weather::_bound_sim() const {
	if (sim_id_.is_valid()) {
		return Object::cast_to<Simulation>(ObjectDB::get_instance(sim_id_));
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
	rs->global_shader_parameter_set("opennova_sky_ambient",
			to_linear(to_vector3(globals.base.sky_ambient)));
	rs->global_shader_parameter_set("opennova_fog_color",
			to_linear(to_vector3(globals.base.fog_color)));
	rs->global_shader_parameter_set("opennova_sun_direction",
			Vector3(globals.base.sun_direction.x, globals.base.sun_direction.y,
					globals.base.sun_direction.z));
	rs->global_shader_parameter_set("opennova_fog_end", globals.base.fog_end);
	rs->global_shader_parameter_set("opennova_fog_start",
			globals.base.fog_start);
	rs->global_shader_parameter_set("opennova_fog_type", globals.base.fog_type);
	// ADR 0043: the weather-smoothed fog block also drives the scene
	// Environment fog (the lit path's fog).
	p_env->apply_scene_fog(to_vector3(globals.base.fog_color),
			globals.base.fog_start, globals.base.fog_end,
			globals.base.fog_type);
	// The HUD cache's per-frame CTRL publication: the local player's position
	// hashed into the wave rings lands in the global FLICKER (amp ring) and
	// SWING (osc ring) registers every model's CTRL tracks read (retail
	// HUD_CacheEntityDisplayInfo @ 0x4a3d9e..0x4a3dd1 -> 0x83FD00 / 0x83FD08).
	// The rings for the models that hash their own position, then the local
	// player's pair as the frame default (retail's HUD/viewmodel legs).
	const opennova::env::WeatherOscillator &osc = runtime_.core().oscillator;
	ObjectData::set_weather_rings(osc);
	Simulation *sim = _bound_sim();
	int32_t pos_q16[3];
	if (sim != nullptr && sim->local_player_position_q16(pos_q16)) {
		const uint8_t slot = osc.ring_slot(pos_q16[0], pos_q16[1], pos_q16[2]);
		ObjectData::set_weather_ctrl_registers(osc.amp_ring[slot], osc.osc_ring[slot]);
	}
}

void Weather::_ready() {
	set_process_priority(-10);
	set_process(true);
	_resolve_environment();
}

void Weather::_exit_tree() {
	bind_simulation(nullptr);
}

Weather::~Weather() {
	bind_simulation(nullptr);
}

void Weather::_process(double p_delta) {
	advance_frame(p_delta);
}

void Weather::advance_frame(double p_delta) {
	MissionEnvironment *env = _env_node();
	runtime_.process_delta(env != nullptr ? &env->state() : nullptr, p_delta);
	_post_runtime(env);
}

// --- the simulation binding ---------------------------------------------------

void Weather::bind_simulation(Object *p_sim) {
	Simulation *previous = _bound_sim();
	if (previous != nullptr) {
		previous->set_weather_render_owner(nullptr);
	}
	Simulation *sim = Object::cast_to<Simulation>(p_sim);
	MissionEnvironment *env = _env_node();
	opennova::world::WeatherState *state = sim != nullptr ? sim->weather_state() : nullptr;
	sim_id_ = state != nullptr ? ObjectID(sim->get_instance_id()) : ObjectID();
	runtime_.attach_state(state, env != nullptr ? &env->state() : nullptr);
	if (state != nullptr) {
		sim->set_weather_render_owner(this);
		runtime_.set_world_tick_driven(true);
	} else {
		// No mission behind the registers: the last hash must not keep
		// feeding every model outside one.
		ObjectData::set_weather_ctrl_registers(0, 0);
		ObjectData::clear_weather_rings();
	}
}

bool Weather::tod_keyframed() const {
	const MissionEnvironment *env = _env_node();
	return env == nullptr || env->state().has_tod_keyframes();
}

void Weather::release_simulation() {
	sim_id_ = ObjectID();
	ObjectData::set_weather_ctrl_registers(0, 0);
	ObjectData::clear_weather_rings();
	MissionEnvironment *env = _env_node();
	runtime_.attach_state(nullptr, env != nullptr ? &env->state() : nullptr);
}

void Weather::weather_render_tick(opennova::world::WeatherState &p_weather) {
	(void)p_weather; // the runtime is attached to this same home
	MissionEnvironment *env = _env_node();
	runtime_.tick_render(env != nullptr ? &env->state() : nullptr);
	// Device work (the typed publication + the shader globals) is the
	// display frame's: advance_frame pushes once per frame after the
	// ticks it banked (the 255-tick settle and catch-up frames would
	// otherwise push per tick).
}

void Weather::run_mission_start_boundary(Object *p_sim, int p_start_time_q8_8,
		int p_minutes_per_day) {
	Simulation *sim = Object::cast_to<Simulation>(p_sim);
	MissionEnvironment *env = _env_node();
	if (sim == nullptr || env == nullptr || sim->weather_state() == nullptr) {
		prewarm_mission_start();
		return;
	}
	// The seed: the loaded .env (its mission overrides already layered) + the
	// BMS clock, the ONE derivation every serving embedder runs (retail
	// Environment_SnapStateToTargets @ 0x57d1e0; Game_StartMission clock
	// @ 0x525371).
	opennova::bms::Header header{};
	header.start_time = p_start_time_q8_8;
	header.minutes_per_day = p_minutes_per_day;
	if (env->state().config() != nullptr) {
		opennova::world::WeatherSeed seed = opennova::env::weather_seed_from_config(
				*env->state().config(), header);
		seed.wind_scale = static_cast<int32_t>(runtime_.wind_strength_pct() / 100.0f * 256.0f);
		sim->seed_weather(seed);
	}
	bind_simulation(sim);
	runtime_.resync_colors();
	runtime_.tick_weather(&env->state(), 0);
	// The authority's eager WAC execution precedes the initializer; both
	// roles then settle 255 complete ticks (the kernel calls this node's
	// render legs each tick) (retail Game_StartMission @ 0x525cb8 -> @ 0x57f878).
	if (!sim->is_joiner()) {
		sim->run_mission_start_wac();
	}
	sim->settle_weather_mission_start();
	if (!sim->is_joiner()) {
		sim->seal_mission_start_baseline();
	}
	_post_runtime(env);
}

void Weather::set_wind_strength(float p_value) {
	// The `wind` named value: on a mission it lands through the bound
	// Simulation's command layer like every other weather command (a joiner
	// is refused and keeps the host's value); the standalone home takes it
	// directly. The accepted strength is remembered for the next seed.
	if (Simulation *sim = _bound_sim(); sim != nullptr) {
		if (!sim->command_wind_scale(static_cast<int>(p_value / 100.0f * 256.0f))) {
			return;
		}
		runtime_.remember_wind_strength_pct(p_value);
		return;
	}
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
	// Autonomous means the standalone home: drop a bound Simulation first
	// (otherwise the kernel hook and process_delta would both tick).
	bind_simulation(nullptr);
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

Dictionary Weather::get_weather_snapshot() const {
	Dictionary result;
	const opennova::world::WeatherState &w = runtime_.state();
	result["valid"] = w.valid;
	result["fog_target_q16"] = static_cast<int64_t>(w.fog_target_q16());
	result["fog_current_q16"] = static_cast<int64_t>(w.fog_current_q16());
	result["fog_accel_clamp"] = static_cast<int64_t>(w.fog_accel_clamp());
	result["fog_type"] = w.fog_type;
	result["tod_fixed24"] = static_cast<int64_t>(w.tod_fixed24);
	result["tod_advance_per_tick"] = static_cast<int64_t>(w.tod_advance_per_tick);
	result["quake_ticks"] = static_cast<int64_t>(w.quake_ticks);
	result["cloud_scroll_rate_target"] = static_cast<int64_t>(w.cloud_scroll_rate_target);
	result["cloud_scroll_rate"] = static_cast<int64_t>(w.cloud_scroll_rate());
	result["rain_pct_current_q16"] = static_cast<int64_t>(w.rain_pct_current_q16());
	result["rain_pct_target_q16"] = static_cast<int64_t>(w.rain_pct_target_q16());
	result["overcast_blend_q16"] = static_cast<int64_t>(w.overcast_blend_q16());
	result["overcast_target_q16"] = static_cast<int64_t>(w.overcast_target_q16());
	result["sun_dim_pct_q16"] = static_cast<int64_t>(w.sun_dim_pct_q16());
	result["sky_height_q16"] = static_cast<int64_t>(w.sky_height_q16());
	result["precipitation_kind"] = static_cast<int64_t>(w.precipitation_kind);
	result["lightning_color"] = static_cast<int64_t>(w.lightning_color);
	result["wind_scale"] = static_cast<int64_t>(w.wind_scale());
	result["night"] = w.is_night_phase();
	return result;
}

void Weather::apply_wire_sample(const Dictionary &p_sample) {
	MissionEnvironment *env = _env_node();
	opennova::world::WeatherWireSample sample;
	sample.fog_dist = static_cast<int>(p_sample.get("fog_dist", 0));
	sample.fog_accel = static_cast<int>(p_sample.get("fog_accel", 0));
	sample.rain_pct = static_cast<int>(p_sample.get("rain_pct", 0));
	sample.overcast = static_cast<int>(p_sample.get("overcast", 0));
	sample.cloud_scroll = static_cast<int>(p_sample.get("cloud_scroll", 0));
	sample.quake_ticks = static_cast<int>(p_sample.get("quake_ticks", 0));
	sample.tod_fixed = static_cast<int>(p_sample.get("tod_fixed", 0));
	sample.precipitation_kind =
			static_cast<int>(p_sample.get("precipitation_kind", 0));
	runtime_.state().apply_wire_sample(sample);
	// Publish immediately even when this render frame contains no quantum.
	runtime_.tick_weather(env != nullptr ? &env->state() : nullptr, 0);
	_post_runtime(env);
}

// The command legs: a bound Simulation owns the mutation path; the
// standalone home takes the command and republishes at zero ticks.
#define OPENNOVA_WEATHER_NODE_COMMAND(sim_call, state_call)             \
	do {                                                                \
		Simulation *sim = _bound_sim();                                 \
		if (sim != nullptr) {                                           \
			sim->sim_call;                                              \
			return;                                                     \
		}                                                               \
		runtime_.state().state_call;                                    \
		MissionEnvironment *env = _env_node();                          \
		runtime_.tick_weather(env != nullptr ? &env->state() : nullptr, 0); \
		_post_runtime(env);                                             \
	} while (0)

void Weather::command_fog_distance(int p_metres) {
	OPENNOVA_WEATHER_NODE_COMMAND(command_fog_distance(p_metres), command_fog_distance(p_metres));
}

void Weather::command_move_fog(int p_metres, int p_seconds) {
	OPENNOVA_WEATHER_NODE_COMMAND(command_move_fog(p_metres, p_seconds), command_move_fog(p_metres, p_seconds));
}

void Weather::command_rain(int p_percent, int p_seconds) {
	OPENNOVA_WEATHER_NODE_COMMAND(command_rain(p_percent, p_seconds), command_rain(p_percent, p_seconds));
}

void Weather::command_snow(int p_percent, int p_seconds) {
	OPENNOVA_WEATHER_NODE_COMMAND(command_snow(p_percent, p_seconds), command_snow(p_percent, p_seconds));
}

void Weather::command_overcast(int p_percent, int p_seconds) {
	OPENNOVA_WEATHER_NODE_COMMAND(command_overcast(p_percent, p_seconds), command_overcast(p_percent, p_seconds));
}

void Weather::command_sky_speed(int p_rate) {
	OPENNOVA_WEATHER_NODE_COMMAND(command_sky_speed(p_rate), command_sky_speed(p_rate));
}

void Weather::command_quake(int p_seconds) {
	OPENNOVA_WEATHER_NODE_COMMAND(command_quake(p_seconds), command_quake(p_seconds));
}

void Weather::command_time_of_day_minutes(int p_minute_of_day) {
	OPENNOVA_WEATHER_NODE_COMMAND(command_time_of_day_minutes(p_minute_of_day),
			command_time_of_day_minutes(p_minute_of_day));
}

void Weather::command_fog_type(int p_type) {
	OPENNOVA_WEATHER_NODE_COMMAND(command_fog_type(p_type), command_fog_type(p_type));
}

#undef OPENNOVA_WEATHER_NODE_COMMAND

void Weather::trigger_lightning_short() {
	Simulation *sim = _bound_sim();
	if (sim != nullptr) {
		sim->command_lightning_flash();
		return;
	}
	runtime_.trigger_lightning_short();
}

void Weather::trigger_lightning_long() {
	Simulation *sim = _bound_sim();
	if (sim != nullptr) {
		sim->command_lightning_far_flash();
		return;
	}
	runtime_.trigger_lightning_long();
}

void Weather::set_wind_duration(int p_seconds) {
	// The authoring extension follows the same authority rule: a joiner never
	// edits the attached home.
	if (Simulation *sim = _bound_sim(); sim != nullptr && sim->is_joiner()) {
		return;
	}
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

int Weather::get_terrain_light_combined_rgb() const {
	return static_cast<int>(runtime_.terrain_light_combined_rgb());
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
