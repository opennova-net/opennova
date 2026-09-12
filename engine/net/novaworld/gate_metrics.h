#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova {

// Five retail METPROTOCOL reporters. Keep the ordered fields: PLAYER emits
// GLANG/GTZB twice when both game and player locale data are available.
struct GateMetricsField {
	std::string name;
	std::string value;
	bool quoted = false;
};
struct GateMetricsReport {
	std::string block;
	std::vector<GateMetricsField> fields;
};

// Payload after NAPI envelope removal. Failure leaves report unchanged.
bool gate_metrics_decode(const uint8_t *data, size_t size, GateMetricsReport &report);

} // namespace opennova
