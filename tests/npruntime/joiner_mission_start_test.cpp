// The joiner's mission-start submissions (D-NET-531, D-NET-1020): the C2S 0x0B
// mission status computed from the side password and the engine build's packed version
// (ONE's), and
// the C2S 0x2F a joiner with no kit seam submits, the fresh profile's page resolved
// through the weapon table by name. Synthetic tables only; the oracles are the cited
// retail arithmetic and the R3 corpus reading (every retail 0x0B in the ASH and 03TR
// cells is 00 x9 07 05 07 01).
#include <runtime/inmatch/joiner_connection.h>
#include <runtime/world/weapon_table.h>

#include <base/version/one_build.h>
#include <formats/playersav/weapon_sav.h>
#include <net/protocol/protocol.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace opennova;

namespace {

int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

// The 0x0B's version dword is the engine build's packed version, one byte per part, the
// first in the high byte: ONE's wire version (0.0.22.0 is 0x00001600) where retail's binary
// stored its own beside its sprintf, 0x01070507 for JO's and DFX2's 1.7.5.7
// [orig: Game_ParseCommandLineAndInit @0x4a7310 - 0x01070507 @0x4a7d86].
void test_mission_status_version_is_one_build() {
	CHECK((Version{ { 1u, 7u, 5u, 7u }, 4 }).packed() == 0x01070507u);
	CHECK((Version{ { 0u, 0u, 22u, 0u }, 4 }).packed() == 0x00001600u);
	const Version wire = one_build().wire_version();
	CHECK(wire.count == 4 && wire.parts[3] == 0u && wire.packed() != 0u);
	inmatch::JoinerConnection joiner("Status");
	const ProtocolMessage status = joiner.mission_start_status();
	CHECK(status.tag == 0x0B);
	CHECK(status.payload == inmatch::build_mission_status("", wire.packed()));
}

// [orig: NetPacket_WriteStringAndThreeInts @0x42a4d0 - the string and its NUL
//  @0x42a4ef..0x42a501, three u32 LE @0x42a520 / @0x42a531 / @0x42a547; the two zeros
//  from Mission_GetMapChecksums @0x6108a0 (ex sub_6108A0), @0x6108a8 / @0x6108ae]
void test_mission_status_body() {
	const std::vector<uint8_t> stock = inmatch::build_mission_status("", 0x01070507u);
	const std::vector<uint8_t> retail_stock = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x07, 0x05, 0x07, 0x01};
	CHECK(stock == retail_stock);
	const std::vector<uint8_t> named = inmatch::build_mission_status("ab", 0x01070507u);
	CHECK(named.size() == 3 + 12);
	CHECK(named[0] == 'a' && named[1] == 'b' && named[2] == 0x00);
	CHECK(named[11] == 0x07 && named[12] == 0x05 && named[13] == 0x07 && named[14] == 0x01);
}

// A weapon table holding `names` at their indices (a hole stays invalid).
world::WeaponTable table_of(const std::vector<std::string> &names) {
	world::WeaponTable table;
	table.entries.resize(names.size());
	for (size_t i = 0; i < names.size(); ++i) {
		if (names[i].empty()) continue;
		table.entries[i].name = names[i];
		table.entries[i].valid = true;
	}
	return table;
}

// The fresh profile's kit: the side block the team names (class 8 on both sides of a
// fresh record), its default page, each name resolved to its ADM index by name (one the
// table lacks dropped), the values the page's "-1" low bytes, slot 195 for both
// submissions with no local slot table.
// [orig: PlayerProfile_InitDefaults @0x54bb40; Game_StartMission @0x525793..0x525836;
//  NetPacket_SendLoadoutSubmit @0x42cdc0 - @0x42cf0b, @0x42cf21; Player_InitPlayer
//  @0x4e17ff]
void test_fresh_profile_kit_resolves_by_name() {
	const playersav::KitPage red =
			playersav::default_kit_page(playersav::SideId::Red, 8);
	const playersav::KitPage blue =
			playersav::default_kit_page(playersav::SideId::Blue, 8);
	CHECK(red.entries.size() >= 2 && !blue.entries.empty());
	if (red.entries.size() < 2 || blue.entries.empty()) return;
	// The table holds the red page's names except the first, in reverse order behind
	// an unrelated entry and a hole, and the blue page's first name.
	std::vector<std::string> names = {"null", "", "WPN_SYNTHETIC"};
	for (size_t i = red.entries.size(); i-- > 1;) names.push_back(red.entries[i].name);
	names.push_back(blue.entries[0].name);
	const world::WeaponTable table = table_of(names);

	const LoadoutSubmit first = inmatch::fresh_profile_loadout_submit(&table, 2, 0);
	const LoadoutSubmit second = inmatch::fresh_profile_loadout_submit(&table, 2, 0);
	CHECK(first.team == 2 && first.player_class == 8 && first.weapon_slot_index == 195u);
	CHECK(second.weapon_slot_index == 195u);
	CHECK(first.entries.size() == red.entries.size() - 1);
	size_t row = 0;
	for (size_t i = 1; i < red.entries.size() && row < first.entries.size(); ++i, ++row) {
		CHECK(first.entries[row].adm_index == static_cast<uint8_t>(table.index_of(red.entries[i].name.c_str())));
		CHECK(first.entries[row].ammo_primary == 0xFF && first.entries[row].ammo_secondary == 0xFF &&
				first.entries[row].variant == 0xFF);
	}
	// Team 1 reads the blue block.
	const LoadoutSubmit blue_submit = inmatch::fresh_profile_loadout_submit(&table, 1, 0);
	CHECK(blue_submit.team == 1 && blue_submit.player_class == 8);
	CHECK(!blue_submit.entries.empty() &&
			blue_submit.entries[0].adm_index ==
					static_cast<uint8_t>(table.index_of(blue.entries[0].name.c_str())));

	// No table: every row drops.
	const LoadoutSubmit none = inmatch::fresh_profile_loadout_submit(nullptr, 2, 0);
	CHECK(none.entries.empty() && none.player_class == 8 && none.weapon_slot_index == 195u);

	// A protocol that fixes the class (DFX2-12) picks that class's page, as its mission start
	// writes the class into the side block ahead of the page pick: 7 here, a mission of type 2.
	// [orig: Game_StartMission @0x52299e..0x5229bd (dfx2.exe)]
	const playersav::KitPage gunner = playersav::default_kit_page(playersav::SideId::Red, 7);
	CHECK(!gunner.entries.empty());
	std::vector<std::string> gunner_names = {"null"};
	for (const playersav::KitEntry &entry : gunner.entries) gunner_names.push_back(entry.name);
	const world::WeaponTable gunner_table = table_of(gunner_names);
	const LoadoutSubmit fixed = inmatch::fresh_profile_loadout_submit(&gunner_table, 2, 7);
	CHECK(fixed.player_class == 7 && fixed.entries.size() == gunner.entries.size());
}

} // namespace

int main() {
	test_mission_status_version_is_one_build();
	test_mission_status_body();
	test_fresh_profile_kit_resolves_by_name();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("joiner_mission_start_test OK\n");
	return 0;
}
