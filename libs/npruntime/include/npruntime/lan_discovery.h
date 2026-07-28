#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::np {

// Godot- and socket-free projection of the retail fields carried by one LAN
// host's NP/NAPI 0x81 ServerHello.
struct LanDiscoveryServer {
	std::string server_name;
	std::string session_id;
	std::string expansion;
	uint32_t gametype = 0;
	uint32_t current_players = 0;
	uint32_t max_players = 0;
};

// Build a complete, ready-to-send NWU datagram containing the same NP/NAPI
// 0x41 JointOperations identity as the existing direct-peer join path. The
// caller owns broadcast/socket policy.
std::vector<uint8_t> build_lan_discovery_probe(uint32_t client_index);

// Decode a complete NWU datagram and project a 0x81 ServerHello into the LAN
// browser model. Returns false for malformed packets or any opcode other than
// ServerHello; `out` is changed only on success.
bool parse_lan_discovery_reply(const uint8_t *data, size_t size, LanDiscoveryServer &out);

} // namespace opennova::np
