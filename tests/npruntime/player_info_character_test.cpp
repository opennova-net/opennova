// The PLAYER_INFO character memory (runtime/inmatch/player_info_character.h):
// a selection resolves only to a combo of its side's alignment and packs the
// authored numbers; a saved side that still resolves is kept, a stale or
// absent one takes the side's first combo, an empty registry leaves none; the
// class outside 5..9 reads 8; the initial side; and the dialog save's shared
// class stamped across both sides while the character stays per side.
// [orig: CAvatarDefs_PackFirstComboOfAlignment @0x57ad40; PlayerInfo_SaveFromDialog
//  @0x55ee10, @0x55ee3f..0x55ee5a]

#include <runtime/inmatch/player_info_character.h>

#include <net/npwire/character_id.h>

#include <cstdio>

namespace {

using namespace opennova;
using inmatch::PlayerInfoSideSelection;

int g_failures = 0;

void check(bool ok, const char *what) {
	if (!ok) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++g_failures;
	}
}

// Nationality 0 (id 3) good: division 0 (id 1) with combos 0 / 1 (ids 1 / 2);
// nationality 1 (id 7) evil: division 0 (id 2) with combo 0 (id 5).
inmatch::CharacterRegistry registry() {
	inmatch::CharacterRegistry r;
	r.add_entry(0, 0, 0, 3, 1, 1, 0, 1, false);
	r.add_entry(0, 0, 1, 3, 1, 2, 0, 3, false);
	r.add_entry(1, 0, 0, 7, 2, 5, 1, 10, true);
	return r;
}

std::optional<PlayerInfoSideSelection> saved(int team, int nat, int div, int combo, int cls) {
	PlayerInfoSideSelection s;
	s.team = team;
	s.nationality = nat;
	s.division = div;
	s.combo = combo;
	s.player_class = cls;
	return s;
}

void test_selection() {
	const inmatch::CharacterRegistry r = registry();
	const auto good = inmatch::player_info_side_selection(r, 0, 0, 0, 1, 6);
	check(good && good->avatar_a == 3 && good->avatar_b == 1 &&
					good->avatar_packed == character_id::pack(3, 1, 2, 0) && good->player_class == 6,
			"a good combo packs its authored numbers");
	check(!inmatch::player_info_side_selection(r, 1, 0, 0, 1, 6), "a good combo is no evil selection");
	check(!inmatch::player_info_side_selection(r, 0, 0, 0, 2, 6), "a combo past the division is none");
	const auto evil = inmatch::player_info_side_selection(r, 1, 1, 0, 0, 8);
	check(evil && evil->avatar_packed == character_id::pack(7, 2, 5, 1), "the evil combo packs its side bit");
}

void test_normalize_and_reads() {
	inmatch::PlayerInfoCharacterState state;
	std::optional<PlayerInfoSideSelection> sides[2] = { saved(0, 0, 0, 1, 4), saved(1, 0, 0, 0, 7) };
	state.set_persisted(1, 9, sides);
	check(state.player_class(0) == 4 && state.player_class(1) == 7, "before validation the saved bytes stand");
	state.set_registry(registry());
	const auto blue = state.side_selection(0);
	check(blue && blue->combo == 1 && blue->player_class == 8, "a resolving side is kept, its class 4 read as 8");
	const auto red = state.side_selection(1);
	check(red && red->nationality == 1 && red->combo == 0 && red->player_class == 7,
			"a stale side takes its side's first combo, keeping its class");
	check(state.initial_team(0) == 1, "the profile's shown side wins over the authored radio");
	check(!state.side_selection(2), "no third side");
	// An absent side takes the default with the profile's class (8 when out of range).
	inmatch::PlayerInfoCharacterState fresh;
	std::optional<PlayerInfoSideSelection> none[2];
	fresh.set_persisted(std::nullopt, 6, none);
	check(fresh.player_class(0) == 6, "no side: the profile's class");
	fresh.set_registry(registry());
	check(fresh.side_selection(0) && fresh.side_selection(0)->combo == 0 &&
					fresh.side_selection(0)->player_class == 6,
			"an absent side takes the first good combo with the profile's class");
	check(fresh.initial_team(5) == 1 && fresh.initial_team(-3) == 0, "the authored radio, clamped");
	inmatch::PlayerInfoCharacterState bare;
	bare.set_persisted(std::nullopt, std::nullopt, none);
	check(bare.player_class(0) == -1, "no side, no profile class: -1");
	bare.set_registry(registry());
	check(bare.side_selection(1) && bare.side_selection(1)->player_class == 8, "the default class is 8");
	// No Avatars.def: no side survives.
	inmatch::PlayerInfoCharacterState empty;
	empty.set_persisted(0, 6, sides);
	empty.set_registry(inmatch::CharacterRegistry());
	check(!empty.side_selection(0) && !empty.side_selection(1), "an empty registry leaves no side");
}

