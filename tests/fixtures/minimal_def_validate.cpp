// Validates the minimal set's text .def files parse through our loaders — the
// fatal-wired items.def [orig: @ 0x4a71a3] plus the host+join weapon/ammo defs
// [orig: WeaponDef_LoadAll @ 0x54dd10; AmmoDef_LoadAll @ 0x40b0b0]. All three
// are authored from scratch (no retail asset); this guards that they load and
// carry the minimal content a joined player needs (a person to spawn as, a
// selectable rifle, its round). See assets/README.md.
#include <formats/def/def.h>

#include <cstdio>
#include <cstring>
#include <string>

using namespace opennova::def;

namespace {

bool expect(bool cond, const char *msg) {
	if (!cond) std::fprintf(stderr, "FAIL: %s\n", msg);
	return cond;
}

int fail = 0;

const DefWeaponDef *find_weapon(const DefWeaponsFile &wf, const char *name) {
	for (int i = 0; i < wf.count; ++i) {
		if (std::strcmp(wf.entries[i].weapon_name, name) == 0) return &wf.entries[i];
	}
	return nullptr;
}
#define CHECK(c, m)                                                                                \
	do {                                                                                           \
		if (!expect((c), (m))) ++fail;                                                              \
	} while (0)

std::string path(const char *name) { return std::string(GAME_ASSETS_DIR) + "/" + name; }

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
		bool has_mp_player = false;
		const DefItemDef *house = nullptr;
		for (size_t i = 0; i < items.count; ++i) {
			// type 8 == person in the witnessed mapping (D-ITEMDEF-1); check by
			// the presence of a spawnable person via its id range instead of the
			// raw enum to stay robust to the FFI field name.
			if (items.entries[i].id == 105310 || items.entries[i].id == 105311) has_person = true;
			// retail declares BOTH player rows and hosts spawn the MP one: the
			// runtime's player template is wire type 0x14B9 = items.def id 105305
			// (engine/net/npwire/entity_class.h kPlayerPersonTypeId), so a set
			// without this row leaves every hosted/joined player with no graphic
			// and no anim_def.
			if (items.entries[i].id == 105305) has_mp_player = true;
			// The set's own model: the synth house (assets/house.3di), placed in
			// mnml.bms as a building. 108001 is the first id of our own range.
			if (items.entries[i].id == 108001) house = &items.entries[i];
		}
		CHECK(items.count >= 1, "items.def has at least one item");
		CHECK(has_person, "items.def carries a spawnable person (player/soldier)");
		CHECK(has_mp_player, "items.def carries the MP player row (105305 = wire 0x14B9)");
		CHECK(house != nullptr, "items.def carries the house row (108001)");
		if (house != nullptr) {
			CHECK(house->type == DEF_ITEM_TYPE_BUILDING, "the house is a building (the BMS building pool)");
			CHECK(std::strcmp(house->graphic, "house") == 0, "the house names assets/house.3di");
		}
		def_free_items(&items);
	}

	// weapon.def — the ONE rifle: WPN_AK47AUTO, the AKM first-person model the
	// bring-up set ships. The engine's hardcoded WPN_M4AUTO spawn default
	// [orig: PlayerClass_InitEntity @ 0x4B1116] is deliberately unanswered: the
	// mission's kit arms the player (minimal_map_validate pins it).
	{
		DefWeaponsFile weapons{};
		CHECK(def_parse_weapons(path("weapon.def").c_str(), &weapons) == 0, "weapon.def parses");
		CHECK(weapons.count == 1, "weapon.def carries exactly one weapon");
		const DefWeaponDef *rifle = find_weapon(weapons, "WPN_AK47AUTO");
		CHECK(rifle != nullptr, "weapon.def carries WPN_AK47AUTO (the one rifle)");
		if (rifle != nullptr) {
			CHECK(std::strcmp(rifle->gfx1, "AKM_1st") == 0, "the rifle names its first-person model");
			CHECK(std::strcmp(rifle->animadm, "AKM_1ST") == 0, "the rifle names its first-person .adm");
			// GFX1A is parse-and-discard in the original (the arms come from the
			// character), carried for retail-shape fidelity.
			CHECK(std::strcmp(rifle->gfx1a, "ARMSG") == 0, "the rifle carries the retail arms row");
			// Raw def units; the /256 scale is the consumer's
			// [runtime/simassets/fp_viewmodel_spec.h kWeaponDefPosScale].
			CHECK(rifle->pos[0] == 10.0f && rifle->pos[2] == -201.0f, "the hip viewmodel offset");
			CHECK(rifle->tpos[0] == -28.046f && rifle->tpos[2] == -187.857f, "the ADS viewmodel offset");
			CHECK(rifle->renderfov == 80.0f, "the rifle takes the default render fov");
		}
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
