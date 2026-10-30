#pragma once

#include <runtime/controls/input_event_queue.h>
#include <runtime/hud/hud_minimap.h> // HudMinimapRadar
#include <runtime/world/local_player_view.h>
#include <runtime/world/player_input.h>
#include <runtime/world/player_look.h> // MouseAxisPump
#include <runtime/world/tick_accumulator.h>

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace opennova::mission {
class MissionKernel;
}

namespace opennova::hud {
struct HudToggleState;
}

namespace opennova::inmatch {

class ClientRuntime;
struct ClientProcessState;
struct NapiNPServerCtx;
struct RoleView;

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
// therefore sees the device as it stands at that tick (D-NET-998).
// [orig: Game_MainLoop @0x52B630 (the mode's +0x24 update call @0x52BA58 behind
//  the phase gate @0x52BA40, the Game Loop mode @0x82F340's slot holding
//  Game_ProcessMainFrame); Game_ProcessMainFrame @0x5263F0 (the pause word's test
//  @0x5265A0..0x5265A6 around ++g_CurrentTick @0x5265B4, the Game_PumpWindowMessages
//  call @0x526608, the UI_IsMenuScreenOpen call @0x52660D, the
//  Input_ProcessFrame(g_CurrentTick) call @0x52661D); UI_ProcessMenuScreenFrame @0x54E650
//  (g_MenuInputPassThrough tested @0x54E686, the Input_ProcessFrame call @0x54E695),
//  the word raised through UI_SetMenuInputPassThrough @0x54B980 by DeathScreen_UpdateUI
//  @0x553150 (the call @0x55316E) and StatScreen_ShowCallback @0x562840 (the calls
//  @0x562877 / @0x5628D6); Game_WindowProc @0x7624C0 -> Input_HandleKeyPress
//  @0x760C90 (the key state @0x760CBB, the Input_QueueKeyEvent call @0x760CC2);
//  Input_ProcessKeyboardEvents @0x49D1F0 (the Input_DequeueKeyEvent calls
//  @0x49D2D2 / @0x49D4A0)]
// The frame's own sample (FrameInput::player) carries what the frame's first
// update reads at its head: the mouse motion, as retail reads the cursor only in
// the frame's first update, and the menu-screen bit [orig: Game_ProcessMainFrame
//  @0x5263F0 (the g_MainLoopFirstUpdate test @0x52640E, the
//  Input_PumpAndCenterCursor call @0x526458); Game_MainLoop @0x52B630
//  (g_MainLoopFirstUpdate raised @0x52B9FC, dropped after each update @0x52BA5E);
//  Input_PumpAndCenterCursor @0x7616D0]. With a source, every update takes its
// movement and action rows from the source (sample_tick, keyed to the logic tick it
// runs as: World::logic_tick at the tick's head, which is retail's g_CurrentTick
// once the tick's increment ran), the frame's first update and a paused one
// included, after its input frame's rows; a paused update's rows meet the head's
// pause gate and the one-shots it leaves drop, as no tick consumes them. With no
// source every update reuses the frame's sample.
// An input frame belongs to an update: a frame whose bank drains no update runs
// none, so its key events wait in the queue for the next update's keyboard pass,
// and a paused update runs its input frame as a running one does (no menu screen
// up). The keyboard pass heads the input frame, ahead of the mouse and toggle
// passes and the drain [orig: Input_ProcessPlayerFrame @0x49D4C0 (the
//  Input_ProcessKeyboardEvents call @0x49D4C6 ahead of the
//  Input_ProcessMouseAxisBindings call @0x49D4D8 and the Input_ProcessToggleBindings
//  call @0x49D4DE)], so the source runs the shell's keyboard pass at the head of
// run_update_rows, and its special-key legs (the round-over keys, the escape row's
// in-game menu, a deploy pick) land in the update that reads them. Each update tests the
// menu-screen bit at its own head, so a screen one update's input frame opens (the escape
// row's INGAME) gates the frame's later updates, which run no input frame: the source
// reads the shell's bit again at each run_update_rows [orig: Game_ProcessMainFrame
//  @0x5263F0 (UI_IsMenuScreenOpen @0x52660D ahead of Input_ProcessFrame @0x52661D, in every
//  update); Input_HandleActionBinding @0x49AD40 (UI_OpenMenuScreen("game.mnu", "INGAME")
//  @0x49B3B6)].
// The session therefore calls run_update_rows at EVERY update it runs, the
// paused ones included: the update's keyboard pass. `new_sample` is true for a
// later update of a running frame, whose device state is its own (the source steps
// its device to `logic_tick` first); false for the frame's first update and each
// paused update, whose device state the shell stepped at the frame's head. The
// update's sample_tick follows, then queue_records, then the update's drain
// (Role::drain_input_events), which the session runs itself in every update ahead of
// the update's body. The key events the frames since the last update queued wait in
// order, a press, its release and a new press each its own event, and the update's
// input frame dispatches every one of them in that order: its keyboard pass first
// moves the whole key ring into the event queue (a talk, HUD or pause row's record
// among the action rows' in key order), and the drain then runs until that queue is
// empty, so case 25's key-queue reset (the ring alone) drops nothing behind the pause
// row. Each pause row the drain dispatches flips the single-player pause word at once
// (Role::dispatch_record; its case returns before the flip in a session), and the
// records behind it meet the action rows' head with the word as it then stands: under
// the pause the bit-0 rows drop, while the talk rows, the HUD rows, the radar zoom and
// a second pause row still run, so a press, release and press of the pause key inside
// one update pauses and resumes there. Each flip that leaves the word set also shuts
// the audio channels down. The session reads the word the drain leaves at that update:
// raised, the update runs paused from there; lowered under a pause, the update resumes
// (Session::run_updates).
// [orig: Game_MainLoop @0x52B630 (the drain @0x52BA08, the update call @0x52BA58);
//  Game_ProcessMainFrame @0x5263F0 (Input_ProcessFrame @0x52661D under no menu
//  screen, after the pause word's first test @0x5265A0); Input_ProcessFrame @0x49D520
//  (the Input_ProcessPlayerFrame call @0x49D58B, the drain @0x49D599..0x49D5E7 with the
//  Input_HandleActionBinding call @0x49D5D8); Input_ProcessPlayerFrame @0x49D4C0 (the
//  Input_ProcessKeyboardEvents call @0x49D4C6); Input_ProcessKeyboardEvents @0x49D1F0
//  (the ring's dequeues @0x49D2D2 / @0x49D4A0, the action queue's Input_QueueEvent calls
//  @0x49D42F / @0x49D488); Input_HandleActionBinding @0x49AD40 (case 25 @0x49B520: the
//  in-session return @0x49B527, `xor dword_A87050, 1` @0x49B52D, the Input_ResetKeyQueue
//  call @0x49B53E, the word's test @0x49B543..0x49B54A ahead of the
//  Audio_ShutdownChannelsAndDeviceTable call @0x49B550; the head's pause gate
//  @0x49ADDD..0x49ADE8); Input_ResetKeyQueue @0x760E00 (the ring's indices @0x760E02 /
//  @0x760E07)]

// The update's records. The sample's rows (a scanned key-down's row, the talk, HUD and
// pause rows' included, then the off-keyboard arms' press edges) are the event queue's
// records: the session hands the role's queue to queue_records right after the update's
// sample, behind the records already waiting there (the console's), and the update's drain
// (Role::drain_input_events) then dispatches every record in order, handing each one whose
// arm the role does not own, and the pause row's after its flip, to dispatch_record
// [orig: Input_ProcessFrame @0x49D520 (Input_ProcessPlayerFrame @0x49D58B appends, the
//  drain @0x49D599..0x49D5E7, Input_HandleActionBinding @0x49D5D8)].
// The update's message pump queues the mouse wheel's records ahead of its input frame, so
// the source queues them first: each whole notch's row (the 256 / 512 event onto the rows
// bound to the wheel's 1024 / 2048 mask) with the arguments 0 and the Shift sign; a frame
// that runs no update pumps no message, so its notches wait for the next update
// [orig: Game_ProcessMainFrame @0x5263F0 (Game_PumpWindowMessages @0x526608 ahead of
//  Input_ProcessFrame @0x52661D); Game_PumpWindowMessages @0x7621C0 (DispatchMessageA
//  @0x762203); Game_WindowProc @0x7624C0 (Input_DispatchMouseEvent @0x762574);
//  Input_DispatchMouseEvent @0x761470 (the callbacks' 256 / 512 events @0x76158F /
//  @0x7615C0); Input_ProcessBindings @0x4DDA50 (the arguments 0 @0x4DDB6D..0x4DDB78);
//  Input_TryDispatchBindingByWeaponType @0x499180 (the sign @0x49919E..0x4991A4, the wheel
//  rows @0x4992FE..0x499327, the queue @0x499238..0x499251 (Input_QueueEvent @0x499251))].
class TickInputSource {
public:
	virtual ~TickInputSource() = default;
	virtual InputPacket sample_tick(uint32_t logic_tick) = 0;
	virtual void run_update_rows(uint32_t logic_tick, bool new_sample) {
		(void)logic_tick;
		(void)new_sample;
	}
	virtual void queue_records(controls::InputEventQueue &records) { (void)records; }
	// The embedder's arms for every record the role does not own (Role::dispatch_record), a
	// key's and a console line's alike: the talk and HUD rows, the shell's records (the
	// escape row, the deploy pick from the record's first argument, the screen shot, the
	// weapon-screen toggle), the action rows' requests past their head (the scope, the seat
	// selects, the weapon rows, ...), the exit (3) and the punt of the slot the first argument
	// names (34, reason 33 "ComIdPunt", the authority's alone).
	// [orig: Input_HandleActionBinding @0x49AD40 - case 3 @0x49AF26..0x49AF42, case 12
	//  @0x49B024 (the pick @0x49B0C3..0x49B17B), case 34 @0x49B766..0x49B773, the class
	//  callback @0x49AEB6 -> Input_HandleActionBinding_0 @0x4E0420 - case 6
	//  @0x4E0492..0x4E04F0, case 182 @0x4E0B7E (the calls @0x4E0B81..0x4E0C22); the catalog
	//  rows 105 (flags 0x4C000C01) and 17..26 (0x0C000C01)]
	virtual void dispatch_record(const controls::InputEventRecord &record) { (void)record; }
	// The input frame's tail after the update's drain, when the frame ran (no menu screen, a
	// joiner in its game loop): the held USE row's release edge (controls::PlayerActions::
	// end_input_frame) [orig: Input_ProcessFrame @0x49D520 - the release edge
	//  @0x49D6C1..0x49D6DC behind the drain @0x49D599..0x49D5E7].
	virtual void end_input_frame() {}
};

struct FrameInput {
	double delta_seconds = 0.0;
	// The main loop's clock (world::frame_tick_count_ms) since the previous frame finished
	// rendering (its present), or negative when the shell has not sampled one. Banked instead
	// of delta_seconds on a frame after a mission-start frame (Session::advance).
	double since_render_seconds = -1.0;
	CameraSample camera;
	InputPacket player;
	// The shell's input frame for this frame's updates, the first included,
	// valid for this advance call only; null when the shell has no device to
	// sample (a dedicated host, a probe's direct frame): every banked tick then
	// reuses this frame's sample.
	TickInputSource *tick_source = nullptr;
	// The host's frozen loaded-model list count (TickInput::loaded_model_list_count).
	uint32_t loaded_model_list_count = 0;
	// The main loop's frame lock: the process's game.cfg lock_framerate word, which the
	// loop reads every frame [orig: Game_MainLoop @0x52B860], and the frame's start stamp
	// in the lock clock's milliseconds (the embedder's frame clock; 0 = no live frame,
	// which never locks: a probe's or a test's direct frame banks only its delta).
	// world::TickAccumulator::bank carries the witness.
	bool frame_lock = false;
	uint32_t frame_stamp_ms = 0;
	// The shell's HUD toggles, whose status view the frame's input rows may flip during the
	// drain: the session reads the word after the drain, where retail's render callback reads
	// it (Session::draws_scene). Null when the shell has none (a dedicated serve, a probe's
	// direct frame): the view reads as down.
	const hud::HudToggleState *hud_toggles = nullptr;
	// The command map is the open menu screen: a menu screen is up and the CMAP flag holds
	// (the commander row's open raises it, the in-game screens' close lowers it). The scene
	// frame then skips its world (FrameOutcome::world_skipped)
	// [orig: Render_ProcessMainSceneFrame @0x5CA17F..0x5CA190 (UI_IsMenuScreenOpen &&
	//  g_CmapScreenOpen); the open, Input_HandleActionBinding case 221 @0x49B920 / @0x49B928].
	bool command_map_open = false;
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
	// The update's input source, which takes the drained records whose arm the role does
	// not own (TickInputSource::dispatch_record); null with no device.
	TickInputSource *source = nullptr;
	// The session already ran this update's drain (Session::run_updates, whose pause
	// decision reads the word the drain leaves), so the input prologue reads its outcome
	// and drains nothing; false for a direct Role::apply_input, which drains itself.
	bool drained = false;
	// The count of the loaded-model list the host froze at its mission start,
	// which the host wraps its S2C 0x68 cursor at (0 until a list froze)
	// [orig: Server_SendLoadedModelPageRequest @0x511375 / @0x511391 reads
	//  g_LoadedModelChallengeCount through CEffectWorld_GetLoadedModelPage @0x5B1578].
	uint32_t loaded_model_list_count = 0;
	// True on the last logic tick of this frame's batch, false while the bank
	// catches up: the outer loop's "<16 ms of backlog left" flag the fire-loop
	// emitter and the lock tone read (World::rules.last_tick_of_batch).
	bool last_tick_of_batch = true;
	// The update's head already ran under the pause word: the resuming update,
	// whose HUD counters stepped as paused ahead of its input frame, so its tick
	// steps them no more (World::rules.update_head_stepped).
	bool head_stepped = false;
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
	int64_t input_us = 0; // the tick's input prologue (Role::apply_input), an instrument
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
	// The updates the frame's drain ran, paused ones included (the CPU share's
	// update count, world::TickAccumulator::record_frame_work).
	int32_t updates = 0;
	// The frame's other phases, an instrument (the embedder's SIM_FRAME_* stats slots;
	// tick_us above is the updates' span): the bank (its frame lock's sleeps included), the
	// legs past the drain, and the lock's sleeps.
	int64_t bank_us = 0;
	int64_t render_legs_us = 0;
	int32_t lock_sleeps = 0;
	// Inside the updates, summed over them: the running updates' device-router passes (the
	// keyboard pass, the sample, the records' queue), their input-event drains, and every
	// tick's input prologue.
	int64_t input_rows_us = 0;
	int64_t input_drain_us = 0;
	int64_t input_apply_us = 0;
};

struct FrameOutcome {
	FrameStatus status = FrameStatus::NotRunning;
	State state = State::Unloaded;
	std::vector<TickOutcome> ticks;
	FramePerf perf;
	SessionError error;
	// The frame lock's last clock read when it slept (0 = it did not): the stamp the
	// embedder's next frame measures its elapsed from [orig: Game_MainLoop @0x52B8DB].
	uint32_t frame_stamp_ms = 0;
	// The main frame's render callback runs (Session::renders): false only while a joiner's
	// mission start is in its final wait for the S2C 0x0F, which renders nothing (no audio legs,
	// no scene, no page; scene_drawn is false with it). The embedder stands its render work down
	// on it (its scene legs, its listener and emitter mix); the update's drains run on.
	bool rendered = true;
	// The render callback's choice after the drain: the scene frame, or the status page in its
	// place (Session::draws_scene). The embedder runs its scene legs on this verdict.
	bool scene_drawn = true;
	// The drawn scene frame skips its world: a cine plays (Cine_IsPlaybackActive), or the
	// command map is the open menu screen (FrameInput::command_map_open). Its camera compose
	// still runs; its facial passes, its Inset pass and its HUD pass do not. The session
	// stamps it into the local view (world::PlayerViewState::world_skipped)
	// [orig: Render_ProcessMainSceneFrame @0x5CA171..0x5CA190 (ebx); the skips
	//  @0x5CA4EA..0x5CA4EC and @0x5CA620..0x5CA640 -> loc_5CAD2D].
	bool world_skipped = false;
	// The drawn scene frame calls its HUD pass (Session::hud_pass_called, the call site's gates:
	// not past the world skip, a death.mnu trigger or the latch, the F1 help, the dead bit, a
	// chase view on foot, camera mode 4 and up). The embedder's HUD compiles its gameplay overlay
	// pass on it (hud::HudFrameState::overlay_pass_called) as the session runs its radar step
	// [orig: Render_ProcessMainSceneFrame @0x5CAB2B..0x5CAD04, the calls @0x5CACE3 / @0x5CAD04].
	bool hud_pass_called = false;
	// The drawn scene frame is one of the three past a mission start: retail's scene frame
	// clears its target to black past every pass it drew (the world, the HUD, the menu and the
	// connection icons) and presents that, so those frames show black; its count-down follows the
	// present. The embedder presents the frame black.
	// [orig: Render_ProcessMainSceneFrame @0x5CA0F0 - `cmp g_MissionStartBlankFrameCount, 0`
	//  @0x5CAE62, CGfxDevice_SetClearColor(0) called @0x5CAE73, CGfxDevice_Clear(flags 1, the
	//  target) called @0x5CAE94, ahead of CD3DDevice_EndScene called @0x5CAE9E and
	//  CD3DDevice_Present called @0x5CAEA8; the count-down @0x5CAF08; Game_StartMission @0x524360 -
	//  `mov g_MissionStartBlankFrameCount, 3` @0x525E1F, on every role]
	bool start_blank = false;

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
	// The process's g_MissionExitReason as this role keeps it: the world's (the bare
	// authority's), the listen host's session context, a joiner's runtime.
	virtual void store_mission_exit(int32_t reason);
	virtual bool send_stance_request(int stance) {
		(void)stance;
		return false;
	}
	// One C2S 0x1D stance request the sim raised (World::out.stance_requests), by action id,
	// queued as the stance key's on the role's client connection with none of the key's
	// gates; a role with no client connection sends nothing. False = nothing sent.
	virtual bool queue_stance_action(uint16_t action_id) {
		(void)action_id;
		return false;
	}
	virtual void run_tick(const TickInput &input) = 0;
	virtual bool session_lost(SessionError &error) const { (void)error; return false; }
	virtual void close() = 0;
	// The client-side replica runtime this role folds (the HostClient's or the
	// joiner's); null for the bare local role and a dedicated host.
	virtual ClientRuntime *client_runtime() { return nullptr; }
	// The authority's server context (a host's, a single player's in-process listen server's
	// alike); null for a joiner and the bare local role. The dispatcher's authority rows act
	// on it (dispatch_record).
	virtual NapiNPServerCtx *server_ctx() { return nullptr; }
	// The process-lived words this role steps (ClientProcessState; the host's and the
	// joiner's); null for the bare local role.
	virtual ClientProcessState *process_state() { return nullptr; }
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
	// The /PROFILE frame leg that closes every logic update, a paused one too: a host
	// with a server log writes FBEG and its PDATs on a tick with tick & 7 == 7
	// (ServerLogRecorder::write_frame); the base writes none.
	virtual void write_profile_frame() {}
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
	// Once, true when this update ended a joiner's mission start: its final wait for the
	// S2C 0x0F, the last loop of the Game Loop mode's initialize, so the main loop's
	// first frame follows (Session::advance carries the witness). Every other role's
	// mission start ends at the load's (Session::complete_load).
	virtual bool take_final_wait_end() { return false; }
	// Inside a joiner's mission start, its final wait for the S2C 0x0F (ClientRuntime::
	// in_final_wait): a busy loop inside the Game Loop mode's initialize, so no main frame
	// renders while it runs (Session::renders). No other role has one.
	virtual bool in_final_wait() const { return false; }

