#include "profile_name.h"

#include <base/io/file_io.h>
#include <formats/playersav/weapon_sav.h>
#include <runtime/inmatch/join_character_profile.h>
#include <runtime/profile/player_profiles.h>

#include <cstdlib>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pwd.h>
#include <unistd.h>
#endif

namespace opennova::serve {

std::string process_account_name() {
#if defined(_WIN32)
	// The whole name, whose length the load's 16-byte rule then judges.
	char name[257] = {};
	DWORD size = sizeof(name);
	if (GetUserNameA(name, &size) == 0) return std::string();
	return std::string(name);
#else
	// The login name, else the login database's entry for the process's user; no
	// environment read.
	if (const char *login = getlogin()) return login;
	const struct passwd *entry = getpwuid(getuid());
	return entry != nullptr && entry->pw_name != nullptr ? std::string(entry->pw_name)
	                                                     : std::string();
#endif
}

ServeProfile load_serve_profile(int32_t player_index, const std::string &account_name,
		const std::vector<uint8_t> *player_sav, const std::vector<uint8_t> *weapon_sav,
		const inmatch::CharacterRegistry &characters) {
	profile::PlayerProfiles profiles;
	profiles.select_slot(player_index);
	profile::PlayerProfiles::LoadInput input;
	input.player_sav = player_sav;
	input.weapon_sav = weapon_sav;
	input.user_name = account_name;
	profiles.load(input, profile::ProfileDefaults{});
	ServeProfile out;
	// The 32-byte name copy keeps 31 characters [orig: Napi_CopyString(.., 0x20) @0x551695].
	out.name = profiles.current().name.substr(0, 31);
	if (weapon_sav == nullptr) return out;
	// The current weapon record's sides as the host's own projection reads them: each side
	// by its packed id against the character table, its class byte and the record's voice
	// override; in a session (D-PLAYERINFO-33 / 34 for the projection's own rules).
	const playersav::Record &weapons = profiles.current_weapons();
	inmatch::JoinSideSelection saved[2];
	for (int side = 0; side < 2; ++side) {
		const playersav::Side &sd = weapons.side(static_cast<playersav::SideId>(side));
		saved[side].voice_override = profile::voice_override(profiles.current(), side);
		const inmatch::CharacterEntry *entry = characters.find_by_packed_id(sd.avatar_packed, side);
		if (entry == nullptr) continue;
		saved[side].present = true;
		saved[side].nationality_index = entry->nationality_index;
		saved[side].division_index = entry->division_index;
		saved[side].combo_index = entry->combo_index;
		saved[side].player_class = sd.player_class;
	}
	out.character_vars = inmatch::character_join_vars(
			inmatch::join_character_profile(characters, saved, /*in_session=*/true));
	return out;
}

ServeProfile read_serve_profile(int32_t player_index, const std::string &account_name,
		const std::string &expansion, const inmatch::CharacterRegistry &characters) {
	std::vector<uint8_t> player_sav;
	std::vector<uint8_t> weapon_sav;
	std::string error;
	const bool have_player = io::read_file_bytes("player.sav", player_sav, error);
	const bool have_weapon =
			io::read_file_bytes(playersav::weapon_sav_relpath(expansion), weapon_sav, error);
	return load_serve_profile(player_index, account_name, have_player ? &player_sav : nullptr,
			have_weapon ? &weapon_sav : nullptr, characters);
}

} // namespace opennova::serve
