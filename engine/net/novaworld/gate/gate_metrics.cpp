#include <net/novaworld/gate_metrics.h>
#include <net/novacrypto/nwu.h>

#include <algorithm>
#include <string_view>
#include <utility>

namespace opennova {

bool gate_metrics_decode(const uint8_t *data, size_t size, GateMetricsReport &report) {
	// [orig: Score_RecordEvent @0x4FA4B0; Server_SendPlayerMetricsToGate @0x4FAA30;
	// Server_SendMetricsToGate @0x4FB0A0; Server_SendMissionMetrics @0x4FB3A0;
	// Server_SendPlayerMissionMetrics @0x4FB8F0]
	// Reporters encrypt strlen bytes (no trailing NUL) with "1010101".
	// Crypto_EncryptBuffer @0x437510 has the same seed + ADD/reverse/progressive/
	// LCG chain as NapiNP_EncryptBuffer, including multiplier 0x04B05731.
	if (!data || size == 0 || size > 65507) return false;
	std::string plain(reinterpret_cast<const char *>(data), size);
	nwu_encrypt(reinterpret_cast<uint8_t *>(plain.data()), plain.size(), "1010101");
	std::string_view remaining(plain);
	auto line = [&remaining](std::string_view &out) {
		const auto end = remaining.find("\r\n");
		if (end == std::string_view::npos) return false;
		out = remaining.substr(0, end);
		remaining.remove_prefix(end + 2);
		return true;
	};
	std::string_view row;
	if (!line(row) || row != "METPROTOCOL 1") return false;
	if (!line(row) || row.size() < 9 || row.substr(0, 7) != "BLOCK \"" ||
		row.back() != '"') return false;
	GateMetricsReport decoded;
	decoded.block = row.substr(7, row.size() - 8);
	if (decoded.block != "SERVER" && decoded.block != "PLAYER" &&
		decoded.block != "ID" && decoded.block != "SERVERMISSION" &&
		decoded.block != "PLAYERMISSION") return false;
	while (line(row)) {
		if (row == "ENDBLOCK") {
			if (!remaining.empty()) return false;
			report = std::move(decoded);
			return true;
		}
		if (row.empty() || row.front() != '\t') return false;
		row.remove_prefix(1);
		const auto space = row.find(' ');
		if (space == std::string_view::npos || space == 0) return false;
		const auto key = row.substr(0, space);
		if (!std::all_of(key.begin(), key.end(), [](char c) {
			return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
		})) return false;
		auto value = row.substr(space + 1);
		if (value.empty()) return false;
		const bool quoted = value.front() == '"';
		if (quoted) {
			if (value.size() < 2 || value.back() != '"') return false;
			value = value.substr(1, value.size() - 2);
			if (value.find_first_of("\"\t\r\n") != std::string_view::npos ||
				value.find('\0') != std::string_view::npos) return false;
		} else {
			auto digits = value;
			if (digits.front() == '-') digits.remove_prefix(1);
			if (digits.empty() || !std::all_of(digits.begin(), digits.end(),
				[](char c) { return c >= '0' && c <= '9'; })) return false;
		}
		decoded.fields.push_back({std::string(key), std::string(value), quoted});
	}
	return false;
}

} // namespace opennova
