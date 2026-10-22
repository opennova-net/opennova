// The session's per-tick input frame (D-NET-NEW-LI-1): retail runs its input
// frame in every logic tick (inmatch::TickInputSource carries the witness), so
// a scripted step due inside a frame that banks six ticks lands on its own
// tick, and a hold keeps its scripted length on the ticks the role packs.
// Driven with the real scenario device (devtools::ScriptedInput) over a probe
// role that records each tick's input at the logic tick it ran as. Also the
// main frame's mouse pump (D-NET-NEW-LI-2), across the frames, an input reset and the
// single-player pause, and a latched fire press's binding test at its
// dispatch (D-NET-NEW-LI-3).
#include <runtime/controls/controls.h> // action_code (the pause row's record)
#include <runtime/devtools/scripted_input.h>
#include <runtime/inmatch/mission_exit.h> // kMissionExitReset (the quit record)
#include <runtime/inmatch/session.h>
#include <runtime/mission/mission_kernel.h>

#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace opennova::inmatch;
using opennova::devtools::ScriptedInput;
using opennova::devtools::ScriptStep;
using opennova::world::TickAccumulator;
using Kind = ScriptStep::Kind;

int g_failures = 0;

#define CHECK(cond, message)                                                          \
	do {                                                                              \
		if (!(cond)) {                                                                \
			std::printf("FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, message, #cond); \
			++g_failures;                                                             \
		}                                                                             \
	} while (0)

constexpr int kForward = 152; // move_forward
constexpr int kFire = 149;    // attack_1

// A role over a bare kernel recording every tick's input and the logic tick
// it ran as (World::logic_tick at the tick's head; the tick then advances it).
// `reset_at` runs the whole input reset inside that recorded tick, as a
// dispatch's Input_ResetAllState does inside its update.
struct TickRecorder final : Role {
	opennova::mission::MissionKernel kernel;
	std::vector<TickInput> inputs;
	std::vector<uint32_t> ticks;
	int reset_at = -1;

	TickRecorder() {
		bind(kernel);
		kernel.world.logic_tick = 500;
	}
	RoleKind kind() const override { return RoleKind::SinglePlayer; }
	void run_tick(const TickInput &input) override {
		if (static_cast<int>(inputs.size()) == reset_at) reset_all_input_state();
		inputs.push_back(input);
		ticks.push_back(kernel.world.logic_tick);
		++kernel.world.logic_tick;
	}
	bool reset_to_baseline(SessionError &) override { return true; }
	void close() override {}

	// The ticks (relative to `start`) whose input satisfied `pred`.
	template <class Pred>
	std::vector<int64_t> ticks_where(int64_t start, Pred pred) const {
		std::vector<int64_t> out;
		for (size_t i = 0; i < inputs.size(); ++i)
			if (pred(inputs[i])) out.push_back(static_cast<int64_t>(ticks[i]) - start);
		return out;
	}
};

// The shell's device: the scenario device stepped to the sample's logic tick,
// its tokens read as the binding scan reads held keys (forward a movement
// key, fire the held fire bit with its press edge), as the Godot input router
// does over the same device.
struct ScriptedDevice final : TickInputSource {
	ScriptedInput driver;
	bool fire_was_held = false;

	InputPacket sample_tick(uint32_t logic_tick) override {
		driver.advance(logic_tick);
		InputPacket p;
		p.movement.forward = driver.token_held("move_forward");
		const bool fire = driver.token_held("attack_1");
		p.held_action_bits = fire ? HELD_FIRE : 0u;
		p.pressed_action_bits = fire && !fire_was_held ? PRESSED_FIRE : 0u;
		fire_was_held = fire;
		return p;
	}
};

ScriptStep step(Kind kind, int64_t tick, int code = 0) {
	ScriptStep s;
	s.kind = kind;
	s.tick = tick;
	s.code = code;
	return s;
}

// The local entity the action rows' head reads: the pause row's arm runs past it
// (Role::dispatch_record).
void spawn_local_player(TickRecorder &role) {
	opennova::world::World &world = role.kernel.world;
	world.registry.configure_pool(0, 8);
	opennova::world::Entity seed;
	seed.kind = opennova::world::EntityKind::Organic;
	seed.alive = true;
	world.cached.local_player = world.registry.spawn(0, seed);
}

bool load(Session &session) {
	return session.begin_load().applied() && session.complete_load().applied();
}

// One shell frame banking six ticks, then the session's advance: with the device
// as the per-update source every update samples it, the first included (the
// frame's own sample carries only the frame's head reads); without it the frame's
// own sample at the logic tick its first tick runs as.
FrameOutcome six_tick_frame(Session &session, TickRecorder &role, ScriptedDevice &device,
		bool per_tick) {
	FrameInput in;
	in.delta_seconds = TickAccumulator::kTickDt * 6.0;
	if (!per_tick) in.player = device.sample_tick(role.kernel.world.logic_tick);
	in.tick_source = per_tick ? &device : nullptr;
	return session.advance(in);
}

struct Run {
	std::unique_ptr<TickRecorder> role = std::make_unique<TickRecorder>();
	std::unique_ptr<ScriptedDevice> device = std::make_unique<ScriptedDevice>();
	std::unique_ptr<Session> session = std::make_unique<Session>(*role);
	int64_t start = -1;
	std::vector<int> ticks_per_frame;

	// A forward hold from tick 3 to tick 9 (six ticks) and a fire press at
	// tick 5, ending at tick 12: three six-tick frames play it.
	explicit Run(bool per_tick) {
		std::string error;
		CHECK(device->driver.load({step(Kind::Down, 3, kForward), step(Kind::Press, 5, kFire),
								step(Kind::Up, 9, kForward), step(Kind::End, 12)},
					  error),
				"the scenario loads");
		CHECK(load(*session), "the session runs");
		CHECK(device->driver.start(), "the scenario arms");
		for (int frame = 0; frame < 3; ++frame)
			ticks_per_frame.push_back(six_tick_frame(*session, *role, *device, per_tick).ticks_run());
		start = device->driver.start_logic_tick();
	}
};

bool forward(const TickInput &t) { return t.player.movement.forward; }
bool fire_edge(const TickInput &t) { return (t.player.pressed_action_bits & PRESSED_FIRE) != 0; }
bool fire_held(const TickInput &t) { return (t.player.held_action_bits & HELD_FIRE) != 0; }

void test_steps_land_on_their_ticks_inside_a_six_tick_frame() {
	Run run(/*per_tick=*/true);
	CHECK(run.ticks_per_frame == std::vector<int>({6, 6, 6}), "each frame banks six ticks");
	CHECK(run.start == 500, "the first frame's sample latches the start at its first tick");
	const auto &records = run.device->driver.records();
	CHECK(records[0].applied_logic_tick == run.start + 3, "the forward step lands at tick 3");
	CHECK(records[1].applied_logic_tick == run.start + 5 &&
					records[1].release_logic_tick == run.start + 6,
			"the fire press lands at tick 5 and releases at tick 6");
	CHECK(records[2].applied_logic_tick == run.start + 9, "the release lands at tick 9");
	CHECK(run.device->driver.end_logic_tick() == run.start + 12, "the end lands at tick 12");
	CHECK(run.role->ticks_where(run.start, forward) == std::vector<int64_t>({3, 4, 5, 6, 7, 8}),
			"the six-tick hold is six ticks the role packs, from tick 3");
	CHECK(run.role->ticks_where(run.start, fire_edge) == std::vector<int64_t>({5}),
			"the fire edge is consumed by tick 5 alone");
	CHECK(run.role->ticks_where(run.start, fire_held) == std::vector<int64_t>({5}),
			"the press holds fire for its one tick");
}

// Without a per-tick source every banked tick reuses the frame's sample: both
// steps due inside the first frame wait for the next frame's first tick (the
// drift the scenario cells measured), the shape the source replaces.
void test_a_frame_sample_alone_quantizes_to_the_frame() {
	Run run(/*per_tick=*/false);
	CHECK(run.ticks_per_frame == std::vector<int>({6, 6, 6}), "each frame banks six ticks");
	const auto &records = run.device->driver.records();
	CHECK(records[0].applied_logic_tick == run.start + 6 &&
					records[1].applied_logic_tick == run.start + 6,
			"steps due at ticks 3 and 5 both land at the next frame's tick 6");
	CHECK(run.role->ticks_where(run.start, fire_edge) == std::vector<int64_t>({6}),
			"the fire edge lands on tick 6");
}

// A tick whose input did not change packs what the frame's sample gave it: a
// device held steady through a frame yields catch-up ticks identical to the
// frame-sample-only path (held state reused, no edges, no look).
struct SteadyDevice final : TickInputSource {
	int samples = 0;
	InputPacket held_sample() const {
		InputPacket p;
		p.movement.forward = true;
		p.movement.lean_left = true;
		p.held_action_bits = HELD_FIRE;
		return p;
	}
	InputPacket sample_tick(uint32_t) override {
		++samples;
		return held_sample();
	}
};

void test_unchanged_input_packs_unchanged_ticks() {
	std::vector<TickInput> seen[2];
	int samples[2] = {};
	for (int per_tick = 0; per_tick < 2; ++per_tick) {
		auto role = std::make_unique<TickRecorder>();
		auto device = std::make_unique<SteadyDevice>();
		Session session(*role);
		CHECK(load(session), "the session runs");
		FrameInput in;
		in.delta_seconds = TickAccumulator::kTickDt * 6.0;
		in.player = device->held_sample();
		in.player.pressed_action_bits = PRESSED_FIRE;
		in.player.look_delta_x = 7.0f;
		in.tick_source = per_tick != 0 ? device.get() : nullptr;
		CHECK(session.advance(in).ticks_run() == 6, "six ticks");
		seen[per_tick] = role->inputs;
		samples[per_tick] = device->samples;
	}
	CHECK(samples[0] == 0 && samples[1] == 6, "every update samples the device, the first included");
	bool same = seen[0].size() == 6 && seen[1].size() == 6;
	for (size_t i = 0; same && i < seen[0].size(); ++i) {
		const InputPacket &a = seen[0][i].player;
		const InputPacket &b = seen[1][i].player;
		same = a.movement.forward == b.movement.forward &&
				a.movement.lean_left == b.movement.lean_left &&
				a.held_action_bits == b.held_action_bits &&
				a.pressed_action_bits == b.pressed_action_bits &&
				a.look_delta_x == b.look_delta_x && a.look_delta_y == b.look_delta_y;
	}
	CHECK(same, "every tick packs the same input either way");
	CHECK(seen[1][0].player.pressed_action_bits == PRESSED_FIRE &&
					seen[1][0].player.look_delta_x == 7.0f &&
					seen[1][1].player.pressed_action_bits == 0 &&
					seen[1][1].player.look_delta_x == 0.0f,
			"the frame's edges and look go to its first tick alone");
	CHECK(seen[1][1].consume_one_shots && !seen[0][1].consume_one_shots,
			"a catch-up tick with its own sample runs its own input frame");
}

// The main frame's mouse pump (world::MouseAxisPump carries the witness): the
// cursor offset, four times over, spread over the frame's updates with the
// remainder on the last, each update summed with the previous one's share.
void test_the_mouse_pump_spreads_four_times_the_cursor_over_the_updates() {
	using opennova::world::MouseAxisPump;
	MouseAxisPump pump;
	int32_t axis[2] = {};
	const int32_t cursor[2] = {10, -5};
	const int32_t none[2] = {0, 0};
	// Three updates: x (1 + 40) / 3 = 13, 13, then 40 - 26 = 14; y (1 - 20) / 3
	// truncates to -6, -6, then -20 + 12 = -8.
	pump.step(true, false, 3, false, cursor, axis);
	CHECK(axis[0] == 13 && axis[1] == -6, "the first update: its share alone");
	pump.step(false, false, 3, false, none, axis);
	CHECK(axis[0] == 26 && axis[1] == -12, "the middle update: its share plus the first's");
	pump.step(false, true, 3, false, none, axis);
	CHECK(axis[0] == 27 && axis[1] == -14, "the last update: the remainder plus the share");
	pump.step(true, true, 1, false, none, axis);
	CHECK(axis[0] == 14 && axis[1] == -8, "the next frame's first update carries the remainder");
	pump.step(true, true, 1, false, none, axis);
	CHECK(axis[0] == 0 && axis[1] == 0, "then nothing");
	// Each pixel reached the axis eight times: 13 + 26 + 27 + 14 = 80.
	// Two updates: (1 - 20) / 2 truncates to -9, the remainder -20 + 9 = -11.
	const int32_t left[2] = {-5, 0};
	pump.step(true, false, 2, false, left, axis);
	CHECK(axis[0] == -9, "a negative share truncates toward zero");
	pump.step(false, true, 2, false, none, axis);
	CHECK(axis[0] == -20, "the remainder plus the share");
	// A menu screen up at the first update zeroes the frame's shares.
	pump.step(true, false, 2, true, cursor, axis);
	CHECK(axis[0] == -11 && axis[1] == 0, "the menu's update still sums the previous share");
	pump.step(false, true, 2, false, none, axis);
	CHECK(axis[0] == 0 && axis[1] == 0, "and its frame's shares are zero");
}

// The session runs the pump on every tick: the frame's cursor offset reaches
// the first tick's axis input, the spread shares the later ticks', also with
// no per-tick source; a menu screen's ticks consume none of it.
void test_the_session_spreads_the_frames_cursor_over_its_ticks() {
	auto role = std::make_unique<TickRecorder>();
	Session session(*role);
	CHECK(load(session), "the session runs");
	FrameInput in;
	in.delta_seconds = TickAccumulator::kTickDt * 3.0;
	in.player.cursor_dx = 10;
	in.player.look_delta_y = 2.0f; // a scripted axis addition rides the first tick
	CHECK(session.advance(in).ticks_run() == 3, "three ticks");
	FrameInput still;
	still.delta_seconds = TickAccumulator::kTickDt;
	const int ran = session.advance(still).ticks_run();
	CHECK(ran >= 1, "the next frame ticks");
	const std::vector<TickInput> &t = role->inputs;
	CHECK(t.size() == static_cast<size_t>(3 + ran), "every tick recorded");
	CHECK(t[0].player.look_delta_x == 13.0f && t[1].player.look_delta_x == 26.0f &&
					t[2].player.look_delta_x == 27.0f,
			"the cursor's four-fold spread over the frame's three ticks");
	CHECK(t[0].player.look_delta_y == 2.0f && t[1].player.look_delta_y == 0.0f,
			"the frame's axis addition goes to its first tick alone");
	CHECK(t[3].player.look_delta_x == 14.0f, "the next frame's first tick carries the remainder");
	float total = 0.0f;
	for (const TickInput &tick : t) total += tick.player.look_delta_x;
	CHECK(total == 80.0f, "ten cursor pixels reach the axis eighty times");

	FrameInput menu;
	menu.delta_seconds = TickAccumulator::kTickDt;
	menu.player.cursor_dx = 10;
	menu.player.held_action_bits = HELD_MENU_OPEN;
	const size_t before = role->inputs.size();
	CHECK(session.advance(menu).ticks_run() >= 1, "the menu frame ticks");
	bool none = true;
	for (size_t i = before; i < role->inputs.size(); ++i)
		none = none && role->inputs[i].player.look_delta_x == 0.0f;
	CHECK(none, "no input frame consumes the axis under a menu screen");
}

// The axis input of the ticks recorded from `from` on.
std::vector<float> axis_x_from(const TickRecorder &role, size_t from) {
	std::vector<float> out;
	for (size_t i = from; i < role.inputs.size(); ++i) out.push_back(role.inputs[i].player.look_delta_x);
	return out;
}

FrameOutcome cursor_frame(Session &session, int32_t cursor_dx, uint32_t held = 0) {
	FrameInput in;
	in.delta_seconds = TickAccumulator::kTickDt * 3.0;
	in.player.cursor_dx = cursor_dx;
	in.player.held_action_bits = held;
	return session.advance(in);
}

// The pump across the single-player pause: the in-game menu's paused frames
// zero all six words, so nothing pumped before the pause reaches the first
// tick after the resume (a pause that banks no paused update: pause()'s zero
// stands in for the menu's first update); the pause row's paused frames run the
// pump on the live cursor, so the first tick after the resume carries the last
// paused update's share, which no paused input frame consumed.
void test_the_pump_across_the_single_player_pause() {
	{
		auto role = std::make_unique<TickRecorder>();
		Session session(*role);
		CHECK(load(session), "the session runs");
		CHECK(cursor_frame(session, 10).ticks_run() == 3, "three ticks");
		CHECK(axis_x_from(*role, 0) == std::vector<float>({13.0f, 26.0f, 27.0f}),
				"the frame's spread, the remainder 14 still to pair");
		CHECK(session.pause().applied() && session.resume().applied(), "the menu pause");
		const size_t before = role->inputs.size();
		CHECK(cursor_frame(session, 0).ticks_run() == 3, "three ticks after the resume");
		CHECK(axis_x_from(*role, before) == std::vector<float>({0.0f, 0.0f, 0.0f}),
				"nothing pumped before the menu reaches a tick after it");
	}
	{
		auto role = std::make_unique<TickRecorder>();
		Session session(*role);
		CHECK(load(session), "the session runs");
		CHECK(cursor_frame(session, 10).ticks_run() == 3, "three ticks");
		CHECK(session.pause().applied(), "the pause row's pause");
		const size_t before = role->inputs.size();
		// Three paused updates over -5 px: (1 - 20) / 3 truncates to -6 twice,
		// then the remainder -20 + 12 = -8.
		CHECK(cursor_frame(session, -5).status == FrameStatus::NotRunning, "a paused frame");
		CHECK(role->inputs.size() == before, "no tick runs under the pause");
		CHECK(session.resume().applied(), "the resume");
		CHECK(cursor_frame(session, 0).ticks_run() == 3, "three ticks after the resume");
		CHECK(axis_x_from(*role, before) == std::vector<float>({-8.0f, 0.0f, 0.0f}),
				"the last paused update's share pairs with the first tick after the resume");
		// A menu screen up at a paused frame's first update zeroes the words.
		CHECK(session.pause().applied(), "paused again");
		CHECK(cursor_frame(session, 10).status == FrameStatus::NotRunning, "a pumped frame");
		CHECK(cursor_frame(session, 10, HELD_MENU_OPEN).status == FrameStatus::NotRunning,
				"a paused frame under a menu screen");
		CHECK(session.resume().applied(), "the resume");
		const size_t after_menu = role->inputs.size();
		CHECK(cursor_frame(session, 0).ticks_run() == 3, "three ticks");
		CHECK(axis_x_from(*role, after_menu) == std::vector<float>({0.0f, 0.0f, 0.0f}),
				"the menu's update left nothing to pair");
	}
}

// An input reset inside an update recentres the cursor, which drops only the
// motion since that update's pump: it writes the pump's deltaX / deltaY, which
// the next update's two-tap overwrites, and leaves the shares, the remainder and
// the previous share alone, so the frame's later ticks and the next frame's
// first tick keep their axis input. Ours' device motion arrives between frames,
// so no motion lies between a tick's pump and its reset.
// [orig: Input_ResetAllState @0x497ED0 (the focus tests @0x497F10..0x497F20, the
//  Input_PumpAndCenterCursor tail jump @0x497F22); Input_PumpAndCenterCursor
//  @0x7616D0 (SetCursorPos @0x761706, the deltaX / deltaY stores
//  @0x761724 / @0x76172A); Game_ProcessMainFrame @0x5263F0 (the two-tap
//  @0x5264E6..0x526508)]
void test_an_input_reset_leaves_the_pumps_words() {
	auto role = std::make_unique<TickRecorder>();
	role->reset_at = 0;
	Session session(*role);
	CHECK(load(session), "the session runs");
	CHECK(cursor_frame(session, 10).ticks_run() == 3, "three ticks, the first resetting");
	CHECK(cursor_frame(session, 0).ticks_run() == 3, "three more");
	CHECK(axis_x_from(*role, 0) == std::vector<float>({13.0f, 26.0f, 27.0f, 14.0f, 0.0f, 0.0f}),
			"the reset's frame keeps its spread and the next frame its remainder");
}

// A fire press latched by a frame that ran no tick dispatches at the next
// tick's input frame, where the attack row tests its binding live: released
// by then, the press fires nothing; still held, it fires on that tick.
void test_a_latched_fire_press_needs_its_binding_held_at_the_dispatch() {
	for (int still_held = 0; still_held < 2; ++still_held) {
		auto role = std::make_unique<TickRecorder>();
		Session session(*role);
		CHECK(load(session), "the session runs");
		// A cold bank drains nothing under 4 ms (inmatch_session_test's
		// zero-tick frame), then a 14 ms frame runs one tick.
		FrameInput press;
		press.delta_seconds = 0.002;
		press.player.held_action_bits = HELD_FIRE;
		press.player.pressed_action_bits = PRESSED_FIRE | PRESSED_RELOAD;
		CHECK(session.advance(press).ticks_run() == 0, "the press's frame runs no tick");
		FrameInput next;
		next.delta_seconds = 0.014;
		next.player.held_action_bits = still_held != 0 ? HELD_FIRE : 0u;
		CHECK(session.advance(next).ticks_run() == 1, "the next frame runs one tick");
		CHECK(role->inputs.size() == 1, "one tick recorded");
		if (role->inputs.size() != 1) continue;
		const InputPacket &tick = role->inputs[0].player;
		if (still_held != 0) {
			CHECK(tick.pressed_action_bits == (PRESSED_FIRE | PRESSED_RELOAD) &&
							tick.held_action_bits == HELD_FIRE,
					"a press still held at its dispatch fires on that tick");
		} else {
			CHECK(tick.pressed_action_bits == PRESSED_RELOAD && tick.held_action_bits == 0,
					"a press released before its dispatch fires nothing; other edges keep");
		}
	}
}

// An input frame belongs to an update (D-NET-NEW-LI-8): a frame whose bank drains
// no update polls no row, the next frame's first update polls over the frame's
// sample, each later one over its own, and a paused update polls its rows too.
// [orig: Game_MainLoop @0x52B630 (the drain @0x52BA08, the update call @0x52BA58);
//  Game_ProcessMainFrame @0x5263F0 (Input_ProcessFrame @0x52661D)]
// Each rows-hook call: (the logic tick, whether the update sampled anew). An
// alias, so no template comma sits inside a CHECK argument.
using RowCalls = std::vector<std::pair<uint32_t, bool>>;

void test_rows_poll_once_per_update_and_never_in_a_no_update_frame() {
	struct Rows final : TickInputSource {
		RowCalls calls;
		int samples = 0;
		InputPacket sample_tick(uint32_t) override {
			++samples;
			return {};
		}
		void run_update_rows(uint32_t logic_tick, bool new_sample) override {
			calls.emplace_back(logic_tick, new_sample);
		}
	};
	{
		// A cold bank drains nothing under 4 ms, then a 14 ms frame drains one
		// quantum on the tick phase (inmatch_session_test's zero-tick frame).
		auto idle_role = std::make_unique<TickRecorder>();
		Rows idle_rows;
		Session idle_session(*idle_role);
		CHECK(load(idle_session), "the session runs");
		FrameInput idle;
		idle.delta_seconds = 0.002;
		idle.tick_source = &idle_rows;
		CHECK(idle_session.advance(idle).ticks_run() == 0 && idle_rows.calls.empty(),
				"a frame that drains no update polls no row");
		FrameInput next;
		next.delta_seconds = 0.014;
		next.tick_source = &idle_rows;
		const RowCalls one_poll{{500u, false}};
		CHECK(idle_session.advance(next).ticks_run() == 1 && idle_rows.calls == one_poll,
				"the next frame's one update polls once over its sample");
	}
	auto role = std::make_unique<TickRecorder>();
	Rows rows;
	Session session(*role);
	CHECK(load(session), "the session runs");
	FrameInput three;
	three.delta_seconds = TickAccumulator::kTickDt * 3.0;
	three.tick_source = &rows;
	CHECK(session.advance(three).ticks_run() == 3, "three updates");
	const RowCalls three_polls{{500u, false}, {501u, true}, {502u, true}};
	CHECK(rows.calls == three_polls && rows.samples == 3,
			"every update polls and samples, the first over the device state the frame's "
			"head stepped, each later one stepped to its own tick");
	rows.calls.clear();
	CHECK(session.pause().applied(), "the pause");
	const int ran = session.advance(three).ticks_run();
	CHECK(ran == 0 && !rows.calls.empty(), "a paused frame runs no tick");
	bool frozen = true;
	for (const auto &call : rows.calls) frozen = frozen && call.first == 503u && !call.second;
	CHECK(frozen, "each paused update polls its rows at the frozen tick over the frame's sample");
}

// The keyboard pass belongs to the update's input frame (D-CTRL-6 item 7): the
// source runs it at the head of run_update_rows and the update's sample follows, the
// frame's first update's included, so a key queued ahead of a frame that drains no
// update waits for the next update, whose first tick dispatches it. A paused
// update's sample takes its rows too, and the one-shots it leaves drop with it; the
// resuming update samples after the rows that lowered the word, so its tick
// dispatches what the same input frame queued.
// [orig: Game_MainLoop @0x52B630 (the update call @0x52BA58); Game_ProcessMainFrame
//  @0x5263F0 (Input_ProcessFrame @0x52661D); Input_ProcessPlayerFrame @0x49D4C0 (the
//  Input_ProcessKeyboardEvents call @0x49D4C6)]
// Each source call: ('r' rows or 's' sample, the logic tick).
using SourceCalls = std::vector<std::pair<char, uint32_t>>;

void test_the_keyboard_pass_runs_in_the_update_that_samples_it() {
	struct KeyDevice final : TickInputSource {
		SourceCalls calls;
		int queued = 0;           // keys the shell queued, not yet passed
		int events = 0;           // the pass's action row events, not yet sampled
		int rows_calls = 0;
		int unpause_at_rows = -1; // the rows call whose pass queues the pause row's record
		bool pause_due = false;
		void run_update_rows(uint32_t logic_tick, bool) override {
			calls.emplace_back('r', logic_tick);
			// The keyboard pass: every queued key becomes its row's event.
			events += queued;
			queued = 0;
			pause_due = rows_calls++ == unpause_at_rows;
		}
		// The pause row's record (catalog row 70, code 25), whose flip lowers the word.
		void queue_records(opennova::controls::InputEventQueue &records) override {
			if (pause_due)
				(void)records.push({opennova::controls::action_code(70), 0, 0, 0, false,
						opennova::controls::InputEventOrigin::Device});
			pause_due = false;
		}
		InputPacket sample_tick(uint32_t logic_tick) override {
			calls.emplace_back('s', logic_tick);
			InputPacket p;
			if (events != 0) p.pressed_action_bits = PRESSED_RELOAD;
			events = 0;
			return p;
		}
	};
	const auto reload = [](const TickInput &t) {
		return (t.player.pressed_action_bits & PRESSED_RELOAD) != 0;
	};
	{
		auto role = std::make_unique<TickRecorder>();
		KeyDevice device;
		Session session(*role);
		CHECK(load(session), "the session runs");
		device.queued = 1;
		// A cold bank drains nothing under 4 ms, then a 14 ms frame drains one update.
		FrameInput idle;
		idle.delta_seconds = 0.002;
		idle.tick_source = &device;
		CHECK(session.advance(idle).ticks_run() == 0 && device.calls.empty() &&
						device.queued == 1,
				"a frame that drains no update runs no keyboard pass: the key waits");
		FrameInput next;
		next.delta_seconds = 0.014;
		next.tick_source = &device;
		const SourceCalls rows_then_sample{{'r', 500u}, {'s', 500u}};
		CHECK(session.advance(next).ticks_run() == 1 && device.calls == rows_then_sample,
				"the next update passes the key at its input frame's head, then samples");
		CHECK(role->inputs.size() == 1 && reload(role->inputs[0]),
				"the update's own tick dispatches the waiting key");
	}
	{
		// The key passes in the first (paused) update; the second update's pass
		// lowers the word: its tick carries no edge of the first's sample.
		auto role = std::make_unique<TickRecorder>();
		KeyDevice device;
		Session session(*role);
		CHECK(load(session), "the session runs");
		spawn_local_player(*role);
		CHECK(session.pause().applied(), "the pause");
		device.queued = 1;
		device.unpause_at_rows = 1;
		FrameInput three;
		three.delta_seconds = TickAccumulator::kTickDt * 3.0;
		three.tick_source = &device;
		CHECK(session.advance(three).ticks_run() == 2, "the second update resumes");
		const SourceCalls paused_calls{
				{'r', 500u}, {'s', 500u}, {'r', 500u}, {'s', 500u}, {'r', 501u}, {'s', 501u}};
		CHECK(device.calls == paused_calls,
				"the paused update passes and samples at the frozen tick, as every update does");
		CHECK(role->inputs.size() == 2 && !reload(role->inputs[0]) && !reload(role->inputs[1]),
				"the paused update's sample dropped its one-shot: no tick dispatches it");
	}
	{
		auto role = std::make_unique<TickRecorder>();
		KeyDevice device;
		Session session(*role);
		CHECK(load(session), "the session runs");
		spawn_local_player(*role);
		CHECK(session.pause().applied(), "the pause");
		device.queued = 1;
		device.unpause_at_rows = 0;
		FrameInput three;
		three.delta_seconds = TickAccumulator::kTickDt * 3.0;
		three.tick_source = &device;
		CHECK(session.advance(three).ticks_run() == 3 && session.state() == State::Running,
				"the first update's pause row resumes it");
		CHECK(role->inputs.size() == 3 && reload(role->inputs[0]) &&
						role->inputs[0].head_stepped,
				"the resuming tick dispatches the key its own input frame passed");
		const SourceCalls resumed_calls{
				{'r', 500u}, {'s', 500u}, {'r', 501u}, {'s', 501u}, {'r', 502u}, {'s', 502u}};
		CHECK(device.calls == resumed_calls, "rows then sample in every update");
	}
}

// D-CTRL-6 (LI-s14): one event-record queue per input frame. The console's queued records
// wait at its head; each update's sample queues its action rows behind them; the update's
// drain dispatches every record in order, the front first, handing the ones the role does
// not own to the source; a reset inside a dispatch ends the drain (the records behind it
// never dispatch); the queue holds 32 records across the console's and the device's; a
// paused update drains its records too.
// [orig: Input_QueueEvent @0x4979C0 (the cap @0x4979C8..0x4979CB); Input_DequeueEvent
//  @0x497F30; Input_ProcessFrame @0x49D520 (the drain @0x49D599..0x49D5E7,
//  Input_HandleActionBinding @0x49D5D8); Input_ResetAllState @0x497ED0 (the memset
//  @0x497EE6); Input_HandleActionBinding @0x49AD40 (case 4 @0x49AF4C)]
using Dispatched = std::vector<std::pair<int32_t, uint32_t>>; // (code, the logic tick)

void test_the_update_drains_its_records_in_queue_order() {
	using opennova::controls::InputEventOrigin;
	using opennova::controls::InputEventQueue;
	using opennova::controls::InputEventRecord;
	struct RecordDevice final : TickInputSource {
		TickRecorder *role = nullptr;
		std::vector<std::vector<int32_t>> per_sample; // the codes each sample queues
		size_t samples = 0;
		Dispatched dispatched;
		bool quit_first = true; // the console's quit record ran ahead of every device record
		int32_t reset_code = -1; // a dispatch that runs the whole input reset
		InputPacket sample_tick(uint32_t) override { return {}; }
		void queue_records(InputEventQueue &records) override {
			if (samples < per_sample.size())
				for (const int32_t code : per_sample[samples])
					(void)records.push({code, 0, 0, 0, false, InputEventOrigin::Device});
			++samples;
		}
		void dispatch_record(const InputEventRecord &record) override {
			dispatched.emplace_back(record.code, role->kernel.world.logic_tick);
			quit_first = quit_first &&
					role->kernel.world.mission_exit_reason == opennova::inmatch::kMissionExitReset;
			if (record.code == reset_code) role->reset_all_input_state();
		}
	};
	const InputEventRecord quit{4, 0, 0, 0, false, InputEventOrigin::Console}; // row 78
	FrameInput three;
	three.delta_seconds = TickAccumulator::kTickDt * 3.0;
	{
		auto role = std::make_unique<TickRecorder>();
		RecordDevice device;
		device.role = role.get();
		device.per_sample = {{201, 203}, {19}};
		Session session(*role);
		CHECK(load(session), "the session runs");
		CHECK(role->input_events().push(quit), "the console's record waits at the head");
		three.tick_source = &device;
		CHECK(session.advance(three).ticks_run() == 3, "three updates");
		const Dispatched order{{201, 500u}, {203, 500u}, {19, 501u}};
		CHECK(device.dispatched == order && device.quit_first && role->input_events().empty(),
				"the console's record first, then each update's records in order, at its update");
	}
	{
		auto role = std::make_unique<TickRecorder>();
		RecordDevice device;
		device.role = role.get();
		device.per_sample = {{201, 300, 203}, {19}};
		device.reset_code = 300;
		Session session(*role);
		CHECK(load(session), "the session runs");
		three.tick_source = &device;
		CHECK(session.advance(three).ticks_run() == 3, "three updates");
		const Dispatched order{{201, 500u}, {300, 500u}, {19, 501u}};
		CHECK(device.dispatched == order,
				"a reset inside a dispatch ends the drain: the record behind it never dispatches");
	}
	{
		auto role = std::make_unique<TickRecorder>();
		RecordDevice device;
		device.role = role.get();
		device.per_sample = {{201, 203}};
		Session session(*role);
		CHECK(load(session), "the session runs");
		for (int i = 0; i < 31; ++i) (void)role->input_events().push(quit);
		CHECK(role->input_events().size() == 31, "31 console records");
		three.tick_source = &device;
		CHECK(session.advance(three).ticks_run() == 3, "three updates");
		const Dispatched order{{201, 500u}};
		CHECK(device.dispatched == order,
				"the queue holds 32 records across the console's and the device's: 203 dropped");
	}
	{
		auto role = std::make_unique<TickRecorder>();
		RecordDevice device;
		device.role = role.get();
		device.per_sample = {{201}};
		Session session(*role);
		CHECK(load(session), "the session runs");
		CHECK(session.pause().applied(), "the pause");
		three.tick_source = &device;
		CHECK(session.advance(three).ticks_run() == 0, "a paused frame runs no tick");
		const Dispatched order{{201, 500u}};
		CHECK(device.dispatched == order, "a paused update drains its records at the frozen tick");
	}
}

// D-CTRL-6 (LI-s15): the reload row is a record of the update's drain, its arm the role's
// past the head: a reload ahead of the pause row latches the weapon pump's reload; one
// behind it meets the head's pause gate (flags 0x0C000801, bit 0) and drops. ToSpecial's
// deferred dispatch is queued again behind the update's records and meets the same gate.
// [orig: Input_HandleActionBinding_0 @0x4E0420 (case 0xD3 -> WeaponSlot_RequestReload
//  @0x53F110); Input_HandleActionBinding @0x49AD40 (case 25 @0x49B520, the pause gate
//  @0x49ADDD..0x49ADE8); Input_ProcessFrame @0x49D520 (Input_FlushDeferredEvents @0x49D591)]
void test_the_reload_record_meets_the_head_in_key_order() {
	using opennova::controls::InputEventOrigin;
	using opennova::controls::InputEventQueue;
	const int32_t pause = opennova::controls::action_code(70);  // 25
	const int32_t reload = opennova::controls::action_code(47); // 211
	CHECK(pause == 25 && reload == 211, "the pause and reload rows' codes");
	struct Queue final : TickInputSource {
		std::vector<int32_t> codes;
		bool queued = false;
		InputPacket sample_tick(uint32_t) override { return {}; }
		void queue_records(InputEventQueue &records) override {
			if (queued) return;
			queued = true;
			for (const int32_t code : codes)
				(void)records.push({code, 0, 0, 0, false, InputEventOrigin::Device});
		}
	};
	for (const bool reload_first : {true, false}) {
		auto role = std::make_unique<TickRecorder>();
		spawn_local_player(*role);
		role->kernel.local.to_special_queued = 1; // a deferred ToSpecial dispatch
		Queue device;
		device.codes = reload_first ? std::vector<int32_t>{reload, pause}
		                            : std::vector<int32_t>{pause, reload};
		Session session(*role);
		CHECK(load(session), "the session runs");
		FrameInput three;
		three.delta_seconds = TickAccumulator::kTickDt * 3.0;
		three.tick_source = &device;
		CHECK(session.advance(three).ticks_run() == 0 && session.state() == State::Paused,
				"the first update's pause row pauses it");
		CHECK(role->kernel.local.weapon.reload_pressed == reload_first,
				reload_first ? "the reload ahead of the pause row latches"
				             : "the reload behind the pause row drops at the head");
		CHECK(role->kernel.local.to_special_queued == 0,
				"the input frame's flush moved the deferred ToSpecial into the event queue");
	}
}

// D-CTRL-6 (LI-s15): each update reads the menu-screen bit at its own head. A screen the
// first update's input frame opens (the escape row's INGAME) reaches the next update's
// sample, which runs no input frame: its records wait in the queue (a screen's close resets
// them), and nothing of the first update's is lost.
// [orig: Game_ProcessMainFrame @0x5263F0 (UI_IsMenuScreenOpen @0x52660D ahead of
//  Input_ProcessFrame @0x52661D, in every update); Input_HandleActionBinding @0x49AD40
//  (UI_OpenMenuScreen("game.mnu", "INGAME") @0x49B3B6)]
void test_a_menu_screen_an_update_opens_gates_the_frames_later_updates() {
	using opennova::controls::InputEventOrigin;
	using opennova::controls::InputEventQueue;
	using opennova::controls::InputEventRecord;
	struct MenuDevice final : TickInputSource {
		int samples = 0;
		std::vector<int32_t> dispatched;
		InputPacket sample_tick(uint32_t) override {
			InputPacket p;
			// The first update's pass opened the screen: every later sample carries the bit.
			if (samples++ > 0) p.held_action_bits = HELD_MENU_OPEN;
			return p;
		}
		void queue_records(InputEventQueue &records) override {
			(void)records.push({samples == 1 ? 201 : 203, 0, 0, 0, false, InputEventOrigin::Device});
		}
		void dispatch_record(const InputEventRecord &record) override {
			dispatched.push_back(record.code);
		}
	};
	auto role = std::make_unique<TickRecorder>();
	MenuDevice device;
	Session session(*role);
	CHECK(load(session), "the session runs");
	FrameInput three;
	three.delta_seconds = TickAccumulator::kTickDt * 3.0;
	three.tick_source = &device;
	CHECK(session.advance(three).ticks_run() == 3, "three updates");
	CHECK(device.dispatched == std::vector<int32_t>({201}),
			"only the first update's input frame dispatches");
	CHECK(role->input_events().size() == 2,
			"the later updates' records wait under the screen");
}

// D-CTRL-6 (LI-s16): the special-key chain's deploy pick (input event 12) and the escape row's
// event (18) are records of the update's drain in key order with the rows': the pick's
// parameter rides its record's first argument to the source's arm, a console record waiting
// ahead of the update's records dispatches first, and the pick's input reset (here the
// role's whole input reset, the memset its case runs) ends the drain there: the escape and the
// row behind it never dispatch.
// [orig: Input_HandleSpecialKeys @0x49c5c0 - Input_QueueEvent(12, 65534, 0, 0) @0x49ca14;
//  Input_ProcessFrame @0x49D520 (the drain @0x49D599..0x49D5E7); Input_HandleActionBinding
//  @0x49AD40 - case 12's Input_ResetAllState @0x49B042; Input_ResetAllState @0x497ED0 (the
//  memset @0x497EE6)]
void test_a_deploy_pick_is_a_record_of_the_drain_in_key_order() {
	using opennova::controls::InputEventOrigin;
	using opennova::controls::InputEventQueue;
	using opennova::controls::InputEventRecord;
	struct PickDevice final : TickInputSource {
		TickRecorder *role = nullptr;
		bool queued = false;
		std::vector<std::pair<int32_t, int32_t>> dispatched; // (code, first argument)
		bool quit_first = false;
		InputPacket sample_tick(uint32_t) override { return {}; }
		void queue_records(InputEventQueue &records) override {
			if (queued) return;
			queued = true;
			for (const InputEventRecord &record :
					{InputEventRecord{201, 0, 0, 0, false, InputEventOrigin::Device},
							InputEventRecord{12, 0xFFFE, 0, 0, false, InputEventOrigin::Device},
							InputEventRecord{18, 0, 0, 0, false, InputEventOrigin::Device},
							InputEventRecord{203, 0, 0, 0, false, InputEventOrigin::Device}})
				(void)records.push(record);
		}
		void dispatch_record(const InputEventRecord &record) override {
			if (dispatched.empty())
				quit_first = role->kernel.world.mission_exit_reason ==
						opennova::inmatch::kMissionExitReset;
			dispatched.emplace_back(record.code, record.p1);
			if (record.code == 12) role->reset_all_input_state();
		}
	};
	auto role = std::make_unique<TickRecorder>();
	PickDevice device;
	device.role = role.get();
	Session session(*role);
	CHECK(load(session), "the session runs");
	CHECK(role->input_events().push({4, 0, 0, 0, false, InputEventOrigin::Console}),
			"the console's quit record waits at the head");
	FrameInput three;
	three.delta_seconds = TickAccumulator::kTickDt * 3.0;
	three.tick_source = &device;
	CHECK(session.advance(three).ticks_run() == 3, "three updates");
	const std::vector<std::pair<int32_t, int32_t>> order{{201, 0}, {12, 0xFFFE}};
	CHECK(device.dispatched == order && device.quit_first,
			"the console's record first, then the rows' and the pick's in key order, the pick's "
			"parameter on its record; its reset drops the escape and the row behind it");
	CHECK(role->input_events().empty(), "the reset emptied the queue");
}

// D-CTRL-6 (LI-s16): the fire row's record (attack_1, code 149 = 0x95: a key's, a click's,
// a press's or a release's) is the role's arm past the head: the case drops it whole under a
// cease fire, then tests the binding live at its dispatch (the update's sample, HELD_FIRE):
// held, the press leg latches the weapon pump's fire request; up (a click whose button came
// up before its update), the release leg latches none (D-NET-NEW-LI-3). A PowerThrow def's
// press leg requests no fire: it latches the press (its windup test) only from the keyboard
// (HELD_FIRE_KEYS), never from a mouse's or a joystick's binding.
// [orig: Input_HandleActionBinding_0 @0x4E0420 - case 0x95: g_InCeaseFire @0x4E07C2,
//  Input_IsBindingActive(0x95) @0x4E07D9, the release leg @0x4E07E9, the press leg
//  @0x4E08DC (the PowerThrow sign bit @0x4E08FD, the device test @0x4E090B)]
void test_the_fire_record_tests_its_binding_live() {
	using opennova::controls::InputEventOrigin;
	using opennova::controls::InputEventQueue;
	CHECK(opennova::controls::action_code(43) == 149, "attack_1's code");
	struct FireDevice final : TickInputSource {
		bool held = false;
		bool keys = false;
		bool queued = false;
		InputPacket sample_tick(uint32_t) override {
			InputPacket p;
			p.held_action_bits = (held ? HELD_FIRE : 0u) | (keys ? HELD_FIRE_KEYS : 0u);
			return p;
		}
		void queue_records(InputEventQueue &records) override {
			if (queued) return;
			queued = true;
			(void)records.push({149, 0, 0, 0, false, InputEventOrigin::Device});
		}
	};
	for (const int face : {0, 1, 2}) { // held; up; held under a cease fire
		auto role = std::make_unique<TickRecorder>();
		spawn_local_player(*role);
		role->kernel.world.rules.cease_fire = face == 2;
		FireDevice device;
		device.held = face != 1;
		Session session(*role);
		CHECK(load(session), "the session runs");
		FrameInput three;
		three.delta_seconds = TickAccumulator::kTickDt * 3.0;
		three.tick_source = &device;
		CHECK(session.advance(three).ticks_run() == 3, "three updates");
		CHECK(role->kernel.local.weapon.fire_pressed == (face == 0),
				face == 0   ? "a held binding's record latches the fire request"
				: face == 1 ? "a binding that reads up takes the release leg: no request"
				            : "a cease fire drops the record whole");
	}
	for (const bool keys : {false, true}) { // a PowerThrow def: the mouse's binding; a key's
		auto role = std::make_unique<TickRecorder>();
		spawn_local_player(*role);
		role->kernel.local.weapon.def.flags = opennova::world::weapon_flag::kPowerThrow;
		FireDevice device;
		device.held = true;
		device.keys = keys;
		Session session(*role);
		CHECK(load(session), "the session runs");
		FrameInput three;
		three.delta_seconds = TickAccumulator::kTickDt * 3.0;
		three.tick_source = &device;
		CHECK(session.advance(three).ticks_run() == 3, "three updates");
		CHECK(role->kernel.local.weapon.fire_pressed == keys,
				keys ? "a PowerThrow's key press latches its windup test"
				     : "a PowerThrow's mouse press latches none: its left-down owns the windup");
	}
}

} // namespace

int main() {
	test_steps_land_on_their_ticks_inside_a_six_tick_frame();
	test_a_frame_sample_alone_quantizes_to_the_frame();
	test_unchanged_input_packs_unchanged_ticks();
	test_the_mouse_pump_spreads_four_times_the_cursor_over_the_updates();
	test_the_session_spreads_the_frames_cursor_over_its_ticks();
	test_the_pump_across_the_single_player_pause();
	test_an_input_reset_leaves_the_pumps_words();
	test_a_latched_fire_press_needs_its_binding_held_at_the_dispatch();
	test_rows_poll_once_per_update_and_never_in_a_no_update_frame();
	test_the_keyboard_pass_runs_in_the_update_that_samples_it();
	test_the_update_drains_its_records_in_queue_order();
	test_the_reload_record_meets_the_head_in_key_order();
	test_a_menu_screen_an_update_opens_gates_the_frames_later_updates();
	test_a_deploy_pick_is_a_record_of_the_drain_in_key_order();
	test_the_fire_record_tests_its_binding_live();
	if (g_failures != 0) {
		std::printf("%d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("inmatch_tick_input_source: ok\n");
	return 0;
}
