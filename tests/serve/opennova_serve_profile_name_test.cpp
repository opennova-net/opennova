// D-NET-352 (Q7-6, L4-s9 FOUND 2): the profile opennova-serve's own row runs under is the boot's
// profile load over the process's working directory: every record fresh, the first named after
// the process's account through GetUserNameA's 16-byte buffer (a name it cannot hold with its NUL
// leaves the field empty), then player.sav's records and weapon.sav's read over them where the
// directory holds them; the current one game.cfg's player_index (an index past the records reads
// the first). The mission start copies the current record's name onto the row's body, and the
// session create's profile init gives the row the current weapon record's sides.
// [orig: PlayerProfile_LoadAllFromDisk @0x54F4D0 - InitDefaults @0x54F51B, GetUserNameA
//  @0x54F535, player.sav File_CheckExists @0x54F586, weapon.sav File_CheckExists @0x54F6C7, the
//  current record @0x54F65D..0x54F682; CNapiGameSession_CreateSession @0x4C97C0 -
//  PlayerSession_InitFromProfile @0x4C9CCE]
// The player.sav / weapon.sav bytes are minted by our own writer (profile::PlayerProfiles).

#include "profile_name.h"

#include <games/jo/jo.h>
#include <runtime/profile/player_profiles.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace opennova;

// The server runs as Joint Operations (its profile files' magic, Title::profile_magic).
const Title &kJo = games::jo_title();

int failures = 0;
#define CHECK(c)                                                                               \
	do {                                                                                       \
		if (!(c)) {                                                                            \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                           \
			++failures;                                                                        \
		}                                                                                      \
	} while (0)

std::string seed_name(int32_t index, const std::string &account) {
	return serve::load_serve_profile(kJo, index, account, nullptr, nullptr, inmatch::CharacterRegistry{})
			.name;
}

void test_the_first_record_takes_the_account() {
	CHECK(seed_name(0, "cdouglass") == "cdouglass");
	// Fifteen characters fit the buffer with the NUL; sixteen fail and leave it empty.
	CHECK(seed_name(0, std::string(15, 'u')) == std::string(15, 'u'));
	CHECK(seed_name(0, std::string(16, 'u')).empty());
	CHECK(seed_name(0, std::string()).empty());
	// Any other record is a nameless fresh one; an index past the records reads the first.
	CHECK(seed_name(3, "cdouglass").empty());
	CHECK(seed_name(6, "cdouglass") == "cdouglass");
	CHECK(seed_name(-1, "cdouglass") == "cdouglass");
	// No weapon.sav: the row keeps the fresh record's sides.
	CHECK(!serve::load_serve_profile(kJo, 0, "cdouglass", nullptr, nullptr, inmatch::CharacterRegistry{})
					.character_vars.has_value());
}

// The working directory's files read over the seed: player.sav's record names, weapon.sav's
// sides (the red side's class byte 6 and its character, resolved by its packed id; the blue
// side's, unresolved, takes the side's default and class 8).
void test_the_saved_files_read_over_the_seed() {
	inmatch::CharacterRegistry characters;
	characters.add_entry(0, 0, 0, 1, 1, 1, /*alignment=*/0, /*head_voice=*/3, false);
	characters.add_entry(1, 0, 0, 2, 1, 1, /*alignment=*/1, /*head_voice=*/7, false);
	const uint16_t red_id = characters.first_character_id(1);
	profile::PlayerProfiles minted(kJo);
	profile::PlayerProfiles::LoadInput none;
	minted.load(none, profile::ProfileDefaults{});
	minted.select_slot(2);
	minted.current().name = "Sarge";
	playersav::Side &red = minted.current_weapons().side(playersav::SideId::Red);
	red.player_class = 6;
	red.avatar_packed = red_id;
	minted.current_weapons().side(playersav::SideId::Blue).avatar_packed = 0x7FFF; // no combo
	const std::vector<uint8_t> player_sav = minted.player_sav_bytes();
	const std::vector<uint8_t> weapon_sav = minted.weapon_sav_bytes();

	const serve::ServeProfile row =
			serve::load_serve_profile(kJo, 2, "cdouglass", &player_sav, &weapon_sav, characters);
	CHECK(row.name == "Sarge");
	CHECK(row.character_vars.has_value());
	if (row.character_vars) {
		CHECK(row.character_vars->char_class[1] == 6);
		CHECK(row.character_vars->char_id[1] == red_id);
		CHECK(row.character_vars->char_class[0] == 8);
		CHECK(row.character_vars->char_id[0] == characters.first_character_id(0));
	}
	// player.sav alone: its names, the fresh sides.
	const serve::ServeProfile names_only =
			serve::load_serve_profile(kJo, 2, "cdouglass", &player_sav, nullptr, characters);
	CHECK(names_only.name == "Sarge" && !names_only.character_vars.has_value());
}

// The account read gives a name the load can judge (the host's own login here).
void test_the_account_name_reads() {
	const std::string name = serve::process_account_name();
	CHECK(name.find('\0') == std::string::npos);
}

} // namespace

int main() {
	test_the_first_record_takes_the_account();
	test_the_saved_files_read_over_the_seed();
	test_the_account_name_reads();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("PASS opennova_serve_profile_name\n");
	return 0;
}