	// The frame's input onto the local player, shared by every role: the update's drain
	// (drain_input_events, with the tick's source), then the movement keys, mouse look and
	// the fire/reload bits [orig: the per-frame input dispatch feeding
	// Player_PackInputStateToEntity @0x4df450].
	void apply_input(const TickInput &input);
	// The input frame's event queue (g_InputEventQueueStorage): the console's queued
	// actions wait here between input frames (ClientRuntime::queue_console_action), the
	// update's device records join them behind (TickInputSource::queue_records).
	controls::InputEventQueue &input_events() { return input_events_; }
	// The update's drain: every queued record in order, the front first, until the queue
	// reads empty, so a reset inside a dispatch (Player_ResetLocalInput's, the deploy pick's
	// Input_ResetAllState) ends it. The arms the role owns run here: the quit row's exit,
	// the Respawn key (55), the medic call (217), the stance selects (169 / 170 / 172), the
	// pause row (25), the reload row (211: the request on the slot equipped then), ToSpecial
	// (220: one dispatch with the keys' state, a deferral kept for the next input frame) and
	// the fire row (149: the binding's live test, the press leg's fire request).
	// Every other record goes to the embedder's one dispatch, `source`
	// (TickInputSource::dispatch_record), a console line's record and a key's alike, as
	// retail's one dispatcher takes both; with no source nothing takes it. A joiner outside
	// its game loop runs no input
	// frame, nor does an update under a menu screen: the records wait (a screen's close
	// resets them). Session::run_updates runs it in every update, ahead of the update's
	// body, and apply_input when called on its own (TickInput::drained). Ahead of the
	// loop the deferred dispatches (ToSpecial's and the fire row's re-queue,
	// LocalPlayer::deferred_events) join the queue behind the update's records in order,
	// each passing the head again at its dispatch.
	// `held_action_bits`: the update's sample's held bits (HELD_MENU_OPEN, under which no
	// input frame runs; HELD_TO_SPECIAL, the keys' state ToSpecial's dispatch reads;
	// HELD_FIRE, the binding state the fire row's dispatch reads).
	// `pause_flips`: the pause row's flip of the session's word applies (the session's
	// main-loop updates; the debug transport's forced step applies none).
	// [orig: Input_ProcessFrame @0x49D520 - Input_FlushDeferredEvents @0x49D591, the drain
	//  @0x49D599..0x49D5E7 -> Input_HandleActionBinding @0x49D5D8; Input_FlushDeferredEvents
	//  @0x497AC0; Game_ProcessMainFrame @0x5263F0 - UI_IsMenuScreenOpen @0x52660D skips
	//  Input_ProcessFrame @0x52661D]
	// Whether this role runs an input frame at all: a joiner outside its game loop runs
	// none (its join screen, its mission start's load and final wait run no main frame),
	// so its update's device records are dropped rather than queued and nothing drains
	// [orig: Game_StartMission @0x524360 - the final wait @0x526295..0x5262E5].
	bool input_frame_runs();
	void drain_input_events(TickInputSource *source, uint32_t held_action_bits,
			bool pause_flips = false);
	// The medic call past the session/entity gates: a dead local player with
	// the cooldown at zero sends through the role and stamps the cooldown.
	bool request_medic();
	// The dispatcher's head for the action a queued event names by its dispatch code,
	// gated by that code's catalog row flags: the local entity is there; bit 0 drops it
	// for a dead local player and under the round-over gate, 0x40 for a live one, 0x400
	// while the local client's pre-round timer runs, 0x8000000 under the deploy overlay,
	// 0x10 in a session. A key row's event and a console-queued one alike (the Respawn
	// row 68, the stance rows 9..11 and the seat rows 17..26, 0x0C000C01; the scope row
	// 105, 0x4C000C01; the mouse look rows, 0x0C100425 / 0x0C200425). True = the event
	// reaches its arm.
	bool action_head_admits(int32_t code);
	// The Respawn key (dispatch 55) past the row's head gates: the local
	// player's input reset, then in a session the role's 0x41. True = sent.
	bool request_respawn();
	// The stance selects (dispatch 169 crouch / 170 prone / 172 stand), a key's or a
	// console-queued one, past their rows' head: the role's C2S 0x1D
	// (send_stance_request). 0 stand / 1 crouch / 2 prone. True = sent.
	bool request_stance(int stance);
	// The sim's raised stance requests (World::out.stance_requests), each through
	// queue_stance_action in raise order, the list emptied: the input drain runs it past
	// each record, and each role at its frame's legs that raise them (the receive's 0x5A
	// apply, the tick, the weapon walk).
	void flush_stance_requests();
	// One drained record, dispatched by its code (drain_input_events): the arms the role
	// owns, every other code to the embedder's dispatch (`source`). The pause row (catalog
	// row 70, code 25, flags 0x05000800: no bit 0, so it runs under the pause) is the role's
	// past the head: out of a session its case flips the single-player pause word the head
	// reads for the records behind it and, when the word is now set, shuts the audio
	// channels down (WorldOutbox::audio_channels_shutdown); in a session it returns first.
	// The record then goes to the embedder, whose HUD keeps its own copy of the word and
	// whose device runs the case's key-queue reset.
	// [orig: Input_HandleActionBinding @0x49AD40 - case 25 @0x49B520: the in-session return
	//  @0x49B527, `xor dword_A87050, 1` @0x49B52D, g_QuitDialogOpen = 0 @0x49B534,
	//  Input_ResetKeyQueue @0x49B53E, the word's test @0x49B543..0x49B54A ahead of
	//  Audio_ShutdownChannelsAndDeviceTable @0x49B550]
	void dispatch_record(const controls::InputEventRecord &record, TickInputSource *source);
	// The dispatcher's server and process rows, each its own case past the head
	// (action_head_admits), the session words its case tests read off World::rules
	// (mp_session = is_in_session, logic_authority = is_authority, mp_session_peer); true
	// when the record's code is one of them (taken, whatever its gates decided), so it goes
	// to no embedder (dispatch_record). Their witness is at each arm (session.cpp).
	bool dispatch_server_record(const controls::InputEventRecord &record);
	// Player_ResetLocalInput, which the Respawn key (case 55) and the Escape key (case
	// 18, ahead of its `!is_in_session && g_RoundOver` test; the embedder's escape runs it)
	// run: the input reset (D-NET-560). True when it ran past its local-entity test, where its
	// Input_DisableScrollLock call turns the auto-run's toggle off: the embedder's device owes
	// that turn-off (D-NET-563; the Respawn key's rides its row's code,
	// controls::PlayerActionFrame::scroll_lock_off) [orig: Input_HandleActionBinding @0x49AD40 -
	//  case 55 @0x49B20B, case 18 Player_ResetLocalInput @0x49B234 ahead of the test
	//  @0x49B23B..0x49B249; Player_ResetLocalInput @0x4B2B80 (the tests @0x4B2B85..0x4B2B8D,
	//  the call @0x4B2B91)].
	bool reset_local_input();
	// The deploy action (dispatch 12) with its pick's [i16] wire value: a dead
	// local player under permanent death converts to a spectator (the C2S
	// 0x51) when spectators are allowed and does nothing when they are not;
	// every other press sends the C2S 0x0E pick. Both send arms run the shared
	// head first: the input reset (the deferred queue here, the devices through
	// take_input_reset) and the dialog reset (D-NET-685). True = queued.
	bool request_deployment_pick(uint16_t wire_value);
	// Input_ResetAllState run outside the input frame, at the armory screen's ACCEPT and
	// CANCEL head: the event queue's records (input_events: the console's queued actions)
	// and the deferred queue go, and the embedder's devices owe their key queue, key state
	// and key-press latch reset (take_input_reset).
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
	controls::InputEventQueue input_events_; // input_events
	// A reset ran in this input frame's drain (Input_ResetAllState's queue memset): the
	// frame's later events are gone, the held rows' with them (apply_input; D-NET-560).
	bool input_queue_reset_ = false;
	// The running drain's pause_flips and its sample's ToSpecial key state and fire binding
	// state (drain_input_events).
	bool pause_flips_ = false;
	bool to_special_held_ = false;
	bool fire_held_ = false;
	bool fire_keys_held_ = false;
};

// The held / pressed action bits of InputPacket, the retail action rows the
// frame input carries besides the movement keys.
enum HeldAction : uint32_t {
	HELD_FIRE = 1u << 0,
	// The ToSpecial keys' live state, which its records' dispatch reads in the update's
	// drain (the hold-swap press or release; Input_HandleActionBinding_0 case 0xDC
	// @0x4E1161..0x4E1181).
	HELD_TO_SPECIAL = 1u << 1,
	// A menu screen (INGAME, WEAPON, CMAP, VEHICLE, or DEATH with its exit
	// confirmation's word down) is up: no input frame runs, so the deferred input
	// queue is not flushed (the close's reset then drops it)
	// [orig: Game_ProcessMainFrame @0x52660D / @0x52661D; Input_ProcessFrame ->
	//  Input_FlushDeferredEvents @0x49D591, its one caller].
	HELD_MENU_OPEN = 1u << 2,
	// A key slot of the fire row is down (Input_IsBindingActive's keyboard leg, device 1):
	// the fire row's press leg starts a PowerThrow windup only from the keyboard
	// (Role::dispatch_record) [orig: Input_IsBindingActive @0x498320 (the key slots
	//  @0x498348); Input_HandleActionBinding_0 case 0x95 - the device test @0x4E090B].
	HELD_FIRE_KEYS = 1u << 3,
};
// The Respawn key (code 55), the medic call (code 217), the reload row (code 211) and
// ToSpecial (code 220) are records the update's drain dispatches in queue order
// (Role::drain_input_events), not edges. PRESSED_RELOAD is a sourceless frame's reload
// latch alone (a probe's direct frame, a scripted frame naming no source), which the input
// prologue ORs into the weapon pump's latch as a reload record's arm does.
enum PressedAction : uint32_t {
	PRESSED_FIRE = 1u << 0,
	PRESSED_RELOAD = 1u << 1,
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
	// THE UPDATE LEGS. The embedder's own legs inside every main-loop update, where retail's
	// update callback runs them: `head` ahead of the update's input frame and tick (the
	// NovaWorld session's periodic update, CNapiGameSession_ProcessPeriodicUpdate, which pumps
	// the listing and hands the match the service's facts), `tail` past its body (the remote
	// admin's pump, CAdminServer_ProcessFrame), paused updates included; an update whose tick
	// ended the session (its mission exit's router returns first) runs no tail, and the
	// transport's forced step (step_once) is no main-loop update and runs neither. Every host
	// embedder installs both (opennova-serve's Server, the game's SessionDrive and GameWorld),
	// so the legs run once per update, never once per display frame.
	// [orig: Game_ProcessMainFrame @0x5263F0 (the update callback, Game_MainLoop @0x52BA5A) -
	//  `UI_GetPendingGameMode` @0x52650D, CNapiGameSession_ProcessPeriodicUpdate @0x526532 ahead
	//  of Input_ProcessFrame @0x52661D and Server_TickUpdate @0x5266B6; the exit router
	//  @0x526806..0x526867, CAdminServer_ProcessFrame @0x5268F1 past it]
	void set_update_head(std::function<void()> head) { update_head_ = std::move(head); }
	void set_update_tail(std::function<void()> tail) { update_tail_ = std::move(tail); }
	// The main loop's clock: the frame lock's (FrameInput::frame_lock) and the one the frame's
	// start stamp and its CPU share read: the main loop's tick-count clock
	// (world::frame_tick_count_ms) and a 1 ms sleep unless the embedder hands its own (a
	// platform's, inmatch::PlatformFrameLockClock; a test's).
	void set_frame_lock_clock(world::FrameLockClock *clock);
	const SessionError &last_error() const { return last_error_; }
	const FramePerf &last_perf() const { return last_perf_; }
	// THE SCENE GATE. Retail's render callback draws EITHER the scene frame or the status
	// page: a frame with no session peer (a dedicated host) always takes the page, a peer's
	// takes it while the authority's status view is up (hud::main_frame_draws_scene carries
	// the witness), the word read after the update's drain. The scene frame's state legs run
	// on its verdict alone: the session's cine pass, its HUD radar step and its mission-start
	// re-base countdown here, the embedder's scene legs on FrameOutcome::scene_drawn.
	// `toggles` null reads the view as down.
	// [orig: GameLoop_RenderFrame @0x521310 - is_mp_session_peer @0x52134E / `jz` @0x521355;
	//  the view word @0x521CD6, is_authority @0x521CDF..0x521CE6; Render_ProcessMainSceneFrame
	//  @0x521CE8 or Server_DrawStatusScreen @0x521CEF]
	// A frame that does not render (renders) draws neither.
	bool draws_scene(const hud::HudToggleState *toggles) const;
	// THE RENDER GATE. Whether the main frame's render callback runs at all. A joiner's mission
	// start ends in its final wait for the S2C 0x0F, a busy loop inside the Game Loop mode's
	// initialize that pumps the window, reads ESC, receives, sends and tests its exit, and
	// renders nothing: no listener or emitter update, no scene, no page, no present (the
	// loading screen's 100 % present ahead of it stays up). Its passes are our frames' updates
	// (JoinerRole::run_tick), so a frame whose drain leaves the role inside the wait renders
	// nothing; the frame whose drain takes the 0x0F renders.
	// [orig: Game_StartMission @0x524360 - the final wait @0x526295..0x5262E5: Game_PumpWindowMessages
	//  @0x52629D, Input_DequeueKeyEvent @0x5262A2, CNapiNetwork_PumpManagerReceive @0x5262B4,
	//  CNapiNetwork_PumpClientProtocolRecv @0x5262BE, CNapiGameSession_ProcessPeriodicUpdate
	//  @0x5262C8, CNapiNetwork_PumpClientProtocolSend @0x5262DA, the exit test @0x5262DF;
	//  GameLoop_Init @0x526370 (the call @0x526377); GameLoop_RenderFrame @0x521310 -
	//  Audio_UpdateListenerFromView @0x52131E, SharedMem_UpdateEngineSoundState @0x521329,
	//  SoundEmitter_UpdateAndMixTop8 @0x521341]
	bool renders() const { return role_ == nullptr || !role_->in_final_wait(); }
	// Whether the session's entities are presented at all. A dedicated host (no session peer)
	// never takes the scene arm its entities are collected and drawn in, and its mission start
	// jumps over the render subsystems' init: nothing presents its entities, and what its
	// session raised for their presentation drops undrawn (world::discard_entity_presentation).
	// Every other role presents them, a listen host under its status page included: the page
	// stands in for a frame's draw, never for the entities' state.
	// [orig: GameLoop_RenderFrame @0x521310 - is_mp_session_peer @0x52134E, `jz` @0x521355 to
	//  Server_DrawStatusScreen @0x521CEF, past Render_ProcessMainSceneFrame @0x521CE8;
	//  Game_StartMission @0x524360 - the dedicated word @0x524942..0x52495C, its test @0x5259B4
	//  past Render_InitRenderingSubsystems @0x5259D1 and Render_InitTerrainSubsystems @0x5259FD]
	bool presents_entities() const { return kind_ != RoleKind::DedicatedHost; }
	// The main loop's frame statistics the authority's status page shows:
	// the frames rendered during the last 62 logic updates and the CPU share
	// of the frame-rate window (world::TickAccumulator::cpu_percent). A paused
	// single-player frame's updates and render count too, as both run past the
	// pause word's tests.
	// [orig: dword_24C193C — Game_ProcessMainFrame @0x5267ab..0x5267df counts
	//  every logic update into dword_24D6130 and at 62 publishes
	//  dword_24C1938, which GameLoop_RenderFrame @0x521cf9 increments once per
	//  rendered frame; its one reader Server_DrawStatusScreen @0x50A2D0 (the
	//  read @0x50AFAF);
	//  g_StatsCpuPercent]
	struct FrameStatistics {
		int32_t frames_last_second = 0;
		int32_t cpu_percent = 0;
	};
	FrameStatistics frame_statistics() const {
		return {frames_last_second_, accumulator_.cpu_percent()};
	}
	// The frame-rate window's four statistics, the frame-rate overlay's words
	// (world::TickAccumulator::frame_rate_stats carries the witness).
	world::TickAccumulator::FrameRateStats frame_rate_stats() const {
		return accumulator_.frame_rate_stats();
	}

