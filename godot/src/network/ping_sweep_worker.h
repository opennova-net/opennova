#pragma once

#include <godot_cpp/variant/callable.hpp>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace godot {

// The server-browser ping sweep's device leg: one owned background thread
// that drives the engine's pass loop (<net/novaworld/ping_sweep.h> — the
// witnessed timeout, retry count, chunking, and fold) through the OS ICMP
// facility, then posts ONE deferred call to `sink` as
// (Dictionary rid -> ping, int generation). This TU is only the ICMP handle,
// the echo pass, and the thread. On a platform without that facility the
// sink receives every row as never-attempted immediately. The sink Callable
// is bound by ObjectID, so a receiver freed mid-sweep drops the deferred
// call safely.
class PingSweepWorker {
public:
	PingSweepWorker() = default;
	~PingSweepWorker();
	PingSweepWorker(const PingSweepWorker &) = delete;
	PingSweepWorker &operator=(const PingSweepWorker &) = delete;

	// Starts a sweep; false (and nothing started) while one is still in
	// flight — the caller re-issues once that sweep's results land.
	bool start(std::vector<std::pair<int64_t, std::string>> targets, Callable sink,
	           int64_t generation);
	// Asks the running sweep to stop at its next chunk boundary and joins it.
	void cancel();

private:
	std::thread thread_;
	std::atomic<bool> running_{false};
	std::atomic<bool> cancel_{false};
};

} // namespace godot
