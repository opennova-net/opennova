// Validator for the minimal map's .env (time-of-day) and .bms (mission) legs.
//
// This used to be a GENERATOR: it built both files from scratch and asserted the committed
// bytes matched its own writer output. Both are now authored in ONED and committed, so a
// byte-equality guard would only assert that the C++ writer still agrees with itself -- and it
// went red the moment the editor authored the mission, which is the wrong signal entirely.
//
// What is worth guarding is what the ENGINE needs, checked against the committed file:
// it parses, it places the player, it points at the minimal terrain, it starts in daylight,
// its offline kit equips the AK-47 by its real weapon identity, and it places the set's own
// model, the house (items.def 108001, assets/house.3di).
//
// `--write` is deliberately a surgical editor rather than a generator: it loads the committed
// ONED-authored mission, adds the AK kit row when the mission has no kit (the PR #610 starting
// state) or changes only the first row's name, adds the house when no building names it, and
// emits it through the production BMS writer (bms::write, from scratch). Existing rows and
// fields are otherwise preserved.
//
// The .env leg was already a load-check rather than a byte compare (text EOLs differ), so it is
// unchanged in substance.
#include <formats/env/env.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/mission/mission.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
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
const char *kFirstPlayerWeapon = "WPN_AK47AUTO";
// Only ids the engine addresses BY NUMBER are reserved; 106001 is the player start marker the
// engine reads to place the hardcoded player item. The team starts are the MP spawn markers
// the host/join acceptance step depends on -- a map without both loads completely and leaves
// one team with nowhere to spawn.
const int kPlayerStartItemId = 106001;
const int kBlueTeamStartItemId = 106003;
const int kRedTeamStartItemId = 106004;
// The house (items.def 108001, assets/house.3di): the set's first authored model, placed
// 24 m north of the player start. BMS axes are x east, y north, z up, so a yaw-0 spawn
// at the origin looks straight at its 8 m x 4 m south wall, with the model's collision-only
// water tank off its east side.
const int kHouseItemId = 108001;
const opennova::mission::EntityTransform kHousePlacement{0.0f, 24.0f, 0.0f, 0, 0, 0};

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

