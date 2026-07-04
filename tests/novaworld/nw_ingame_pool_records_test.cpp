// Byte-witness the §5.11 (S2C 0x0D pool-entity spawn batch) and §5.12 (S2C
// 0x20 bulk pool-3 entity sync) field maps against a live loopback capture.
//
// Drives the SAME outer-decode pipeline as `nw_ingame_histogram_test` —
// envelope CRC -> outer NWU -> per-session SCRK -> 0x43/0x83 -> protocol
// dispatch / reassembly — then hands each completed S2C 0x0D / 0x20 payload
// to the shared `decode_pool_spawn_batch` / `decode_pool3_sync_batch`
// decoders in `libs/npwire/include/npwire/ingame_decode.h`. Asserts:
//
//   - 37 × S2C 0x0D payloads + 29 × S2C 0x20 payloads in the capture (matches
//     the 2026-06-16b loopback; user-confirmed counts driving Stage C).
//   - Every payload's body is consumed EXACTLY by the shared decoder — a
//     non-clean return means the field map is wrong (or, equivalently, the
//     builder is wrong: D-NET-52..55).
//   - Position fields land in plausible 16.16 world bounds.
//
// Skips cleanly when `NW_INGAME_HEXCAP` is unset (CI stays green; see
// `tools/net/pcap_to_hexcap.py` for the converter).

#include <napi/envelope.h>
#include <napi/tlv.h>
#include <novacrypto/nwu.h>
#include <npwire/ingame_decode.h>
#include <npwire/protocol_message.h>
#include <npwire/session_hello.h>
#include <npwire/session_keys.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace opennova;

namespace {

bool hex_to_bytes(const std::string &hex, std::vector<uint8_t> &out) {
	if (hex.size() % 2 != 0) return false;
	out.clear();
	auto nib = [](char c) -> int {
		if (c >= '0' && c <= '9') return c - '0';
		if (c >= 'a' && c <= 'f') return c - 'a' + 10;
		if (c >= 'A' && c <= 'F') return c - 'A' + 10;
		return -1;
	};
	for (size_t i = 0; i < hex.size(); i += 2) {
		const int hi = nib(hex[i]), lo = nib(hex[i + 1]);
		if (hi < 0 || lo < 0) return false;
		out.push_back(static_cast<uint8_t>((hi << 4) | lo));
	}
	return true;
}

bool decode_outer(const std::vector<uint8_t> &raw, uint8_t &opcode,
                  std::vector<uint8_t> &body) {
	std::vector<uint8_t> stripped(raw.size());
	size_t out = 0;
	if (napi_envelope_decode(raw.data(), raw.size(), stripped.data(),
	                         stripped.size(), &out) != 0)
		return false;
	stripped.resize(out);
	if (stripped.empty()) return false;
	opcode = stripped[0];
	body.assign(stripped.begin() + 1, stripped.end());
	if (!body.empty()) nwu_encrypt(body.data(), body.size(), SESSION_NWU_KEY);
	return true;
}

bool position_plausible(int32_t v) {
	// 30000 * 0x10000 = 1.96e9, fits an int32; 50000 * 0x10000 would overflow.
	constexpr int32_t kHalfWorld = 30000 * 0x10000;
	return v > -kHalfWorld && v < kHalfWorld;
}

struct Stats {
	int payloads = 0;
	int payloads_clean = 0;
	int entities = 0;
	int entities_with_trailer = 0;  // 0x0D only
	int entities_empty_slot = 0;     // 0x20 only
	uint32_t any_flag_seen = 0;
	int max_count_per_payload = 0;
	int bad_position = 0;
};

Stats g_stats_0d;
Stats g_stats_20;

void walk_tag_0d(const std::vector<uint8_t> &body) {
	g_stats_0d.payloads++;
	PoolSpawnBatch batch;
	const bool clean = decode_pool_spawn_batch(body.data(), body.size(), batch);
	if (clean) g_stats_0d.payloads_clean++;
	if (batch.entity_count > g_stats_0d.max_count_per_payload)
		g_stats_0d.max_count_per_payload = batch.entity_count;
	for (const auto &r : batch.records) {
		g_stats_0d.entities++;
		g_stats_0d.any_flag_seen |= r.spawn_flags;
		if (r.spawn_flags & 0x0800) g_stats_0d.entities_with_trailer++;
		if (!position_plausible(r.pos_x) || !position_plausible(r.pos_y) ||
		    !position_plausible(r.pos_z))
			g_stats_0d.bad_position++;
	}
}

void walk_tag_20(const std::vector<uint8_t> &body) {
	g_stats_20.payloads++;
	Pool3SyncBatch batch;
	const bool clean = decode_pool3_sync_batch(body.data(), body.size(), batch);
	if (clean) g_stats_20.payloads_clean++;
	if (batch.entity_count > g_stats_20.max_count_per_payload)
		g_stats_20.max_count_per_payload = batch.entity_count;
	for (const auto &r : batch.records) {
		g_stats_20.entities++;
		if (r.is_empty_slot) { g_stats_20.entities_empty_slot++; continue; }
		g_stats_20.any_flag_seen |= r.flags_byte;
		if (!position_plausible(r.pos_x) || !position_plausible(r.pos_y) ||
		    !position_plausible(r.pos_z))
			g_stats_20.bad_position++;
	}
}

void process_protocol(const std::vector<uint8_t> &body, const std::string &scrk,
                      ProtocolReassemblyState &rs) {
	if (scrk.empty()) return;
	ProtocolPacketHeader hdr;
	std::vector<ProtocolMessage> msgs;
	if (!decode_protocol_packet_plaintext(body.data(), body.size(), scrk, hdr,
	                                      msgs))
		return;
	for (const auto &pm : msgs) {
		std::vector<uint8_t> assembled;
		if (!reassemble_protocol_payload(rs, pm, assembled)) continue;
		if (pm.flags.settings_update) continue;
		const int tag = int(pm.full_tag);
		if (tag == 0x0D) walk_tag_0d(assembled);
		else if (tag == 0x20) walk_tag_20(assembled);
	}
}

} // namespace

