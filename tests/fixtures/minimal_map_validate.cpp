// Validator for the minimal map's .env (time-of-day) and .bms (mission) legs.
//
// This used to be a GENERATOR: it built both files from scratch and asserted the committed
// bytes matched its own writer output. Both are now authored in ONED and committed, so a
// byte-equality guard would only assert that the C++ writer still agrees with itself -- and it
// went red the moment the editor authored the mission, which is the wrong signal entirely.
//
// What is worth guarding is what the ENGINE needs, checked against the committed file:
// it parses, it places the player, it points at the minimal terrain, and it starts in daylight.
//
// The .env leg was already a load-check rather than a byte compare (text EOLs differ), so it is
// unchanged in substance.
#include <env/env.h>
#include <mission/mission.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int fail = 0;

#define CHECK(cond, msg)                                  \
	do {                                                  \
		if (!(cond)) {                                    \
			std::fprintf(stderr, "FAIL: %s\n", (msg));    \
			++fail;                                       \
		}                                                 \
	} while (0)

const char *kMapBase = "mnml";
// Only ids the engine addresses BY NUMBER are reserved; 106001 is the player start marker the
// engine reads to place the hardcoded player item. The team starts are the MP spawn markers
// the host/join acceptance step depends on -- a map without both loads completely and leaves
// one team with nowhere to spawn.
const int kPlayerStartItemId = 106001;
const int kBlueTeamStartItemId = 106003;
const int kRedTeamStartItemId = 106004;

// start_time is Q8.8 HOURS [orig: the BMS header's Q8.8 start hour widens into the 8.24
// accumulator at Game_StartMission @ 0x525371]. A mission that starts at 0 renders under the
// .env's midnight ramp no matter what its curtime says -- which is exactly how this map spent
// its first life looking like an unlit void.
const int kQ8_8Hour = 256;
const int kDaylightFirstHour = 6;
const int kDaylightLastHour = 18;

std::string path(const std::string &name) { return std::string(GAME_ASSETS_DIR) + "/" + name; }

bool read_file(const std::string &p, std::vector<uint8_t> &out) {
	std::ifstream f(p, std::ios::binary);
	if (!f) return false;
	out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	return true;
}

bool is_lfs_pointer(const std::vector<uint8_t> &b) {
	static const char kSentinel[] = "version https://git-lfs";
	const size_t n = sizeof(kSentinel) - 1;
	return b.size() >= n && std::equal(kSentinel, kSentinel + n, b.begin());
}

} // namespace

int main() {
	// ---- .env: it loads ----
	{
		std::vector<uint8_t> committed;
		CHECK(read_file(path(std::string(kMapBase) + ".env"), committed), "committed .env missing");
		CHECK(!committed.empty(), "committed .env is empty");
		if (!committed.empty() && !is_lfs_pointer(committed)) {
			opennova::env::Config cfg;
			std::string err;
			std::string text(committed.begin(), committed.end());
			std::istringstream is(text);
			CHECK(opennova::env::load_env(is, cfg, err), err.c_str());
		}
	}

	// ---- .bms: it parses and carries what the engine needs ----
	{
		std::vector<uint8_t> committed;
		CHECK(read_file(path(std::string(kMapBase) + ".bms"), committed), "committed .bms missing");
		// An empty file is not a mission; it must not pass by having nothing to check.
		CHECK(!committed.empty(), "committed .bms is empty");
		if (committed.empty()) return fail == 0 ? 0 : 1;
		if (is_lfs_pointer(committed)) {
			std::printf("[skip] mnml.bms is an unpulled LFS pointer\n");
			return fail == 0 ? 0 : 1;
		}

		opennova::mission::MissionDocument doc;
		CHECK(doc.load_bms_bytes(committed.data(), committed.size()), "committed .bms parses");

		const size_t markers = doc.entity_count(opennova::mission::EntityKind::Marker);
		int player_starts = 0;
		int blue_starts = 0;
		int red_starts = 0;
		for (size_t i = 0; i < markers; ++i) {
			opennova::mission::EntityRecord rec{};
			if (!doc.get_entity(opennova::mission::EntityKind::Marker, i, rec)) continue;
			if (rec.item_id == kPlayerStartItemId) ++player_starts;
			if (rec.item_id == kBlueTeamStartItemId) ++blue_starts;
			if (rec.item_id == kRedTeamStartItemId) ++red_starts;
		}
		// A mission without one loads completely and then strands you with nowhere to spawn;
		// retail's own missions carry exactly one (00TRa.bms: 1331 entities, one 106001).
		CHECK(player_starts == 1, "the map places exactly one 106001 player start");
		CHECK(blue_starts >= 1, "the map places a Blue Team start (106003) for host/join");
		CHECK(red_starts >= 1, "the map places a Red Team start (106004) for host/join");
		CHECK(doc.info().terrain == kMapBase, "the header points at the minimal terrain");

		const int start_hour = doc.info().start_time / kQ8_8Hour;
		CHECK(start_hour >= kDaylightFirstHour && start_hour <= kDaylightLastHour,
		      "the mission starts in daylight (start_time is Q8.8 HOURS; 0 means midnight, and "
		      "the header overrides the .env's own curtime)");
	}

	if (fail == 0) std::printf("OK: minimal map .env + .bms valid\n");
	return fail == 0 ? 0 : 1;
}
