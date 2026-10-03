#pragma once

#include <runtime/hud/hud_minimap.h> // HudMinimapRadar
#include <runtime/world/local_player_view.h>
#include <runtime/world/player_input.h>
#include <runtime/world/tick_accumulator.h>

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::mission {
class MissionKernel;
}

namespace opennova::inmatch {

class ClientRuntime;

enum class State : uint8_t {
	Unloaded = 0,
	Connecting,
	Loading,
	Running,
	Paused,
	Stopping,
	Failed,
};

enum class RoleKind : uint8_t {
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
	// A dedicated host's round-end linger expired: retail stores the mission
	// exit reason and leaves the mission; a headless host exits its loop.
	// [orig: Server_TickUpdate @0x51DB57/@0x51DB63 g_MissionExitReason 4/3]
	RoundEnded,
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
	// Wall-clock since the previous frame finished rendering (its present),
	// or negative when the shell has not sampled one. Banked instead of
	// delta_seconds on a frame after a mission-start frame (Session::advance).
	double since_render_seconds = -1.0;
	CameraSample camera;
	InputPacket player;
	int32_t viewport_height = 0;
};

struct TickInput {
	CameraSample camera;
	InputPacket player;
	// True only until the first successful tick in an outer frame. Targets use
	// this to consume edge-triggered actions exactly once across catch-up.
	bool consume_one_shots = false;
	// The renderer viewport height a listen host wraps its S2C 0x68 cursor
	// against (0 = headless / no renderer, the seam left unset).
	int32_t viewport_height = 0;
	// True on the last logic tick of this frame's batch, false while the bank
	// catches up: the outer loop's "<16 ms of backlog left" flag the fire-loop
	// emitter and the lock tone read (World::rules.last_tick_of_batch).
	bool last_tick_of_batch = true;
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
	int64_t tick_us = 0; // the role's tick alone (no input prologue, no observer)
	int64_t net_us = 0;  // the tick's wire leg, as the role measured it
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
// The shell's per-tick observer. after_tick runs after every tick that ran
// (the shell's per-tick device gates); accept_tick then offers the outcome of
// a tick that did not lose the session, and a false return ends the frame's
// batch as SessionLost (the shell's presentation pipeline declined to go on).
class TickObserver {
public:
	virtual ~TickObserver() = default;
	virtual void after_tick() {}
	virtual bool accept_tick(const TickOutcome &tick) { (void)tick; return true; }
};

// One role's tick over the kernel it binds: the SP/no-net frame, the listen
// or dedicated host frame, or the joiner frame -- each the leg order it was
// witnessed with, line for line (ADR 0043 d3). Session::run_one_tick applies
// the frame's input through the role first (the death screen's input filter
// over the role's replica and the medic-call send are the role's facts), then
// runs the role's tick.
class Role {
public:
	virtual ~Role() = default;
	virtual RoleKind kind() const = 0;
	virtual void bind(mission::MissionKernel &kernel) { kernel_ = &kernel; }
	mission::MissionKernel *kernel() const { return kernel_; }
	// The dead player's medic call (C2S 0x2E) for this role; the entity, dead
	// and cooldown gates are the shared prologue's. False = nothing sent.
	virtual bool send_medic_request() { return false; }
	virtual void run_tick(const TickInput &input) = 0;
	virtual bool session_lost(SessionError &error) const { (void)error; return false; }
	virtual bool reset_to_baseline(SessionError &error) = 0;
	virtual void close() = 0;
	// The client-side replica runtime this role folds (the HostClient's or the
	// joiner's); null for the bare local role and a dedicated host.
	virtual ClientRuntime *client_runtime() { return nullptr; }
	// The last tick's wire leg, for the shell's stats board.
	virtual int64_t last_net_us() const { return 0; }
	// The main loop's measured frame rate (the FR counter's g_StatsAvgFps,
	// world::TickAccumulator::average_fps), handed over by the session once
	// per banked frame before that frame's ticks run. The base passes it to the
	// role's replica runtime (the client quality window's frame-pressure term);
	// a host also hands it to its server context.
	virtual void observe_frame_rate(int32_t fps);
	// The same window's CPU share (g_StatsCpuPercent, world::TickAccumulator::
	// cpu_percent), handed over beside the frame rate, after the bank and before
	// the drain, as retail publishes both ahead of its logic updates [orig:
	// Game_MainLoop @0x52B948 / @0x52B98F, the drain @0x52BA08]. A joiner's
	// C2S 0x0C carries both; the base keeps neither.
	virtual void observe_cpu_share(int32_t cpu_percent) { (void)cpu_percent; }
	// The main loop's frame statistics after each frame (the frames drawn in
	// the last 62 logic updates, the window's CPU share): a host hands them to
	// its server context, where its status page reads them
	// (Session::frame_statistics carries the witness).
	virtual void observe_frame_statistics(int32_t frames_last_second, int32_t cpu_percent) {
		(void)frames_last_second;
		(void)cpu_percent;
	}
	// The kernel boot's net bring-up (KernelBootOptions::bringup_net_session),
	// run between the world wiring and the system registration [orig:
	// SinglePlayer_StartMission @0x561af0]: the host stands its session up
	// from its staged bring-up record, the joiner (re)builds its non-authority
	// ClientRuntime, the bare local role installs nothing. True when a FRESH
	// joiner ClientRuntime replaced the previous one (the embedder re-installs
	// its retained join inputs on it); the host's own HostClient view is
	// rebuilt every bring-up and carries no embedder inputs, so it reports
	// false.
	virtual bool bring_up() { return false; }

