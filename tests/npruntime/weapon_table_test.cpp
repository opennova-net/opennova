// D-NET-141 — the armory table build + the witnessed loadout-service rules, exercised against
// the shipped weapon.def from the reference fixture set (94 weapons; NOTE a live install's VFS-resolved
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

#include <runtime/world/weapon_table_build.h>
#include <runtime/world/ammo_table_build.h>
#include <runtime/session/loadout_submit.h>

#include <formats/def/def.h>
#include <base/resource_index/resource_index.h>
#include <base/vfs/vfs.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "common/test_paths.h"
#include "common/retail_paths.h"

using namespace opennova;

static int failures = 0;
#define CHECK(c) \
	do { \
		if (!(c)) { \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); \
			++failures; \
		} \
	} while (0)

// The retail-install oracle: the base mount and every expansion the install
// ships (OPENNOVA_JO_DIR), so protocol work can inspect the exact
// expansion-scoped ADM indices and ammo classes a capture used. A retail leg of
// an otherwise hermetic test; reported, never a pin.
static int live_weapon_oracle(const std::string &install, const std::string &expansion) {
	opennova::Vfs vfs;
	if (!vfs.mount_game(install, expansion, opennova::VfsMountMode::Packed)) {
		std::fprintf(stderr, "FAIL: live weapon mount (%s): %s\n", expansion.c_str(),
		             vfs.last_error().c_str());
		return 1;
	}
	std::vector<uint8_t> bytes;
	if (!vfs.read_file("weapon.def", bytes) || bytes.empty()) {
		std::fprintf(stderr, "FAIL: mounted weapon.def is unavailable\n");
		return 1;
	}
	DefWeaponsFile live_file{};
	if (def_parse_weapons_memory(bytes.data(), bytes.size(), &live_file) != 0) {
		std::fprintf(stderr, "FAIL: mounted weapon.def does not parse\n");
		return 1;
	}
	const world::WeaponTable live = world::build_weapon_table(live_file);
	std::printf("LIVE weapon.def bytes=%zu entries=%zu expansion=%s\n", bytes.size(),
	            live.entries.size(), vfs.mounted_expansion().c_str());
	for (size_t i = 0; i < live.ammo_class_names.size(); ++i)
		std::printf("LIVE ammo-class local=%zu cap=%d name=%s\n", i,
		            live.ammo_class_caps[i], live.ammo_class_names[i].c_str());
	for (const int index : {1, 3, 16, 83, 84, 85, 88, 93, 97}) {
		const world::WeaponTableEntry *entry =
				live.by_index(static_cast<uint8_t>(index));
		if (entry == nullptr) continue;
		std::printf(
				"LIVE adm=%d name=%s combo=%u:%u class=%s local=%d units=%d "
				"clip=%d start=%d maxclips=%d bucket=%d weapon-class=%d\n",
				index, entry->name.c_str(), entry->category, entry->rank,
				entry->ammo_class.c_str(), entry->ammo_class_id,
				entry->ammo_class_count, entry->clipsize, entry->startrounds,
				entry->maxclips, entry->ammo_bucket, entry->weapon_class_slot);
	}
	def_free_weapons(&live_file);
	return 0;
}

