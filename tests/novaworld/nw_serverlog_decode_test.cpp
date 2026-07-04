// Witness the /PROFILE .sph server-log decoder against the controlled dvxi5
// probe recordings (host.sph + client.sph) — the engine's own per-frame view
// of the SAME session captured in
// host_and_join_game_on_opennovaworld_loopback_mission_probe.pcapng.
//
// These are the engine's decoded values, so they cross-validate the in-game
// replication RE independently of the wire decoders. The known mission layout
// (two teams, Blue at -X / Red at +X, facing opposite) gives exact expected
// values. See docs/net/novaworld-net-re.md §5.22.
//
// Asserts (the controlled knowns):
//   - both files decode with leftover==0 and end on .END;
//   - client roster = {id2 Blue "TestPlayer", id3 Red "FooPlayer"};
//   - frame-0 player states: id2 at (-70, 25, ~38) yaw 90deg (0x40000000),
//     id3 at (+70, 25, ~56) yaw 270deg (0xc0000000);
//   - host names mission "mission.bms", logs FooPlayer's join + its disconnect
//     + at least one death;
//   - host and client agree on the shared frame-0 state for id2.
//
// Skips cleanly when NW_PROFILE_SPH_DIR is unset (CI stays green); point it at
// the folder containing host.sph / client.sph.

#include <npwire/serverlog_decode.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace opennova;

namespace {

int g_failures = 0;

#define CHECK(cond, ...)                                                   \
	do {                                                                   \
		if (!(cond)) {                                                     \
			std::printf("FAIL: " __VA_ARGS__);                             \
			std::printf("  (%s:%d: %s)\n", __FILE__, __LINE__, #cond);     \
			++g_failures;                                                  \
		}                                                                  \
	} while (0)

bool read_file(const std::string &path, std::vector<uint8_t> &out) {
	std::ifstream f(path, std::ios::binary);
	if (!f) return false;
	out.assign(std::istreambuf_iterator<char>(f),
	           std::istreambuf_iterator<char>());
	return true;
}

const ServerLogEntity *find_entity(const ServerLogFrame &fr, uint32_t id) {
	for (const auto &e : fr.entities)
		if (e.net_id == id) return &e;
	return nullptr;
}

const ServerLogPlayer *find_player(const ServerLogDocument &d, uint32_t id) {
	for (const auto &p : d.roster)
		if (p.net_id == id) return &p;
	return nullptr;
}

// 16.16 constants for the controlled knowns.
constexpr int32_t FP_NEG70 = -70 * 65536;  // 0xFFBA0000
constexpr int32_t FP_POS70 = 70 * 65536;   // 0x00460000
constexpr int32_t FP_25 = 25 * 65536;      // 0x00190000

} // namespace

int main() {
	const char *dir = std::getenv("NW_PROFILE_SPH_DIR");
	if (!dir || !*dir) {
		std::printf("[skip] set NW_PROFILE_SPH_DIR to a folder with host.sph / "
		            "client.sph to run the .sph server-log witness\n");
		return 0;
	}
	const std::string base = std::string(dir);
	std::vector<uint8_t> host_bytes, client_bytes;
	if (!read_file(base + "/host.sph", host_bytes) ||
	    !read_file(base + "/client.sph", client_bytes)) {
		std::printf("FAILED to open host.sph / client.sph under %s\n", dir);
		return 1;
	}

	ServerLogDocument host, client;
	const bool host_ok = decode_server_log(host_bytes.data(), host_bytes.size(), host);
	const bool client_ok =
			decode_server_log(client_bytes.data(), client_bytes.size(), client);

	// --- structural: clean decode to .END --------------------------------
	CHECK(client_ok && client.ended_clean && client.leftover_bytes == 0,
	      "client.sph did not decode cleanly (leftover=%zu)\n",
	      client.leftover_bytes);
	CHECK(host_ok && host.ended_clean && host.leftover_bytes == 0,
	      "host.sph did not decode cleanly (leftover=%zu)\n",
	      host.leftover_bytes);

	// --- client roster: the two known players -----------------------------
	CHECK(client.roster.size() == 2, "client roster size %zu != 2\n",
	      client.roster.size());
	const ServerLogPlayer *cp2 = find_player(client, 2);
	const ServerLogPlayer *cp3 = find_player(client, 3);
	CHECK(cp2 && cp2->team == 1 && cp2->name == "TestPlayer",
	      "client id2 is not Blue TestPlayer\n");
	CHECK(cp3 && cp3->team == 2 && cp3->name == "FooPlayer",
	      "client id3 is not Red FooPlayer\n");

	// --- frame-0 player states (the authored knowns) ----------------------
	CHECK(!client.frames.empty(), "client has no frames\n");
	if (!client.frames.empty()) {
		const ServerLogFrame &f0 = client.frames.front();
		const ServerLogEntity *e2 = find_entity(f0, 2);
		const ServerLogEntity *e3 = find_entity(f0, 3);
		CHECK(e2 && e2->pos_x == FP_NEG70 && e2->pos_y == FP_25 &&
		              e2->yaw_bam == 0x40000000u,
		      "client frame0 id2 (Blue) pos/yaw mismatch\n");
		CHECK(e3 && e3->pos_x == FP_POS70 && e3->pos_y == FP_25 &&
		              e3->yaw_bam == 0xc0000000u,
		      "client frame0 id3 (Red) pos/yaw mismatch\n");
	}

	// --- host: mission name, join roster, event timeline ------------------
	CHECK(host.mission == "mission.bms", "host mission '%s' != mission.bms\n",
	      host.mission.c_str());
	const ServerLogPlayer *hp2 = find_player(host, 2);
	const ServerLogPlayer *hp3 = find_player(host, 3);
	CHECK(hp2 && hp2->team == 1, "host id2 not Blue\n");
	CHECK(hp3 && hp3->team == 2, "host id3 not Red (join event)\n");
	int deaths = 0;
	bool disc3 = false;
	for (const auto &ev : host.events) {
		if (ev.kind == ServerLogEventKind::Death) ++deaths;
		if (ev.kind == ServerLogEventKind::Disconnect && ev.net_id == 3)
			disc3 = true;
	}
	CHECK(deaths >= 1, "host logged no deaths\n");
	CHECK(disc3, "host did not log FooPlayer (id3) disconnect\n");

	// --- host/client agree on the shared frame-0 state for id2 ------------
	if (!host.frames.empty() && !client.frames.empty()) {
		const ServerLogEntity *h2 = find_entity(host.frames.front(), 2);
		const ServerLogEntity *c2 = find_entity(client.frames.front(), 2);
		CHECK(h2 && c2 && h2->pos_x == c2->pos_x && h2->pos_y == c2->pos_y &&
		              h2->pos_z == c2->pos_z && h2->yaw_bam == c2->yaw_bam,
		      "host vs client disagree on frame0 id2 state\n");
	}

	std::printf("nw_serverlog_decode: client frames=%zu host frames=%zu "
	            "host events=%zu failures=%d\n",
	            client.frames.size(), host.frames.size(), host.events.size(),
	            g_failures);
	return g_failures == 0 ? 0 : 1;
}
