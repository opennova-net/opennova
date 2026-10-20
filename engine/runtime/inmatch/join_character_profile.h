#pragma once

// The profile-to-wire projection a joiner's ClientAuth uploads: retail does
// not invent a network-only player id — it packs the two profile character
// selections from Avatars.def (CI0/CI1), stamps the two class bytes
// (CTA/CTB) and the two avatar bytes (VCA/VCB, each the profile's voice
// override for the side where nonzero, else the selected combo's head voice).
// A side that is absent or no longer resolves takes the retail default: the
// first combo of its alignment, else entry 0 (an Avatars.def with no combo of
// that side's), class 8 (rifleman).
// [orig: PlayerProfile_InitDefaults @0x54BB40 (the fresh profile),
//  EntitySlot_LookupAndPackEntry @0x57AD40 (the per-side default),
//  Avatars_ResolveSelectionIndex @0x57AE60,
//  CNapiServerInfo_SerializeToSession @0x4C3650 <- the profile's two 0x8006
//  side blocks; a saved id the registry no longer resolves is reallocated to
//  the same default by PlayerSession_InitFromProfile @0x50ca80]

#include <runtime/inmatch/character_registry.h>
#include <runtime/inmatch/napi_np_connection.h>

#include <algorithm>
#include <cstdint>

