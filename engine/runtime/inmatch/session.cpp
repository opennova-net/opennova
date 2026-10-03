#include <runtime/inmatch/session.h>

#include <base/io/perf_clock.h>
#include <runtime/hud/hud_frame.h> // HudFrameCompiler::kRadarGate*
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/role_feeds.h> // step_hud_radar
#include <runtime/inmatch/spectator_session.h>
#include <runtime/mission/mission_kernel.h>

#include <utility>

namespace opennova::inmatch {

// The session banks real time through the retail frame-time bank
// (world::TickAccumulator: the 500 ms clamp, the 7/8 EMA, 4 ms drain quanta
// with a logic tick on every fourth, the mission-start re-base below) and
// dispatches one role tick per logic tick, matching the original
// outer/logic-loop split [orig: Game_MainLoop @ 0x52b630
// -> Game_ProcessMainFrame @ 0x5263f0].

namespace {

SessionError invalid_transition(const char *message) {
	return {SessionErrorCode::InvalidTransition, message};
}

} // namespace

Session::Session(Role &role) : role_(&role), kind_(role.kind()) {}

void Role::apply_input(const TickInput &input) {
	mission::MissionKernel &kernel = *kernel_;
	ClientRuntime *runtime = client_runtime();
	const bool joiner = kind() == RoleKind::Joiner;
	// The input pass's head drops the orbit-yaw bits before this tick's
	// dispatch can raise them again [orig: Input_ProcessFrame
	// `and g_InputActionBits, 0FFFFFFAFh` @0x49d52b].
	kernel.world.script.input_action_bits &= ~0x50u;
	// The spectate state the free-fly motor reads, ahead of the pack and the
	// entity update (a joiner restamps it after its receive).
	stamp_spectator_motor(kernel, runtime, joiner);
	if (kernel.world.spectator.death_screen && runtime != nullptr) {
		apply_death_screen_input(kernel, *runtime, joiner, input.player);
		return;
	}
	const world::PlayerInput &movement = input.player.movement;
	kernel.local.set_movement_keys(movement.forward, movement.back, movement.left,
			movement.right, movement.lean_left, movement.lean_right, movement.jump);
    kernel.local.set_view_keys(movement.free_look, movement.look_up,
            movement.look_down, movement.turn_left, movement.turn_right);
	if (input.player.look_delta_x != 0.0f || input.player.look_delta_y != 0.0f)
		kernel.local.look(input.player.look_delta_x, input.player.look_delta_y);
	kernel.local.set_weapon_input((input.player.held_action_bits & HELD_FIRE) != 0,
			(input.player.pressed_action_bits & PRESSED_FIRE) != 0,
			(input.player.pressed_action_bits & PRESSED_RELOAD) != 0);
	// The medic-call edge is an action binding, not weapon state: it fires its
	// request immediately like retail's binding dispatch (the gates and the
	// cooldown live in request_medic).
	if ((input.player.pressed_action_bits & PRESSED_MEDIC_REQUEST) != 0)
		request_medic();
	// The ToSpecial dispatches, with the deferred ones, once per outer frame
	// on its first tick, as retail's input frame precedes the logic tick
	// [orig: Input_ProcessFrame @0x49D520 -> Input_FlushDeferredEvents @0x49D591].
	// The row's flag 1 keeps a dead player's PRESS out of the queue; its
	// release is dispatched regardless [orig: Input_ProcessKeyboardEvents
	// @0x49D330..0x49D339 (the press pass), the release pass @0x49D249 has no
	// such gate; ToSpecial's row flags 0x8C000801].
	if (!spectating && input.consume_one_shots) {
		const bool held = (input.player.held_action_bits & HELD_TO_SPECIAL) != 0;
		if ((input.player.pressed_action_bits & PRESSED_TO_SPECIAL) != 0) {
			const bool local_dead = kind() == RoleKind::Joiner && client_runtime() != nullptr
					? client_runtime()->local_player_dead()
					: kernel.local.local_player_dead();
			if (!held || !local_dead) kernel.local.queue_to_special();
		}
		kernel.local.dispatch_to_special(held);
	}
}

void Role::observe_frame_rate(int32_t fps) {
	if (ClientRuntime *runtime = client_runtime()) runtime->set_observed_frame_rate(fps);
}

bool Role::request_medic() {
	mission::MissionKernel &kernel = *kernel_;
	// The session/entity gates: a replica runtime and a local entity; the
	// dead-bit and cooldown gates are the local player's.
	if (client_runtime() == nullptr || !kernel.world.cached.local_player.valid()) return false;
	const bool local_dead = kind() == RoleKind::Joiner
			? client_runtime()->local_player_dead() : kernel.local.local_player_dead();
	if (!kernel.local.medic_request_allowed(local_dead)) return false;
	if (!send_medic_request()) return false;
	kernel.local.stamp_medic_request();
	return true;
}

world::LocalViewSessionInputs Role::view_session_inputs_for(
		const ClientRuntime *runtime, bool joiner, bool local_dead, bool in_session) {
	// What the arbiter reads from the session: the net layer sits above the
	// world group, so its client state crosses as plain values.
	world::LocalViewSessionInputs s;
	// The session word is the launch's network type, not the replica fold:
	// the SP launch sets type 0, so is_in_session reads 0 for the whole SP
	// mission although its in-process listen server folds a loopback client.
	// [orig: CNapiNetwork_SetNetworkType @0x4c4a50 stores +0x58 = (type in
	//  1..3) @0x4c4a85; SinglePlayer_StartMission passes 0 @0x561bce
	//  (`xor ebx, ebx` @0x561b30); the readers here: Render_ProcessMainSceneFrame
	//  @0x5ca22d (the forced first person) and @0x5ca8f6 (the dead-in-session
	//  distortion skip)]
	s.in_session = in_session;
	s.joiner = joiner;
	// The client-local death-screen latch: the 0x0A flags1 bit-0 edges every
	// role's view folds (the listen host's own loopback included)
	// [orig: g_DeathScreenActive, NapiNPClientMsg_0x00A @0x42ff88..0x43002b].
	s.hud_hit_feedback_frames = runtime != nullptr ? runtime->state().hud_hit_feedback_frames : 0;
	if (runtime) {
		s.hud_service.preround_seconds = runtime->state().preround_delay_seconds;
		s.hud_service.reload_seconds = runtime->state().vehicle_reload_seconds;
		s.hud_service.owned_zone_mask = runtime->state().owned_zone_mask;
		// Keep link order: an equal-distance designation retains the first.
		// [orig: SpawnPoint_FindNearestByTypeAndTeam @0x5BBF42..0x5BBFC3]
		const auto &state = runtime->state();
		for (const auto &link : state.minimap.linked) {
			if (!link.active || !link.remaining_ticks || link.type != 1)
				continue;
			const auto *owner = state.find(link.handle);
			s.hud_designations.push_back(
					{ link.x, link.y, link.radius_q16, uint8_t(owner ? owner->team : 0) });
		}
	}
	s.death_screen_active = runtime != nullptr && runtime->state().death_screen_active;
	s.death_screen_submode = runtime != nullptr ? runtime->state().death_screen_submode : 0;
	if (runtime != nullptr && runtime->state().spectate_target != 0xFFFF) {
		const replication::ClientState &cs = runtime->state();
		s.spectate_target_key = world::kCameraTrackedTargetKey | cs.spectate_target;
		const replication::ClientEntityState *row = cs.find(cs.spectate_target);
		s.spectate_target_dead =
				row != nullptr && row->state_flags_known && (row->state_flags & 2u) != 0;
	}
	s.end_round_known = runtime != nullptr && runtime->state().end_round.known;
	// [orig: NapiNPClientMsg_0x01D @0x430840 -> g_EndRoundWinnerTeam]
	s.end_round_winner_team = runtime != nullptr && runtime->state().end_round.header_known
			? static_cast<int32_t>(runtime->state().end_round.header.winner_team)
			: 0;
	s.local_dead = local_dead;
	s.death_camera_target_known = runtime != nullptr;
	if (runtime != nullptr) {
		const replication::ClientDeathCameraTarget &t = runtime->state().death_camera;
		s.death_camera_target[0] = t.x;
		s.death_camera_target[1] = t.y;
		s.death_camera_target[2] = t.z;
	}
	return s;
}

int64_t Session::now_us() {
	return static_cast<int64_t>(io::perf_now_us());
}

TransitionResult Session::transition(State to) {
	TransitionResult out;
	out.from = state_;
	out.to = to;
	if (state_ == to) {
		out.code = TransitionCode::NoOp;
		return out;
	}
	state_ = to;
	out.code = TransitionCode::Applied;
	return out;
}

TransitionResult Session::rejected(
		TransitionCode code, SessionError error) const {
	TransitionResult out;
	out.code = code;
	out.from = state_;
	out.to = state_;
	out.error = std::move(error);
	return out;
}

TransitionResult Session::configure_role(Role &role) {
	if (state_ != State::Unloaded &&
			state_ != State::Failed) {
		return rejected(TransitionCode::InvalidState,
				invalid_transition("role can change only while unloaded or failed"));
	}
	if (role_ == &role && kind_ == role.kind()) return rejected(TransitionCode::NoOp);
	role_ = &role;
	kind_ = role.kind();
	return {TransitionCode::Applied, state_, state_, {}};
}

TransitionResult Session::begin_connect() {
	if (kind_ != RoleKind::Joiner) {
		return rejected(TransitionCode::InvalidState,
				invalid_transition("only a joiner can enter Connecting"));
	}
	if (state_ != State::Unloaded &&
			state_ != State::Failed) {
		return rejected(TransitionCode::InvalidState,
				invalid_transition("connect requires Unloaded or Failed"));
	}
	last_error_ = {};
	reset_bank();
	return transition(State::Connecting);
}

TransitionResult Session::begin_load() {
	if (state_ != State::Unloaded &&
			state_ != State::Connecting &&
			state_ != State::Failed) {
		return rejected(TransitionCode::InvalidState,
				invalid_transition("load requires Unloaded, Connecting, or Failed"));
	}
	last_error_ = {};
	reset_bank();
	return transition(State::Loading);
}

TransitionResult Session::complete_load() {
	if (state_ != State::Loading) {
		return rejected(TransitionCode::InvalidState,
				invalid_transition("load completion requires Loading"));
	}
	last_error_ = {};
	reset_bank();
	// The mission starts inside the Game Loop mode's initialize, and the loop
	// reads its clock only after that returns, so the load is never banked
	// [orig: Game Loop mode @0x82F340 -> Game_StartMission; Game_MainLoop
	// clock read @0x52B75C]. The start also arms three frames whose render
	// time is never banked [orig: Game_StartMission @0x525e1f].
	start_rebase_frames_ = kStartRebaseFrames;
	rebase_clock_ = true;
	return transition(State::Running);
}

TransitionResult Session::fail(SessionError error) {
	if (!error) error = {SessionErrorCode::TickFailed, "session failed"};
	last_error_ = error;
	reset_bank();
	TransitionResult out = transition(State::Failed);
	out.error = std::move(error);
	return out;
}

void Session::latch_input(const FrameInput &input) {
	pending_input_.movement = input.player.movement;
	pending_input_.look_delta_x += input.player.look_delta_x;
	pending_input_.look_delta_y += input.player.look_delta_y;
	pending_input_.held_action_bits = input.player.held_action_bits;
	pending_input_.pressed_action_bits |= input.player.pressed_action_bits;
	pending_input_.sequence = input.player.sequence;
	latest_camera_ = input.camera;
}

TickInput Session::merged_tick_input(
		const FrameInput &input, bool consume_one_shots) {
	TickInput out;
	out.camera = latest_camera_;
	out.viewport_height = input.viewport_height;
	out.player = pending_input_;
	out.consume_one_shots = consume_one_shots;
	if (!consume_one_shots) {
		out.player.look_delta_x = 0.0f;
		out.player.look_delta_y = 0.0f;
		out.player.pressed_action_bits = 0;
	}
	return out;
}

void Session::consume_pending_one_shots() {
	pending_input_.look_delta_x = 0.0f;
	pending_input_.look_delta_y = 0.0f;
	pending_input_.pressed_action_bits = 0;
}

FrameOutcome Session::run_ticks(int32_t due, const FrameInput &input) {
	FrameOutcome out;
	out.status = FrameStatus::Ok;
	out.state = state_;
	const int64_t tick_start = now_us();
	bool consume_one_shots = true;
	for (int32_t i = 0; i < due; ++i) {
		TickInput tick_input = merged_tick_input(input, consume_one_shots);
		// [orig: Game_MainLoop @0x52ba21..0x52ba3a -- dword_24E0E80 = 1 when
		//  less than one 16 ms tick of backlog remains after this quantum, 0
		//  while catching up (and always 0 under g_CineFixedStepMode, which
		//  the port does not model)]
		tick_input.last_tick_of_batch = i + 1 == due;
		TickOutcome tick = run_one_tick(tick_input);
		if (tick.terminal()) {
			out.status = tick.status == TickStatus::SessionLost
					? FrameStatus::SessionLost : FrameStatus::Fatal;
			out.error = tick.error;
			fail(tick.error ? tick.error : SessionError{
					SessionErrorCode::TickFailed, "mission tick failed"});
			out.state = state_;
			break;
		}
		if (!tick.ran()) continue;
		out.ticks.push_back(std::move(tick));
		if (consume_one_shots) {
			consume_pending_one_shots();
			consume_one_shots = false;
		}
	}
	out.perf.tick_us = now_us() - tick_start;
	out.perf.ticks = out.ticks_run();
	return out;
}

// One fixed tick: the shared input prologue through the role, the role's
// tick, then the outcome and the shell's observer.
TickOutcome Session::run_one_tick(const TickInput &input) {
	TickOutcome out;
	if (role_ == nullptr || role_->kernel() == nullptr) {
		out.status = TickStatus::Fatal;
		out.error = {SessionErrorCode::TickFailed, "no role is bound to the session"};
		return out;
	}
	role_->kernel()->world.rules.last_tick_of_batch = input.last_tick_of_batch;
	role_->apply_input(input);
	const int64_t tick_start = now_us();
	role_->run_tick(input);
	out.tick_us = now_us() - tick_start;
	out.net_us = role_->last_net_us();
	out.logic_tick = static_cast<int32_t>(role_->kernel()->world.logic_tick);
	if (observer_ != nullptr) observer_->after_tick();
	SessionError lost;
	if (role_->session_lost(lost)) {
		out.status = TickStatus::SessionLost;
		out.error = lost;
		return out;
	}
	out.status = TickStatus::Ran;
	if (observer_ != nullptr && !observer_->accept_tick(out)) {
		out.status = TickStatus::SessionLost;
		out.error = {SessionErrorCode::SessionLost, "the tick observer cancelled the batch"};
	}
	return out;
}

void Session::step_hud_radar_frame() {
	if (role_ == nullptr || role_->kernel() == nullptr) return;
	static_assert(kHudRadarGatesDefault == hud::HudFrameCompiler::kRadarGatePass,
			"the default gates run the pass");
	ClientRuntime *runtime = role_->client_runtime();
	// The pass's spawn-success early-out [orig: g_SpawnSuccessGate
	// @0x5a8084], live; the hud_detail-3 early-out and the map site are the
	// embedder's HUD's.
	const bool spawn_gate = runtime != nullptr && runtime->state().spawn_success_gate;
	const bool pass_runs =
			(hud_radar_gates_ & hud::HudFrameCompiler::kRadarGatePass) != 0u && !spawn_gate;
	const bool map_site = (hud_radar_gates_ & hud::HudFrameCompiler::kRadarGateMapSite) != 0u;
	step_hud_radar(*role_->kernel(), runtime, pass_runs, map_site, state_ == State::Paused,
			hud_radar_);
}

FrameOutcome Session::advance(const FrameInput &input) {
	FrameOutcome out;
	out.state = state_;
	if (state_ != State::Running) {
		out.status = FrameStatus::NotRunning;
		last_perf_ = out.perf;
		// The paused frame still runs its HUD pass: no tick ran, so nothing
		// ages, and the menu pause holds the lock tone.
		if (state_ == State::Paused) step_hud_radar_frame();
		return out;
	}
	latch_input(input);
	// The frame's start stamp, in whole milliseconds like GetTickCount
	// [orig: Game_MainLoop @0x52b798 / @0x52b7ae (after the frame lock
	//  @0x52b8d7)].
	const int64_t frame_start_ms = now_us() / 1000;
	// A mission-start frame re-based the clock once it had rendered, so this
	// frame banks only the time since that render [orig: Game_MainLoop
	// @0x52bac8..0x52bad2].
	const double elapsed = rebase_clock_ && input.since_render_seconds >= 0.0
			? input.since_render_seconds : input.delta_seconds;
	rebase_clock_ = false;
	const int32_t due = accumulator_.bank(elapsed);
	// The FR counter is published before the drain runs this frame's ticks
	// [orig: Game_MainLoop g_StatsAvgFps store @0x52B98F, drain @0x52BA08].
	if (role_ != nullptr) {
		role_->observe_frame_rate(accumulator_.average_fps());
		role_->observe_cpu_share(accumulator_.cpu_percent());
	}
	out = run_ticks(due, input);
	// The CPU share's inputs: this frame's work from its start stamp to the
	// end of the drain, and the updates the drain ran [orig: @0x52ba4f;
	// @0x52ba9b..0x52baa1].
	accumulator_.record_frame_work(
			static_cast<uint32_t>(now_us() / 1000 - frame_start_ms), out.ticks_run());
	// Every logic update counts toward the 62-update second; each second
	// publishes the frames rendered since [orig: Game_ProcessMainFrame
	// @0x5267ab..0x5267df], and this frame's render then counts
	// [orig: GameLoop_RenderFrame @0x521cf9].
	for (int32_t i = 0; i < out.ticks_run(); ++i) {
		if (++second_update_count_ >= io::kTicksPerSecondInt) {
			second_update_count_ -= io::kTicksPerSecondInt;
			frames_last_second_ = frames_rendered_;
			frames_rendered_ = 0;
		}
	}
	++frames_rendered_;
	if (role_ != nullptr)
		role_->observe_frame_statistics(frames_last_second_, accumulator_.cpu_percent());
	// Each of the first frames drawn after the mission start counts down and
	// raises the re-base flag [orig: Render_ProcessMainSceneFrame
	// @0x5caeff..0x5caf0e].
	if (start_rebase_frames_ > 0) {
		--start_rebase_frames_;
		rebase_clock_ = true;
	}
	last_perf_ = out.perf;
	// The frame's HUD pass follows the drain [orig: Game_MainLoop's
	// Game_ProcessMainFrame drain, then the render's HUD_RenderAllOverlays].
	if (!out.terminal()) step_hud_radar_frame();
	return out;
}

FrameOutcome Session::step_once(const FrameInput &input) {
	FrameOutcome out;
	out.state = state_;
	if (state_ != State::Paused ||
			kind_ != RoleKind::SinglePlayer) {
		out.status = FrameStatus::NotRunning;
		last_perf_ = out.perf;
		return out;
	}
	latch_input(input);
	out = run_ticks(1, input);
	last_perf_ = out.perf;
	return out;
}

FrameOutcome Session::drive_one(const FrameInput &input) {
	FrameOutcome out;
	out.state = state_;
	const bool local_paused = state_ == State::Paused &&
			kind_ == RoleKind::SinglePlayer;
	if (state_ != State::Running && !local_paused) {
		out.status = FrameStatus::NotRunning;
		last_perf_ = out.perf;
		return out;
	}
	latch_input(input);
	out = run_ticks(1, input);
	last_perf_ = out.perf;
	return out;
}

TransitionResult Session::pause() {
	if (kind_ != RoleKind::SinglePlayer) {
		return rejected(TransitionCode::RejectedForNetworkRole,
				{SessionErrorCode::NetworkRoleLocked,
						"network sessions cannot pause"});
	}
	if (state_ == State::Paused)
		return rejected(TransitionCode::NoOp);
	if (state_ != State::Running) {
		return rejected(TransitionCode::InvalidState,
				invalid_transition("pause requires Running"));
	}
	reset_bank();
	return transition(State::Paused);
}

TransitionResult Session::resume() {
	if (state_ == State::Running)
		return rejected(TransitionCode::NoOp);
	if (state_ != State::Paused) {
		return rejected(TransitionCode::InvalidState,
				invalid_transition("resume requires Paused"));
	}
	reset_bank();
	return transition(State::Running);
}

TransitionResult Session::reset_to_baseline() {
	if (kind_ != RoleKind::SinglePlayer) {
		return rejected(TransitionCode::RejectedForNetworkRole,
				{SessionErrorCode::NetworkRoleLocked,
						"network sessions cannot reset"});
	}
	if (state_ != State::Running &&
			state_ != State::Paused) {
		return rejected(TransitionCode::InvalidState,
				invalid_transition("reset requires Running or Paused"));
	}
	const State from = state_;
	SessionError error;
	if (role_ == nullptr || !role_->reset_to_baseline(error)) {
		if (!error) error = {SessionErrorCode::TickFailed,
				"mission baseline reset failed"};
		return fail(error);
	}
	reset_bank();
	// A restart is a mission start [orig: Game_RestartRoundSP @0x5263a0 ->
	// Game_StartMission @0x525e1f].
	start_rebase_frames_ = kStartRebaseFrames;
	rebase_clock_ = true;
	state_ = State::Paused;
	return {TransitionCode::Applied, from,
			State::Paused, {}};
}

TransitionResult Session::close() {
	if (state_ == State::Unloaded)
		return rejected(TransitionCode::NoOp);
	const State from = state_;
	state_ = State::Stopping;
	if (role_ != nullptr) role_->close();
	reset_bank();
	start_rebase_frames_ = 0;
	rebase_clock_ = false;
	last_error_ = {};
	hud_radar_gates_ = kHudRadarGatesDefault;
	hud_radar_ = {};
	state_ = State::Unloaded;
	return {TransitionCode::Applied, from, state_, {}};
}

void Session::reset_bank() {
	accumulator_.reset();
	pending_input_ = {};
	latest_camera_ = {};
	last_perf_ = {};
}

} // namespace opennova::inmatch
