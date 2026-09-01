#pragma once

#include <godot_cpp/variant/callable.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace godot {

// The server-browser ping sweep's device leg: echo every target IPv4
// concurrently on a background thread and post ONE deferred call per pass to
// `sink` as (Dictionary rid -> ping, int generation). The witnessed semantics
// (per-echo timeout, retry count, the result fold) live in the engine header
// <net/novaworld/ping_sweep.h>; this TU is only the OS ICMP facility and the
// thread. On a platform without that facility the sink receives one empty
// pass immediately. The sink Callable is bound by ObjectID, so a receiver
// freed mid-sweep drops the deferred call safely.
void run_ping_sweep(std::vector<std::pair<int64_t, std::string>> targets,
                    Callable sink, int64_t generation);

} // namespace godot
