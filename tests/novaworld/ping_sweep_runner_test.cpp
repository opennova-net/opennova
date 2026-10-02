// The browser's ping-sweep lifecycle (engine/net/novaworld/ping_sweep_runner.h):
// a new sweep supersedes the running one at once and the superseded sweep never
// reports [orig: NapiGameList_StartPingSweep @0x63bcf0 -> NapiTimer_Pause @0x63bd0c,
// NapiPingManager_Start @0x63bd7b]. A device pass blocked mid-chunk (an OS echo in
// flight) must not hold the new sweep back.

#include <net/novaworld/ping_sweep_runner.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace opennova;

namespace {

int g_failures = 0;

bool expect(bool cond, const char *msg) {
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", msg);
		++g_failures;
	}
	return cond;
}

template <typename Pred>
bool wait_for(Pred pred, int timeout_ms) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	while (std::chrono::steady_clock::now() < deadline) {
		if (pred()) return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return pred();
}

} // namespace

int main() {
	std::mutex gate_mutex;
	std::condition_variable gate;
	bool release_first = false;
	std::atomic<int> bodies_started{0};
	std::atomic<int> first_reports{0};
	std::atomic<int> second_reports{0};
	std::atomic<int64_t> second_generation{0};
	std::atomic<int> second_ms{-100};

	PingSweepRunner runner;
	// The first sweep's pass blocks as an in-flight echo chunk would.
	runner.start({{1, "10.0.0.1"}},
			[&](std::vector<PingEcho> &echoes, const std::function<bool()> &) {
				++bodies_started;
				std::unique_lock<std::mutex> lock(gate_mutex);
				gate.wait(lock, [&] { return release_first; });
				for (PingEcho &e : echoes) { e.answered = true; e.ms = 5; }
			},
			[&](std::vector<std::pair<int64_t, int>>, int64_t) { ++first_reports; }, 1);
	expect(wait_for([&] { return bodies_started.load() == 1; }, 2000), "the first sweep starts");

	// A refresh: the second sweep starts at once, while the first is still blocked.
	runner.start({{2, "10.0.0.2"}},
			[&](std::vector<PingEcho> &echoes, const std::function<bool()> &) {
				++bodies_started;
				for (PingEcho &e : echoes) { e.answered = true; e.ms = 9; }
			},
			[&](std::vector<std::pair<int64_t, int>> rows, int64_t generation) {
				second_generation.store(generation);
				if (!rows.empty()) second_ms.store(rows.front().second);
				++second_reports;
			},
			2);
	expect(wait_for([&] { return second_reports.load() == 1; }, 2000),
	       "the refreshed list's sweep reports without waiting for the superseded one");
	expect(second_generation.load() == 2 && second_ms.load() == 9,
	       "the report carries the new sweep's generation and its folded row");

	{
		std::lock_guard<std::mutex> lock(gate_mutex);
		release_first = true;
	}
	gate.notify_all();
	runner.stop();
	expect(first_reports.load() == 0, "the superseded sweep never reports");
	expect(runner.sweep_count() == 0, "stop joins every sweep");

	// stop() supersedes a running sweep too: it never reports.
	std::atomic<int> stopped_reports{0};
	runner.start({{3, "10.0.0.3"}},
			[&](std::vector<PingEcho> &, const std::function<bool()> &cancelled) {
				wait_for([&] { return cancelled(); }, 2000);
			},
			[&](std::vector<std::pair<int64_t, int>>, int64_t) { ++stopped_reports; }, 3);
	runner.stop();
	expect(stopped_reports.load() == 0, "a stopped sweep never reports");

	if (g_failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("ping_sweep_runner_test OK\n");
	return 0;
}