	// THE FRAME'S RADAR STEP. Retail's HUD pass runs on every frame that draws
	// the scene, the in-game menu's included (the menu only stops the
	// single-player ticks; the status page stands in for the whole scene
	// frame, draws_scene), and its radar update ages the local player's
	// contacts and the retained minimap banks (the 0x6B designations, the
	// transient slots, the linked markers' lapse) by the ticks since its last
	// run. The session runs that step after each scene frame's tick drain, paused
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

	// The shell's pause and resume (the in-game menu's share of the pause word, the
	// escape chain's clear, the debug transport). Neither touches the frame-time
	// bank: retail's pause word gates the update's legs, never the main loop's bank
	// (world::TickAccumulator); the pending one-shots drop, as nothing consumes a
	// queued event under the menu and its close resets the queue. The pause row's
	// own flip applies inside the update that reads it (run_updates).
	TransitionResult pause();
	TransitionResult resume();
	TransitionResult close();

	// A mode change's reset: the bank, its smoothing history and the FR counter
	// (the mode init, Game_MainLoop @0x52B715..0x52B757), and the input latch.
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
	// The frame's `due` updates, each in retail's update order: its input frame
	// (TickInputSource::run_update_rows, then its sample_tick), the mouse
	// pump, then the running update's role tick, the pausing update (its drain left
	// the pause word raised), the paused update's legs or the resuming update's
	// tick. `transport`: the debug transport's forced step (step_once, drive_one),
	// whose updates run the role's tick whatever the state and apply no flip.
	FrameOutcome run_updates(int32_t due, const FrameInput &input, bool transport);
	// The update's device records into the role's queue, or dropped where the role runs
	// no input frame (Role::input_frame_runs).
	void queue_update_records(TickInputSource &source);
	TickOutcome run_one_tick(const TickInput &input);
	void step_cine_render_frame();
	// The legs a paused update runs past the pause word's tests, in its order (the
	// pending-sound pass, the tracer pool, the third-person camera, the weather, the
	// 62-update second, the /PROFILE frame), the pausing update's and every paused
	// one's. A new paused leg goes here.
	void run_paused_update_legs();
	// Every state change, with the pause word the action rows' head reads
	// (world::SessionRules::pause_word).
	void set_state(State to);
	// One update into the 62-update second, and one rendered frame into the
	// frames it publishes, for the running and the paused frame alike.
	void count_update_into_second();
	void count_rendered_frame();
	// The drawn scene frame's read of the HUD target cursor ahead of its HUD pass
	// (world::LocalPlayer::note_hud_scene_head), then the pass's own step.
	void step_hud_scene_cursor();
	void step_hud_radar_frame();
	// The render callback's cine and world-skip legs after the drain: the listener update's
	// cine test on a session peer, under the page too, then the drawn scene frame's
	// world-skip word, stamped into the local view.
	void step_scene_head(const FrameInput &input, FrameOutcome &out);
	// The role's facts for the scene frame's legs (inmatch/role_feeds.h).
	RoleView frame_role_view() const;
	// Whether the drawn frame's HUD pass runs: not on a frame that skips its world, else
	// at its call site's gates (inmatch::hud_overlays_called).
	bool hud_pass_called(const FrameInput &input, const FrameOutcome &out) const;
	// The world block's map overlay leg after the HUD pass: a drawn frame that draws its
	// world closes an open map for a body with the dead bit (hud::HudMapControl).
	void step_map_overlay_death_clear(const FrameOutcome &out);
	static int64_t now_us();

	Role *role_ = nullptr;
	RoleKind kind_ = RoleKind::SinglePlayer;
	TickObserver *observer_ = nullptr;
	std::function<void()> update_head_;
	std::function<void()> update_tail_;
	State state_ = State::Unloaded;
	world::TickAccumulator accumulator_;
	world::FrameLockClock *frame_lock_clock_ = nullptr; // null = the tick-count one
	// The main loop's clock the session reads (set_frame_lock_clock).
	world::FrameLockClock &frame_clock() const;
	// The frame's bank with its lock when the input asks for one (live frames only): the
	// ticks due; `lock_stamp_ms` the lock's last read when it slept, else 0.
	int32_t bank_frame(double elapsed, const FrameInput &input, uint32_t &lock_stamp_ms,
			int32_t &lock_sleeps);
	// The mission-start frames still to draw (g_MissionStartBlankFrameCount) and whether the
	// last one re-based the clock after its render (g_MainLoopRebaseClockAfterRender).
	static constexpr int32_t kStartRebaseFrames = 3;
	int32_t start_rebase_frames_ = 0;
	bool rebase_clock_ = false;
	// A joiner's final wait ended in this frame's drain (Role::take_final_wait_end).
	bool final_wait_ended_ = false;
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
