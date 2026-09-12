#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova {

// Six retail METPROTOCOL reporters. Keep the ordered fields: PLAYER emits
// GLANG/GTZB twice whenever the host is dedicated -- the game locale pair,
// then the player's own (+556/+588) pair; the one gate is g_is_dedicated_server
// [orig: Server_SendPlayerMetricsToGate @0x4fae5f]. PING nests one
// ENTRY/ENDENTRY row group per sorted ping entry (BIP1..BIP4, MS)
// [orig: Server_SendPingMetricsToGate @0x511BF0].
struct GateMetricsField {
	std::string name;
	std::string value;
	bool quoted = false;
};
struct GateMetricsReport {
	std::string block;
	std::vector<GateMetricsField> fields; // the block's own rows, in order
	// PING's `\tENTRY` .. `\tENDENTRY` groups, in order; each holds its
	// `\t\tKEY value` rows. Empty for the five flat reporters.
	std::vector<std::vector<GateMetricsField>> entries;
};

// Payload after NAPI envelope removal. Failure leaves report unchanged.
bool gate_metrics_decode(const uint8_t *data, size_t size, GateMetricsReport &report);

} // namespace opennova
