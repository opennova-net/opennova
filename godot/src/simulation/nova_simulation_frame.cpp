// Simulation — the game-frame binding (ADR 0033 R1). The loop shape, the
// per-tick leg order, and the post-batch frame-leg order are
// frame::FrameDriver's (engine/runtime/frame); this TU installs the shell's
// device legs as hooks, supplies step/logic-tick/effects natively, and reads
// the perf spans back for the probe/F3 seam.
#include "simulation/nova_simulation_internal.h"
#include <godot_cpp/variant/utility_functions.hpp>

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

namespace {

// Install-time leg-contract wiring: every named leg must exist on the
// registrant — a missing one rejects the whole install (fail loudly; no
// fallback path).
bool wire_leg_contract(Object *p_owner, const char *const *p_names, int p_count,
		Callable *const *p_slots) {
	for (int i = 0; i < p_count; ++i) {
		if (!p_owner->has_method(StringName(p_names[i]))) {
			UtilityFunctions::push_error(String("frame leg owner ") +
					p_owner->get_class() + " is missing leg " + p_names[i] +
					"; install rejected");
			return false;
		}
	}
	for (int i = 0; i < p_count; ++i) {
		*p_slots[i] = Callable(p_owner, StringName(p_names[i]));
	}
	return true;
}

} // namespace

void Simulation::set_frame_shell(Object *p_shell,
		const Callable &p_listener) {
	if (p_shell == nullptr) {
		frame_listener_cb_ = Callable();
		frame_begin_effect_cb_ = Callable();
		frame_sync_fixed_cb_ = Callable();
		frame_effects_cb_ = Callable();
		frame_fixed_done_cb_ = Callable();
		frame_present_rows_cb_ = Callable();
		frame_present_frame_cb_ = Callable();
		return;
	}
	static const char *const kNames[] = {
		"_begin_present_effect_tick",
		"_frame_sync_fixed_leg",
		"_frame_effects_drained",
		"_frame_fixed_tick_completed",
		"_frame_present_rows_leg",
		"_frame_present_frame_leg",
	};
	Callable *const slots[] = {
		&frame_begin_effect_cb_,
		&frame_sync_fixed_cb_,
		&frame_effects_cb_,
		&frame_fixed_done_cb_,
		&frame_present_rows_cb_,
		&frame_present_frame_cb_,
	};
	if (!wire_leg_contract(p_shell, kNames, 6, slots)) {
		return;
	}
	frame_listener_cb_ = p_listener;
}

void Simulation::set_frame_world(Object *p_world) {
	if (p_world == nullptr) {
		frame_terrain_cb_ = Callable();
		frame_foliage_cb_ = Callable();
		frame_net_drive_cb_ = Callable();
		frame_weather_cb_ = Callable();
		frame_blink_cb_ = Callable();
		frame_occlusion_cb_ = Callable();
		frame_iris_cb_ = Callable();
		frame_audio_cb_ = Callable();
		return;
	}
	static const char *const kNames[] = {
		"_frame_terrain_leg",
		"_frame_foliage_leg",
		"_frame_net_drive_leg",
		"_frame_weather_leg",
		"_frame_blink_leg",
		"_frame_occlusion_leg",
		"_frame_iris_leg",
		"_frame_audio_leg",
	};
	Callable *const slots[] = {
		&frame_terrain_cb_,
		&frame_foliage_cb_,
		&frame_net_drive_cb_,
		&frame_weather_cb_,
		&frame_blink_cb_,
		&frame_occlusion_cb_,
		&frame_iris_cb_,
		&frame_audio_cb_,
	};
	wire_leg_contract(p_world, kNames, 8, slots);
}

opennova::frame::FrameHooks Simulation::build_frame_hooks() {
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

int Simulation::frame_realtime(double p_delta) {
	frame_net_us_ = 0;
	frame_driver_.set_clock([]() {
		return static_cast<int64_t>(Time::get_singleton()->get_ticks_usec());
	});
	return frame_driver_.run_frame(p_delta, build_frame_hooks());
}

bool Simulation::frame_single() {
	frame_net_us_ = 0;
	frame_driver_.set_clock([]() {
		return static_cast<int64_t>(Time::get_singleton()->get_ticks_usec());
	});
	return frame_driver_.run_single(build_frame_hooks());
}

Dictionary Simulation::get_frame_perf() const {
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
