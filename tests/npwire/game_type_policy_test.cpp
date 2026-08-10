// Pins for the wire-session policy that moved engine-side in the task-12
// sweep: the mission-attrib -> g_GameType map, the ClientAuth character_id
// bit-pack, the witnessed session rule defaults, and the BMS game-mode
// single-select. The values were previously pinned by a GUT test against
// GDScript twins; the engine home is the truth now.
// [orig: AI_GetTaskTypeFromFlags @ 0x40DAE0; lookup_entity_slot_and_pack_entry
//  @ 0x57AD40; Config_SetDefaults; dfx2med sub_402770]

#include <mission/bms.h>
#include <npruntime/game_config.h>
#include <npwire/game_type.h>
#include <npwire/session_hello.h>

#include <cstdio>

namespace {

int failures = 0;

#define CHECK(cond, msg)                                                       \
	do {                                                                       \
		if (!(cond)) {                                                         \
			std::fprintf(stderr, "FAIL: %s\n", msg);                           \
			++failures;                                                        \
		}                                                                      \
	} while (0)

} // namespace

int main() {
	using namespace opennova;
	using bms::AttribFlags;

	// The attrib-mode -> g_GameType map [orig: AI_GetTaskTypeFromFlags
	// @ 0x40DAE0]: a Co-op mission derives the OBJECTIVE Co-op word; no (or
	// unknown) multiplayer attrib resolves to the stock/training Co-op word.
	CHECK(game_type::for_mission_mode(
				  static_cast<uint32_t>(AttribFlags::Coop)) ==
					game_type::kObjectiveCoop,
			"a Co-op mission derives the objective Co-op code word");
	CHECK(game_type::for_mission_mode(
				  static_cast<uint32_t>(AttribFlags::Deathmatch)) ==
					game_type::kDeathmatch,
			"DM attrib selects the deathmatch code word");
	CHECK(game_type::for_mission_mode(
				  static_cast<uint32_t>(AttribFlags::TeamDeathmatch)) ==
					game_type::kTeamDeathmatch,
			"TDM attrib selects the team-deathmatch code word");
	CHECK(game_type::for_mission_mode(0) == game_type::kCoop,
			"no multiplayer attrib falls to the stock/training Co-op word");

	// The character_id pack [alignment:1 | combo:6 | division:4 | nat:5]
	// [orig: lookup_entity_slot_and_pack_entry @ 0x57AD40]: round-trips, and
	// the witnessed fresh-profile defaults reproduce.
	const uint16_t packed = character_id::pack(3, 2, 5, 1);
	CHECK(character_id::nationality(packed) == 3, "nationality round-trips");
	CHECK(character_id::division(packed) == 2, "division round-trips");
	CHECK(character_id::combo(packed) == 5, "combo round-trips");
	CHECK(character_id::alignment(packed) == 1, "alignment round-trips");
	CHECK(character_id::pack(0, 0, 1, 0) == 0x0200,
			"the stock good-side default packs to 0x0200");
	CHECK(character_id::pack(7, 0, 1, 1) == 0x8207,
			"the stock evil-side default packs to 0x8207");

	// The witnessed session rule defaults [orig: Config_SetDefaults].
	CHECK(game_rules::kDefaultRespawnTime == 30, "respawn default");
	CHECK(game_rules::kDefaultTimeLimitMinutes == 10, "time-limit default");
	CHECK(game_rules::kDefaultMaxTeamLives == 100, "team-lives default");
	CHECK(game_rules::kDefaultScoreLimit == 50, "score default");
	CHECK(game_rules::kMaxCallsignLength == 15, "callsign cap = Name[16]");
	CHECK(np::kMaxPlayersCap == 65, "player cap [orig: the 1..65 clamp]");

	// The BMS game-mode decode priority + single-select encode
	// [orig: dfx2med sub_402770 decode @ 0x4050c7 / encode @ 0x4031cd].
	CHECK(bms::selected_game_mode(AttribFlags::Coop | AttribFlags::Deathmatch) ==
					static_cast<uint32_t>(AttribFlags::Coop),
			"co-op wins the decode priority over deathmatch");
	AttribFlags flags = AttribFlags::Deathmatch | AttribFlags::ForceIndoors;
	CHECK(bms::set_game_mode(flags,
				  static_cast<uint32_t>(AttribFlags::Coop)),
			"set_game_mode accepts one mode bit");
	CHECK(!bms::has_flag(flags, AttribFlags::Deathmatch) &&
					bms::has_flag(flags, AttribFlags::Coop) &&
					bms::has_flag(flags, AttribFlags::ForceIndoors),
			"set_game_mode single-selects inside the mode mask only");
	CHECK(!bms::set_game_mode(flags,
				  static_cast<uint32_t>(AttribFlags::Coop) |
						  static_cast<uint32_t>(AttribFlags::Deathmatch)),
			"set_game_mode rejects multi-bit input");

	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("game_type_policy_test: all checks passed\n");
	return 0;
}