	// The frame's input onto the local player, shared by every role: movement
	// keys, mouse look, the fire/reload bits and the medic edge [orig: the
	// per-frame input dispatch feeding Player_PackInputStateToEntity @0x4df450
	// and Input_HandleActionBinding case 217 @0x49b4b4].
	void apply_input(const TickInput &input);
	// The medic call past the session/entity gates: a dead local player with
	// the cooldown at zero sends through the role and stamps the cooldown.
	bool request_medic();
	// What the view arbiter reads from the session (death screen, end round,
	// the death camera), as plain values off the role's replica runtime, plus
	// the retail is_in_session fact (the world's mp_session rule: single
	// player, the in-process listen server included, is outside the session,
	// whatever replica runtime it folds).
	static world::LocalViewSessionInputs view_session_inputs_for(
			const ClientRuntime *runtime, bool joiner, bool local_dead, bool in_session);

protected:
	mission::MissionKernel *kernel_ = nullptr;
};

// The held / pressed action bits of InputPacket, the retail action rows the
// frame input carries besides the movement keys.
enum HeldAction : uint32_t {
	HELD_FIRE = 1u << 0,
	// The ToSpecial keys' live state, which its dispatch reads (the hold-swap
	// press or release; Input_HandleActionBinding_0 case 0xDC @0x4E1161..0x4E1181).
	HELD_TO_SPECIAL = 1u << 1,
};
enum PressedAction : uint32_t {
	PRESSED_FIRE = 1u << 0,
	PRESSED_RELOAD = 1u << 1,
	// The dead player's medic call edge (the MedicReq action row; retail
	// Input_HandleActionBinding case 217 @0x49b4b4).
	PRESSED_MEDIC_REQUEST = 1u << 2,
	// A ToSpecial dispatch: a press or a release edge of its keys.
	PRESSED_TO_SPECIAL = 1u << 3,
};


class Session {
public:
	// A session without a role can only hold state; configure_role binds the
	// strategy the ticks run through.
	Session() = default;
	explicit Session(Role &role);
	State state() const { return state_; }
	RoleKind kind() const { return kind_; }
	Role *role() const { return role_; }
	void set_tick_observer(TickObserver *observer) { observer_ = observer; }
	const SessionError &last_error() const { return last_error_; }
	const FramePerf &last_perf() const { return last_perf_; }
	// The main loop's frame statistics the authority's status page shows:
	// the frames rendered during the last 62 logic ticks and the CPU share
	// of the frame-rate window (world::TickAccumulator::cpu_percent).
	// [orig: dword_24C193C — Game_ProcessMainFrame @0x5267ab..0x5267df counts
	//  every logic update into dword_24D6130 and at 62 publishes
	//  dword_24C1938, which GameLoop_RenderFrame @0x521cf9 increments once per
	//  rendered frame; g_StatsCpuPercent]
	struct FrameStatistics {
		int32_t frames_last_second = 0;
		int32_t cpu_percent = 0;
	};
	FrameStatistics frame_statistics() const {
		return {frames_last_second_, accumulator_.cpu_percent()};
	}

