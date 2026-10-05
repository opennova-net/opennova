#pragma once

#include <net/npwire/lan_discovery.h>
#include <net/npwire/net_ports.h>

#include "net_sockets.h"

#include <cstdint>
#include <string>

namespace opennova::lan_probe {

enum class Status {
	Ready,
	Timeout,
	SocketError,
	SendError,
};

struct Options {
	net::Endpoint endpoint{{127, 0, 0, 1}, kRetailLanPortMin};
	int timeout_ms = 120000;
	int retry_interval_ms = 1000;
	uint32_t client_index = 1;
	std::string expected_server_name;
};

struct Result {
	Status status = Status::Timeout;
	opennova::LanDiscoveryServer server;
	net::Endpoint source;
	int probes_sent = 0;
};

// Wait until the endpoint answers the exact retail JO 0x41 LAN discovery
// probe with a valid game-server 0x81. The caller owns net::startup/shutdown.
// A non-empty expected_server_name rejects stale/unrelated listeners while
// continuing to probe until the same bounded deadline.
Result wait_for_server(const Options &options);

} // namespace opennova::lan_probe
