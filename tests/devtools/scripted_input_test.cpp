// ScriptedInput: the scenario driver's scripted input device (a probe
// instrument). Pins the shared script's validation, the retail action code ->
// catalog token resolution, the logic-tick keyed schedule (frame-quantized,
// one state change per token per sample, end/cancel releasing everything)
// and the setup pose's conversion into the retail bridge's ApplyPose words.
#include <runtime/devtools/scripted_input.h>

#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using opennova::devtools::ScriptedInput;
using opennova::devtools::ScriptPose;
using opennova::devtools::ScriptStep;
using Kind = opennova::devtools::ScriptStep::Kind;
using State = opennova::devtools::ScriptedInput::State;

namespace {

int g_failures = 0;

#define CHECK(cond, message)                                                          \
	do {                                                                              \
		if (!(cond)) {                                                                \
			std::printf("FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, message, #cond); \
			++g_failures;                                                             \
		}                                                                             \
	} while (0)

ScriptStep key(Kind kind, int64_t tick, int code) {
	ScriptStep s;
	s.kind = kind;
	s.tick = tick;
	s.code = code;
	return s;
}

ScriptStep look(int64_t tick, double dx, double dy) {
	ScriptStep s;
	s.kind = Kind::Look;
	s.tick = tick;
	s.dx_px = dx;
	s.dy_px = dy;
	return s;
}

ScriptStep end(int64_t tick) {
	ScriptStep s;
	s.kind = Kind::End;
	s.tick = tick;
	return s;
}

bool refuses(const std::vector<ScriptStep> &steps, const char *needle) {
	ScriptedInput driver;
	std::string error;
	const bool loaded = driver.load(steps, error);
	if (loaded) return false;
	if (error.find(needle) == std::string::npos) {
		std::printf("  refusal text was: %s\n", error.c_str());
		return false;
	}
	return !driver.loaded();
}

void test_load_refuses_malformed_scripts() {
	CHECK(refuses({key(Kind::Down, -1, 152), end(5)}, "step 0: the tick must be >= 0"),
			"a negative tick is refused");
	CHECK(refuses({key(Kind::Press, 0, 9999), end(5)}, "action code 9999"),
			"a code no catalog row dispatches is refused");
	CHECK(refuses({key(Kind::Down, 0, 152)}, "no end step"), "a script needs an end");
	CHECK(refuses({end(3), end(4)}, "second end step"), "one end only");
	CHECK(refuses({key(Kind::Down, 0, 152), end(3), key(Kind::Up, 4, 152)},
				  "step 2 at tick 4 falls after the end at tick 3"),
			"a step after the end is refused");
	CHECK(refuses({key(Kind::Down, 0, 152), end(3), key(Kind::Up, 3, 152)}, "step 2"),
			"a step listed after an end at the same tick never runs: refused");
	CHECK(refuses({key(Kind::Press, 3, 149), end(3)}, "step 0's release at tick 4"),
			"a press releasing after the end is refused");
	CHECK(refuses({look(0, std::numeric_limits<double>::infinity(), 0.0), end(1)}, "look_px"),
			"a non-finite look is refused");

	// A refusal leaves a loaded script in place.
	ScriptedInput driver;
	std::string error;
	CHECK(driver.load({key(Kind::Press, 0, 152), end(4)}, error), "a well-formed script loads");
	CHECK(!driver.load({key(Kind::Press, 0, 152)}, error), "the bad script is refused");
	CHECK(driver.loaded() && driver.start(), "the earlier script stays loaded and starts");
}

// The codes are the catalog rows' dispatch codes (controls::action_for_code).
void test_codes_hold_their_catalog_tokens() {
	const struct {
		int code;
		const char *token;
	} rows[] = {
		{152, "move_forward"}, {151, "move_back"}, {156, "strafe_left"}, {157, "strafe_right"},
		{153, "move_jump"}, {148, "LeanRoll_left"}, {147, "LeanRoll_right"}, {149, "attack_1"},
		{211, "magazine"}, {205, "FragGrenade"}, {201, "Knife"}, {209, "medpack"},
		{177, "useitem"}, {182, "seat1"}, {191, "seat10"}, {169, "Crouch"}, {170, "Prone"},
		{172, "Stand"}, {212, "cycleweaponP"}, {214, "cycleweaponN"},
	};
	for (const auto &row : rows) {
		ScriptedInput driver;
		std::string error;
		CHECK(driver.load({key(Kind::Down, 0, row.code), end(2)}, error), row.token);
		CHECK(driver.start(), row.token);
		driver.advance(7);
		CHECK(driver.token_held(row.token), row.token);
		CHECK(driver.held_codes() == std::vector<int>{row.code}, row.token);
	}
}

void test_steps_apply_on_the_logic_tick_they_are_keyed_to() {
	ScriptedInput driver;
	std::string error;
	CHECK(driver.load({key(Kind::Down, 2, 152), key(Kind::Up, 5, 152), end(8)}, error),
			"the walk script loads");
	CHECK(driver.state() == State::Idle && driver.start_logic_tick() == -1, "loaded, not started");
	CHECK(!driver.token_held("move_forward"), "nothing held before the start");
	CHECK(driver.start() && driver.state() == State::Armed, "start arms");
	CHECK(!driver.start(), "a started script does not re-arm");
	driver.advance(100);
	CHECK(driver.state() == State::Running && driver.start_logic_tick() == 100,
			"the first sample latches the start");
	driver.advance(101);
	CHECK(!driver.token_held("move_forward"), "tick 1: not yet");
	driver.advance(102);
	CHECK(driver.token_held("move_forward"), "tick 2: down");
	driver.advance(102);
	CHECK(driver.token_held("move_forward"), "a frame that ran no tick changes nothing");
	driver.advance(104);
	CHECK(driver.token_held("move_forward"), "a down holds across ticks until its up");
	driver.advance(105);
	CHECK(!driver.token_held("move_forward"), "tick 5: up");
	driver.advance(107);
	CHECK(driver.state() == State::Running, "tick 7: the end is still ahead");
	driver.advance(109);
	CHECK(driver.state() == State::Finished && driver.end_logic_tick() == 109,
			"the end applies at the first sample past its tick");
	CHECK(driver.cancel_reason().empty(), "a clean end carries no reason");
	const auto &records = driver.records();
	CHECK(records.size() == 3, "one record per source step");
	CHECK(records[0].applied_logic_tick == 102 && records[1].applied_logic_tick == 105 &&
					records[2].applied_logic_tick == 109,
			"each record is the logic tick its sample ran at");
	CHECK(records[0].release_logic_tick == -1, "only a press records a release");
	driver.advance(120);
	CHECK(driver.state() == State::Finished, "a finished script stays finished");
}

// One display frame may bank several logic ticks: a press whose down and up
// both fall due in one sample still reaches a sample held, and every later
// step keeps its order behind the deferred release.
void test_a_press_survives_a_multi_tick_frame() {
	ScriptedInput driver;
	std::string error;
	CHECK(driver.load({key(Kind::Press, 3, 149), key(Kind::Down, 4, 152), end(9)}, error),
			"the press script loads");
	driver.start();
	driver.advance(200);
	driver.advance(210); // ticks 1..10 due at once
	CHECK(driver.token_held("attack_1"), "the press's down holds for this sample");
	CHECK(!driver.token_held("move_forward"), "a later step waits behind the deferred release");
	CHECK(driver.state() == State::Running, "the end waits too");
	driver.advance(211);
	CHECK(!driver.token_held("attack_1"), "the release lands on the next sample");
	CHECK(driver.token_held("move_forward"), "the next step follows it in order");
	CHECK(driver.state() == State::Running, "the end waits for the down it would release");
	driver.advance(212);
	CHECK(driver.state() == State::Finished && driver.end_logic_tick() == 212, "then the end");
	CHECK(!driver.token_held("move_forward"), "the end releases everything");
	const auto &records = driver.records();
	CHECK(records[0].applied_logic_tick == 210 && records[0].release_logic_tick == 211,
			"the press records its down and its release");
	CHECK(records[1].applied_logic_tick == 211, "the deferred step records its real sample");
}

void test_press_is_down_then_up_one_tick_later() {
	ScriptedInput driver;
	std::string error;
	CHECK(driver.load({key(Kind::Press, 0, 177), end(3)}, error), "a USE press loads");
	driver.start();
	driver.advance(50);
	CHECK(driver.token_held("useitem"), "tick 0 down at the start sample");
	driver.advance(51);
	CHECK(!driver.token_held("useitem"), "tick 1 up: USE fires on this release");
	CHECK(driver.records()[0].applied_logic_tick == 50 &&
					driver.records()[0].release_logic_tick == 51,
			"down and release ticks");
}

void test_look_pixels_accumulate_until_taken() {
	ScriptedInput driver;
	std::string error;
	CHECK(driver.load({look(0, 10.0, -4.0), look(1, 5.0, 2.0), look(3, 1.0, 1.0), end(4)}, error),
			"the look script loads");
	driver.start();
	driver.advance(10);
	driver.advance(11);
	double dx = 0.0, dy = 0.0;
	driver.take_look(dx, dy);
	CHECK(dx == 15.0 && dy == -2.0, "the samples' look steps sum");
	driver.take_look(dx, dy);
	CHECK(dx == 0.0 && dy == 0.0, "take drains");
	driver.advance(14);
	driver.take_look(dx, dy);
	CHECK(dx == 1.0 && dy == 1.0 && driver.state() == State::Finished,
			"a look due with the end still reaches the frame");
}

void test_cancel_and_a_backwards_clock_release_everything() {
	ScriptedInput driver;
	std::string error;
	CHECK(driver.load({key(Kind::Down, 0, 152), key(Kind::Down, 0, 149), end(100)}, error),
			"two holds load");
	driver.start();
	driver.advance(5);
	CHECK(driver.token_held("move_forward") && driver.token_held("attack_1"), "both held");
	driver.cancel();
	CHECK(driver.state() == State::Cancelled && !driver.token_held("move_forward") &&
					!driver.token_held("attack_1"),
			"cancel releases every held token");
	driver.advance(6);
	CHECK(!driver.token_held("move_forward"), "a cancelled script never resumes");

	CHECK(driver.load({key(Kind::Down, 0, 152), end(100)}, error), "a reload replaces it");
	driver.start();
	driver.advance(500);
	CHECK(driver.token_held("move_forward"), "held again");
	driver.advance(1);
	CHECK(driver.state() == State::Cancelled && !driver.token_held("move_forward"),
			"a logic clock that runs backwards cancels and releases");
	CHECK(!driver.cancel_reason().empty(), "with its reason");
}

void test_setup_pose_converts_like_the_retail_bridge() {
	ScriptPose pose;
	std::string error;
	CHECK(opennova::devtools::script_pose_from_mission(12.5, -3.25, 128.75, 180.0, -22.5, pose,
				  error),
			"the onhook snapshot fixture's pose converts");
	CHECK(pose.position_q16[0] == 819200 && pose.position_q16[1] == -212992 &&
					pose.position_q16[2] == 8437760,
			"positions are truncated 16.16");
	CHECK(pose.heading_bam == static_cast<int32_t>(0xC0000000u),
			"yaw 180 is heading 270 (90 - yaw, wrapped)");
	CHECK(pose.pitch_bam == -268435456, "pitch -22.5 truncates to its BAM");
	CHECK(opennova::devtools::script_pose_from_mission(0.00001, -0.00001, 0.0, 0.0, 0.0, pose,
				  error),
			"tiny offsets convert");
	CHECK(pose.position_q16[0] == 0 && pose.position_q16[1] == 0,
			"truncation runs toward zero on both sides");
	CHECK(pose.heading_bam == 0x40000000, "yaw 0 faces heading 90");
	CHECK(!opennova::devtools::script_pose_from_mission(0.0, 0.0, 0.0, 0.0, 91.0, pose, error) &&
					error.find("pitch") != std::string::npos,
			"a pitch past 90 is refused");
	CHECK(!opennova::devtools::script_pose_from_mission(40000.0, 0.0, 0.0, 0.0, 0.0, pose, error),
			"a position outside the 16.16 range is refused");
}

} // namespace

int main() {
	test_load_refuses_malformed_scripts();
	test_codes_hold_their_catalog_tokens();
	test_steps_apply_on_the_logic_tick_they_are_keyed_to();
	test_a_press_survives_a_multi_tick_frame();
	test_press_is_down_then_up_one_tick_later();
	test_look_pixels_accumulate_until_taken();
	test_cancel_and_a_backwards_clock_release_everything();
	test_setup_pose_converts_like_the_retail_bridge();
	if (g_failures == 0) std::printf("scripted_input: all checks passed\n");
	return g_failures == 0 ? 0 : 1;
}