int main() {
	const char *path = std::getenv("NW_INGAME_HEXCAP");
	if (!path || !*path) {
		std::printf("[skip] set NW_INGAME_HEXCAP to a '<srcport> <frame> <hex>' "
		            "capture to run the pool-records witness\n");
		return 0;
	}
	std::ifstream file(path);
	if (!file) {
		std::printf("FAILED to open NW_INGAME_HEXCAP=%s\n", path);
		return 1;
	}

	struct Datagram {
		int srcport = 0;
		int frame = 0;
		std::vector<uint8_t> bytes;
	};
	std::vector<Datagram> dgrams;
	std::string line;
	while (std::getline(file, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.empty() || line[0] == '#') continue;
		std::istringstream ls(line);
		Datagram d;
		std::string hex;
		if (!(ls >> d.srcport >> d.frame >> hex)) continue;
		if (!hex_to_bytes(hex, d.bytes)) continue;
		dgrams.push_back(std::move(d));
	}
	std::printf("loaded %zu datagrams from %s\n", dgrams.size(), path);

	std::string client_scrk, server_scrk;
	ProtocolReassemblyState s_rs;

	for (const auto &d : dgrams) {
		uint8_t op = 0;
		std::vector<uint8_t> body;
		if (!decode_outer(d.bytes, op, body)) continue;
		switch (op) {
		case SESSION_OPCODE_CLIENT_AUTH: {
			ClientAuth a;
			if (parse_client_auth(body.data(), body.size(), a))
				client_scrk = a.scrk;
			break;
		}
		case SESSION_OPCODE_SERVER_AUTH: {
			ServerAuth a;
			if (parse_server_auth(body.data(), body.size(), a))
				server_scrk = a.scrk;
			break;
		}
		case SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE:
			process_protocol(body, server_scrk, s_rs);
			break;
		default:
			break;
		}
	}

	auto dump = [](const char *label, const Stats &s) {
		std::printf("\n=== %s ===\n", label);
		std::printf("  payloads:           %d (clean: %d)\n",
		            s.payloads, s.payloads_clean);
		std::printf("  entities total:     %d (max %d per payload)\n",
		            s.entities, s.max_count_per_payload);
		std::printf("  entities w/ trailer:%d\n", s.entities_with_trailer);
		std::printf("  entities empty slot:%d\n", s.entities_empty_slot);
		std::printf("  flag bits seen:     0x%x\n", s.any_flag_seen);
		std::printf("  bad positions:      %d\n", s.bad_position);
	};
	dump("S2C 0x0D pool-entity spawn batch (§5.11)", g_stats_0d);
	dump("S2C 0x20 bulk pool-3 entity sync (§5.12)", g_stats_20);

	int failures = 0;
	auto check = [&](bool cond, const char *what) {
		if (!cond) { std::printf("FAIL: %s\n", what); failures++; }
	};
	check(g_stats_0d.payloads == 37, "S2C 0x0D payload count == 37");
	check(g_stats_0d.payloads_clean == 37,
	      "every S2C 0x0D payload consumed exactly");
	check(g_stats_0d.bad_position == 0,
	      "S2C 0x0D positions all within world bounds");
	check(g_stats_20.payloads == 29, "S2C 0x20 payload count == 29");
	check(g_stats_20.payloads_clean == 29,
	      "every S2C 0x20 payload consumed exactly");
	check(g_stats_20.bad_position == 0,
	      "S2C 0x20 positions all within world bounds");

	if (failures) {
		std::printf("\n%d assertion(s) failed\n", failures);
		return 1;
	}
	std::printf("\nPASS: §5.11 and §5.12 field maps consume the wire exactly.\n");
	return 0;
}
