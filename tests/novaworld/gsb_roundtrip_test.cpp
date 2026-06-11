// Sanity-check that gsb_build_response emits onnet's GSB wire format:
//   "GSB " header
//   <u32 enc_len><nwu_encrypt(payload, "3209452104342624532341")><4-byte magic>
// Per-chunk decrypt (nwu_decrypt) must yield intelligible bytes, and the
// magic tags must appear in the order IVAR / FLDS / SVRS / SVRS / XXXX.

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

void dump_hex(const uint8_t *p, size_t n) {
	for (size_t i = 0; i < n; ++i) {
		std::printf("%02X ", p[i]);
		if ((i + 1) % 16 == 0) std::printf("\n");
	}
	if (n % 16 != 0) std::printf("\n");
}

// Walk chunks: returns offset of the 4-byte magic trailer for each in
// order, ensuring each payload decrypts without corruption.
bool expect_next_chunk(const uint8_t *wire, size_t wire_len, size_t &cursor,
                       const char (&expected_tag)[5],
                       std::vector<uint8_t> *payload_out = nullptr) {
	if (cursor + 4 > wire_len) {
		std::fprintf(stderr, "chunk len truncated at cursor=%zu\n", cursor);
		return false;
	}
	const uint32_t enc_len = read_le32(wire + cursor);
	cursor += 4;
	if (cursor + enc_len + 4 > wire_len) {
		std::fprintf(stderr,
				"chunk payload+magic truncated at cursor=%zu enc_len=%u total=%zu\n",
				cursor, enc_len, wire_len);
		return false;
	}
	// Payload is encrypted (server used our nwu_decrypt == onnet's
	// nwu_encrypt, the ADD chain). Client decodes by running the inverse
	// SUBTRACT chain == our nwu_encrypt.
	std::vector<uint8_t> payload(wire + cursor, wire + cursor + enc_len);
	if (!payload.empty()) {
		opennova::nwu_encrypt(payload.data(), payload.size(), opennova::GSB_NWU_KEY);
	}
	if (payload_out) *payload_out = payload;
	cursor += enc_len;
	if (!check_tag(wire + cursor, expected_tag)) {
		std::fprintf(stderr, "FAIL: expected magic '%s' at cursor=%zu, got %02X %02X %02X %02X\n",
				expected_tag, cursor, wire[cursor], wire[cursor + 1],
				wire[cursor + 2], wire[cursor + 3]);
		return false;
	}
	std::printf("  chunk '%s': enc_len=%u at cursor=%zu\n", expected_tag, enc_len, cursor - enc_len);
	cursor += 4;
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
	entry.ip = {127, 0, 0, 1};
	entry.port = 64206;
	entry.server_name = "Test";
	entry.game_type = "COOP";
	entry.mission_name = "ASH_G11A";
	entry.region = "US";
	entry.players = 3;
	entry.max_players = 16;
	entry.dedicated = "Y";
	entry.password = "N";
	entry.country = "US";
	entry.stat = "Y";
	entry.ver1 = "3";
	entry.exp = "JO";
	entry.exp_bits = "3";
	entry.joicon2 = "4000";

	std::vector<opennova::GsbServerEntry> servers = {entry};
	std::vector<uint8_t> wire = opennova::gsb_build_response(servers);
	std::printf("wire length = %zu\n", wire.size());
	std::printf("header (first 16 bytes):\n");
	dump_hex(wire.data(), wire.size() < 16 ? wire.size() : 16);

	if (wire.size() < 4) {
		std::fprintf(stderr, "FAIL: wire too short to hold 'GSB ' header\n");
		return 1;
	}
	if (!check_tag(wire.data(), "GSB ")) {
		std::fprintf(stderr, "FAIL: header is %02X %02X %02X %02X, expected 'GSB '\n",
				wire[0], wire[1], wire[2], wire[3]);
		return 1;
	}

	size_t cursor = 4;
	std::vector<uint8_t> servers_payload;
	if (!expect_next_chunk(wire.data(), wire.size(), cursor, "IVAR")) return 1;
	if (!expect_next_chunk(wire.data(), wire.size(), cursor, "FLDS")) return 1;
	if (!expect_next_chunk(wire.data(), wire.size(), cursor, "SVRS")) return 1; // fields
	if (!expect_next_chunk(wire.data(), wire.size(), cursor, "SVRS", &servers_payload)) return 1; // servers
	if (!expect_next_chunk(wire.data(), wire.size(), cursor, "XXXX")) return 1;

	if (!contains_ascii(servers_payload, "Test") ||
	    !contains_ascii(servers_payload, "COOP") ||
	    !contains_ascii(servers_payload, "ASH_G11A") ||
	    !contains_ascii(servers_payload, "JO") ||
	    !contains_ascii(servers_payload, "4000")) {
		std::fprintf(stderr, "FAIL: decrypted server payload is missing expected GSB field values\n");
		return 1;
	}

	if (cursor != wire.size()) {
		std::fprintf(stderr, "FAIL: trailing bytes after XXXX: cursor=%zu wire=%zu\n",
				cursor, wire.size());
		return 1;
	}
	std::printf("PASS: GSB chunk sequence IVAR/FLDS/SVRS/SVRS/XXXX verified\n");
	return 0;
}
