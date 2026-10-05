#pragma once

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector4.hpp>

#include <runtime/environment/weather_runtime.h>
#include <runtime/world/weather_state.h>

namespace godot {

class MissionEnvironment;
class Simulation;

// The weather's render-owner node — the ADR 0033 device leg over the
// engine's env::WeatherRuntime (engine/runtime/environment), the color half
// of the retail weather tick over ONE weather home (world::WeatherState in
// the Simulation's World on a mission, the runtime's private state
// standalone; the engine headers carry the cites). Bound to a Simulation it
// registers itself as the kernel's per-tick render owner (the sim legs run in
// the kernel after every logic tick, then this node's color legs), seeds the
// World's weather from the mission environment at load, and runs the
// mission-start boundary. This node keeps only device work: the environment
// NodePath resolution, the advance_frame drive the frame pipeline calls, the iris-sample
// property the in-world shell stamps, and the opennova_* global shader
// parameter pushes. RE record: docs/env/env-tod-re.md.
class Weather : public Node3D, private opennova::world::IWeatherRenderTick {
	GDCLASS(Weather, Node3D)

public:
	void set_environment_path(const NodePath &p_path);
	NodePath get_environment_path() const { return environment_path_; }

	// 100 = the witnessed retail constant (g_EnvWindScale 256, always on — the
	// ambient foliage sway every retail map has), which is also the default.
	void set_wind_strength(float p_value);
	float get_wind_strength() const;

	// The marched iris-exposure samples (D-RLIT-2): the in-world shell stamps
	// three per-sample classification codes each frame; empty keeps the
	// outdoor fallback sample when no world supplies classification codes.
	void set_iris_samples(const PackedInt32Array &p_samples);
	PackedInt32Array get_iris_samples() const;

	// --- the simulation binding (ADR 0042 d2/d3) ----------------------------
	// Bind the World's weather home: the runtime attaches to it, the env's
	// scalar/clock reads follow it, and the kernel calls this node's render
	// legs after each sim tick. Null unbinds back to the private state.
	void bind_simulation(Object *p_sim);
	// The bound Simulation's World is going away (reset_world / its
	// destructor): drop the pointer into it and fall back to the
	// environment's standalone weather home without touching the sim.
	void release_simulation();
	opennova::world::IWeatherRenderTick *render_tick_interface() { return this; }
	// The mission-start environment boundary over this weather device: the
	// sim's phase B of the one host boot (Simulation::start_mission ->
	// inmatch::start_host_mission) seeds the World's weather from the loaded
	// .env + the mission clock, binds this node as its render owner, then
	// completes the mission start (the authority's eager WAC, the initializer
	// + 255-tick settle, the baseline) and the session's load. With no sim the
	// standalone home prewarms.
	void run_mission_start_boundary(Object *p_sim);

	void set_world_tick_driven(bool p_enabled);
	void prepare_world_driven();
	void prepare_autonomous();
	void prewarm_mission_start();
	void tick_fixed();
	void resync_colors();
	void resync_colors_now();
	// The per-frame sun-veil exposure stop-down feed (the world's veil leg;
	// weather_runtime.h routes to the witnessed modulator-2 writer).
	void set_sun_veil_stopdown(int p_stopdown);
	// The frozen-fixture exposure settle (GameWorld's capture-refresh seam):
	// chase the stamped iris samples' modulator target to its fixed point
	// and republish, without advancing weather time (weather_runtime.h
	// carries the cites and the freeze contract).
	void settle_exposure();

	// The probe/test view of the attached weather home in native units.
	Dictionary get_weather_snapshot() const;
	// A decoded phase-2 sample in WIRE units into the attached home's
	// targets (the standalone test seam; a joiner's samples land in the
	// engine bridge).
	void apply_wire_sample(const Dictionary &p_sample);

