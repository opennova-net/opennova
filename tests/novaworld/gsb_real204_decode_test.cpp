// Decodes the GENUINE NovaLogic server-browser blob captured from
// 207.178.209.204 (fixtures/novaworld/nw204_jop_2.gsb — the HTTP body of
// `GET /jop_2.gsb?a=1`, exported from the retail Wireshark trace). This is the
// byte-level oracle for the GSB wire format (docs/net/novaworld-net-re.md §8
// Wave 7, D-NET-32..35): if our gsb_parse_response — built only from the
// witnessed retail parser NapiGameList_ProcessEncryptedResponse @ 0x63d740 —
// walks this real blob through to its XXXX terminator and recovers the FLDS
// column table + SVRS rows, the chunk framing/row structure is confirmed
// against retail (not just self-consistent with our own builder).
//
// It also DUMPS the recovered field names + the first rows so the exact retail
// FLDS column set / IP / port / player tail can be read off and reconciled.

#include <novaworld/gsb.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace opennova;

int main() {
	const std::string path = std::string(FIXTURE_DIR) + "/nw204_jop_2.gsb";
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		std::fprintf(stderr, "FAIL: cannot open fixture %s\n", path.c_str());
		return 1;
	}
	std::vector<uint8_t> blob((std::istreambuf_iterator<char>(file)),
	                          std::istreambuf_iterator<char>());
	std::printf("blob: %zu bytes\n", blob.size());

	GsbResponse r;
	if (!gsb_parse_response(blob.data(), blob.size(), r)) {
		std::fprintf(stderr,
		    "FAIL: gsb_parse_response rejected the genuine .204 blob — chunk "
		    "framing/row layout still diverges from retail.\n");
		return 1;
	}

	std::printf("total_servers=%d total_players=%d field_count=%zu\n",
	            r.total_servers, r.total_players, r.field_names.size());
	std::printf("FLDS columns:");
	for (const auto &n : r.field_names) std::printf(" [%s]", n.c_str());
	std::printf("\n");

	const size_t dump_n = r.servers.size() < 3 ? r.servers.size() : 3;
	for (size_t i = 0; i < dump_n; ++i) {
		const auto &s = r.servers[i];
		std::printf("server[%zu]: rid=%u (0x%08X) port=%u name='%s' "
		            "players=%d/%d region='%s' game='%s' player_names=%zu\n",
		            i, s.rid, s.rid, s.port,
		            s.server_name.c_str(), s.players, s.max_players,
		            s.region.c_str(), s.game_type.c_str(), s.player_names.size());
	}

	int failures = 0;
	if (r.field_names.empty()) {
		std::fprintf(stderr, "FAIL: no FLDS field names recovered\n");
		++failures;
	}
	if (r.servers.empty()) {
		std::fprintf(stderr, "FAIL: no SVRS server rows recovered\n");
		++failures;
	}
	if (failures == 0) {
		std::printf("OK: genuine .204 GSB blob decoded through the XXXX terminator\n");
		return 0;
	}
	return 1;
}
