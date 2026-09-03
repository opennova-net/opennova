// Simulation's first-class Godot adapter to the portable inmatch::Session.
// Lifecycle, input deposit, fixed cadence, catch-up, and cancellation stay in
// engine/runtime/inmatch. Godot supplies one synchronous typed tick sink so its
// presentation devices consume a tick before the next catch-up tick runs.
#include "simulation/simulation_internal.h"

#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/window.hpp>

using namespace sim_internal;

namespace {

Ref<MissionFrameOutcome> godot_outcome(
		const opennova::inmatch::FrameOutcome &p_native) {
	Ref<MissionFrameOutcome> out;
	out.instantiate();
	out->assign(p_native);
	return out;
}

} // namespace

// The engine role this sim's mode runs its ticks through (ADR 0043 d3): the
// joiner's, the host's (the SP listen server keeps the SinglePlayer kind so
// the session's pause/step/reset stay available; a LAN host is ListenHost or
// DedicatedHost by serve_and_play), else the bare local role.
opennova::inmatch::Role &Simulation::configured_session_role() {
	using RoleKind = opennova::inmatch::RoleKind;
	if (joiner_) return joiner_role_;
	if (host_listen_) {
		host_role_.set_kind(host_serve_and_play_ ? RoleKind::ListenHost : RoleKind::DedicatedHost);
		return host_role_;
	}
	if (listen_server_) {
		host_role_.set_kind(RoleKind::SinglePlayer);
		return host_role_;
	}
	return local_role_;
}

opennova::inmatch::Role &Simulation::active_role() {
	return session_.role() != nullptr ? *session_.role() : configured_session_role();
}

bool Simulation::begin_session_load() {
	using State = opennova::inmatch::State;
	const State state = session_.state();
	// A pre-connected joiner deliberately carries its live socket into load.
	// Every other prior session, including Failed, closes its concrete target
	// before a replacement world is installed.
	if (state != State::Unloaded && state != State::Connecting) {
		(void)session_.close();
	}
	if (session_.state() != State::Connecting) {
		const opennova::inmatch::TransitionResult role =
				session_.configure_role(configured_session_role());
		if (role.code != opennova::inmatch::TransitionCode::Applied &&
				role.code != opennova::inmatch::TransitionCode::NoOp) {
			return false;
		}
	}
	return session_.begin_load().applied();
}

void Simulation::complete_session_load() {
	if (session_.state() != opennova::inmatch::State::Loading) return;
	if (!session_.complete_load().applied()) return;
	// Direct/local simulations historically start paused. Live GameFramePipeline
	// resumes them after presentation setup; network roles must keep pumping.
	if (session_.kind() == opennova::inmatch::RoleKind::SinglePlayer) {
		(void)session_.pause();
	}
}

void Simulation::fail_session_load(const char *p_message) {
	world_installed_ = false;
	(void)session_.fail({opennova::inmatch::SessionErrorCode::LoadFailed,
			p_message != nullptr ? p_message : "mission load failed"});
}

bool Simulation::is_loaded() const {
	if (!world_installed_) return false;
	const opennova::inmatch::State state = session_.state();
	return state == opennova::inmatch::State::Running ||
			state == opennova::inmatch::State::Paused;
}

bool Simulation::pause_session() {
	const opennova::inmatch::TransitionResult out = session_.pause();
	return out.applied() || out.code == opennova::inmatch::TransitionCode::NoOp;
}

bool Simulation::resume_session() {
	const opennova::inmatch::TransitionResult out = session_.resume();
	return out.applied() || out.code == opennova::inmatch::TransitionCode::NoOp;
}

bool Simulation::reset_session() {
	const opennova::inmatch::TransitionResult out = session_.reset_to_baseline();
	if (out.applied()) restore_world_baseline();
	return out.applied();
}

void Simulation::close_session() {
	(void)session_.close();
}

// Server_SendRandomSeedSync's non-dedicated S2C 0x68 cursor advances by 50
// and wraps against the current renderer viewport height [orig:
// Server_SendRandomSeedSync @ 0x511360 -- CEffectWorld_GetViewportDimensions
// @ 0x5b1560 (call @ 0x511375), wrap @ 0x511391]. Resolve the render window
// the way retail's CEffectWorld query does: the live window (the scene
// tree's root; the simulation is a RefCounted the runtime owns, not a node
// in that tree). A headless DisplayServer has no
// renderer (the dedicated-host analogue); a missing/non-drawable viewport
// hands the host role 0 and npruntime suppresses 0x68 instead of inventing
// a screen size (D-NET-206).
int32_t Simulation::renderer_viewport_height() const {
	Viewport *viewport = nullptr;
	DisplayServer *display = DisplayServer::get_singleton();
	if (display != nullptr && display->get_name() != "headless") {
		SceneTree *tree = Object::cast_to<SceneTree>(
				Engine::get_singleton()->get_main_loop());
		if (tree != nullptr)
			viewport = tree->get_root();
	}
	return viewport != nullptr ? static_cast<int32_t>(viewport->get_visible_rect().size.y) : 0;
}

