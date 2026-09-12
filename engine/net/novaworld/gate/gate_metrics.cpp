#include <net/novaworld/gate_metrics.h>
#include <net/novacrypto/nwu.h>

#include <algorithm>
#include <string_view>
#include <utility>

namespace opennova {

bool gate_metrics_decode(const uint8_t *data, size_t size, GateMetricsReport &report) {
	// [orig: Score_RecordEvent @0x4FA4B0; Server_SendPlayerMetricsToGate @0x4FAA30;
	// Server_SendMetricsToGate @0x4FB0A0; Server_SendMissionMetrics @0x4FB3A0;
	// Server_SendPlayerMissionMetrics @0x4FB8F0; Server_SendPingMetricsToGate @0x511BF0]
	// Reporters encrypt strlen bytes (no trailing NUL) with "1010101".
	// Crypto_EncryptBuffer @0x437510 has the same seed + ADD/reverse/progressive/
	// LCG chain as NapiNP_EncryptBuffer, including multiplier 0x04B05731.
	// PING nests its per-player rows: `\tENTRY` @0x511f20, `\t\tBIP1..BIP4`
	// @0x511f4f..0x511fdd, `\t\tMS` @0x51200b, `\tENDENTRY` @0x512040, then the
	// shared `ENDBLOCK` @0x512079 and the same cipher @0x5120ab.
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
	auto field = [](std::string_view row, GateMetricsField &out) {
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
		out = {std::string(key), std::string(value), quoted};
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
		decoded.block != "PLAYERMISSION" && decoded.block != "PING") return false;
	bool in_entry = false;
	std::vector<GateMetricsField> entry;
	while (line(row)) {
		if (row == "ENDBLOCK") {
			if (in_entry || !remaining.empty()) return false;
			report = std::move(decoded);
			return true;
		}
		if (row.empty() || row.front() != '\t') return false;
		row.remove_prefix(1);
		if (row == "ENTRY") {
			if (in_entry) return false;
			in_entry = true;
			entry.clear();
			continue;
		}
		if (row == "ENDENTRY") {
			if (!in_entry) return false;
			in_entry = false;
			decoded.entries.push_back(std::move(entry));
			entry.clear();
			continue;
		}
		const bool nested = !row.empty() && row.front() == '\t';
		if (nested != in_entry) return false;
		if (nested) row.remove_prefix(1);
		GateMetricsField parsed;
		if (!field(row, parsed)) return false;
		(nested ? entry : decoded.fields).push_back(std::move(parsed));
	}
	return false;
}

} // namespace opennova
