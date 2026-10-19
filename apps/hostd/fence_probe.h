#pragma once

// The fences' self-test at the agent's start (nft_table.h): as a slot's user
// (UID 10001, slot 0's), a TCP connection to each of what only the agent may
// reach must be refused (or go unanswered): the instance metadata service,
// the fleet's internal port, the ECS task credentials address and the ECS
// agent's introspection port. One that connects means a server on the
// machine could reach it, so the agent refuses to run (main.cpp exits non-zero)
// rather than start servers behind a fence that does not hold. Platform
// plumbing, not a port.

#include "net_sockets.h"

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::hostd {

struct ProbeTarget {
	std::string what; // for the log: "the instance metadata service (169.254.169.254:80)"
	net::Endpoint endpoint;
};

// What the self-test probes: 169.254.169.254:80, 169.254.170.2:80,
// 127.0.0.1:51678 and every address of the fleet's internal listener
// (`fleet_addresses`, its name resolved) at `fleet_port`.
std::vector<ProbeTarget> fence_probe_targets(
		const std::vector<net::Endpoint> &fleet_addresses, uint16_t fleet_port);

// As this process: the first target a TCP connection reaches within
// `timeout_ms` (its `what`), or "" when every one was refused or unanswered.
std::string first_answering(const std::vector<ProbeTarget> &targets, int timeout_ms);

// first_answering as user `uid` (a child forked and dropped to it, its group
// too, no supplementary groups): true with `answered` its result, false with
// `error` when the child could not run as `uid`.
bool probe_as(uint32_t uid, const std::vector<ProbeTarget> &targets, int timeout_ms,
		std::string &answered, std::string &error);

} // namespace opennova::hostd