void Simulation::after_tick() {
	// The dead-player map-mode clear rides every advanced tick -- retail's
	// render-frame gate, observed before the presenters read the mode.
	tick_hud_map_death_gate();
	// The joiner's ~1 Hz frozen-session tripwire sampled this tick: the
	// env-gated print is the shell's channel.
	if (joiner_ && joiner_role_.take_diagnostic_sample())
		print_joiner_net_diagnostic_sample();
}

bool Simulation::accept_tick(const opennova::inmatch::TickOutcome &p_tick) {
	const bool profiling = runtime_profiling_enabled_;
	last_net_tick_us_ = static_cast<uint64_t>(p_tick.net_us);
	if (profiling) {
		frame_sim_us_ += p_tick.tick_us;
		frame_net_us_ += p_tick.net_us;
	}
	if (!session_tick_sink_.is_valid()) return true;
	Ref<MissionTickOutcome> value;
	value.instantiate();
	value->assign(p_tick);
	const int64_t sink_start =
			profiling ? Time::get_singleton()->get_ticks_usec() : 0;
	const Variant accepted = session_tick_sink_.call(value);
	if (profiling)
		frame_sink_us_ += Time::get_singleton()->get_ticks_usec() - sink_start;
	return !(accepted.get_type() == Variant::BOOL && !static_cast<bool>(accepted));
}

Ref<MissionFrameOutcome> Simulation::advance_session_frame(
		const Ref<MissionFrameInput> &p_input,
		const Callable &p_tick_sink) {
	frame_net_us_ = 0;
	frame_sim_us_ = 0;
	frame_sink_us_ = 0;
	if (kernel_ != nullptr) kernel_->profile.reset();
	opennova::inmatch::FrameInput input;
	if (p_input.is_valid()) input = p_input->native_value();
	if (input.camera.listener_valid) {
		set_sound_listener(Vector3(input.camera.position[0],
				input.camera.position[1], input.camera.position[2]));
	}
	input.viewport_height = renderer_viewport_height();
	session_tick_sink_ = p_tick_sink;
	const opennova::inmatch::FrameOutcome outcome = session_.advance(input);
	session_tick_sink_ = Callable();
	fold_frame_stats(outcome);
	return godot_outcome(outcome);
}

Ref<MissionFrameOutcome> Simulation::step_session_frame(
		const Ref<MissionFrameInput> &p_input,
		const Callable &p_tick_sink) {
	frame_net_us_ = 0;
	frame_sim_us_ = 0;
	frame_sink_us_ = 0;
	if (kernel_ != nullptr) kernel_->profile.reset();
	opennova::inmatch::FrameInput input;
	if (p_input.is_valid()) input = p_input->native_value();
	if (input.camera.listener_valid) {
		set_sound_listener(Vector3(input.camera.position[0],
				input.camera.position[1], input.camera.position[2]));
	}
	input.viewport_height = renderer_viewport_height();
	session_tick_sink_ = p_tick_sink;
	const opennova::inmatch::FrameOutcome outcome = session_.step_once(input);
	session_tick_sink_ = Callable();
	fold_frame_stats(outcome);
	return godot_outcome(outcome);
}

bool Simulation::step() {
	// Focused probes deposit input directly on Simulation before asking for one
	// deterministic tick. Preserve that public seam without adding a second tick
	// path: snapshot the concrete target's held/edge latches into the same typed
	// frame value inmatch::Session consumes. Direct look input has already updated
	// kernel_->local.input's composed heading/pitch, so it must not be replayed as a
	// second pixel delta here.
	opennova::inmatch::FrameInput input;
	input.player.movement = kernel_->local.input;
	input.player.held_action_bits = kernel_->local.weapon.fire_held
			? opennova::inmatch::HELD_FIRE : 0u;
	input.player.pressed_action_bits =
			(kernel_->local.weapon.fire_pressed ? opennova::inmatch::PRESSED_FIRE : 0u) |
			(kernel_->local.weapon.reload_pressed ? opennova::inmatch::PRESSED_RELOAD : 0u);
	input.viewport_height = renderer_viewport_height();
	return session_.drive_one(input).ticks_run() == 1;
}

