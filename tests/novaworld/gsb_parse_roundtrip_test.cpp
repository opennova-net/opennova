// gsb_parse_response is the exact client-side inverse of gsb_build_response, in
// the retail GSB format (NapiGameList_ProcessEncryptedResponse @ 0x63d740, docs
// §7 Waves 7+9). Build a GSB blob from known entries, parse it back, and assert
// every serialized field round-trips: the SVRS row carries [u32 rid][4-byte
// IPv4 in_addr] then the positional FLDS values then the [u16 count][player
// names] tail — so rid, ip, the named fields, AND the player list all survive
// the wire.

#include <novaworld/gsb.h>

#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

using namespace opennova;

namespace {

int g_failures = 0;

bool expect(bool cond, const char *msg) {
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", msg);
		++g_failures;
	}
	return cond;
}

bool entries_match(const GsbServerEntry &a, const GsbServerEntry &b) {
	return a.rid == b.rid && a.ip == b.ip &&
	       a.server_name == b.server_name && a.game_type == b.game_type &&
	       a.mission_name == b.mission_name && a.region == b.region &&
	       a.players == b.players && a.max_players == b.max_players &&
	       a.dedicated == b.dedicated && a.time_left == b.time_left &&
	       a.password == b.password && a.country == b.country &&
	       a.msg == b.msg && a.age == b.age && a.time_of_day == b.time_of_day &&
	       a.stat == b.stat && a.level_range == b.level_range &&
	       a.locked == b.locked && a.tracers == b.tracers && a.skins == b.skins &&
	       a.bb_mode == b.bb_mode && a.mod == b.mod && a.pix == b.pix &&
	       a.pb_server == b.pb_server && a.ver1 == b.ver1 && a.exp == b.exp &&
	       a.exp_bits == b.exp_bits && a.joicon2 == b.joicon2 &&
	       a.player_names == b.player_names;
}

} // namespace

int main() {
	// Two servers with distinct, non-default values across every field so a
	// positional/ordering bug or a wrong field mapping is caught.
	GsbServerEntry a{};
	a.rid = 0x0A000123u;
	a.ip = "10.0.1.35";
	a.server_name = "Alpha Base";
	a.game_type = "AAS";
	a.mission_name = "ASH_G11A";
	a.region = "us";
	a.players = 7;
	a.max_players = 32;
	a.dedicated = "Y";
	a.time_left = "12:34";
	a.password = "N";
	a.country = "US";
	a.msg = "welcome";
	a.age = "0 01:02:03";
	a.time_of_day = "day";
	a.stat = "Y";
	a.level_range = "1-50";
	a.locked = "N";
	a.tracers = "Y";
	a.skins = "N";
	a.bb_mode = "1";
	a.mod = "stock";
	a.pix = "1";
	a.pb_server = "0";
	a.ver1 = "3";
	a.exp = "JO";
	a.exp_bits = "3";
	a.joicon2 = "4000";
	a.player_names = {"alice", "bob", "carol"};

	GsbServerEntry b{};
	b.rid = 0x0A000456u;
	b.ip = "192.0.2.254";
	b.server_name = "Bravo";
	b.game_type = "COOP";
	b.mission_name = "DESERT";
	b.region = "eu";
	b.players = 0;
	b.max_players = 16;
	b.dedicated = "N";
	b.password = "Y";
	b.country = "DE";
	b.locked = "Y";
	b.ver1 = "3";
	b.exp_bits = "1";
	b.joicon2 = "4001";
	// b.player_names left empty.

	std::vector<GsbServerEntry> servers = {a, b};
	auto wire = gsb_build_response(servers);

	GsbResponse parsed;
	if (!expect(gsb_parse_response(wire.data(), wire.size(), parsed), "parse round-trips")) {
		return 1;
	}
	expect(parsed.field_names.size() == 26, "26 field names recovered");
	expect(!parsed.field_names.empty() && parsed.field_names[0] == "ServerName",
	       "first field name is ServerName");
	expect(parsed.total_servers == 2, "total_servers == row count (2)");
	expect(parsed.total_players == 3, "total_players == sum of player-name lists (3 + 0)");
	if (expect(parsed.servers.size() == 2, "two servers parsed")) {
		expect(entries_match(parsed.servers[0], a), "server[0] fields round-trip (incl. ip + players)");
		expect(entries_match(parsed.servers[1], b), "server[1] fields round-trip");
	}

	// Empty list: still a valid blob (GSB / FLDS / SVRS / XXXX).
	auto empty_wire = gsb_build_response({});
	GsbResponse empty_parsed;
	expect(gsb_parse_response(empty_wire.data(), empty_wire.size(), empty_parsed),
	       "empty server list parses");
	expect(empty_parsed.servers.empty(), "empty list yields no servers");
	expect(empty_parsed.total_servers == 0, "empty list total_servers == 0");

	// Truncation: a buffer cut mid-blob must be rejected, not crash.
	expect(!gsb_parse_response(wire.data(), wire.size() / 2, empty_parsed),
	       "truncated buffer rejected");
	expect(!gsb_parse_response(wire.data(), 3, empty_parsed),
	       "buffer too short for a chunk header rejected");

	// Forged chunk length: overwrite the first ("GSB ") chunk's u32 length (bytes
	// 4..7, right after the 4-byte magic) with a near-UINT32_MAX value. The
	// bounds check must reject it without an out-of-bounds read.
	{
		std::vector<uint8_t> forged = wire;
		forged[4] = 0xFD; forged[5] = 0xFF; forged[6] = 0xFF; forged[7] = 0xFF; // 0xFFFFFFFD
		GsbResponse forged_parsed;
		expect(!gsb_parse_response(forged.data(), forged.size(), forged_parsed),
		       "forged oversized chunk length rejected");
	}

	if (g_failures == 0) {
		std::printf("OK: GSB build->parse round-trip verified (%zu servers, %zu-byte wire)\n",
		            servers.size(), wire.size());
		return 0;
	}
	std::fprintf(stderr, "%d assertion(s) failed\n", g_failures);
	return 1;
}