	// THE FRAME'S RADAR STEP. Retail's HUD pass runs on every rendered
	// frame, the in-game menu's included (the menu only stops the
	// single-player ticks), and its radar update ages the local player's
	// contacts and the retained minimap banks (the 0x6B designations, the
	// transient slots, the linked markers' lapse) by the ticks since its last
	// run. The session runs that step after each frame's tick drain, paused
	// or not, so the aging never waits on the embedder's HUD
	// (inmatch::step_hud_radar carries the legs). The embedder hands over the
	// HUD pass's own gates whenever its HUD compiles
	// (hud::HudFrameCompiler::radar_frame_gates: the hud_detail-3 early-out
	// and the corner map's update site; the pass runs until told otherwise);
	// the spawn-success early-out is read live off the role's replica
	// runtime, and the in-game menu pause is the Paused state.
	// [orig: Render_ProcessMainSceneFrame @0x5cad04 -> HUD_RenderAllOverlays
	//  @0x5a8070 (skipped only under a cine fade or the CMAP screen,
	//  @0x5ca17d..0x5ca190) -> Radar_UpdateContacts @0x5a817d ->
	//  MapOverlay_UpdateTimers @0x59a9ce; the menu pause dword_A87050 is
	//  raised only outside a session, UI_OptionsScreenInit @0x554dcf..0x554dd8]
	void set_hud_radar_gates(uint32_t gates) { hud_radar_gates_ = gates; }
	uint32_t hud_radar_gates() const { return hud_radar_gates_; }
	// The last frame's radar snapshot, the HUD's bit-10 legs' input.
	const hud::HudMinimapRadar &hud_radar() const { return hud_radar_; }
	// The gate bits' default (hud::HudFrameCompiler::kRadarGatePass): the pass
	// runs, the corner map's site does not.
	static constexpr uint32_t kHudRadarGatesDefault = 1u;

	TransitionResult configure_role(Role &role);
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
	TickOutcome run_one_tick(const TickInput &input);
	void step_hud_radar_frame();
	static int64_t now_us();

	Role *role_ = nullptr;
	RoleKind kind_ = RoleKind::SinglePlayer;
	TickObserver *observer_ = nullptr;
	State state_ = State::Unloaded;
	world::TickAccumulator accumulator_;
	// The mission-start frames still to draw (dword_24C1174) and whether the
	// last one re-based the clock after its render (dword_24E1F30).
	static constexpr int32_t kStartRebaseFrames = 3;
	int32_t start_rebase_frames_ = 0;
	bool rebase_clock_ = false;
	// [orig: dword_24D6130 (the 62-update countdown), dword_24C1938 (frames
	//  rendered since), dword_24C193C (the published count) — process-lifetime
	//  words nothing else resets]
	int32_t second_update_count_ = 0;
	int32_t frames_rendered_ = 0;
	int32_t frames_last_second_ = 0;
	InputPacket pending_input_;
	CameraSample latest_camera_;
	SessionError last_error_;
	FramePerf last_perf_;
	// The HUD pass's gates as the embedder last handed them, and the radar
	// step's last snapshot.
	uint32_t hud_radar_gates_ = kHudRadarGatesDefault;
	hud::HudMinimapRadar hud_radar_;
};

} // namespace opennova::inmatch
