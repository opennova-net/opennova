// Sanity-check that gsb_build_response emits the retail GSB wire format
// (NapiGameList_ProcessEncryptedResponse @ 0x63d740, docs §7 Waves 7+9):
//   flat chunk stream, each chunk = <4-byte magic PREFIX><u32 LE len><payload>,
//   no bare file header, tags in the order GSB / FLDS / SVRS / XXXX.
// Per-chunk decrypt (nwu_encrypt, the SUBTRACT chain) must yield the values.

#include <novacrypto/nwu.h>
#include <novaworld/gsb.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

uint32_t read_le32(const uint8_t *p) {
	return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
	       (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

bool check_tag(const uint8_t *p, const char (&expected)[5]) {
	for (int i = 0; i < 4; ++i) {
		if (p[i] != static_cast<uint8_t>(expected[i])) return false;
	}
	return true;
}

// Walk one chunk in retail prefix order: <magic:4><u32 len><payload>. Verifies
// the tag and returns the decrypted payload.
bool expect_next_chunk(const uint8_t *wire, size_t wire_len, size_t &cursor,
                       const char (&expected_tag)[5],
                       std::vector<uint8_t> *payload_out = nullptr) {
	if (cursor + 8 > wire_len) {
		std::fprintf(stderr, "chunk header truncated at cursor=%zu\n", cursor);
		return false;
	}
	if (!check_tag(wire + cursor, expected_tag)) {
		std::fprintf(stderr, "FAIL: expected magic '%s' at cursor=%zu, got %02X %02X %02X %02X\n",
				expected_tag, cursor, wire[cursor], wire[cursor + 1],
				wire[cursor + 2], wire[cursor + 3]);
		return false;
	}
	cursor += 4;
	const uint32_t payload_len = read_le32(wire + cursor);
	cursor += 4;
	if (cursor + payload_len > wire_len) {
		std::fprintf(stderr, "chunk payload truncated at cursor=%zu len=%u total=%zu\n",
				cursor, payload_len, wire_len);
		return false;
	}
	// Payload is encrypted with the ADD chain (our nwu_decrypt); the client
	// decodes with the inverse SUBTRACT chain == our nwu_encrypt.
	std::vector<uint8_t> payload(wire + cursor, wire + cursor + payload_len);
	if (!payload.empty()) {
		opennova::nwu_encrypt(payload.data(), payload.size(), opennova::GSB_NWU_KEY);
	}
	if (payload_out) *payload_out = payload;
	cursor += payload_len;
	std::printf("  chunk '%s': len=%u\n", expected_tag, payload_len);
	return true;
}

bool contains_ascii(const std::vector<uint8_t> &haystack, const std::string &needle) {
	if (needle.empty() || haystack.size() < needle.size()) return false;
	for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
		if (std::memcmp(haystack.data() + i, needle.data(), needle.size()) == 0) {
			return true;
		}
	}
	return false;
}

} // namespace

int main() {
	opennova::GsbServerEntry entry{};
	entry.rid = 0xDEADBEEF;
	entry.ip = "203.0.113.7";
	entry.server_name = "Test";
	entry.game_type = "COOP";
	entry.mission_name = "ASH_G11A";
	entry.region = "US";
	entry.players = 3;
	entry.max_players = 16;
	entry.exp = "JO";
	entry.joicon2 = "4000";
	entry.player_names = {"alice", "bob"};

	std::vector<opennova::GsbServerEntry> servers = {entry};
	std::vector<uint8_t> wire = opennova::gsb_build_response(servers);
	std::printf("wire length = %zu\n", wire.size());

	// No bare header — the first chunk's magic is "GSB " itself.
	size_t cursor = 0;
	std::vector<uint8_t> servers_payload;
	if (!expect_next_chunk(wire.data(), wire.size(), cursor, "GSB ")) return 1;   // init
	if (!expect_next_chunk(wire.data(), wire.size(), cursor, "FLDS")) return 1;   // field names
	if (!expect_next_chunk(wire.data(), wire.size(), cursor, "SVRS", &servers_payload)) return 1; // rows
	if (!expect_next_chunk(wire.data(), wire.size(), cursor, "XXXX")) return 1;   // terminator

	// Row header: [u32 rid][4-byte IPv4]. The IP must be the raw a.b.c.d in_addr
	// bytes at payload[6..9] (after [u16 count][u32 rid]) — retail casts entry+4
	// to `struct in_addr` for the XXXX ping sweep [orig: NapiGameList_StartPingSweep
	// @ 0x63BCF0]; a little-endian integer here (the old u16-port encoding) makes
	// retail ping garbage addresses (D-NET-190).
	if (servers_payload.size() < 10 ||
	    servers_payload[6] != 203 || servers_payload[7] != 0 ||
	    servers_payload[8] != 113 || servers_payload[9] != 7) {
		std::fprintf(stderr, "FAIL: SVRS row dword1 is not the in_addr bytes of 203.0.113.7\n");
		return 1;
	}

	if (!contains_ascii(servers_payload, "Test") ||
	    !contains_ascii(servers_payload, "COOP") ||
	    !contains_ascii(servers_payload, "ASH_G11A") ||
	    !contains_ascii(servers_payload, "JO") ||
	    !contains_ascii(servers_payload, "4000") ||
	    !contains_ascii(servers_payload, "alice") ||
	    !contains_ascii(servers_payload, "bob")) {
		std::fprintf(stderr, "FAIL: decrypted SVRS payload is missing expected field/player values\n");
		return 1;
	}

	if (cursor != wire.size()) {
		std::fprintf(stderr, "FAIL: trailing bytes after XXXX: cursor=%zu wire=%zu\n",
				cursor, wire.size());
		return 1;
	}
	std::printf("PASS: GSB chunk sequence GSB/FLDS/SVRS/XXXX (prefix magic) verified\n");
	return 0;
}
