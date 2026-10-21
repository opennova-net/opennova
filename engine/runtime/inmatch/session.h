#pragma once

#include <runtime/hud/hud_minimap.h> // HudMinimapRadar
#include <runtime/world/local_player_view.h>
#include <runtime/world/player_input.h>
#include <runtime/world/player_look.h> // MouseAxisPump
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

// One input sample: the outer frame's, or one tick's (TickInputSource). Held
// state is replaced by the newest sample; look deltas and one-shot actions
// accumulate until a logic tick consumes them.
struct InputPacket {
	world::PlayerInput movement;
	// The mouse cursor's offset in whole pixels (screen sense) since the
	// shell's previous sample; the session's mouse pump reads it in the frame's
	// first tick and spreads it over the frame's ticks (world::MouseAxisPump).
	int32_t cursor_dx = 0;
	int32_t cursor_dy = 0;
	// Axis input added at this tick's input frame as it stands (the scripted
	// device's look, which onHook's driver adds to the axis word); the tick's
	// mouse-pump share joins it in TickInput.
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

// The shell's per-tick input frame. Retail runs its input frame in every
// logic tick, not once per rendered frame: the main loop's drain calls the
// mode's update once per 16 ms tick it banked, and each update pumps the
// window messages (the key state and the key-event ring the window procedure
// fills) and then runs Input_ProcessFrame(g_CurrentTick), which dequeues the
// ring and reads the key state for that tick alone, unless a menu screen that
// does not pass input through is up (the death and stat screens pass it
// through: the menu's scene teardown runs the input frame under them). Each
// update steps g_CurrentTick unless the pause word is up. A catch-up tick
// therefore sees the device as it stands at that tick (D-NET-NEW-LI-1).
// [orig: Game_MainLoop @0x52B630 (the mode's +0x24 update call @0x52BA58 behind
//  the phase gate @0x52BA40, the Game Loop mode @0x82F340's slot holding
//  Game_ProcessMainFrame); Game_ProcessMainFrame @0x5263F0 (the pause word's test
//  @0x5265A0..0x5265A6 around ++g_CurrentTick @0x5265B4, the Game_PumpWindowMessages
//  call @0x526608, the UI_IsMenuScreenOpen call @0x52660D, the
//  Input_ProcessFrame(g_CurrentTick) call @0x52661D); UI_TeardownScene @0x54E650
//  (g_MenuInputPassThrough tested @0x54E686, the Input_ProcessFrame call @0x54E695),
//  the word raised through UI_SetMenuInputPassThrough @0x54B980 by DeathScreen_UpdateUI
//  @0x553150 (the call @0x55316E) and StatScreen_ShowCallback @0x562840 (the calls
//  @0x562877 / @0x5628D6); Game_WindowProc @0x7624C0 -> Input_HandleKeyPress
//  @0x760C90 (the key state @0x760CBB, the Input_QueueKeyEvent call @0x760CC2);
//  Input_ProcessKeyboardEvents @0x49D1F0 (the Input_DequeueKeyEvent calls
//  @0x49D2D2 / @0x49D4A0)]
// The frame's own sample (FrameInput::player) is its first tick's input frame;
// every later tick the frame banked asks the source for its own, keyed to the
// logic tick it runs as (World::logic_tick at the tick's head, which is
// retail's g_CurrentTick once the tick's increment ran). The mouse motion
// rides the frame's sample, as retail reads the cursor only in the frame's
// first update [orig: Game_ProcessMainFrame @0x5263F0 (the g_MainLoopFirstUpdate
//  test @0x52640E, the Input_PumpAndCenterCursor call @0x526458); Game_MainLoop
//  @0x52B630 (g_MainLoopFirstUpdate raised @0x52B9FC, dropped after each update
//  @0x52BA5E); Input_PumpAndCenterCursor @0x7616D0].
class TickInputSource {
public:
	virtual ~TickInputSource() = default;
	virtual InputPacket sample_tick(uint32_t logic_tick) = 0;
};

struct FrameInput {
	double delta_seconds = 0.0;
	// Wall-clock since the previous frame finished rendering (its present),
	// or negative when the shell has not sampled one. Banked instead of
	// delta_seconds on a frame after a mission-start frame (Session::advance).
	double since_render_seconds = -1.0;
	CameraSample camera;
	InputPacket player;
	// The shell's input frame for this frame's later ticks, valid for this
	// advance call only; null when the shell has no device to sample (a
	// dedicated host, a probe's direct frame): every banked tick then reuses
	// this frame's sample.
	TickInputSource *tick_source = nullptr;
	// The host's frozen loaded-model list count (TickInput::loaded_model_list_count).
	uint32_t loaded_model_list_count = 0;
};

struct TickInput {
	CameraSample camera;
	InputPacket player;
	// True on a tick that runs its own input frame: the frame's first
	// successful tick, and each later tick that sampled the shell's
	// TickInputSource. Targets use this to consume edge-triggered actions
	// exactly once; a catch-up tick with no source of its own reuses the held
	// state with no edges.
	bool consume_one_shots = false;
	// The count of the loaded-model list the host froze at its mission start,
	// which the host wraps its S2C 0x68 cursor at (0 until a list froze)
	// [orig: Server_SendLoadedModelPageRequest @0x511375 / @0x511391 reads
	//  g_LoadedModelChallengeCount through CEffectWorld_GetLoadedModelPage @0x5B1578].
	uint32_t loaded_model_list_count = 0;
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
	// The Respawn key's bodiless C2S 0x41 for this role, past request_respawn's
	// gates: a session peer queues it on its connection. False = nothing sent.
	virtual bool send_respawn_request() { return false; }
	// A stance select past request_stance's head: the key's own refusals, then the
	// C2S 0x1D on the role's client connection (JoinerRole, HostRole); a role with
	// no client sends nothing. 0 stand / 1 crouch / 2 prone. False = nothing sent.
	virtual bool send_stance_request(int stance) {
		(void)stance;
		return false;
	}
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
	// The dispatcher's head for an action row whose flags carry bit 0, 0x400 and
	// 0x8000000 (the Respawn row 68, the stance rows 9..11 and the seat rows 17..26,
	// 0x0C000C01; the scope row 105, 0x4C000C01; the mouse look rows 0x0C100425 /
	// 0x0C200425): the local entity is there, and none of a dead local player, the
	// round-over gate, the pre-round timer or the deploy overlay holds. True = the row
	// reaches its arm.
	bool local_action_head_admits();
	// The Respawn key (dispatch 55) past the row's head gates: the local
	// player's input reset, then in a session the role's 0x41. True = sent.
	bool request_respawn();
	// The stance selects (dispatch 169 crouch / 170 prone / 172 stand), a key's or a
	// console-queued one, past their rows' head: the role's C2S 0x1D
	// (send_stance_request). 0 stand / 1 crouch / 2 prone. True = sent.
	bool request_stance(int stance);
	// A console-queued input action (ClientRuntime::queue_console_action), dispatched
	// by its action code at the input frame: the arms the role owns (the Respawn
	// key's code, the medic call's, the stance selects'), every other code to the
	// embedder's dispatch (ClientRuntime::set_console_action_sink).
	void dispatch_console_action(int32_t code, int32_t p1, int32_t p2, int32_t p3);
	// Player_ResetLocalInput, which the Respawn key (case 55) and the Escape key (case
	// 18, ahead of its `!is_in_session && g_RoundOver` test; the embedder's escape runs it)
	// run: the input reset (D-NET-560) [orig: Input_HandleActionBinding @0x49AD40 - case 55
	//  @0x49B20B, case 18 Player_ResetLocalInput @0x49B234 ahead of the test
	//  @0x49B23B..0x49B249].
	void reset_local_input();
	// The deploy action (dispatch 12) with its pick's [i16] wire value: a dead
	// local player under permanent death converts to a spectator (the C2S
	// 0x51) when spectators are allowed and does nothing when they are not;
	// every other press sends the C2S 0x0E pick. Both send arms run the shared
	// head first: the input reset (the deferred queue here, the devices through
	// take_input_reset) and the dialog reset (D-NET-685). True = queued.
	bool request_deployment_pick(uint16_t wire_value);
	// Input_ResetAllState run outside the input frame, at the armory screen's ACCEPT and
	// CANCEL head: the event queue's records (the console's queued actions) and the
	// deferred queue go, and the embedder's devices owe their key queue, key state and
	// key-press latch reset (take_input_reset).
	// [orig: Input_ResetAllState @0x497ED0 - Input_ResetKeyQueue, Input_ClearKeyState, the
	//  event queue's memset @0x497EE6, the deferred queue's @0x497EF7, the key-press latch
	//  @0x497F08; WeaponLoadout_ApplyFromBuffer @0x565CD0 - the guards @0x565CE4..0x565CF8,
	//  the call @0x565D01 ahead of the CANCEL test @0x565D06]
	void reset_all_input_state();
	// Whether a role action owes the embedder's device input reset (the key state
	// and the key-press latches of Input_ResetAllState); reading clears it.
	bool take_input_reset() {
		const bool due = input_reset_pending_;
		input_reset_pending_ = false;
		return due;
	}
	// What the view arbiter reads from the session (death screen, end round,
	// the death camera), as plain values off the role's replica runtime, plus
	// the retail is_in_session fact (the world's mp_session rule: single
	// player, the in-process listen server included, is outside the session,
	// whatever replica runtime it folds).
	static world::LocalViewSessionInputs view_session_inputs_for(
			const ClientRuntime *runtime, bool joiner, bool local_dead, bool in_session);

protected:
	mission::MissionKernel *kernel_ = nullptr;
	bool input_reset_pending_ = false; // take_input_reset
	// A reset ran in this input frame (Input_ResetAllState's queue memset): the
	// frame's later dispatches are gone (apply_input; D-NET-560).
	bool input_queue_reset_ = false;
};

// The held / pressed action bits of InputPacket, the retail action rows the
// frame input carries besides the movement keys.
enum HeldAction : uint32_t {
	HELD_FIRE = 1u << 0,
	// The ToSpecial keys' live state, which its dispatch reads (the hold-swap
	// press or release; Input_HandleActionBinding_0 case 0xDC @0x4E1161..0x4E1181).
	HELD_TO_SPECIAL = 1u << 1,
	// A menu screen (INGAME, WEAPON, CMAP) is up: no input frame runs, so the
	// deferred input queue is not flushed (the close's reset then drops it)
	// [orig: Game_ProcessMainFrame @0x52660D / @0x52661D; Input_ProcessFrame ->
	//  Input_FlushDeferredEvents @0x49D591, its one caller].
	HELD_MENU_OPEN = 1u << 2,
};
enum PressedAction : uint32_t {
	PRESSED_FIRE = 1u << 0,
	PRESSED_RELOAD = 1u << 1,
	// The dead player's medic call edge (the MedicReq action row; retail
	// Input_HandleActionBinding case 217 @0x49b4b4).
	PRESSED_MEDIC_REQUEST = 1u << 2,
	// A ToSpecial dispatch: a press or a release edge of its keys.
	PRESSED_TO_SPECIAL = 1u << 3,
	// The Respawn row's press edge (catalog row 68; retail
	// Input_HandleActionBinding case 55 @0x49B20B).
	PRESSED_RESPAWN = 1u << 4,
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
	void latch_packet(const InputPacket &packet);
	void consume_pending_one_shots();
	// One update of the main frame's mouse pump: the first update of `due` takes the
	// cursor offset the frame banked; `axis` is the update's two-tap sum.
	void step_mouse_pump(int32_t update, int32_t due, bool menu_open, int32_t (&axis)[2]);
	FrameOutcome run_ticks(int32_t due, const FrameInput &input);
	TickOutcome run_one_tick(const TickInput &input);
	void step_cine_render_frame();
	// The paused frame's banked frames step the world's main-frame counter.
	void step_paused_frame_counters(const FrameInput &input);
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
	// The main frame's mouse pump; its words never reset with the bank. Retail's are
	// process words only the main frame writes; these live with the session, which
	// differs only where a mission ends with no menu frame between it and the next
	// one (world::MouseAxisPump carries the witness).
	world::MouseAxisPump mouse_pump_;
	CameraSample latest_camera_;
	SessionError last_error_;
	FramePerf last_perf_;
	// The HUD pass's gates as the embedder last handed them, and the radar
	// step's last snapshot.
	uint32_t hud_radar_gates_ = kHudRadarGatesDefault;
	hud::HudMinimapRadar hud_radar_;
};

} // namespace opennova::inmatch