int main(void) {
	if (const std::string install = retail::install(); !install.empty()) {
		if (live_weapon_oracle(install, std::string()) != 0) return 1;
		for (const std::string &expansion : retail::expansions())
			if (live_weapon_oracle(install, expansion) != 0) return 1;
	} else {
		retail::skip_leg("OPENNOVA_JO_DIR (the live weapon.def oracle over the install's expansions)");
	}

	// The shipped weapon.def from the reference fixture set: the index/field pins
	// below are its SKIP-LEG retail leg; the inline tables after it run always.
	const std::string fixture = retail::reference_fixture("def/weapon.def");
	if (fixture.empty()) {
		retail::skip_leg("OPENNOVA_JO_ASSETS/fixtures/def/weapon.def (the shipped weapon table's index and field pins)");
	} else {
	const char *path = fixture.c_str();
	DefWeaponsFile wf;
	std::memset(&wf, 0, sizeof(wf));
	if (def_parse_weapons(path, &wf) != 0) {
		std::fprintf(stderr, "FAIL: def_parse_weapons failed for %s\n", path);
		return 1;
	}

	const world::WeaponTable table = world::build_weapon_table(wf);

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
	// Slot types 0..10 exist before weapon.def parses. Ammo classes are registered
	// into that SAME retail score-type namespace, so their ids are visible in the
	// fixed 128-i32 S2C 0x0F image (retail-ashi5a: .45=15, 5.56=22 in this
	// fixture; revx02 inserts AK47GP and therefore carries 5.56 at 23).
	CHECK(table.ammo_class_id_of("") == 0);
	CHECK(table.ammo_class_id_of("CLASS_MANA") == 1);
	CHECK(table.ammo_class_id_of("CLASS_HP") == 2);
	CHECK(table.ammo_class_id_of("CLASS_POWER1") == 3);
	CHECK(table.ammo_class_id_of("CLASS_POWER8") == 10);
	CHECK(table.ammo_class_id_of("CLASS_RGRENADE") == 11);
	CHECK(table.ammo_class_id_of("CLASS_GRENADEHE") == 12);
	CHECK(table.ammo_class_id_of("CLASS_45cal") == 15);
	CHECK(table.ammo_class_id_of("CLASS_556MM") == 22);

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
	CHECK(m4->ammo_class == "CLASS_556MM" && m4->ammo_class_id == 22);
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
		world::LoadoutAmmoBytes a = world::resolve_loadout_ammo(table, 2, 0xFF); // KNIFE2: no-clip
		CHECK(a.primary == 0xFF && a.secondary == 0xFF);
		a = world::resolve_loadout_ammo(table, 3, 0xFF); // colt45: 35/7
		CHECK(a.primary == 5 && a.secondary == 0xFF);
		a = world::resolve_loadout_ammo(table, 9, 0xFF); // M4AUTO: 300/30; variant M4 shares class
		CHECK(a.primary == 10 && a.secondary == 0xFF);
		a = world::resolve_loadout_ammo(table, 9, 4); // requested 4 -> min(4, maxclips 10)
		CHECK(a.primary == 4 && a.secondary == 0xFF);
		a = world::resolve_loadout_ammo(table, 9, 200); // requested over maxclips -> clamp to 10
		CHECK(a.primary == 10);
		// M4M203AUTO @11 (LSC 2): M4M203 shares CLASS_556MM, M4M203HE is CLASS_40MMNADE -> 6/1.
		CHECK(table.index_of("WPN_M4M203AUTO") == 11);
		a = world::resolve_loadout_ammo(table, 11, 0xFF);
		CHECK(a.primary == 10 && a.secondary == 6);
		const int he = table.index_of("WPN_GRENADEHE");
		CHECK(he > 0);
		a = world::resolve_loadout_ammo(table, static_cast<uint8_t>(he), 0xFF); // 3/1
		CHECK(a.primary == 3 && a.secondary == 0xFF);
		a = world::resolve_loadout_ammo(table, 250, 0xFF); // no entry
		CHECK(a.primary == 0xFF && a.secondary == 0xFF);
	}

	// --- permission masks.
	{
		const world::WeaponTableEntry &m4e = *table.by_index(9);
		CHECK(world::loadout_entry_permitted(m4e, 1, 8));  // blue rifleman
		CHECK(!world::loadout_entry_permitted(m4e, 2, 8)); // red side: M4 is blue-only
		CHECK(!world::loadout_entry_permitted(m4e, 1, 6)); // sniper bit not in rifleman|medic|engineer
		CHECK(world::loadout_entry_permitted(m4e, 1, 5));  // medic bit present
		CHECK(world::loadout_entry_permitted(m4e, 0, 2));  // class 1..3 -> all char bits; class 0 -> both teams
		const world::WeaponTableEntry &k2 = *table.by_index(2); // KNIFE2: red, all classes
		CHECK(world::loadout_entry_permitted(k2, 2, 8));
		CHECK(!world::loadout_entry_permitted(k2, 1, 8));
		// An unfiltered (emplaced) entry is never loadout-visible: masks are 0.
		const int mini = table.index_of("WPN_WEAKAIMINI");
		CHECK(mini > 0 && !world::loadout_entry_permitted(*table.by_index(static_cast<uint8_t>(mini)), 1, 8));
		const world::WeaponTableEntry &minie =
				*table.by_index(static_cast<uint8_t>(mini));
		CHECK(minie.action_fsm.auto_fire);
		CHECK(minie.action_fsm.clip_capacity == -1);
		CHECK(minie.action_fsm.actions[world::weapon_action::kFire].delay_end == 0);
		CHECK(minie.action_fsm.actions[world::weapon_action::kRecoil].delay_end == 7);
	}

	// --- token -> bit maps [orig: @0x830EB0 / @0x830ED8].
	CHECK(world::charfilter_bit("medic") == 0x01 && world::charfilter_bit("sniper") == 0x02 &&
	      world::charfilter_bit("gunner") == 0x04 && world::charfilter_bit("rifleman") == 0x08 &&
	      world::charfilter_bit("engineer") == 0x10 && world::charfilter_bit("bogus") == 0);
	CHECK(world::teamfilter_bit("red") == 0x01 && world::teamfilter_bit("BLUE") == 0x02 &&
	      world::teamfilter_bit("green") == 0);

	// --- C2S 0x2F send-time slot re-resolution. The requested red-only
	// category-3 slot is corrected to the first populated blue slot for teams
	// 1/3, but stays raw when the local table/category cannot supply a legal
	// replacement [orig: NetPacket_SendLoadoutSubmit @0x42ce2d..0x42ce8b].
	{
		world::WeaponTable slots_table;
		slots_table.entries.resize(5);
		const auto define = [&](int adm, uint8_t category, uint8_t rank,
				uint8_t teamfilter) {
			world::WeaponTableEntry &entry = slots_table.entries[adm];
			entry.valid = true;
			entry.category = category;
			entry.rank = rank;
			entry.teamfilter = teamfilter;
		};
		define(1, 3, 4, 1); // red, combo 199 (the requested live slot)
		define(2, 3, 1, 2); // blue, combo 196 (first legal fallback)
		define(3, 3, 2, 2); // blue, combo 197
		define(4, 4, 0, 1); // red, combo 260; no blue peer in category 4

		world::WeaponInventory inventory;
		inventory.reset(slots_table);
		inventory.slots[199].adm_index = 1;
		inventory.slots[196].adm_index = 2;
		inventory.slots[197].adm_index = 3;
		inventory.slots[260].adm_index = 4;

		CHECK(np::resolve_loadout_submit_combo(slots_table, &inventory, 1, 199) == 196);
		CHECK(np::resolve_loadout_submit_combo(slots_table, &inventory, 3, 199) == 196);
		CHECK(np::resolve_loadout_submit_combo(slots_table, &inventory, 2, 199) == 199);
		CHECK(np::resolve_loadout_submit_combo(slots_table, &inventory, 0, 199) == 199);
		CHECK(np::resolve_loadout_submit_combo(slots_table, &inventory, 1, 258) == 196);
		CHECK(np::resolve_loadout_submit_combo(slots_table, &inventory, 1, 260) == 260);
		CHECK(np::resolve_loadout_submit_combo(slots_table, nullptr, 1, 199) == 199);
		CHECK(np::resolve_loadout_submit_combo(slots_table, &inventory, 1, 900) == 900);

		world::World submit_world;
		submit_world.weapons = slots_table;
		playersav::Record profile;
		world::LocalPlayerLoadout loadout;
		np::JoinerConnection::LoadoutKit wire_kit;
		np::build_joiner_loadout_kit(submit_world, profile, 1, loadout, 199,
				&inventory, wire_kit);
		CHECK(wire_kit.equipped_combo == 196);
	}

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
		source.secondary_anim = 258;
		source.kz_physics = -1;
		source.scorch_id = 7;
		DefAmmoFile af{&source, 1};
		const world::AmmoTable ammo_table = world::build_ammo_table(af);
		const world::AmmoTableEntry *baked = ammo_table.by_index(0);
		CHECK(baked != nullptr && baked->spread_error_fp16 == 12345);
		CHECK(baked != nullptr && baked->recoil[0] == 255 && baked->recoil[1] == 0 &&
		      baked->recoil[2] == 255);
		CHECK(baked != nullptr && baked->secondary_anim == 2 &&
		      baked->kz_physics == 255);
		CHECK(baked != nullptr && baked->scorch_id == 7);
	}

	def_free_weapons(&wf);
	}  // retail leg

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
		const world::WeaponTable t2 = world::build_weapon_table(mini);
		CHECK(t2.entries.size() == 3); // null + A + B (the re-parsed A reused its slot)
		CHECK(t2.index_of("WPN_A") == 1 && t2.index_of("WPN_B") == 2);
		CHECK(t2.by_index(1)->category == 3); // overwritten by the second WPN_A block
		def_free_weapons(&mini);
	}

	// --- D-WPN-26: the production table bake reads authored automatic action
	// durations from the weapon's ADM instead of collapsing them to zero. The
	// committed soldier fixture maps anim_idle to a 0.266667-second BAD, which
	// Anim_GetDurationTicks converts to the independently pinned literal 18.
	{
		static const char kAutomatic[] =
				"weapon \"WPN_AUTO_FIXTURE\"\n"
				"\tanimadm soldier\n"
				"\taction \"fire\"\n"
				"\t\tdelaystart auto\n"
				"\t\tdelayend auto\n"
				"\t\tanim anim_idle\n"
				"\t\tfunction wpn_std_fire\n"
				"\tend\n"
				"end\n";
		DefWeaponsFile automatic{};
		CHECK(def_parse_weapons_memory(
				reinterpret_cast<const uint8_t *>(kAutomatic),
				sizeof(kAutomatic) - 1, &automatic) == 0);

		opennova::ResourceIndex index;
		CHECK(index.scan(std::string(test_paths_repo_root(__FILE__)) + "/fixtures/anim"));
		const world::WeaponTable unresolved = world::build_weapon_table(automatic);
		const world::WeaponTable resolved = world::build_weapon_table(automatic, &index);
		const world::WeaponFsmAction &unresolved_fire =
				unresolved.by_index(1)->action_fsm.actions[world::weapon_action::kFire];
		const world::WeaponFsmAction &resolved_fire =
				resolved.by_index(1)->action_fsm.actions[world::weapon_action::kFire];
		CHECK(unresolved_fire.delay_start == 0 && unresolved_fire.delay_end == 0);
		CHECK(resolved_fire.delay_start == 18 && resolved_fire.delay_end == 18);
		def_free_weapons(&automatic);

		// A definition with NO animadm has no anim object, so its 'auto' fields
		// collapse to zero even with the resource index mounted
		// [orig: Anim_InitActions @0x542180 "Error, need to define a anim adm"].
		static const char kNoAdm[] =
				"weapon \"WPN_AUTO_NOADM\"\n"
				"\taction \"fire\"\n"
				"\t\tdelaystart auto\n"
				"\t\tdelayend auto\n"
				"\t\tanim anim_idle\n"
				"\t\tfunction wpn_std_fire\n"
				"\tend\n"
				"end\n";
		DefWeaponsFile no_adm{};
		CHECK(def_parse_weapons_memory(
				reinterpret_cast<const uint8_t *>(kNoAdm),
				sizeof(kNoAdm) - 1, &no_adm) == 0);
		const world::WeaponTable no_adm_table = world::build_weapon_table(no_adm, &index);
		const world::WeaponFsmAction &no_adm_fire =
				no_adm_table.by_index(1)->action_fsm.actions[world::weapon_action::kFire];
		CHECK(no_adm_fire.delay_start == 0 && no_adm_fire.delay_end == 0);
		def_free_weapons(&no_adm);
	}

	std::printf("weapon_table: %s\n", failures == 0 ? "OK" : "FAILED");
	return failures == 0 ? 0 : 1;
}
