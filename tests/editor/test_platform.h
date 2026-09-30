#pragma once
// The process seams the editor's tests run a ProjectSession or a PlaySession over (ADR 0046
// d8), one home for every test: NoProcess starts nothing (a session a test drives never
// plays; the tries are counted), and FakePlatform is a pretend OS: children numbered from
// `next_pid`, a clock only a wait moves, the code each child a test ends exited with, each
// child's identity (the plan's executable, "created <pid>"), how process_liveness answers for a
// game it does not hold (a Play lease's) and the creation time each such ask named, and a log of
// every call ("spawn 500", "terminate 500", "kill 500", "release 500"); fixed_launcher is the
// launcher source a test's Play spawns through.
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <editor/run/process_platform.h>

namespace editor_test {

// A launcher source (the session's set_launcher_source) that gives `launcher` whenever the session
// asks, its port the one the test fixed: the shell's allocates a fresh one when asked at spawn time.
inline opennova::editor::PlayLauncherSource fixed_launcher(opennova::editor::PlayLauncher launcher) {
	return [launcher](bool) { return launcher; };
}

struct NoProcess : opennova::editor::ProcessPlatform {
	int spawns = 0; // the children Play tried to start

	int64_t spawn(const opennova::editor::LaunchPlan &) override {
		++spawns;
		return -1;
	}
	bool is_running(int64_t) override { return false; }
	bool terminate(int64_t) override { return true; }
	bool kill(int64_t) override { return true; }
	void release(int64_t) override {}
	int64_t now_ms() override { return 0; }
	void sleep_ms(int64_t) override {}
};

struct FakePlatform : opennova::editor::ProcessPlatform {
	int64_t next_pid = 500;
	int64_t clock = 0;
	bool spawn_fails = false;
	bool spawn_supported = true; // can_spawn
	// A stop request ends the child at once (a game that quits when asked); false: the child
	// runs on until the test ends it (exit_child) or a kill does.
	bool terminate_exits = true;
	std::vector<int64_t> running;
	std::map<int64_t, uint32_t> codes; // the code each exited child ended with
	// How process_liveness answers for a game this platform does not hold (one an earlier editor
	// started), by pid: Dead for any it does not list; and the creation time each ask named.
	std::map<int64_t, opennova::editor::ProcessLiveness> elsewhere;
	std::map<int64_t, std::string> asked_created;
	std::vector<std::string> log;
	opennova::editor::LaunchPlan last_plan;
	int spawns = 0;

	bool can_spawn() const override { return spawn_supported; }
	int64_t spawn(const opennova::editor::LaunchPlan &plan) override {
		last_plan = plan;
		++spawns;
		if (spawn_fails) return -1;
		running.push_back(next_pid);
		log.push_back("spawn " + std::to_string(next_pid));
		return next_pid++;
	}
	bool is_running(int64_t pid) override {
		for (const int64_t p : running)
			if (p == pid) return true;
		return false;
	}
	bool process_identity(int64_t pid, opennova::editor::ProcessIdentity &out) override {
		out.image = last_plan.executable;
		out.created = "created " + std::to_string(pid);
		return true;
	}
	opennova::editor::ProcessLiveness process_liveness(int64_t pid, const std::string &created) override {
		asked_created[pid] = created;
		const auto found = elsewhere.find(pid);
		return found == elsewhere.end() ? opennova::editor::ProcessLiveness::Dead : found->second;
	}
	bool terminate(int64_t pid) override {
		log.push_back("terminate " + std::to_string(pid));
		if (terminate_exits) exit_child(pid);
		return true;
	}
	bool kill(int64_t pid) override {
		log.push_back("kill " + std::to_string(pid));
		exit_child(pid);
		return true;
	}
	bool exit_code(int64_t pid, uint32_t &out) override {
		const auto it = codes.find(pid);
		if (it == codes.end() || is_running(pid)) return false;
		out = it->second;
		return true;
	}
	void release(int64_t pid) override { log.push_back("release " + std::to_string(pid)); }
	int64_t now_ms() override { return clock; }
	void sleep_ms(int64_t ms) override { clock += ms; } // a wait only ever moves this clock
	// The child `pid` ends (on its own, or as a stop or a kill ends it).
	void exit_child(int64_t pid) {
		for (size_t i = 0; i < running.size(); ++i)
			if (running[i] == pid) running.erase(running.begin() + static_cast<std::ptrdiff_t>(i));
	}
};

} // namespace editor_test
