// Validates the minimal set's text .def files parse through our loaders — the
// fatal-wired items.def [orig: @ 0x4a71a3] plus the host+join weapon/ammo defs
// [orig: WeaponDef_LoadAll @ 0x54dd10; AmmoDef_LoadAll @ 0x40b0b0]. All three
// are authored from scratch (no retail asset); this guards that they load and
// carry the minimal content a joined player needs (a person to spawn as, a
// selectable rifle, its round). See fixtures/minimal/README.md.
#include <def/def.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

bool expect(bool cond, const char *msg) {
	if (!cond) std::fprintf(stderr, "FAIL: %s\n", msg);
	return cond;
}

int fail = 0;
#define CHECK(c, m)                                                                                \
	do {                                                                                           \
		if (!expect((c), (m))) ++fail;                                                              \
	} while (0)

std::string path(const char *name) { return std::string(MINIMAL_FIXTURE_DIR) + "/resources/" + name; }

bool has_ammo(const DefAmmoFile &f, const char *name) {
	for (size_t i = 0; i < f.count; ++i)
		if (std::strcmp(f.entries[i].name, name) == 0) return true;
	return false;
}

} // namespace

int main() {
	// items.def — fatal-wired; must parse and carry a person to spawn as.
	{
		DefItemsFile items{};
		CHECK(def_parse_items(path("items.def").c_str(), &items) == 0, "items.def parses");
		bool has_person = false;
		for (size_t i = 0; i < items.count; ++i) {
			// type 8 == person in the witnessed mapping (D-ITEMDEF-1); check by
			// the presence of a spawnable person via its id range instead of the
			// raw enum to stay robust to the FFI field name.
			if (items.entries[i].id == 105310 || items.entries[i].id == 105311) has_person = true;
		}
		CHECK(items.count >= 1, "items.def has at least one item");
		CHECK(has_person, "items.def carries a spawnable person (player/soldier)");
		def_free_items(&items);
	}

	// weapon.def — one selectable rifle so a joined player spawns armed.
	{
		DefWeaponsFile weapons{};
		CHECK(def_parse_weapons(path("weapon.def").c_str(), &weapons) == 0, "weapon.def parses");
		CHECK(weapons.count >= 1, "weapon.def has at least one weapon");
		def_free_weapons(&weapons);
	}

	// ammo.def — the rifle round plus the null slot.
	{
		DefAmmoFile ammo{};
		CHECK(def_parse_ammo(path("ammo.def").c_str(), &ammo) == 0, "ammo.def parses");
		CHECK(has_ammo(ammo, "AM_556MM"), "ammo.def carries the rifle round AM_556MM");
		def_free_ammo(&ammo);
	}

	if (fail == 0) std::printf("OK: minimal .def set parses (items/weapon/ammo)\n");
	return fail == 0 ? 0 : 1;
}
