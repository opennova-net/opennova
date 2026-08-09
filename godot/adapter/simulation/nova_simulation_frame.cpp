// NovaSimulation — the game-frame binding (ADR 0033 R1). The loop shape, the
// per-tick leg order, and the post-batch frame-leg order are
// frame::FrameDriver's (engine/runtime/frame); this TU installs the shell's
// device legs as hooks, supplies step/logic-tick/effects natively, and reads
// the perf spans back for the probe/F3 seam.
#include "simulation/nova_simulation_internal.h"

#include <godot_cpp/classes/time.hpp>

using namespace novasim;

namespace {

// Box a shell Callable as a niladic hook; an invalid Callable is an absent leg.
std::function<void()> leg(const Callable &p_cb) {
	if (!p_cb.is_valid()) {
		return {};
	}
	return [p_cb]() { p_cb.call(); };
}

std::function<void(int32_t)> leg_int(const Callable &p_cb) {
	if (!p_cb.is_valid()) {
		return {};
	}
	return [p_cb](int32_t v) { p_cb.call(v); };
}

} // namespace

void NovaSimulation::set_frame_shell_hooks(const Callable &p_listener,
		const Callable &p_begin_effect_tick, const Callable &p_sync_fixed,
		const Callable &p_effects_drained, const Callable &p_fixed_done,
		const Callable &p_present_rows, const Callable &p_present_frame) {
	frame_listener_cb_ = p_listener;
	frame_begin_effect_cb_ = p_begin_effect_tick;
	frame_sync_fixed_cb_ = p_sync_fixed;
	frame_effects_cb_ = p_effects_drained;
	frame_fixed_done_cb_ = p_fixed_done;
	frame_present_rows_cb_ = p_present_rows;
	frame_present_frame_cb_ = p_present_frame;
}

void NovaSimulation::set_frame_world_hooks(const Callable &p_terrain,
		const Callable &p_foliage,
		const Callable &p_net_drive, const Callable &p_weather,
		const Callable &p_blink_gates, const Callable &p_occlusion,
		const Callable &p_iris, const Callable &p_audio) {
	frame_terrain_cb_ = p_terrain;
	frame_foliage_cb_ = p_foliage;
	frame_net_drive_cb_ = p_net_drive;
	frame_weather_cb_ = p_weather;
	frame_blink_cb_ = p_blink_gates;
	frame_occlusion_cb_ = p_occlusion;
	frame_iris_cb_ = p_iris;
	frame_audio_cb_ = p_audio;
}

opennova::frame::FrameHooks NovaSimulation::build_frame_hooks() {
	opennova::frame::FrameHooks hooks;
	hooks.terrain = leg(frame_terrain_cb_);
	hooks.foliage = leg(frame_foliage_cb_);
	// The listener stamp: read the shell's camera listener and stamp the
	// sim's fire-sound gate; a role with no listener (dedicated) never stamps
	// — the witnessed peer gate [orig: @ 0x528e57; world/fire_sound.h].
	if (frame_listener_cb_.is_valid()) {
		const Callable listener = frame_listener_cb_;
		hooks.stamp_listener = [this, listener]() {
			const Variant v = listener.call();
			if (v.get_type() == Variant::VECTOR3) {
				const Vector3 pos = v;
				if (pos.is_finite()) {
					set_sound_listener(pos);
				}
			}
		};
	}
	hooks.step = [this]() {
		const bool did_tick = step();
		frame_net_us_ += get_last_net_tick_us();
		return did_tick;
	};
	hooks.logic_tick = [this]() { return static_cast<int32_t>(get_logic_tick()); };
	hooks.begin_effect_tick = leg_int(frame_begin_effect_cb_);
	hooks.sync_fixed_effects = leg(frame_sync_fixed_cb_);
	// The drain is native; the shell leg only receives a non-empty batch (the
	// old driver's emit-if-any behavior, one Array boundary per tick).
	if (frame_effects_cb_.is_valid()) {
		const Callable effects = frame_effects_cb_;
		hooks.drain_effects = [this, effects]() {
			const Array drained = drain_effects();
			if (!drained.is_empty()) {
				effects.call(drained);
			}
		};
	} else {
		hooks.drain_effects = [this]() { (void)drain_effects(); };
	}
	hooks.fixed_tick_completed = leg_int(frame_fixed_done_cb_);
	hooks.present_rows = leg(frame_present_rows_cb_);
	hooks.present_frame = leg(frame_present_frame_cb_);
	hooks.net_drive = leg(frame_net_drive_cb_);
	hooks.weather = leg(frame_weather_cb_);
	hooks.blink_gates = leg(frame_blink_cb_);
	hooks.occlusion_frame = leg(frame_occlusion_cb_);
	hooks.iris_samples = leg(frame_iris_cb_);
	hooks.audio = leg_int(frame_audio_cb_);
	return hooks;
}

int NovaSimulation::frame_realtime(double p_delta) {
	frame_net_us_ = 0;
	frame_driver_.set_clock([]() {
		return static_cast<int64_t>(Time::get_singleton()->get_ticks_usec());
	});
	return frame_driver_.run_frame(p_delta, build_frame_hooks());
}

bool NovaSimulation::frame_single() {
	frame_net_us_ = 0;
	frame_driver_.set_clock([]() {
		return static_cast<int64_t>(Time::get_singleton()->get_ticks_usec());
	});
	return frame_driver_.run_single(build_frame_hooks());
}

Dictionary NovaSimulation::get_frame_perf() const {
	const opennova::frame::FramePerf &perf = frame_driver_.perf();
	Dictionary out;
	out["tick_us"] = static_cast<int64_t>(perf.tick_us);
	out["sim_us"] = static_cast<int64_t>(perf.sim_us);
	out["present_us"] = static_cast<int64_t>(perf.present_us);
	out["effects_us"] = static_cast<int64_t>(perf.effects_us);
	out["net_us"] = frame_net_us_;
	out["did_tick"] = perf.did_tick;
	out["ticks"] = static_cast<int64_t>(perf.ticks);
	return out;
}
