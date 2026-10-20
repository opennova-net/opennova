#pragma once

// The stock-Avatars.def FRESH-PROFILE character vars, a witnessed oracle for tests
// with no mounted Avatars.def: what a retail client with no saved selection uploads
// and what a listen host's own slot carries before a profile is installed. Each side's
// first AvatarDefs entry of the matching alignment, packed
// {nat=0,div=0,combo=1,good} = 0x0200 and {nat=7,div=0,combo=1,evil} = 0x8207, class 8
// (rifleman) on both sides, the two heads' voice bytes 1 / 10, no side request (TR 0xFF).
// The runtime computes these (inmatch::fresh_profile_character_vars over the mounted
// file); only a test with no table installs this literal.
// [orig: PlayerProfile_InitDefaults @0x54BB40 (class 8 @0x54bbe0 / @0x54bbe3, the packs
//  @0x54bbea / @0x54bc0f) -> CAvatarDefs_PackFirstComboOfAlignment @0x57AD40; the avatar byte via
//  Avatars_ResolveSelectionIndex @0x57AE60; wire: retail join f=199140 (its VCB=4 is a
//  saved profile override, not the fresh default)]

#include <runtime/inmatch/napi_np_connection.h>

namespace opennova::inmatch {

inline CharacterJoinVars stock_fresh_profile_character_vars() {
	CharacterJoinVars vars;
	vars.char_id[0] = 0x0200;
	vars.char_id[1] = 0x8207;
	vars.team_request = 0xFF;
	vars.char_class[0] = 8;
	vars.char_class[1] = 8;
	vars.avatar[0] = 1;
	vars.avatar[1] = 10;
	return vars;
}

} // namespace opennova::inmatch
