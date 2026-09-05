// Pins the character registry (runtime/inmatch/character_registry.h) — the
// packed-id decode with its alignment gate, the per-side default walk — and
// the joiner's profile-to-wire projection (join_character_profile.h): the
// fresh-profile defaults, a persisted side selection, the class-byte rule.

#include <runtime/inmatch/character_registry.h>
#include <runtime/inmatch/join_character_profile.h>

#include <formats/avatars/avatars.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

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
		"define head HEAD_A\n{\n\tvoice 1\n\tsex m\n}\n"
		"define head HEAD_B\n{\n\tvoice 3\n\tsex m\n}\n"
		"define head HEAD_E\n{\n\tvoice 10\n\tsex f\n}\n"
		"define body BODY_A\n{\n\tgraphic body.3di\n}\n"
		"nationality N00 AV_US\n{\n\talignment good\n\tdivision D00 AV_SEAL\n\t{\n"
		"\t\tcombo 001 HEAD_A BODY_A\n"
		"\t\tcombo 002 HEAD_B BODY_A\n"
		"\t}\n}\n"
		"nationality N07 AV_REBEL\n{\n\talignment evil\n\tdivision D00 AV_REB\n\t{\n"
		"\t\tcombo 001 HEAD_E BODY_A\n"
		"\t}\n}\n";

} // namespace

int main() {
	using namespace opennova::npruntime;

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
	// selected combos' head voices, side unassigned.
	{
		JoinSideSelection none[2];
		const JoinCharacterProfile p = join_character_profile(registry, none);
		check(p.character_ids[0] == 0x0200 && p.character_ids[1] == 0x8207, "fresh ids");
		check(p.player_classes[0] == 8 && p.player_classes[1] == 8, "fresh classes 8/8");
		check(p.avatars[0] == 1 && p.avatars[1] == 10, "fresh avatars = head voices");
		check(p.team_request == -1, "fresh team request -1");
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
		const JoinCharacterProfile p = join_character_profile(registry, saved);
		check(p.character_ids[0] == 0x0400, "side A takes the saved combo");
		check(p.character_ids[1] == 0x8207, "side B keeps its default");
		check(p.avatars[0] == 3, "side A avatar = the saved combo's head voice");
		check(p.player_classes[0] == 6 && p.player_classes[1] == 6, "class 6 on both blocks");
	}
	// A saved selection of the wrong alignment or out of range falls back; a
	// class outside 5..9 keeps the default.
	{
		JoinSideSelection saved[2];
		saved[0].present = true;
		saved[0].nationality_index = 1; // the evil nationality on side A
		saved[0].division_index = 0;
		saved[0].combo_index = 0;
		saved[0].player_class = 4;
		saved[1].present = true;
		saved[1].nationality_index = 9;
		saved[1].player_class = 9;
		const JoinCharacterProfile p = join_character_profile(registry, saved);
		check(p.character_ids[0] == 0x0200, "wrong-alignment selection falls back");
		check(p.character_ids[1] == 0x8207, "out-of-range selection falls back");
		check(p.player_classes[0] == 8, "class 4 keeps the default");
		check(p.player_classes[1] == 9, "class 9 is kept");
	}

	// The wire projection: ids and bytes narrowed, a side request outside
	// {0, 1} is the absent 0xFF.
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
		check(vars.team_request == 0xFF, "team_request -1 is the absent 0xFF");
		p.team_request = 1;
		check(character_join_vars(p).team_request == 1, "a side request in {0, 1} rides");
		p.team_request = 2;
		check(character_join_vars(p).team_request == 0xFF, "a side request past 1 is absent");
	}

	if (g_failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", g_failures);
		return EXIT_FAILURE;
	}
	std::puts("character_registry_test: ok");
	return EXIT_SUCCESS;
}
