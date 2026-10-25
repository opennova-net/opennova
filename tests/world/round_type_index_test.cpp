// D-NET-NEW-L7-1: a weapon's round_type resolves through the original's ammo lookup,
// which answers 0 for a miss and for the first row alike; the parse stores only a
// nonzero answer, so a round naming ammo.def's first block (AT_NULL) is an unknown
// round and the weapon keeps the def's -1 [orig: WeaponDefs_ParseLineCallback - the
// round_type arm @0x543C57..0x543CA9 (the 0 test @0x543C75, the store @0x543CA9);
// AmmoDef_LookupByName @0x409870; AdmDef_InitEntryDefaults @0x53FEF0 (@0x53FF58)].

#include <runtime/world/ammo_table.h>
#include <runtime/world/ammo_table_build.h>
#include <runtime/world/weapon_table.h>

#include <cstdio>

namespace {

namespace w = opennova::world;

int failures = 0;
#define CHECK(c)                                                                               \
	do {                                                                                       \
		if (!(c)) {                                                                            \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                           \
			++failures;                                                                        \
		}                                                                                      \
	} while (0)

void test_the_first_ammo_row_is_an_unknown_round() {
	w::AmmoTable ammo;
	ammo.entries.resize(2);
	ammo.entries[0].valid = true;
	ammo.entries[0].name = "AT_NULL";
	ammo.entries[1].valid = true;
	ammo.entries[1].name = "ROCKET";
	w::WeaponTable weapons;
	weapons.entries.resize(4);
	for (w::WeaponTableEntry &e : weapons.entries) e.valid = true;
	weapons.entries[0].round_type = "at_null"; // the first row, any case
	weapons.entries[1].round_type = "ROCKET";
	weapons.entries[2].round_type = "NO_SUCH_ROUND";
	weapons.entries[3].round_type.clear();
	w::resolve_weapon_round_types(weapons, ammo);
	CHECK(weapons.entries[0].ammo_index == -1);
	CHECK(weapons.entries[1].ammo_index == 1);
	CHECK(weapons.entries[2].ammo_index == -1);
	CHECK(weapons.entries[3].ammo_index == -1);
}

} // namespace

int main() {
	test_the_first_ammo_row_is_an_unknown_round();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("PASS round_type_index\n");
	return 0;
}
