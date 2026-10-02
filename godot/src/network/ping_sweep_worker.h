#pragma once

#include <godot_cpp/variant/callable.hpp>

#include <net/novaworld/ping_sweep_runner.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace godot {

// The server-browser ping sweep's device leg: the OS ICMP echo pass over the
// engine's sweep lifecycle (<net/novaworld/ping_sweep_runner.h> — a new sweep
// supersedes the running one at once; the witnessed timeout, retry count,
// chunking and fold live in <net/novaworld/ping_sweep.h>). A finished sweep
// posts ONE deferred call to `sink` as (Dictionary rid -> ping, int
// generation). This TU is only the ICMP handle and the echo pass. On a
// platform without that facility the sink receives every row as
// never-attempted. The sink Callable is bound by ObjectID, so a receiver freed
// mid-sweep drops the deferred call safely.
class PingSweepWorker {
public:
	// Starts a sweep; a running one is superseded at once and never reports.
	void start(std::vector<std::pair<int64_t, std::string>> targets, Callable sink,
	           int64_t generation);
	// Supersedes every sweep and joins their threads (nothing reports afterwards).
	void cancel() { runner_.stop(); }

private:
	opennova::PingSweepRunner runner_;
};

} // namespace godot
