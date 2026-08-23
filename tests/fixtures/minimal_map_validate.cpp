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
// Only ids the engine addresses BY NUMBER are reserved; 106001 is the player start marker.
const int kPlayerStartItemId = 106001;

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
		if (committed.empty()) return fail == 0 ? 0 : 1;
		if (is_lfs_pointer(committed)) {
			std::printf("[skip] mnml.bms is an unpulled LFS pointer\n");
			return fail == 0 ? 0 : 1;
		}

		opennova::mission::MissionDocument doc;
		CHECK(doc.load_bms_bytes(committed.data(), committed.size()), "committed .bms parses");

		const size_t markers = doc.entity_count(opennova::mission::EntityKind::Marker);
		CHECK(markers >= 3, "the map carries the player start plus the two team starts");

		bool has_player_start = false;
		for (size_t i = 0; i < markers; ++i) {
			opennova::mission::EntityRecord rec{};
			if (doc.get_entity(opennova::mission::EntityKind::Marker, i, rec) &&
			    rec.item_id == kPlayerStartItemId) {
				has_player_start = true;
			}
		}
		// A mission without one loads completely and then strands you with nowhere to spawn.
		CHECK(has_player_start, "the map places the 106001 player start");
		CHECK(doc.info().terrain == kMapBase, "the header points at the minimal terrain");

		const int start_hour = doc.info().start_time / kQ8_8Hour;
		CHECK(start_hour >= kDaylightFirstHour && start_hour <= kDaylightLastHour,
		      "the mission starts in daylight (start_time is Q8.8 HOURS; 0 means midnight, and "
		      "the header overrides the .env's own curtime)");
	}

	if (fail == 0) std::printf("OK: minimal map .env + .bms valid\n");
	return fail == 0 ? 0 : 1;
}
