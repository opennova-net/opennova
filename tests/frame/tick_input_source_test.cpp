// The session's per-tick input frame (D-NET-NEW-LI-1): retail runs its input
// frame in every logic tick (inmatch::TickInputSource carries the witness), so
// a scripted step due inside a frame that banks six ticks lands on its own
// tick, and a hold keeps its scripted length on the ticks the role packs.
// Driven with the real scenario device (devtools::ScriptedInput) over a probe
// role that records each tick's input at the logic tick it ran as.
#include <runtime/devtools/scripted_input.h>
#include <runtime/inmatch/session.h>
#include <runtime/mission/mission_kernel.h>

#include <cstdio>
#include <memory>
#include <string>
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
struct TickRecorder final : Role {
	opennova::mission::MissionKernel kernel;
	std::vector<TickInput> inputs;
	std::vector<uint32_t> ticks;

	TickRecorder() {
		bind(kernel);
		kernel.world.logic_tick = 500;
	}
	RoleKind kind() const override { return RoleKind::SinglePlayer; }
	void run_tick(const TickInput &input) override {
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

bool load(Session &session) {
	return session.begin_load().applied() && session.complete_load().applied();
}

// One shell frame banking six ticks: the frame's own sample at the logic tick
// its first tick runs as, then the session's advance with (or without) the
// device as the per-tick source.
FrameOutcome six_tick_frame(Session &session, TickRecorder &role, ScriptedDevice &device,
		bool per_tick) {
	FrameInput in;
	in.delta_seconds = TickAccumulator::kTickDt * 6.0;
	in.player = device.sample_tick(role.kernel.world.logic_tick);
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
	CHECK(samples[0] == 0 && samples[1] == 5, "the five catch-up ticks each sample the device");
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

} // namespace

int main() {
	test_steps_land_on_their_ticks_inside_a_six_tick_frame();
	test_a_frame_sample_alone_quantizes_to_the_frame();
	test_unchanged_input_packs_unchanged_ticks();
	test_the_mouse_pump_spreads_four_times_the_cursor_over_the_updates();
	test_the_session_spreads_the_frames_cursor_over_its_ticks();
	if (g_failures != 0) {
		std::printf("%d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("inmatch_tick_input_source: ok\n");
	return 0;
}
