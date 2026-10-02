#pragma once

#include <net/novaworld/ping_sweep.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace opennova {

// The browser's ping-sweep lifecycle. A new sweep supersedes the running one at
// once: retail pauses the ping manager (stop-and-wait its thread), clears its
// entries and starts the fresh sweep in the same call, so a list refresh never
// waits for the previous list's pings [orig: NapiGameList_StartPingSweep
// @0x63bcf0 -> NapiTimer_Pause @0x63bd0c (Thread_RequestStopAndWait @0x62f846),
// the entry clear sub_62FE10 @0x63bd17, NapiPingManager_Start @0x63bd7b]. The
// superseded sweep never reports.
//
// The device leg differs in one respect: an OS echo already in flight writes
// into its reply buffer until it completes, so a superseded sweep cannot be
// torn down mid-chunk. The runner therefore stops it at its next chunk
// boundary on its own thread (its buffers live there until then) and starts
// the new sweep without waiting; finished sweeps are reaped on the next start
// and every one is joined by stop(). Each sweep runs `body`, which drives the
// echo passes over its echoes (run_ping_passes with the device pass) and polls
// `cancelled` between chunks; `done` receives the folded rows on the sweep's
// thread, only for a sweep nothing superseded.
class PingSweepRunner {
public:
	using SweepBody = std::function<void(std::vector<PingEcho> &echoes,
			const std::function<bool()> &cancelled)>;
	using SweepDone = std::function<void(std::vector<std::pair<int64_t, int>> results,
			int64_t generation)>;

	PingSweepRunner() = default;
	~PingSweepRunner() { stop(); }
	PingSweepRunner(const PingSweepRunner &) = delete;
	PingSweepRunner &operator=(const PingSweepRunner &) = delete;

	// Starts a sweep over (row id, dotted IPv4) targets; any running sweep is
	// superseded and never reports.
	void start(std::vector<std::pair<int64_t, std::string>> targets, SweepBody body,
			SweepDone done, int64_t generation) {
		reap_finished();
		for (const std::unique_ptr<Sweep> &sweep : sweeps_) sweep->cancelled.store(true);
		std::unique_ptr<Sweep> sweep = std::make_unique<Sweep>();
		Sweep *raw = sweep.get();
		raw->thread = std::thread([raw, targets = std::move(targets), body = std::move(body),
				done = std::move(done), generation]() {
			std::vector<PingEcho> echoes = make_ping_echoes(targets);
			if (body) body(echoes, [raw]() { return raw->cancelled.load(); });
			if (!raw->cancelled.load() && done) done(fold_ping_sweep(echoes), generation);
			raw->finished.store(true);
		});
		sweeps_.push_back(std::move(sweep));
	}

	// Supersedes every sweep and waits for their threads (the session's stop and
	// the owner's teardown); nothing reports afterwards.
	void stop() {
		for (const std::unique_ptr<Sweep> &sweep : sweeps_) sweep->cancelled.store(true);
		for (const std::unique_ptr<Sweep> &sweep : sweeps_) {
			if (sweep->thread.joinable()) sweep->thread.join();
		}
		sweeps_.clear();
	}

	// Sweeps whose threads are still held (running, or finished and not yet reaped).
	std::size_t sweep_count() const { return sweeps_.size(); }

private:
	struct Sweep {
		std::thread thread;
		std::atomic<bool> cancelled{false};
		std::atomic<bool> finished{false};
	};

	void reap_finished() {
		for (auto it = sweeps_.begin(); it != sweeps_.end();) {
			if ((*it)->finished.load()) {
				if ((*it)->thread.joinable()) (*it)->thread.join();
				it = sweeps_.erase(it);
			} else {
				++it;
			}
		}
	}

	std::vector<std::unique_ptr<Sweep>> sweeps_;
};

} // namespace opennova
