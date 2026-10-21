// Pins the character registry (runtime/inmatch/character_registry.h) — the
// packed-id decode with its alignment gate, the per-side default walk — and
// the joiner's profile-to-wire projection (join_character_profile.h): the
// fresh-profile defaults, a persisted side selection, the class-byte rule.

#include <runtime/inmatch/character_registry.h>
#include <runtime/inmatch/join_character_profile.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_spawn.h>

#include <formats/avatars/avatars.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

using namespace opennova::avatars;

namespace {

int g_failures = 0;

void check(bool ok, const char *what) {
	if (!ok) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++g_failures;
	}
}

// Two nationalities: N00 good (combos 001/002, head voices 1/3), N07 evil
// (combo 001, head voice 10, female).
const char kDef[] =
		"define head HEAD_A\r\n{\r\n\tvoice 1\r\n\tsex m\r\n}\r\n"
		"define head HEAD_B\r\n{\r\n\tvoice 3\r\n\tsex m\r\n}\r\n"
		"define head HEAD_E\r\n{\r\n\tvoice 10\r\n\tsex f\r\n}\r\n"
		"define body BODY_A\r\n{\r\n\tgraphic body.3di\r\n}\r\n"
		"nationality N00 AV_US\r\n{\r\n\talignment good\r\n\tdivision D00 AV_SEAL\r\n\t{\r\n"
		"\t\tcombo 001 HEAD_A BODY_A\r\n"
		"\t\tcombo 002 HEAD_B BODY_A\r\n"
		"\t}\r\n}\r\n"
		"nationality N07 AV_REBEL\r\n{\r\n\talignment evil\r\n\tdivision D00 AV_REB\r\n\t{\r\n"
		"\t\tcombo 001 HEAD_E BODY_A\r\n"
		"\t}\r\n}\r\n";

} // namespace

