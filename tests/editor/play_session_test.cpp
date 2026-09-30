// Pins the Play launch plan and the one managed child (ADR 0046 d8/d10) over a fake
// platform: the argument vector, where a packaged editor finds its runtime, one child at a
// time, the stop request, the deadline kill, exit on its own with the code it exited with,
// and the build directory the session protects while alive.
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include <editor/run/launch_plan.h>
#include <editor/run/play_session.h>

#include "common/test_expect.h"
#include "editor/test_platform.h"

using namespace opennova::editor;

using editor_test::FakePlatform;

static int test_launch_plans() {
	const LaunchPlan play = make_play_launch_plan("C:/tools/opennova.exe", "C:/p/.opennova/build/play/abc", "jo", 8975, "m.bms");
	TEST_EXPECT(play.executable == "C:/tools/opennova.exe");
	TEST_EXPECT(play.working_dir == "C:/p/.opennova/build/play/abc");
	TEST_EXPECT(play.log_file == "C:/p/.opennova/build/play/abc/session.log");
	const std::vector<std::string> expected = {"--log-file", "C:/p/.opennova/build/play/abc/session.log", "--",
	                                           "--resource-dir", "C:/p/.opennova/build/play/abc", "/game", "jo",
	                                           "--mcp-port", "8975", "--mission", "m.bms"};
	TEST_EXPECT(play.args == expected);
	TEST_EXPECT(launch_plan_command_line(play).find("--resource-dir C:/p/.opennova/build/play/abc") != std::string::npos);

	const LaunchPlan quiet = make_play_launch_plan("opennova", "/b", "", 0);
	TEST_EXPECT(quiet.args == std::vector<std::string>({"--log-file", "/b/session.log", "--", "--resource-dir", "/b"}));
	const LaunchPlan headless = make_play_launch_plan("opennova", "/b", "", 0, "", {"--headless", "--quit-after", "3"});
	TEST_EXPECT(headless.args[0] == "--headless" && headless.args[2] == "3" && headless.args[3] == "--log-file");

	const LaunchPlan source = make_source_launch_plan("godot", "C:/repo/godot", "C:/p/build", "jo", 0);
	TEST_EXPECT(source.args[0] == "--path" && source.args[1] == "C:/repo/godot");
	TEST_EXPECT(source.args[2] == "res://game/game_runtime_root.tscn");
	TEST_EXPECT(source.args[3] == "--log-file");
	TEST_EXPECT(launch_plan_command_line(make_play_launch_plan("C:/a b/opennova.exe", "/x y", "", 0))
	                    .find("\"C:/a b/opennova.exe\"") == 0);

	// A packaged editor runs the runtime the release layout puts beside it; a source run
	// drives its own binary on the checkout.
	const PlayLauncher packaged =
	        make_play_launcher(false, "C:/OpenNova/editor/opennova_editor.exe", "", 8123, {"--headless"});
	TEST_EXPECT(!packaged.source_run && packaged.executable == "C:/OpenNova/runtime/opennova.exe");
	TEST_EXPECT(packaged.godot_project_dir.empty() && packaged.mcp_port == 8123);
	TEST_EXPECT(packaged.engine_args == std::vector<std::string>({"--headless"}));
	const PlayLauncher from_source = make_play_launcher(true, "C:/tools/godot.exe", "C:/repo/godot/", 0, {});
	TEST_EXPECT(from_source.source_run && from_source.executable == "C:/tools/godot.exe" &&
	            from_source.godot_project_dir == "C:/repo/godot/");
	return 0;
}

static int test_lifecycle() {
	// A child that runs on after a stop request until the test ends it (or the deadline kills it).
	FakePlatform platform;
	platform.next_pid = 100;
	platform.clock = 1000;
	platform.terminate_exits = false;
	PlaySession session(platform);
	Diagnostic error;
	TEST_EXPECT(session.state() == PlayState::Stopped);
	TEST_EXPECT(session.running_build_dir().empty());
	const LaunchPlan plan = make_play_launch_plan("opennova.exe", "/build/1", "jo", 9000);

	TEST_EXPECT(session.start(plan, error));
	TEST_EXPECT(session.state() == PlayState::Running && session.pid() == 100);
	TEST_EXPECT(session.running_build_dir() == "/build/1");
	TEST_EXPECT(platform.last_plan.working_dir == "/build/1");
	TEST_EXPECT(!session.start(plan, error)); // one child at a time
	TEST_EXPECT(error.code == "play.already_running");
	TEST_EXPECT(session.poll() == PlayState::Running);

	// The child exits on its own: the session notices on the next poll, and reads the code
	// it exited with before it lets the handle go.
	platform.codes[100] = 0xC0000005u;
	platform.exit_child(100);
	TEST_EXPECT(session.poll() == PlayState::Stopped);
	TEST_EXPECT(session.exited_on_its_own() && session.exit_code() == 0xC0000005LL);
	TEST_EXPECT(session.pid() == -1 && session.running_build_dir().empty());
	TEST_EXPECT(platform.log.back() == "release 100");

	// A stop request that the child honours.
	TEST_EXPECT(session.start(plan, error));
	session.stop();
	TEST_EXPECT(session.state() == PlayState::Stopping);
	TEST_EXPECT(platform.log.back() == "terminate 101");
	TEST_EXPECT(session.poll() == PlayState::Stopping);
	platform.codes[101] = 1;
	platform.exit_child(101);
	TEST_EXPECT(session.poll() == PlayState::Stopped);
	TEST_EXPECT(!session.exited_on_its_own() && session.exit_code() == -1); // stopped: no code of its own

	// A stop request the child ignores: killed once the deadline passes.
	TEST_EXPECT(session.start(plan, error));
	session.stop();
	platform.clock += kPlayStopDeadlineMs - 1;
	TEST_EXPECT(session.poll() == PlayState::Stopping);
	platform.clock += 1;
	TEST_EXPECT(session.poll() == PlayState::Stopped);
	TEST_EXPECT(platform.log[platform.log.size() - 2] == "kill 102");

	// wait() polls on the clock, pausing between observations, and gives up honestly.
	TEST_EXPECT(session.start(plan, error));
	const int64_t before_wait = platform.clock;
	TEST_EXPECT(!session.wait(35));
	TEST_EXPECT(platform.clock - before_wait >= 35 && platform.clock - before_wait < 35 + 2 * kPlayWaitStepMs);
	TEST_EXPECT(session.state() == PlayState::Running); // a timed-out wait changes nothing
	platform.exit_child(103);
	TEST_EXPECT(session.wait(35));
	TEST_EXPECT(session.exited_on_its_own() && session.exit_code() == -1); // a code the platform could not read

	platform.spawn_fails = true;
	TEST_EXPECT(!session.start(plan, error));
	TEST_EXPECT(error.code == "play.spawn");
	TEST_EXPECT(session.state() == PlayState::Stopped);
	TEST_EXPECT(std::string(play_state_label(PlayState::Stopping)) == "stopping");
	return 0;
}

int main() {
	int failures = 0;
	failures += test_launch_plans();
	failures += test_lifecycle();
	if (failures == 0) std::printf("editor_play_session: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
