#pragma once

#include <world/player_input.h>
#include <world/tick_accumulator.h>

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::inmatch {

enum class State : uint8_t {
	Unloaded = 0,
	Connecting,
	Loading,
	Running,
	Paused,
	Stopping,
	Failed,
};

enum class Role : uint8_t {
	SinglePlayer = 0,
	ListenHost,
	Joiner,
	DedicatedHost,
};

enum class SessionErrorCode : uint8_t {
	None = 0,
	InvalidTransition,
	NetworkRoleLocked,
	LoadFailed,
	SessionLost,
	TickFailed,
};

struct SessionError {
	SessionErrorCode code = SessionErrorCode::None;
	std::string message;

	explicit operator bool() const { return code != SessionErrorCode::None; }
};

enum class TransitionCode : uint8_t {
	Applied = 0,
	NoOp,
	InvalidState,
	RejectedForNetworkRole,
	Failed,
};

struct TransitionResult {
	TransitionCode code = TransitionCode::NoOp;
	State from = State::Unloaded;
	State to = State::Unloaded;
	SessionError error;

	bool applied() const { return code == TransitionCode::Applied; }
};

// One outer-frame input sample. Held state is replaced by the newest sample;
// look deltas and one-shot actions accumulate until a logic tick consumes them.
struct InputPacket {
	world::PlayerInput movement;
	float look_delta_x = 0.0f;
	float look_delta_y = 0.0f;
	// Held actions survive every catch-up tick; pressed actions and look deltas
	// are consumed by the first successful tick only.
	uint32_t held_action_bits = 0;
	uint32_t pressed_action_bits = 0;
	uint64_t sequence = 0;
};

struct CameraSample {
	float position[3] = {0.0f, 0.0f, 0.0f};
	float forward[3] = {0.0f, 0.0f, 1.0f};
	bool listener_valid = false;
};

struct FrameInput {
	double delta_seconds = 0.0;
	CameraSample camera;
	InputPacket player;
};

struct TickInput {
	CameraSample camera;
	InputPacket player;
	// True only until the first successful tick in an outer frame. Targets use
	// this to consume edge-triggered actions exactly once across catch-up.
	bool consume_one_shots = false;
};

enum class TickStatus : uint8_t {
	Ran = 0,
	Declined,
	SessionLost,
	Fatal,
};

struct TickOutcome {
	TickStatus status = TickStatus::Declined;
	int32_t logic_tick = 0;
	SessionError error;

	bool ran() const { return status == TickStatus::Ran; }
	bool terminal() const {
		return status == TickStatus::SessionLost || status == TickStatus::Fatal;
	}
};

enum class FrameStatus : uint8_t {
	Ok = 0,
	NotRunning,
	SessionLost,
	Fatal,
};

struct FramePerf {
	int64_t frame_us = 0;
	int64_t tick_us = 0;
	int32_t ticks = 0;
};

struct FrameOutcome {
	FrameStatus status = FrameStatus::NotRunning;
	State state = State::Unloaded;
	std::vector<TickOutcome> ticks;
	FramePerf perf;
	SessionError error;

	int32_t ticks_run() const { return static_cast<int32_t>(ticks.size()); }
	bool terminal() const {
		return status == FrameStatus::SessionLost || status == FrameStatus::Fatal;
	}
};

	// The session's one real internal seam. Godot and the headless server both
// provide an adapter; callers never see the former semantic callback lattice.
class TickTarget {
public:
	virtual ~TickTarget() = default;
	virtual TickOutcome advance_mission_tick(const TickInput &input) = 0;
	virtual bool reset_mission_to_baseline(SessionError &error) = 0;
	virtual void close_mission() = 0;
};

class Session {
public:
	explicit Session(TickTarget &target,
			Role role = Role::SinglePlayer);

	State state() const { return state_; }
	Role role() const { return role_; }
	const SessionError &last_error() const { return last_error_; }
	const FramePerf &last_perf() const { return last_perf_; }

	TransitionResult configure_role(Role role);
	TransitionResult begin_connect();
	TransitionResult begin_load();
	TransitionResult complete_load();
	TransitionResult fail(SessionError error);

	FrameOutcome advance(const FrameInput &input);
	FrameOutcome step_once(const FrameInput &input = {});
	// Deterministic external-clock drive used by focused native/Godot probes.
	// Unlike step_once(), a running network role may use it; it still routes
	// through the same target and terminal-state handling as realtime cadence.
	FrameOutcome drive_one(const FrameInput &input = {});

	TransitionResult pause();
	TransitionResult resume();
	TransitionResult reset_to_baseline();
	TransitionResult close();

	void reset_bank();

private:
	TransitionResult transition(State to);
	TransitionResult rejected(TransitionCode code, SessionError error = {}) const;
	TickInput merged_tick_input(const FrameInput &input, bool consume_one_shots);
	void latch_input(const FrameInput &input);
	void consume_pending_one_shots();
	FrameOutcome run_ticks(int32_t due, const FrameInput &input);
	static int64_t now_us();

	TickTarget &target_;
	Role role_;
	State state_ = State::Unloaded;
	world::TickAccumulator accumulator_;
	InputPacket pending_input_;
	CameraSample latest_camera_;
	SessionError last_error_;
	FramePerf last_perf_;
};

} // namespace opennova::inmatch
