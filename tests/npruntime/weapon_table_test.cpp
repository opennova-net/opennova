// D-NET-141 — the armory table build + the witnessed loadout-service rules, exercised against
// the committed fixtures/def/weapon.def (94 weapons; NOTE a live install's VFS-resolved
// weapon.def differs — e.g. the JO:CA host resolves 126 weapons — so LIVE index anchors beyond
// the shared low range belong to the live wire gates, not this unit test).
//
// Witnessed rules under test (docs/net/novaworld-net-re.md §5.57, 2026-07-02 grill):
//   index    — null@0 [orig: AnimDef_InitAll @0x543615]; weapon blocks reuse-by-name else
//              lowest-free [orig: WeaponDefs_ParseLineCallback @0x5436e1] => file order.
//   ammo     — primary = pool/clipsize, clipsize -1 -> 0xFF, clamp 127 [orig:
//              WeaponSlot_GetTotalClips @0x5425F0]; secondary = first different-ammoclass
//              sub-variant in parent+1..parent+LSC [orig: @0x5027c8].
//   filters  — team red=1/blue=2, char medic..engineer bits; class/type masks
//              [orig: @0x502666/@0x502693/@0x502716].

#include <npruntime/weapon_table_build.h>
#include <npruntime/ammo_table_build.h>

#include <def/def.h>

#include <cstdio>
#include <cstring>

#include "common/test_paths.h"

using namespace opennova;

static int failures = 0;
#define CHECK(c) \
	do { \
		if (!(c)) { \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); \
			++failures; \
		} \
	} while (0)

