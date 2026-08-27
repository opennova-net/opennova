#pragma once

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector4.hpp>

#include <runtime/environment/weather_runtime.h>

namespace godot {

class MissionEnvironment;

// The weather/light smoothing owner node — the ADR 0033 device leg over the
// engine's env::WeatherRuntime (engine/runtime/environment), which owns the
// witnessed embedding of the retail weather tick (the engine header
// carries the cites):
// the 62 Hz tick-credit accumulator, the mission reset epoch, the 255-tick
// prewarm, the per-tick target-feed/snap/writeback sequence, and the typed
// network snapshot/sample projection (the math cluster itself is
// env::WeatherCore in engine/formats/env). This node keeps only device work:
// the environment NodePath resolution, the _process hook, the iris-sample
// property the in-world shell stamps, and the opennova_* global shader
// parameter pushes. Ported from nova_weather.gd (2026-08-09 de-scripting);
// RE record: docs/env/env-tod-re.md.
class Weather : public Node3D {
	GDCLASS(Weather, Node3D)

public:
	void set_environment_path(const NodePath &p_path);
	NodePath get_environment_path() const { return environment_path_; }

	// 100 = the witnessed retail constant (Env_WindScale 256, always on — the
	// ambient foliage sway every retail map has), which is also the default.
	void set_wind_strength(float p_value);
	float get_wind_strength() const;

	// The marched iris-exposure samples (D-RLIT-2): the in-world shell stamps
	// three per-sample classification codes each frame; empty keeps the
	// outdoor fallback sample when no world supplies classification codes.
	void set_iris_samples(const PackedInt32Array &p_samples);
	PackedInt32Array get_iris_samples() const;

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

	Dictionary get_network_environment_snapshot();
	void apply_network_environment_sample(const Dictionary &p_sample);

	// --- world-composer orchestration (ADR 0033; the witnessed sequencing
	// lives in engine/runtime/environment weather_runtime.h) ---------------
	// The world's weather leg: bank the render delta on the engine's
	// recovered 62 Hz weather clock and run each due quantum's sequence —
	// advance the integer mission clock, tick every weather block once, and
	// on the authority advance + republish the network environment.
	void advance_world_driven(double p_delta, Object *p_sim);
	// Publish the live typed network snapshot onto the authority sim
	// (no-op for joiners / without a loaded environment).
	void push_network_environment(Object *p_sim);
	// The mission-start environment boundary: T0 seed publication, authority
	// WAC direct execution, the prewarm settle (local + network mirrors),
	// the settled republication, and the baseline seal.
	void run_mission_start_boundary(Object *p_sim);
	// Drain one authoritative phase-2 join environment update (if any) into
	// the weather/env owners. Joiner-only; render-frame safe (the sim owns
	// the receive-revision cursor).
	void apply_join_network_update(Object *p_sim);

	void trigger_lightning_short();
	void trigger_lightning_long();
	void set_wind_duration(int p_seconds);
	int get_wind_duration() const;

	float get_sway_amount() const;
	float get_sway_phase() const;
	float get_lightning_intensity() const;

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
	// (cam_x, cam_z) world units — U carries the accumulator NEGATIVELY, V
	// positively (weather_runtime.h carries the cites). The sky dome
	// (and the water scroll below) consume these through this seam, like the
	// terrain consumes get_smooth_sun.
	Vector2 get_cloud_uv_offset1(float p_cam_x, float p_cam_z) const;
	Vector2 get_cloud_uv_offset2(float p_cam_x, float p_cam_z) const;
	float get_cloud_uv_rate_per_second() const;
	// The witnessed water UV transform (scale, bias, offset_u, offset_v) —
	// the water surface shares the layer-1 cloud accumulators with a 32x
	// camera term (weather_runtime.h carries the cites).
	Vector4 get_water_uv_state(float p_cam_x, float p_cam_z,
			float p_fog_distance) const;

	// C++-only seam for sibling native appliers.
	opennova::env::WeatherRuntime &runtime() { return runtime_; }
	const opennova::env::WeatherRuntime &runtime() const { return runtime_; }

	// One render-frame advance (the _process body) — the externally-callable
	// drive the test harness uses; the engine's virtual delegates here.
	void advance_frame(double p_delta);

	void _ready() override;
	void _process(double p_delta) override;

protected:
	static void _bind_methods();

private:
	MissionEnvironment *_env_node() const;
	opennova::env::EnvironmentState *_env_state() const;
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
	PackedInt32Array iris_samples_;
};

} // namespace godot
