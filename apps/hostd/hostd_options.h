#pragma once

// opennova-hostd's command line. Every setting is a flag (the machine's
// systemd unit names them); the agent reads no environment variable.

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::hostd {

struct HostdOptions {
	std::string service_url; // --service: http://<the service's private address>:<port>
	int64_t machine_id = 0; // --machine
	std::string token_file = "/etc/opennova/hostd.token";
	std::string state_dir = "/var/lib/opennova-hostd";
	std::string slots_root = "/srv/slots";
	std::string docker = "/usr/bin/docker";
	std::string iptables = "/usr/sbin/iptables"; // "none": no egress counters
	std::string ip6tables = "/usr/sbin/ip6tables"; // "none", or used where the kernel has IPv6
	bool once = false;
	bool verbose = false;
};

enum class ParseOutcome : uint8_t { Run, Help, Version, Error };

ParseOutcome parse_hostd_options(
		const std::vector<std::string> &args, HostdOptions &out, std::string &error);
const char *hostd_usage();
// The agent's build (OPENNOVA_HOSTD_VERSION at configure time; "dev" without it).
const char *hostd_version();

} // namespace opennova::hostd