void test_snapshot_stamps_the_class() {
	inmatch::PlayerInfoCharacterState state;
	std::optional<PlayerInfoSideSelection> none[2];
	state.set_persisted(0, 8, none);
	state.set_registry(registry());
	const auto snap = state.snapshot(0, 0, 0, 1, 5);
	check(snap.current && snap.current->combo == 1 && snap.current->player_class == 5,
			"the shown side's selection with the selected class");
	check(snap.sides[0] && snap.sides[0]->combo == 1 && snap.sides[0]->player_class == 5,
			"the shown side is remembered");
	check(snap.sides[1] && snap.sides[1]->nationality == 1 && snap.sides[1]->player_class == 5,
			"the other side keeps its character and takes the class");
	check(state.player_class(1) == 8, "the stamp is the snapshot's; the other side's memory keeps its byte");
	const auto stale = state.snapshot(1, 0, 0, 0, 9);
	check(!stale.current, "a selection of the wrong side resolves to none");
	check(stale.sides[0] && stale.sides[0]->player_class == 9 && stale.sides[1]->player_class == 9,
			"both remembered sides still take the class");
}

// A one-sided Avatars.def: the side with no combo of its alignment takes entry
// 0, the other side's first combo, as the packer leaves it in the side block
// [orig: CAvatarDefs_PackFirstComboOfAlignment @0x57ad84..0x57adda].
void test_one_sided_default() {
	inmatch::CharacterRegistry good_only;
	good_only.add_entry(0, 0, 0, 3, 1, 1, 0, 1, false);
	good_only.add_entry(0, 0, 1, 3, 1, 2, 0, 3, false);
	inmatch::PlayerInfoCharacterState state;
	std::optional<PlayerInfoSideSelection> sides[2] = { std::nullopt, saved(1, 0, 0, 1, 6) };
	state.set_persisted(std::nullopt, std::nullopt, sides);
	state.set_registry(good_only);
	const auto blue = state.side_selection(0);
	check(blue && blue->combo == 0, "one-sided: the populated side takes its first combo");
	const auto red = state.side_selection(1);
	check(red && red->team == 1 && red->nationality == 0 && red->division == 0 && red->combo == 0,
			"one-sided: the empty side falls back to entry 0");
	check(red && red->avatar_a == 3 && red->avatar_b == 1 &&
					red->avatar_packed == character_id::pack(3, 1, 1, 0) && red->player_class == 6,
			"one-sided: entry 0's numbers and packed id (its side bit kept), the saved class");
	const auto snap = state.snapshot(1, 0, 0, 1, 7);
	check(!snap.current, "one-sided: a side-B pick of a good combo resolves to none");
	check(state.player_class(1) == 6, "one-sided: the fallback side's class reads");
	check(!snap.sides[1], "one-sided: the fallback side has no record of its own to save");
	check(snap.sides[0] && snap.sides[0]->combo == 0 && snap.sides[0]->player_class == 7,
			"one-sided: the populated side takes the class");
}

// An authored id wider than its packed field (combo 64 packs as 0) still
// resolves its own side, whose record the snapshot keeps.
void test_wide_id_keeps_its_side() {
	inmatch::CharacterRegistry r;
	r.add_entry(0, 0, 0, 3, 1, 64, 0, 1, false);
	r.add_entry(1, 0, 0, 7, 2, 5, 1, 10, true);
	inmatch::PlayerInfoCharacterState state;
	std::optional<PlayerInfoSideSelection> sides[2] = { saved(0, 0, 0, 0, 6), std::nullopt };
	state.set_persisted(0, 6, sides);
	state.set_registry(r);
	const auto snap = state.snapshot(0, 0, 0, 0, 7);
	check(snap.current && snap.current->avatar_packed == character_id::pack(3, 1, 64, 0),
			"wide id: the shown side resolves");
	check(snap.sides[0] && snap.sides[0]->combo == 0 && snap.sides[0]->player_class == 7,
			"wide id: the side's record is saved");
	check(snap.sides[1] && snap.sides[1]->nationality == 1, "wide id: the other side keeps its first combo");
}

} // namespace

int main() {
	test_selection();
	test_normalize_and_reads();
	test_snapshot_stamps_the_class();
	test_one_sided_default();
	test_wide_id_keeps_its_side();
	if (g_failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("player_info_character: ok\n");
	return 0;
}