int main(int argc, char **argv) {
	bool write_mode = false;
	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "--write") == 0) {
			write_mode = true;
		} else {
			std::fprintf(stderr, "usage: minimal_map_validate_test [--write]\n");
			return 2;
		}
	}

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
		const std::string bms_path = path(std::string(kMapBase) + ".bms");
		std::vector<uint8_t> committed;
		CHECK(read_file(bms_path, committed), "committed .bms missing");
		// An empty file is not a mission; it must not pass by having nothing to check.
		CHECK(!committed.empty(), "committed .bms is empty");
		if (committed.empty()) return fail == 0 ? 0 : 1;
		if (is_lfs_pointer(committed)) {
			if (write_mode) {
				CHECK(false, "mnml.bms is an unpulled LFS pointer; pull LFS before --write");
				return 1;
			}
			std::printf("[skip] mnml.bms is an unpulled LFS pointer\n");
			return fail == 0 ? 0 : 1;
		}

		opennova::bms::File doc;
		std::string error;
		if (!opennova::bms::parse(committed.data(), committed.size(), doc, error)) {
			CHECK(false, error.c_str());
			return 1;
		}

		std::vector<opennova::mission::WeaponLoadoutEntry> loadout =
				opennova::mission::weapon_loadout(doc);
		std::string previous_weapon;
		bool weapon_changed = false;
		if (write_mode && (loadout.empty() || loadout.front().name != kFirstPlayerWeapon)) {
			if (loadout.empty()) {
				previous_weapon = "<none>";
				opennova::mission::WeaponLoadoutEntry first;
				first.name = kFirstPlayerWeapon;
				first.ammo_primary = "6";
				first.ammo_secondary = "-1";
				first.flags = "-1";
				loadout.push_back(std::move(first));
			} else {
				previous_weapon = loadout.front().name;
				loadout.front().name = kFirstPlayerWeapon;
			}
			CHECK(opennova::mission::set_weapon_loadout(doc, loadout, error), error.c_str());
			weapon_changed = fail == 0;
		}
		CHECK(!loadout.empty(), "the mission carries an offline weapon kit");
		if (!loadout.empty())
			CHECK(loadout.front().name == kFirstPlayerWeapon,
			      "the first offline weapon is WPN_AK47AUTO (the actual AK identity)");

		int houses = 0;
		for (const opennova::bms::Entity &rec : doc.buildings)
			if (opennova::mission::entity_item_id(rec) == kHouseItemId) ++houses;
		bool house_added = false;
		if (write_mode && houses == 0) {
			opennova::mission::add_entity(doc, opennova::mission::EntityKind::Building, kHouseItemId,
			                              kHousePlacement);
			houses = 1;
			house_added = true;
		}
		CHECK(houses == 1, "the map places exactly one house (108001), the set's own model");

		int player_starts = 0;
		int blue_starts = 0;
		int red_starts = 0;
		for (const opennova::bms::Entity &rec : doc.markers) {
			const int item_id = opennova::mission::entity_item_id(rec);
			if (item_id == kPlayerStartItemId) ++player_starts;
			if (item_id == kBlueTeamStartItemId) ++blue_starts;
			if (item_id == kRedTeamStartItemId) ++red_starts;
		}
		// A mission without one loads completely and then strands you with nowhere to spawn;
		// retail's own missions carry exactly one (00TRa.bms: 1331 entities, one 106001).
		CHECK(player_starts == 1, "the map places exactly one 106001 player start");
		CHECK(blue_starts >= 1, "the map places a Blue Team start (106003) for host/join");
		CHECK(red_starts >= 1, "the map places a Red Team start (106004) for host/join");
		const opennova::mission::MissionInfo info = opennova::mission::mission_info(doc);
		CHECK(info.terrain == kMapBase, "the header points at the minimal terrain");

		const int start_hour = info.start_time / kQ8_8Hour;
		CHECK(start_hour >= kDaylightFirstHour && start_hour <= kDaylightLastHour,
		      "the mission starts in daylight (start_time is Q8.8 HOURS; 0 means midnight, and "
		      "the header overrides the .env's own curtime)");

		if (write_mode && (weapon_changed || house_added) && fail == 0) {
			std::vector<uint8_t> generated;
			CHECK(opennova::bms::write(doc, generated, error), error.c_str());

			// Never replace the committed mission until the produced bytes parse and carry the edit.
			opennova::bms::File verify;
			if (fail == 0)
				CHECK(opennova::bms::parse(generated.data(), generated.size(), verify, error),
				      "rewritten mnml.bms parses");
			if (fail == 0) {
				const std::vector<opennova::mission::WeaponLoadoutEntry> written_loadout =
						opennova::mission::weapon_loadout(verify);
				CHECK(!written_loadout.empty() && written_loadout.front().name == kFirstPlayerWeapon,
				      "rewritten mnml.bms keeps WPN_AK47AUTO first");
				int written_houses = 0;
				for (const opennova::bms::Entity &rec : verify.buildings)
					if (opennova::mission::entity_item_id(rec) == kHouseItemId) ++written_houses;
				CHECK(written_houses == 1, "rewritten mnml.bms places the house once");
			}

			if (fail == 0) {
				std::ofstream output(bms_path, std::ios::binary | std::ios::trunc);
				CHECK(output.good(), "cannot open mnml.bms for writing");
				if (output.good() && !generated.empty())
					output.write(reinterpret_cast<const char *>(generated.data()),
					             static_cast<std::streamsize>(generated.size()));
				CHECK(output.good(), "cannot write mnml.bms");
				if (output.good())
					std::printf("wrote %s (kit %s -> %s; house %s, %zu bytes)\n", bms_path.c_str(),
					            weapon_changed ? previous_weapon.c_str() : "kept", kFirstPlayerWeapon,
					            house_added ? "added" : "kept", generated.size());
			}
		} else if (write_mode && fail == 0) {
			std::printf("%s already starts with %s and places the house\n", bms_path.c_str(),
			            kFirstPlayerWeapon);
		}
	}

	if (fail == 0)
		std::printf("OK: minimal map .env + .bms valid; offline kit starts with AK-47; the house stands\n");
	return fail == 0 ? 0 : 1;
}
