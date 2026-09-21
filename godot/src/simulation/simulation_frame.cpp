// Simulation's first-class Godot adapter to the portable inmatch::Session.
// Lifecycle, input deposit, fixed cadence, catch-up, and cancellation stay in
// engine/runtime/inmatch. Godot supplies one synchronous typed tick sink so its
// presentation devices consume a tick before the next catch-up tick runs.
#include "simulation/simulation_internal.h"
#include "simulation/tick_sink.h"

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

// The ONE engine role this session runs its ticks through (ADR 0043 d3):
// installed per session by kind, never null after construction.
opennova::inmatch::Role &Simulation::active_role() {
	return *role_;
}

opennova::inmatch::ListenHostState *Simulation::host_state() {
	return host_role_ != nullptr ? &host_role_->state : nullptr;
}

const opennova::inmatch::ListenHostState *Simulation::host_state() const {
	return host_role_ != nullptr ? &host_role_->state : nullptr;
}

opennova::inmatch::NapiNPServerCtx *Simulation::host_ctx() {
	return host_role_ != nullptr ? &host_role_->state.host_owner.ctx : nullptr;
}

const opennova::inmatch::NapiNPServerCtx *Simulation::host_ctx() const {
	return host_role_ != nullptr ? &host_role_->state.host_owner.ctx : nullptr;
}

// What the role feeds (inmatch/role_feeds.h) read of the running role: the
// kernel, the role's replica runtime, the authority's session context, the
// joiner bit and the staged host option word.
opennova::inmatch::RoleView Simulation::role_view() const {
	opennova::inmatch::RoleView view;
	view.kernel = kernel_.get();
	view.runtime = runtime_;
	view.host = host_ctx();
	view.joiner = is_joiner();
	view.staged_mp_attributes = net_.host_session_config.mp_attributes;
	return view;
}

// The ONE writer of the kind-derived world rules: a joiner is never the
// projectile authority; a LAN host or a joiner is an mp session (the SP
// listen server keeps the SinglePlayer kind and stays offline). Runs on the
// fresh kernel at reset_world and whenever a role installs.
void Simulation::apply_session_rules() {
	if (kernel_ == nullptr) return;
	kernel_->world.rules.projectile_authority = !is_joiner();
	kernel_->world.rules.mp_session = is_host_listening() || is_joiner();
}

bool Simulation::adopt_role(std::unique_ptr<opennova::inmatch::Role> p_role,
		opennova::inmatch::HostRole *p_host, opennova::inmatch::JoinerRole *p_joiner) {
	const opennova::inmatch::TransitionResult out = session_.configure_role(*p_role);
	if (out.code != opennova::inmatch::TransitionCode::Applied &&
			out.code != opennova::inmatch::TransitionCode::NoOp) {
		return false;
	}
	role_ = std::move(p_role);
	host_role_ = p_host;
	joiner_role_ = p_joiner;
	if (kernel_ != nullptr) role_->bind(*kernel_);
	// A fresh host/joiner has no runtime until its bring-up / dial builds one.
	runtime_ = role_->client_runtime();
	apply_session_rules();
	return true;
}

// The SP listen server keeps the SinglePlayer kind so the session's
// pause/step/reset stay available; a LAN host is ListenHost or DedicatedHost
// by serve_and_play (enable_host_listen / ensure_session_role).
bool Simulation::install_offline_role() {
	if (listen_server_) {
		return install_role(std::make_unique<opennova::inmatch::HostRole>(
				opennova::inmatch::RoleKind::SinglePlayer, item_class_resolver()));
	}
	return install_role(std::make_unique<opennova::inmatch::LocalRole>());
}

// A joiner (enable_join) stays; a LAN host (enable_host_listen) stays and
// re-kinds to the UI server type; otherwise the SP listen server or the bare
// local role by listen_server_. Only while the session can switch roles.
void Simulation::ensure_session_role() {
	using State = opennova::inmatch::State;
	using RoleKind = opennova::inmatch::RoleKind;
	const State state = session_.state();
	if (state != State::Unloaded && state != State::Failed) return;
	if (net_.lan_host_pending) {
		// The LAN host enable_host_listen could not install mid-mission: the
		// role lands now, over the pump that was bound then.
		net_.lan_host_pending = false;
		if (install_role(std::make_unique<opennova::inmatch::HostRole>(
					net_.host_serve_and_play ? RoleKind::ListenHost : RoleKind::DedicatedHost,
					item_class_resolver())) &&
				net_.pump_socket != nullptr) {
			host_role_->set_socket(net_.pump_socket.get());
		}
		return;
	}
	if (joiner_role_ != nullptr) return;
	if (host_role_ != nullptr && host_role_->kind() != RoleKind::SinglePlayer) {
		const RoleKind kind = net_.host_serve_and_play ? RoleKind::ListenHost : RoleKind::DedicatedHost;
		if (host_role_->kind() != kind) {
			host_role_->set_kind(kind);
			(void)session_.configure_role(*role_);
			apply_session_rules();
		}
		return;
	}
	if ((host_role_ != nullptr) != listen_server_) (void)install_offline_role();
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
		ensure_session_role();
		const opennova::inmatch::TransitionResult role =
				session_.configure_role(*role_);
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
	// Direct/local simulations historically start paused. The live GameWorld frame
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
// hands the host role 0 and inmatch suppresses 0x68 instead of inventing
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
	if (joiner_role_ != nullptr && joiner_role_->take_diagnostic_sample())
		print_joiner_net_diagnostic_sample();
}

bool Simulation::accept_tick(const opennova::inmatch::TickOutcome &p_tick) {
	const bool profiling = runtime_profiling_enabled_;
	last_net_tick_us_ = static_cast<uint64_t>(p_tick.net_us);
	if (profiling) frame_sim_us_ += p_tick.tick_us;
	if (session_tick_sink_ == nullptr) return true;
	const int64_t sink_start =
			profiling ? Time::get_singleton()->get_ticks_usec() : 0;
	const bool accepted = session_tick_sink_->on_session_tick(p_tick);
	if (profiling)
		frame_sink_us_ += Time::get_singleton()->get_ticks_usec() - sink_start;
	return accepted;
}

// The shared head of a session frame: reset the frame's profile spans, take the
// typed input, place the sound listener and stamp the viewport height.
opennova::inmatch::FrameInput Simulation::begin_session_frame(
		const Ref<MissionFrameInput> &p_input) {
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
	return input;
}

Ref<MissionFrameOutcome> Simulation::advance_session_frame(
		const Ref<MissionFrameInput> &p_input) {
	const opennova::inmatch::FrameOutcome outcome =
			session_.advance(begin_session_frame(p_input));
	fold_frame_stats(outcome);
	return godot_outcome(outcome);
}

Ref<MissionFrameOutcome> Simulation::step_session_frame(
		const Ref<MissionFrameInput> &p_input) {
	const opennova::inmatch::FrameOutcome outcome =
			session_.step_once(begin_session_frame(p_input));
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

