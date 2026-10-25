#pragma once

#include <runtime/inmatch/character_registry.h>
#include <runtime/inmatch/napi_np_connection.h> // CharacterJoinVars

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace opennova::serve {

// The process's account name, as retail's profile load reads it into its first record: the
// Windows account name through GetUserNameA; off Windows, which retail never runs on, the login
// name (getlogin(), else the login database: getpwuid(getuid())), an input with no retail counterpart.
// [orig: PlayerProfile_LoadAllFromDisk @0x54F4D0 - GetUserNameA(&profile0+4, 16) @0x54F535]
std::string process_account_name();

// The profile opennova-serve's own row runs under: the boot's profile load. Every record is
// seeded fresh, the first named after the account (a name the 16-byte buffer cannot hold leaves
// it empty), then player.sav's records are read over them and weapon.sav's, each when the
// process's working directory holds it; game.cfg's player_index picks the current record (an
// index past the records reads the first). The mission start copies its name onto the row's
// body as the local profile's, which every joiner's 0x0C carries; the session create's profile
// init gives the row the current weapon record's sides (its class by the side requirement, its
// character ids and avatars), the projection the game's own host makes of the same record
// (inmatch::join_character_profile in a session, each side resolved by its packed id; a side
// the character table does not resolve takes the side's default). With no weapon.sav the
// row keeps the fresh record's sides (`character_vars` empty). (D-NET-352)
// [orig: PlayerProfile_LoadAllFromDisk @0x54F4D0 - InitDefaults @0x54F51B, GetUserNameA
//  @0x54F535, the flag bit @0x54F53B, player.sav @0x54F577..0x54F5A0 (File_CheckExists
//  @0x54F586), weapon.sav "%s" / "expansion\%s\%s" @0x54F695..0x54F6B7 (File_CheckExists
//  @0x54F6C7), the current record @0x54F65D..0x54F682; Game_ApplySessionSettingsToGlobals
//  @0x551680..0x551695; Player_InitPlayer @0x4E184F..0x4E186A; CNapiGameSession_CreateSession
//  @0x4C97C0 - PlayerSession_InitFromProfile @0x4C9CCE]
struct ServeProfile {
	std::string name;
	std::optional<inmatch::CharacterJoinVars> character_vars;
};
ServeProfile load_serve_profile(int32_t player_index, const std::string &account_name,
		const std::vector<uint8_t> *player_sav, const std::vector<uint8_t> *weapon_sav,
		const inmatch::CharacterRegistry &characters);

// load_serve_profile over the files the process's working directory holds: player.sav, and
// weapon.sav at its path for the mounted expansion (playersav::weapon_sav_relpath).
ServeProfile read_serve_profile(int32_t player_index, const std::string &account_name,
		const std::string &expansion, const inmatch::CharacterRegistry &characters);

} // namespace opennova::serve