namespace opennova::inmatch {

// One side's persisted selection: the Avatars.def tree indices and the class
// byte. `present` false = the side was never saved (the retail default).
inline constexpr int kJoinDefaultPlayerClass = 8; // rifleman
inline constexpr int kJoinPlayerClassMin = 5;
inline constexpr int kJoinPlayerClassMax = 9;
inline constexpr int kJoinDefaultAvatar = 1;

struct JoinSideSelection {
	bool present = false;
	int nationality_index = -1;
	int division_index = -1;
	int combo_index = -1;
	int player_class = kJoinDefaultPlayerClass;
	// The current profile record's voice override byte for the side, +1532
	// blue / +1533 red (profile::voice_override); 0 = none. It stands whether
	// or not the side's character was saved.
	int voice_override = 0;
};

struct JoinCharacterProfile {
	uint16_t character_ids[2] = { 0, 0 }; // CI0 / CI1
	int player_classes[2] = { kJoinDefaultPlayerClass, kJoinDefaultPlayerClass };
	int avatars[2] = { kJoinDefaultAvatar, kJoinDefaultAvatar };
};

// The join's side requirement, the TR byte: the join copies the low byte of
// the config block's +0x424 (GameCfg::side_req) into the session info +0xF4,
// and only -1 is ever stored there (the defaults, every pre-game pass; the
// game.cfg `side_req` key is never read back, and PRE.MNU has no team
// control: the side comes from the side password, JSP). Every join sends
// the automatic TR=-1 (D-NET-590).
// [orig: UI_JoinSelectedSession @0x569bff / @0x569c23;
//  CNapiGameSession_BuildAndCreateSession @0x56979f / @0x5697c3;
//  Config_SetDefaults @0x54d1fa; UI_PreGameMenuStateMachine @0x568c55]
inline constexpr int8_t kJoinSideRequirement = -1;

// The selected side character when the saved tree indices still resolve to a
// combo of the side's alignment: its packed id and head voice.
inline bool join_side_character(const CharacterRegistry &registry, int side,
		int nationality_index, int division_index, int combo_index,
		uint16_t &out_packed_id, int &out_avatar) {
	const CharacterEntry *entry = registry.find_by_indices(nationality_index,
			division_index, combo_index);
	if (entry == nullptr || entry->alignment != side) return false;
	out_packed_id = entry->packed_id;
	out_avatar = entry->head_voice;
	return true;
}

// `in_session`: the session start takes the side blocks' class bytes only in
// a session's first round, each clamped in place, below 5 to 5 and above 9 to
// 9 (an unsigned byte compare), so a saved 4 goes up as 5 and a 200 as 9; a
// join's apply and a listen host's run in a session (the pre-game screens set
// the network type, CNapiNetwork_SetNetworkType's is_in_session = type != 0,
// and the LAN pick zeroes the round count), single player's does not. Out of a
// session the record gives no class (the mission's word does, D-PLAYERINFO-33),
// and a byte outside 5..9 keeps the default 8 here. The round gate is NOT
// ported as a gate: this gates on `in_session` alone, which matches only
// because the projection is consumed at a session's first round (the host's
// loopback char vars at start_host_session, host_session.cpp; the joiner's
// ClientAuth, JoinerConnection::build_client_auth). A later round in retail
// keeps the globals from round 1 or from the server's class sync, and a join
// whose round count was not zeroed skips the clamp (D-PLAYERINFO-34).
// [orig: Game_ApplySessionSettingsToGlobals @0x5516ab (the session gate),
//  @0x5516b3 (the first-round gate, g_SessionRoundCount == 0),
//  @0x5516d0..0x5516ec (the clamp, the stores @0x5516d7 / @0x5516e1),
//  @0x5516f8 / @0x5516fd (the class globals); NapiNPClientMsg_HandleWeaponLoadoutSync
//  @0x429427 / @0x42941f (the server's class sync); CNapiNetwork_SetNetworkType
//  @0x4c4a50; UI_ProcessLANSessionStateMachine @0x558e8c;
//  UI_HandleLANGameListEvent @0x556c26; UI_JoinSelectedSession @0x569b07 (the
//  join's apply), @0x569c1c / @0x569c39 (CTA / CTB)]
inline JoinCharacterProfile join_character_profile(
		const CharacterRegistry &registry, const JoinSideSelection saved[2], bool in_session) {
	JoinCharacterProfile profile;
	for (int side = 0; side < 2; ++side) {
		// The retail default: the first combo of the side's alignment, else
		// entry 0 whatever its side (an Avatars.def with no combo of this
		// side's alignment packs the other side's first entry), so the id
		// resolves with no side filter [orig: EntitySlot_LookupAndPackEntry
		// @0x57ad40 - the side loop @0x57ad6b..0x57ad80, the entry-0 fallback
		// @0x57ad84..0x57adda].
		const CharacterEntry *first = registry.find_by_packed_id(
				registry.first_character_id(side));
		if (first != nullptr) {
			profile.character_ids[side] = first->packed_id;
			profile.avatars[side] = first->head_voice;
		}
		if (saved[side].present) {
			uint16_t packed = 0;
			int avatar = 0;
			if (join_side_character(registry, side, saved[side].nationality_index,
						saved[side].division_index, saved[side].combo_index, packed,
						avatar)) {
				profile.character_ids[side] = packed;
				profile.avatars[side] = avatar;
			}
			const uint8_t cls = static_cast<uint8_t>(saved[side].player_class);
			if (in_session)
				profile.player_classes[side] = cls < kJoinPlayerClassMin ? kJoinPlayerClassMin
						: cls > kJoinPlayerClassMax ? kJoinPlayerClassMax
						: cls;
			else if (cls >= kJoinPlayerClassMin && cls <= kJoinPlayerClassMax)
				profile.player_classes[side] = cls;
		}
		// The session start's avatar byte: the record's voice override where
		// nonzero, else the side's character's own voice. A join uploads the
		// pair as VCA / VCB and the local player takes its team's as its
		// entity +0x374 (Entity::anim_slot).
		// [orig: Game_ApplySessionSettingsToGlobals @0x551500 - +1532
		//  @0x551735..0x551769 -> g_AvatarTeam1, +1533 @0x55176e..0x5517a2 ->
		//  g_AvatarTeam2, a zero byte Avatars_ResolveSelectionIndex @0x57ae60;
		//  UI_JoinSelectedSession @0x569c2a / @0x569c46;
		//  CNapiServerInfo_SerializeToSession @0x4c3bb4 / @0x4c3c04;
		//  Player_InitPlayer @0x4e1838 / @0x4e1825, the store @0x4e1843]
		if (saved[side].voice_override != 0) profile.avatars[side] = saved[side].voice_override;
	}
	return profile;
}

// The projection with no Avatars.def: an absent file leaves the avatar table
// empty, and the session start still takes the record's voice override where
// nonzero, else the resolve's miss, 1, on both sides; the projection over the
// empty registry gives exactly that (no side resolves a character, so each
// takes the default id, 0, and the class its selection gives it).
// [orig: CAvatarDefs_Init @0x57b180 - the clears @0x57b1ae / @0x57b1c1 before
//  the parse call @0x57b1d5; Game_ApplySessionSettingsToGlobals @0x551735..0x5517a2;
//  Avatars_ResolveSelectionIndex @0x57ae60 - the miss @0x57ae85]
inline JoinCharacterProfile join_character_profile_without_table(const JoinSideSelection saved[2],
		bool in_session) {
	return join_character_profile(CharacterRegistry(), saved, in_session);
}

// The profile as the ClientAuth tag block carries it (CharacterJoinVars): the
// two packed ids, the two class bytes and the two avatar bytes narrowed to
// their wire widths, and the side requirement's byte (0xFF, TR=-1).
inline inmatch::CharacterJoinVars character_join_vars(const JoinCharacterProfile &profile) {
	inmatch::CharacterJoinVars vars{};
	for (int side = 0; side < 2; ++side) {
		vars.char_id[side] = profile.character_ids[side];
		vars.char_class[side] = static_cast<uint8_t>(
				std::clamp(profile.player_classes[side], 0, 0xFF));
		vars.avatar[side] = static_cast<uint8_t>(std::clamp(profile.avatars[side], 0, 0xFF));
	}
	// [orig: UI_JoinSelectedSession @0x569bff / @0x569c23]
	vars.team_request = static_cast<uint8_t>(kJoinSideRequirement);
	return vars;
}

// A fresh profile's tag block over an Avatars.def registry: no side saved, so each side
// takes its alignment's first combo and that combo's head voice, class 8 and the automatic
// side request; over the empty registry (no Avatars.def) the ids are 0 and both avatar
// bytes 1. What an embedder that installs no selection carries: the host boot computes
// it over the mounted file for the host's own slot, and a joiner starts from the
// no-table form until its shell installs the mounted selection. The stock Avatars.def
// gives {0x0200, 0x8207}, class 8 / 8, avatars 1 / 10 (the oracle in the tests).
// [orig: PlayerProfile_InitDefaults @0x54BB40 - class 8 @0x54bbe0 / @0x54bbe3, the
//  packs EntitySlot_LookupAndPackEntry @0x57AD40 (calls @0x54bbea / @0x54bc0f), the
//  voice overrides zeroed @0x54bc1e / @0x54bc24; Avatars_ResolveSelectionIndex
//  @0x57AE60] (D-NET-NEW-L2-7)
inline CharacterJoinVars fresh_profile_character_vars(const CharacterRegistry &registry) {
	const JoinSideSelection none[2] = {};
	return character_join_vars(join_character_profile(registry, none, /*in_session=*/true));
}

} // namespace opennova::inmatch