int main() {
	using namespace opennova::inmatch;

	AvatarsFile file;
	check(avatars_parse_memory(kDef, sizeof(kDef) - 1, &file) == 0, "fixture parses");
	const CharacterRegistry registry = CharacterRegistry::from_file(file);
	avatars_free(&file);

	check(registry.entries().size() == 3, "three combos in file order");
	check(registry.entries()[0].packed_id == 0x0200, "N00/D00/001 packs 0x0200");
	check(registry.entries()[1].packed_id == 0x0400, "N00/D00/002 packs 0x0400");
	check(registry.entries()[2].packed_id == 0x8207, "N07/D00/001 evil packs 0x8207");
	check(registry.entries()[2].head_female, "the evil combo's head is female");

	// The packed-id decode and its alignment gate.
	const CharacterEntry *good = registry.find_by_packed_id(0x0400);
	check(good != nullptr && good->combo_index == 1 && good->head_voice == 3,
			"0x0400 resolves to combo index 1");
	check(registry.find_by_packed_id(0x0400, AVATAR_ALIGN_EVIL) == nullptr,
			"a good id contradicts an evil expectation");
	check(registry.find_by_packed_id(0x8400) == nullptr,
			"an evil bit never matches a good entry");
	check(registry.find_by_packed_id(0x0600) == nullptr, "unknown combo resolves nothing");
	check(registry.find_by_indices(1, 0, 0) != nullptr, "indices resolve");
	check(registry.find_by_indices(1, 0, 1) == nullptr, "out-of-range indices resolve nothing");

	// The per-side default walk.
	check(registry.first_character_id(AVATAR_ALIGN_GOOD) == 0x0200, "first good = entry 0");
	check(registry.first_character_id(AVATAR_ALIGN_EVIL) == 0x8207, "first evil = entry 2");
	check(CharacterRegistry().first_character_id(AVATAR_ALIGN_GOOD) == 0, "empty registry packs 0");

	// The fresh profile: first good / first evil, rifleman both sides, the
	// selected combos' head voices.
	{
		JoinSideSelection none[2];
		const JoinCharacterProfile p = join_character_profile(registry, none, true);
		check(p.character_ids[0] == 0x0200 && p.character_ids[1] == 0x8207, "fresh ids");
		check(p.player_classes[0] == 8 && p.player_classes[1] == 8, "fresh classes 8/8");
		check(p.avatars[0] == 1 && p.avatars[1] == 10, "fresh avatars = head voices");
		check(character_join_vars(p).team_request == 0xFF, "the fresh profile's TR is -1");
	}
	// The computed fresh profile the runtime carries when nothing is installed
	// (D-NET-NEW-L2-7): over this stock-shaped table the witnessed oracle, over
	// no table ids 0 and the resolve's miss 1.
	{
		const CharacterJoinVars v = fresh_profile_character_vars(registry);
		check(v.char_id[0] == 0x0200 && v.char_id[1] == 0x8207 && v.char_class[0] == 8 &&
				v.char_class[1] == 8 && v.avatar[0] == 1 && v.avatar[1] == 10 &&
				v.team_request == 0xFF,
				"the computed fresh profile is the stock oracle");
		const CharacterJoinVars none = fresh_profile_character_vars(CharacterRegistry());
		check(none.char_id[0] == 0 && none.char_id[1] == 0 && none.avatar[0] == 1 &&
				none.avatar[1] == 1 && none.char_class[0] == 8 && none.team_request == 0xFF,
				"no Avatars.def: ids 0, avatars 1");
	}
	// A persisted side-A selection with class 6 on both blocks.
	{
		JoinSideSelection saved[2];
		saved[0].present = true;
		saved[0].nationality_index = 0;
		saved[0].division_index = 0;
		saved[0].combo_index = 1;
		saved[0].player_class = 6;
		saved[1].present = true;
		saved[1].player_class = 6;
		const JoinCharacterProfile p = join_character_profile(registry, saved, true);
		check(p.character_ids[0] == 0x0400, "side A takes the saved combo");
		check(p.character_ids[1] == 0x8207, "side B keeps its default");
		check(p.avatars[0] == 3, "side A avatar = the saved combo's head voice");
		check(p.player_classes[0] == 6 && p.player_classes[1] == 6, "class 6 on both blocks");
	}
	// A saved selection of the wrong alignment or out of range falls back.
	{
		JoinSideSelection saved[2];
		saved[0].present = true;
		saved[0].nationality_index = 1; // the evil nationality on side A
		saved[0].division_index = 0;
		saved[0].combo_index = 0;
		saved[0].player_class = 6;
		saved[1].present = true;
		saved[1].nationality_index = 9;
		saved[1].player_class = 9;
		const JoinCharacterProfile p = join_character_profile(registry, saved, true);
		check(p.character_ids[0] == 0x0200, "wrong-alignment selection falls back");
		check(p.character_ids[1] == 0x8207, "out-of-range selection falls back");
		check(p.player_classes[0] == 6 && p.player_classes[1] == 9, "classes 6 / 9 are kept");
	}
	// A class byte outside 5..9: the session start clamps it in a session, below
	// 5 to 5 and above 9 to 9 by an unsigned byte compare, so a 4 goes up as 5
	// and a 0xFF as 9 (the wire's CTA / CTB too); out of a session the record
	// gives no class and the byte keeps the default 8
	// [orig: Game_ApplySessionSettingsToGlobals @0x5516ab (the session gate),
	//  @0x5516d0..0x5516ec (the clamp); UI_JoinSelectedSession @0x569c1c /
	//  @0x569c39].
	{
		JoinSideSelection saved[2];
		saved[0].present = true;
		saved[0].player_class = 4;
		saved[1].present = true;
		saved[1].player_class = 0xFF;
		JoinCharacterProfile p = join_character_profile(registry, saved, true);
		check(p.player_classes[0] == 5 && p.player_classes[1] == 9,
				"in a session 4 clamps to 5 and 0xFF to 9");
		const opennova::inmatch::CharacterJoinVars vars = character_join_vars(p);
		check(vars.char_class[0] == 5 && vars.char_class[1] == 9, "the clamped bytes ride as CTA / CTB");
		saved[0].player_class = 0;
		saved[1].player_class = 10;
		p = join_character_profile_without_table(saved, true);
		check(p.player_classes[0] == 5 && p.player_classes[1] == 9,
				"the empty table clamps the same: 0 to 5, 10 to 9");
		p = join_character_profile(registry, saved, false);
		check(p.player_classes[0] == 8 && p.player_classes[1] == 8,
				"out of a session a byte outside 5..9 keeps the default 8");
		saved[0].player_class = 7;
		p = join_character_profile(registry, saved, false);
		check(p.player_classes[0] == 7, "out of a session a 5..9 byte stands");
	}

	// The record's voice override pair: a nonzero byte is the side's avatar
	// byte over its character's head voice, on a side with no saved character
	// too; a zero byte leaves the head voice
	// [orig: Game_ApplySessionSettingsToGlobals @0x551735..0x5517a2].
	{
		JoinSideSelection saved[2];
		saved[0].present = true;
		saved[0].nationality_index = 0;
		saved[0].division_index = 0;
		saved[0].combo_index = 1;
		saved[0].voice_override = 7;
		JoinCharacterProfile p = join_character_profile(registry, saved, true);
		check(p.character_ids[0] == 0x0400, "the override leaves the saved character");
		check(p.avatars[0] == 7, "side A's nonzero override is its avatar byte");
		check(p.avatars[1] == 10, "side B's zero override keeps the head voice");
		saved[0].voice_override = 0;
		saved[1].voice_override = 11;
		p = join_character_profile(registry, saved, true);
		check(p.avatars[0] == 3, "side A's zero override keeps the saved combo's head voice");
		check(p.avatars[1] == 11, "an unsaved side's nonzero override is its avatar byte");
		check(character_join_vars(p).avatar[1] == 11, "the override rides as VCB");
	}

	// No Avatars.def: the table is empty and the session start still takes the
	// record's override where nonzero, else the resolve's miss, 1, on BOTH
	// sides (side B is not the stock table's head voice 10); no character
	// resolves, so the id is the empty walk's 0 and a saved class stands
	// [orig: CAvatarDefs_Init @0x57b180 - the clears @0x57b1ae / @0x57b1c1;
	//  Game_ApplySessionSettingsToGlobals @0x551735..0x5517a2;
	//  Avatars_ResolveSelectionIndex @0x57ae60 - the miss @0x57ae85].
	{
		JoinSideSelection none[2];
		JoinCharacterProfile p = join_character_profile_without_table(none, true);
		check(p.avatars[0] == 1 && p.avatars[1] == 1, "no table: a zero override is 1 on both sides");
		check(p.character_ids[0] == 0 && p.character_ids[1] == 0, "no table: no character id");
		check(p.player_classes[0] == 8 && p.player_classes[1] == 8, "no table: the default classes");
		JoinSideSelection saved[2];
		saved[0].voice_override = 7;
		saved[1].present = true;
		saved[1].nationality_index = 0;
		saved[1].division_index = 0;
		saved[1].combo_index = 0;
		saved[1].player_class = 6;
		p = join_character_profile_without_table(saved, true);
		check(p.avatars[0] == 7, "no table: a nonzero override is the avatar byte");
		check(p.avatars[1] == 1, "no table: a saved side's zero override is 1");
		check(p.character_ids[1] == 0 && p.player_classes[1] == 6,
				"no table: a saved side resolves no id and keeps its class");
		const opennova::inmatch::CharacterJoinVars vars = character_join_vars(p);
		check(vars.avatar[0] == 7 && vars.avatar[1] == 1, "the bytes ride as VCA / VCB");
	}

	// The wire projection: ids and bytes narrowed, and the TR byte the join's
	// side requirement, 0xFF (-1) whatever the profile holds (D-NET-590)
	// [orig: UI_JoinSelectedSession @0x569bff / @0x569c23].
	{
		JoinCharacterProfile p;
		p.character_ids[0] = 0x0400;
		p.character_ids[1] = 0x8207;
		p.player_classes[0] = 6;
		p.player_classes[1] = 300;
		p.avatars[0] = 3;
		p.avatars[1] = -1;
		const opennova::inmatch::CharacterJoinVars vars = character_join_vars(p);
		check(vars.char_id[0] == 0x0400 && vars.char_id[1] == 0x8207, "packed ids ride as-is");
		check(vars.char_class[0] == 6 && vars.char_class[1] == 0xFF, "class bytes clamp to a byte");
		check(vars.avatar[0] == 3 && vars.avatar[1] == 0, "avatar bytes clamp to a byte");
		check(vars.team_request == 0xFF, "the TR byte is the side requirement's 0xFF");
		check(static_cast<int8_t>(vars.team_request) == kJoinSideRequirement,
				"the side requirement is -1");
	}

	// A one-sided Avatars.def: the side with no combo of its alignment takes
	// entry 0's id (the other side's, its side bit kept) and its head voice
	// [orig: CAvatarDefs_PackFirstComboOfAlignment @0x57ad84..0x57adda].
	{
		opennova::inmatch::CharacterRegistry good_only;
		good_only.add_entry(0, 0, 0, 3, 1, 1, AVATAR_ALIGN_GOOD, 4, false);
		good_only.add_entry(0, 0, 1, 3, 1, 2, AVATAR_ALIGN_GOOD, 5, false);
		JoinSideSelection none[2];
		const JoinCharacterProfile p = join_character_profile(good_only, none, true);
		check(p.character_ids[0] == good_only.entries()[0].packed_id && p.avatars[0] == 4,
				"one-sided: side A takes its first combo");
		check(p.character_ids[1] == good_only.entries()[0].packed_id && p.avatars[1] == 4,
				"one-sided: side B falls back to entry 0");
		JoinSideSelection saved[2];
		saved[1].present = true;
		saved[1].nationality_index = 0;
		saved[1].division_index = 0;
		saved[1].combo_index = 1;
		const JoinCharacterProfile q = join_character_profile(good_only, saved, true);
		check(q.character_ids[1] == good_only.entries()[0].packed_id,
				"one-sided: a side-B pick of a good combo does not resolve, the fallback stands");
	}

	// The record's raw words (D-PLAYERINFO-33): the session apply copies each side block's
	// word raw and the join's CI0 / CI1 carry it as it is, resolved or not; the class is the
	// block's own byte (clamped in a session) whatever the table resolves; the avatar byte is
	// the combo the word names (its own side bit, no side filter), else the resolve's miss, 1.
	// [orig: Game_ApplySessionSettingsToGlobals @0x551500 - @0x551F7A..0x551FB1, the class
	//  clamp @0x5516D0..0x5516EC, the resolves @0x551752 / @0x55178B; UI_JoinSelectedSession
	//  @0x5699D0 - @0x569C06..0x569C31; Avatars_ResolveSelectionIndex @0x57AE60 - the miss
	//  @0x57AE85]
	{
		JoinSideSelection saved[2];
		saved[0].present = true;
		saved[0].character_id = 0x0600; // a combo this table has no entry for
		saved[0].player_class = 7;
		saved[1].present = true;
		saved[1].character_id = 0x0400; // a good word in the evil side's block
		saved[1].player_class = 4;
		JoinCharacterProfile p = join_character_profile(registry, saved, true);
		check(p.character_ids[0] == 0x0600 && p.character_ids[1] == 0x0400,
				"the record's raw words ride as CI0 / CI1");
		check(p.player_classes[0] == 7 && p.player_classes[1] == 5,
				"an unresolved side keeps its class byte, clamped in a session");
		check(p.avatars[0] == 1, "a word the table does not name: the miss's 1");
		check(p.avatars[1] == 3, "the word's own combo voice, whichever block holds it");
		p = join_character_profile_without_table(saved, true);
		check(p.character_ids[0] == 0x0600 && p.character_ids[1] == 0x0400 &&
				p.avatars[0] == 1 && p.avatars[1] == 1 && p.player_classes[0] == 7,
				"no table: the raw words and the class bytes still ride");
		saved[0].voice_override = 9;
		p = join_character_profile(registry, saved, true);
		check(p.avatars[0] == 9, "a nonzero override stands over the word's voice");
	}

	// The host's own row: the profile init keeps a word the host's table names (no side test),
	// else packs the side's default; over an empty table both are 0.
	// [orig: PlayerSession_InitFromProfile @0x50CA80 - CAvatarDefs_HasPackedId @0x50CAC4, the
	//  side-0 pack @0x50CADA; @0x50CAF1, the side-1 pack @0x50CB08; CAvatarDefs_HasPackedId
	//  @0x57B140 (the call @0x57B14F) -> CAvatarDefs_FindComboByPackedId]
	{
		auto ctx = std::make_unique<NapiNPServerCtx>();
		ctx->characters = registry;
		CharacterJoinVars vars;
		vars.char_id[0] = 0x0600;
		vars.char_id[1] = 0x0400;
		PlayerSession_ResolveOwnCharacterIds(*ctx, vars);
		check(vars.char_id[0] == 0x0200, "the own row: an unnamed word packs the side default");
		check(vars.char_id[1] == 0x0400, "the own row: a named word is kept whatever its block");
		ctx->characters = CharacterRegistry();
		vars.char_id[0] = 0x0400;
		vars.char_id[1] = 0x8207;
		PlayerSession_ResolveOwnCharacterIds(*ctx, vars);
		check(vars.char_id[0] == 0 && vars.char_id[1] == 0, "the own row over no table: 0 / 0");
	}

	if (g_failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", g_failures);
		return EXIT_FAILURE;
	}
	std::puts("character_registry_test: ok");
	return EXIT_SUCCESS;
}
