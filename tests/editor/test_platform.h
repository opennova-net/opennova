#pragma once
// The process seams the editor's tests run a ProjectSession or a PlaySession over (ADR 0046
// d8), one home for every test: NoProcess starts nothing (a session a test drives never
// plays; the tries are counted), and FakePlatform is a pretend OS: children numbered from
// `next_pid`, a clock only a wait moves, the code each child a test ends exited with, and a
// log of every call ("spawn 500", "terminate 500", "kill 500", "release 500").
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <editor/run/process_platform.h>

namespace editor_test {

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