	void trigger_lightning_short();
	void trigger_lightning_long();
	// The WAC weather commands on the attached home: through the bound
	// Simulation's command layer on a mission (ADR 0042 d5), directly on the
	// standalone home otherwise (previews, fixtures). world::WeatherState
	// carries the handler cites.
	void command_fog_distance(int p_metres);
	void command_move_fog(int p_metres, int p_seconds);
	void command_rain(int p_percent, int p_seconds);
	void command_snow(int p_percent, int p_seconds);
	void command_overcast(int p_percent, int p_seconds);
	void command_sky_speed(int p_rate);
	void command_quake(int p_seconds);
	void command_time_of_day_minutes(int p_minute_of_day);
	void command_fog_type(int p_type);
	void command_sky_height(int p_height_raw);
	void command_sun_fade(int p_percent, int p_seconds);
	void command_color_fade(int p_seconds);
	void command_wind_scale(int p_value);
	// One weather color block by world::WeatherColorTarget index, packed
	// 0xRRGGBB; the lightning color the same way.
	void command_weather_color(int p_target, int p_rgb);
	void command_lightning_color(int p_rgb);
	void set_wind_duration(int p_seconds);
	int get_wind_duration() const;

	float get_sway_amount() const;
	float get_sway_phase() const;
	float get_lightning_intensity() const;
	// g_EnvTerrainLightCombined packed 0x00RRGGBB — the precipitation color.
	int get_terrain_light_combined_rgb() const;

	Vector3 get_smooth_fill() const;
	Vector3 get_smooth_sun() const;
	Vector3 get_smooth_fog() const;
	Vector3 get_smooth_sky() const;
	Vector3 get_smooth_skyfog() const;
	Vector3 get_smooth_ceiling() const;
	Vector3 get_smooth_cloud() const;
	Vector3 get_smooth_floor() const;
	Vector3 get_smooth_sky_base() const;
	Vector3 get_smooth_sky_bright() const;
	Vector3 get_smooth_sky_highlight() const;
	Vector3 get_smooth_cloud_base() const;
	Vector3 get_smooth_cloud_highlight() const;
	Vector3 get_smooth_cloud_edge() const;

	// The witnessed cloud-scroll UV translations for a camera at
	// (render_x, render_z) — the RENDER basis the dome's UVs use, so
	// render_x is the Godot z and render_z the Godot x (util/axes.h). U
	// carries the accumulator NEGATIVELY, V positively (weather_runtime.h
	// carries the cites). The sky dome consumes these through this seam,
	// like the terrain consumes get_smooth_sun.
	Vector2 get_cloud_uv_offset1(float p_render_x, float p_render_z) const;
	Vector2 get_cloud_uv_offset2(float p_render_x, float p_render_z) const;
	float get_cloud_uv_rate_per_second() const;

	// C++-only seams for sibling native appliers.
	opennova::env::WeatherRuntime &runtime() { return runtime_; }
	const opennova::env::WeatherRuntime &runtime() const { return runtime_; }

	// One render-frame advance — the externally-callable
	// drive the test harness uses; the engine's virtual delegates here.
	void advance_frame(double p_delta);

	void _ready() override;
	void _exit_tree() override;
	// A Weather freed off-tree never sees _exit_tree: release the sim's
	// render-owner pointer and the env's weather view here too.
	~Weather() override;

protected:
	static void _bind_methods();

private:
	// The kernel's per-tick render hook (world::IWeatherRenderTick).
	void weather_render_tick(opennova::world::WeatherState &p_weather) override;
	bool tod_keyframed() const override;

	MissionEnvironment *_env_node() const;
	Simulation *_bound_sim() const;
	// Re-resolve the cached env node from environment_path_ (in-tree only).
	// Deliberately separate from the setter: _ready re-resolves without
	// re-assigning the stored path (NodePath self-assignment through the
	// setter would destroy the aliased value).
	void _resolve_environment();
	// The device tail after any runtime advance: flush the env node's typed
	// light publication and push the weather-owned shader globals (the
	// witnessed per-writeback refresh).
	void _post_runtime(MissionEnvironment *p_env);

	NodePath environment_path_;
	opennova::env::WeatherRuntime runtime_;
	ObjectID env_node_id_;
	ObjectID sim_id_;
	PackedInt32Array iris_samples_;
};

} // namespace godot