int main(void) {
	const char *repo_root = test_paths_repo_root(__FILE__);
	char path[4096];
	std::snprintf(path, sizeof(path), "%s/fixtures/def/weapon.def", repo_root);

	DefWeaponsFile wf;
	std::memset(&wf, 0, sizeof(wf));
	if (def_parse_weapons(path, &wf) != 0) {
		std::fprintf(stderr, "FAIL: def_parse_weapons failed for %s\n", path);
		return 1;
	}

	const world::WeaponTable table = np::build_weapon_table(wf);

	// --- index allocation: null@0 + file order, 1-based.
	CHECK(table.entries.size() == wf.count + 1);
	CHECK(table.by_index(0) != nullptr && table.by_index(0)->name == "null");
	CHECK(table.index_of("WPN_KNIFE") == 1);
	CHECK(table.index_of("WPN_KNIFE2") == 2);
	CHECK(table.index_of("WPN_colt45") == 3);
	CHECK(table.index_of("WPN_M4AUTO") == 9);
	CHECK(table.index_of("wpn_m4auto") == 9); // by-name lookups are case-insensitive (stricmp)
	CHECK(table.index_of("WPN_NOPE") == -1);
	CHECK(table.by_index(static_cast<uint8_t>(table.entries.size())) == nullptr);

	// --- field mapping (fixture truth).
	const world::WeaponTableEntry *m4 = table.by_index(9);
	CHECK(m4 != nullptr);
	CHECK(m4->category == 3 && m4->rank == 0);
	CHECK(m4->maxclips == 10 && m4->clipsize == 30 && m4->startrounds == 300);
	CHECK(m4->action_fsm.auto_fire);
	CHECK(m4->action_fsm.clip_capacity == 30);
	CHECK(m4->action_fsm.actions[world::weapon_action::kFire].id ==
	      world::weapon_action::kFire);
	CHECK(m4->action_fsm.actions[world::weapon_action::kFire].delay_end == 3);
	CHECK(m4->action_fsm.actions[world::weapon_action::kRecoil].delay_end == 2);
	CHECK(m4->charfilter == (0x08u | 0x01u | 0x10u)); // rifleman|medic|engineer
	CHECK(m4->teamfilter == 0x02u);                   // blue
	CHECK(m4->loadout_subclasses == 1 && m4->loadout_selectable == 1);
	CHECK(m4->ammo_class == "CLASS_556MM");
	CHECK(m4->has_first_person_model_reference);
	const int32_t expected_m4_error[6] = {1081, 13107, 16384, 1081, 2097, 3080};
	for (int row = 0; row < 6; ++row) CHECK(m4->error_fp16[row] == expected_m4_error[row]);
	CHECK(m4->weaponweight_fp16 == 360448 && m4->clipweight_fp16 == 98304);
	const int mortar_index = table.index_of("WPN_MORTAR");
	CHECK(mortar_index > 0);
	const world::WeaponTableEntry *mortar =
			table.by_index(static_cast<uint8_t>(mortar_index));
	CHECK(mortar != nullptr && mortar->error_fp16[0] == 262144 &&
	      mortar->error_hip_theta_fp16 == 262144 && mortar->error_up_theta_fp16 == 262144);
	const int magnum_index = table.index_of("WPN_357");
	CHECK(magnum_index > 0 &&
	      table.by_index(static_cast<uint8_t>(magnum_index))->clipweight_fp16 == 22938);
	const int empl50_index = table.index_of("WPN_EMPLCD50");
	const int avenger_index = table.index_of("WPN_AVENGER");
	CHECK(empl50_index > 0 && avenger_index > 0);
	CHECK(table.by_index(static_cast<uint8_t>(empl50_index))->has_first_person_model_reference);
	CHECK(!table.by_index(static_cast<uint8_t>(avenger_index))->has_first_person_model_reference);
	const world::WeaponTableEntry *knife = table.by_index(1);
	CHECK(knife != nullptr && knife->clipsize == -1 && knife->startrounds == -1);
	CHECK(knife->charfilter == 0x1Fu && knife->teamfilter == 0x02u);

	// --- ammo resolution branches.
	{
		np::LoadoutAmmoBytes a = np::resolve_loadout_ammo(table, 2, 0xFF); // KNIFE2: no-clip
		CHECK(a.primary == 0xFF && a.secondary == 0xFF);
		a = np::resolve_loadout_ammo(table, 3, 0xFF); // colt45: 35/7
		CHECK(a.primary == 5 && a.secondary == 0xFF);
		a = np::resolve_loadout_ammo(table, 9, 0xFF); // M4AUTO: 300/30; variant M4 shares class
		CHECK(a.primary == 10 && a.secondary == 0xFF);
		a = np::resolve_loadout_ammo(table, 9, 4); // requested 4 -> min(4, maxclips 10)
		CHECK(a.primary == 4 && a.secondary == 0xFF);
		a = np::resolve_loadout_ammo(table, 9, 200); // requested over maxclips -> clamp to 10
		CHECK(a.primary == 10);
		// M4M203AUTO @11 (LSC 2): M4M203 shares CLASS_556MM, M4M203HE is CLASS_40MMNADE -> 6/1.
		CHECK(table.index_of("WPN_M4M203AUTO") == 11);
		a = np::resolve_loadout_ammo(table, 11, 0xFF);
		CHECK(a.primary == 10 && a.secondary == 6);
		const int he = table.index_of("WPN_GRENADEHE");
		CHECK(he > 0);
		a = np::resolve_loadout_ammo(table, static_cast<uint8_t>(he), 0xFF); // 3/1
		CHECK(a.primary == 3 && a.secondary == 0xFF);
		a = np::resolve_loadout_ammo(table, 250, 0xFF); // no entry
		CHECK(a.primary == 0xFF && a.secondary == 0xFF);
	}

	// --- permission masks.
	{
		const world::WeaponTableEntry &m4e = *table.by_index(9);
		CHECK(np::loadout_entry_permitted(m4e, 1, 8));  // blue rifleman
		CHECK(!np::loadout_entry_permitted(m4e, 2, 8)); // red side: M4 is blue-only
		CHECK(!np::loadout_entry_permitted(m4e, 1, 6)); // sniper bit not in rifleman|medic|engineer
		CHECK(np::loadout_entry_permitted(m4e, 1, 5));  // medic bit present
		CHECK(np::loadout_entry_permitted(m4e, 0, 2));  // class 1..3 -> all char bits; class 0 -> both teams
		const world::WeaponTableEntry &k2 = *table.by_index(2); // KNIFE2: red, all classes
		CHECK(np::loadout_entry_permitted(k2, 2, 8));
		CHECK(!np::loadout_entry_permitted(k2, 1, 8));
		// An unfiltered (emplaced) entry is never loadout-visible: masks are 0.
		const int mini = table.index_of("WPN_WEAKAIMINI");
		CHECK(mini > 0 && !np::loadout_entry_permitted(*table.by_index(static_cast<uint8_t>(mini)), 1, 8));
		const world::WeaponTableEntry &minie =
				*table.by_index(static_cast<uint8_t>(mini));
		CHECK(minie.action_fsm.auto_fire);
		CHECK(minie.action_fsm.clip_capacity == -1);
		CHECK(minie.action_fsm.actions[world::weapon_action::kFire].delay_end == 0);
		CHECK(minie.action_fsm.actions[world::weapon_action::kRecoil].delay_end == 7);
	}

	// --- token -> bit maps [orig: @0x830EB0 / @0x830ED8].
	CHECK(np::charfilter_bit("medic") == 0x01 && np::charfilter_bit("sniper") == 0x02 &&
	      np::charfilter_bit("gunner") == 0x04 && np::charfilter_bit("rifleman") == 0x08 &&
	      np::charfilter_bit("engineer") == 0x10 && np::charfilter_bit("bogus") == 0);
	CHECK(np::teamfilter_bit("red") == 0x01 && np::teamfilter_bit("BLUE") == 0x02 &&
	      np::teamfilter_bit("green") == 0);

	// --- ammo spread/recoil bake: exact fixed carrier plus the original byte stores.
	// Values outside byte range wrap exactly as the parser's atol-to-byte assignment
	// does. [orig: AmmoDef_ParseProperty @0x40A2D0]
	{
		DefAmmoDef source{};
		std::strcpy(source.name, "AMMO_BYTE_NARROW");
		source.error_fp16 = 12345;
		source.recoil[0] = -1;
		source.recoil[1] = 256;
		source.recoil[2] = 511;
		DefAmmoFile af{&source, 1};
		const world::AmmoTable ammo_table = np::build_ammo_table(af);
		const world::AmmoTableEntry *baked = ammo_table.by_index(0);
		CHECK(baked != nullptr && baked->spread_error_fp16 == 12345);
		CHECK(baked != nullptr && baked->recoil[0] == 255 && baked->recoil[1] == 0 &&
		      baked->recoil[2] == 255);
	}

	def_free_weapons(&wf);

	// --- by-name reuse: a re-parsed name keeps its index and takes the new fields
	//     [orig: WeaponDefs_ParseLineCallback @0x5436e1 AvatarDef_FindIndexByName leg].
	{
		static const char kMini[] =
				"weapon \"WPN_A\"\n\tcategory 1\nend\n"
				"weapon \"WPN_B\"\n\tcategory 2\nend\n"
				"weapon \"WPN_A\"\n\tcategory 3\nend\n";
		DefWeaponsFile mini;
		std::memset(&mini, 0, sizeof(mini));
		CHECK(def_parse_weapons_memory(reinterpret_cast<const uint8_t *>(kMini),
		                               sizeof(kMini) - 1, &mini) == 0);
		const world::WeaponTable t2 = np::build_weapon_table(mini);
		CHECK(t2.entries.size() == 3); // null + A + B (the re-parsed A reused its slot)
		CHECK(t2.index_of("WPN_A") == 1 && t2.index_of("WPN_B") == 2);
		CHECK(t2.by_index(1)->category == 3); // overwritten by the second WPN_A block
		def_free_weapons(&mini);
	}

	std::printf("weapon_table: %s\n", failures == 0 ? "OK" : "FAILED");
	return failures == 0 ? 0 : 1;
}
