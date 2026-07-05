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
