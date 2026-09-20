// Pins the Play launch plan and the one managed child (ADR 0046 d8/d10) over a fake
// platform: the argument vector, one child at a time, the stop request, the deadline
// kill, exit on its own, and the build directory the session protects while alive.
#include <cstdio>
#include <string>
#include <vector>

#include <editor/run/launch_plan.h>
#include <editor/run/play_session.h>

#include "common/test_expect.h"

using namespace opennova::editor;

struct FakePlatform : ProcessPlatform {
	int64_t next_pid = 100;
	int64_t clock = 1000;
	bool spawn_fails = false;
	std::vector<int64_t> running;
	std::vector<std::string> log;
	LaunchPlan last_plan;

	int64_t spawn(const LaunchPlan &plan) override {
		last_plan = plan;
		if (spawn_fails) return -1;
		running.push_back(next_pid);
		log.push_back("spawn " + std::to_string(next_pid));
		return next_pid++;
	}
	bool is_running(int64_t pid) override {
		for (int64_t p : running) if (p == pid) return true;
		return false;
	}
	bool terminate(int64_t pid) override {
		log.push_back("terminate " + std::to_string(pid));
		return true;
	}
	bool kill(int64_t pid) override {
		log.push_back("kill " + std::to_string(pid));
		exit_child(pid);
		return true;
	}
	void release(int64_t pid) override { log.push_back("release " + std::to_string(pid)); }
	int64_t now_ms() override { return clock; }
	void sleep_ms(int64_t ms) override { clock += ms; } // a wait only ever advances this clock
	void exit_child(int64_t pid) {
		for (size_t i = 0; i < running.size(); ++i)
			if (running[i] == pid) running.erase(running.begin() + static_cast<long>(i));
	}
};

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

	const LaunchPlan source = make_source_launch_plan("godot", "C:/repo/godot", "C:/p/build", "jo", 0);
	TEST_EXPECT(source.args[0] == "--path" && source.args[1] == "C:/repo/godot");
	TEST_EXPECT(source.args[2] == "res://game/game_runtime_root.tscn");
	TEST_EXPECT(source.args[3] == "--log-file");
	TEST_EXPECT(launch_plan_command_line(make_play_launch_plan("C:/a b/opennova.exe", "/x y", "", 0))
	                    .find("\"C:/a b/opennova.exe\"") == 0);
	return 0;
}

static int test_lifecycle() {
	FakePlatform platform;
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

	// The child exits on its own: the session notices on the next poll.
	platform.exit_child(100);
	TEST_EXPECT(session.poll() == PlayState::Stopped);
	TEST_EXPECT(session.exited_on_its_own());
	TEST_EXPECT(session.pid() == -1 && session.running_build_dir().empty());
	TEST_EXPECT(platform.log.back() == "release 100");

	// A stop request that the child honours.
	TEST_EXPECT(session.start(plan, error));
	session.stop();
	TEST_EXPECT(session.state() == PlayState::Stopping);
	TEST_EXPECT(platform.log.back() == "terminate 101");
	TEST_EXPECT(session.poll() == PlayState::Stopping);
	platform.exit_child(101);
	TEST_EXPECT(session.poll() == PlayState::Stopped);
	TEST_EXPECT(!session.exited_on_its_own());

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
