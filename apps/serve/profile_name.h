#pragma once

#include <cstdint>
#include <string>

namespace opennova::serve {

// The process's account name, as retail's profile load reads it into its first record: the
// Windows account name through GetUserNameA; off Windows, which retail never runs on, the login
// name (getlogin(), else $USER), an input with no retail counterpart.
// [orig: PlayerProfile_LoadAllFromDisk @0x54F4D0 - GetUserNameA(&profile0+4, 16) @0x54F535]
std::string process_account_name();

// The name opennova-serve runs its own row under: it keeps no player.sav, so the profile the
// host runs under is the boot's own seed, every record a fresh one and the first named after
// the account (a name the 16-byte buffer cannot hold leaves it empty), picked by game.cfg's
// player_index as the boot's current record (an index past the records reads the first). The
// mission start copies it onto the row's body as the local profile's name, which every joiner's
// 0x0C carries (D-NET-352, profile::PlayerProfiles::load).
// [orig: PlayerProfile_LoadAllFromDisk @0x54F4D0 - InitDefaults @0x54F51B, GetUserNameA
//  @0x54F535, the flag bit @0x54F53B, no player.sav @0x54F586, the current record
//  @0x54F65D..0x54F682; Game_ApplySessionSettingsToGlobals @0x551680..0x551695;
//  Player_InitPlayer @0x4E184F..0x4E186A]
std::string serve_profile_name(int32_t player_index, const std::string &account_name);

} // namespace opennova::serve
