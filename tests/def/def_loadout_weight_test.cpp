// The PLAYER_INFO loadout weight + encumbrance math, ported from the binary and
// pinned to the witnessed formula/thresholds [orig: calculate_loadout_weight
// @ 0x55f1f0; update_player_info_weight_and_weapon_icons @ 0x55f480].
#include <def/def.h>

#include <cmath>
#include <cstdio>

namespace {
int fail = 0;
#define CHECK(c, m)                                                                                 \
	do {                                                                                            \
		if (!(c)) {                                                                                 \
			std::fprintf(stderr, "FAIL: %s\n", m);                                                  \
			++fail;                                                                                 \
		}                                                                                           \
	} while (0)

bool near(double a, double b) { return std::fabs(a - b) < 1e-4; }
} // namespace

int main() {
	// One weapon: weaponweight + ammo*clipweight. With an explicit ammo count.
	{
		DefWeaponDef w{};
		w.weaponweight = 8.0f;
		w.clipweight = 0.5f;
		w.maxclips = 7;
		const int ammo[1] = {3};
		// 8 + 3*0.5 = 9.5
		CHECK(near(def_loadout_weight(&w, ammo, 1), 9.5), "weapon + explicit ammo weight");
		// ammo <= 0 -> maxclips: 8 + 7*0.5 = 11.5 (the engine's <=0 -> default clip branch)
		const int none[1] = {0};
		CHECK(near(def_loadout_weight(&w, none, 1), 11.5), "ammo<=0 uses maxclips");
		// A NULL ammo array also selects maxclips.
		CHECK(near(def_loadout_weight(&w, nullptr, 1), 11.5), "null ammo uses maxclips");
	}

	// Multiple weapons sum.
	{
		DefWeaponDef ws[2] = {};
		ws[0].weaponweight = 10.0f;
		ws[0].clipweight = 1.0f;
		ws[0].maxclips = 2;
		ws[1].weaponweight = 4.0f;
		ws[1].clipweight = 0.25f;
		ws[1].maxclips = 4;
		const int ammo[2] = {1, 0}; // slot0 explicit 1, slot1 default 4
		// (10 + 1*1) + (4 + 4*0.25) = 11 + 5 = 16
		CHECK(near(def_loadout_weight(ws, ammo, 2), 16.0), "two weapons sum");
	}

	// Empty loadout weighs 0.
	CHECK(near(def_loadout_weight(nullptr, nullptr, 0), 0.0), "empty loadout is 0");

	// The category-3 extra-ammo term [orig: @ 0x5655c9..0x56561c; the
	// @ 0x55f1f0 family]: count * clipweight only — no weaponweight; -1 takes
	// the maxclips default, a chosen zero row weighs nothing.
	{
		DefWeaponDef g = {};
		g.weaponweight = 99.0f; // must NOT contribute
		g.clipweight = 0.5f;
		g.maxclips = 4;
		CHECK(near(def_extra_ammo_weight(&g, 3), 1.5), "count * clipweight only");
		CHECK(near(def_extra_ammo_weight(&g, -1), 2.0), "-1 defaults to maxclips");
		CHECK(near(def_extra_ammo_weight(&g, 0), 0.0), "a chosen zero row weighs nothing");
		CHECK(near(def_extra_ammo_weight(nullptr, 3), 0.0), "null def weighs nothing");
	}

	// The sub-weapon walk [orig: the round-type walk @ 0x55def0 / @ 0x55e8b0 /
	// @ 0x55f1f0]: same-round entries inside the loadout_subclasses window are
	// ammo expansions; the first differing round_type is the sub-weapon.
	{
		DefWeaponDef t[4] = {};
		snprintf(t[0].round_type, sizeof t[0].round_type, "satchel");
		t[0].loadout_subclasses = 2;
		snprintf(t[1].round_type, sizeof t[1].round_type, "SATCHEL"); // case-folds: expansion
		snprintf(t[2].round_type, sizeof t[2].round_type, "detonator");
		snprintf(t[3].round_type, sizeof t[3].round_type, "flare");
		CHECK(def_subclass_weapon_index(t, 4, 0) == 2, "first differing round_type wins");
		CHECK(def_subclass_weapon_index(t, 4, 3) == -1, "zero subclasses walks nothing");
		t[0].loadout_subclasses = 1;
		CHECK(def_subclass_weapon_index(t, 4, 0) == -1, "window of expansions only finds none");
		t[2].loadout_subclasses = 5;
		CHECK(def_subclass_weapon_index(t, 4, 2) == 3, "walk stops inside the table");
		CHECK(def_subclass_weapon_index(t, 3, 2) == -1, "table end before a differing entry");
		CHECK(def_subclass_weapon_index(nullptr, 0, 0) == -1, "empty table");
		CHECK(def_subclass_weapon_index(t, 4, 9) == -1, "parent out of range");
	}

	// Encumbrance thresholds — exact boundaries [orig: @ 0x55f480].
	CHECK(def_encumbrance_class(0.0) == DEF_ENCUMBRANCE_LIGHT, "0 is LIGHT");
	CHECK(def_encumbrance_class(33.29) == DEF_ENCUMBRANCE_LIGHT, "just under 33.3 is LIGHT");
	CHECK(def_encumbrance_class(33.3) == DEF_ENCUMBRANCE_NORMAL, "33.3 is NORMAL");
	CHECK(def_encumbrance_class(66.59) == DEF_ENCUMBRANCE_NORMAL, "just under 66.6 is NORMAL");
	CHECK(def_encumbrance_class(66.6) == DEF_ENCUMBRANCE_HEAVY, "66.6 is HEAVY");
	CHECK(def_encumbrance_class(100.0) == DEF_ENCUMBRANCE_HEAVY, "100 is HEAVY");

	if (fail == 0) std::printf("OK: def loadout weight + encumbrance match the witnessed formula\n");
	return fail == 0 ? 0 : 1;
}
