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
#include <runtime/world/round_sim.h>
#include <runtime/inmatch/loadout_submit.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/mission/runtime_boot.h>

#include <formats/def/def.h>
#include <base/resource_index/resource_index.h>
#include <runtime/assets/asset_store.h>
#include <base/vfs/vfs.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "common/test_paths.h"
#include "common/retail_paths.h"

using namespace opennova;
using namespace opennova::def;

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

// The install's own mount answers the SIGHTS arm's file check: JO:CA's Colt
// .45 authors `sights clt45aim.tga`, which no archive carries, so the weapon
// the kernel loads has no SIGHTS row, while weapons whose cards exist keep
// theirs [orig: WeaponDefs_ParseLineCallback's sights arm,
// FileSystem_FileExists @0x544AE2, the return @0x544B10].
static int colt_sights_through_the_mount(const std::string &install) {
	ResourceIndex index;
	if (!index.scan(install)) {
		std::fprintf(stderr, "FAIL: the base mount of %s does not scan\n", install.c_str());
		return 1;
	}
	const mission::BootFileSource files = mission::boot_files_from_index(index);
	mission::MissionKernel kernel;
	CHECK(kernel.load_weapon_table(files, nullptr));
	const DefWeaponDef *colt = nullptr;
	size_t rows = 0;
	for (size_t i = 0; i < kernel.weapon_defs.count; ++i) {
		const DefWeaponDef &w = kernel.weapon_defs.entries[i];
		rows += w.sights_count;
		if (std::strcmp(w.weapon_name, "WPN_colt45") == 0) colt = &w;
	}
	CHECK(colt != nullptr);
	if (colt != nullptr) CHECK(colt->sights_count == 0);
	CHECK(rows > 0);
	std::printf("mounted weapon.def: WPN_colt45 has %zu SIGHTS rows, the table %zu\n",
	            colt != nullptr ? colt->sights_count : (size_t)0, rows);
	return 0;
}

int main(int argc, char **argv) {
    retail::configure_mixed(argc, argv);
    // Authored/default stability survives DEF -> runtime-table promotion.
    {
        static const char kStability[] =
            "weapon \"WPN_DEFAULT_STABILITY\"\r\nend\r\n"
            "weapon \"WPN_CUSTOM_STABILITY\"\r\nstability 0.5, 2, 1.5\r\nend\r\n";
        DefWeaponsFile parsed{};
        CHECK(def_parse_weapons_memory(reinterpret_cast<const uint8_t *>(kStability),
                sizeof(kStability) - 1, &parsed) == 0);
        const world::WeaponTable table = world::build_weapon_table(parsed);
        const int expected[2][3] = {{65536,65536,65536}, {32768,131072,98304}};
        for (int row = 1; row <= 2; ++row) {
            const world::WeaponTableEntry *weapon = table.by_index(row);
            CHECK(weapon != nullptr);
            if (weapon != nullptr)
                for (int stance = 0; stance < 3; ++stance)
                    CHECK(weapon->stability_fp16[stance] == expected[row - 1][stance]);
        }
        def_free_weapons(&parsed);
    }

    // The zoom seed's three def words survive the promotion: 'scope_max_mag'
    // max (+0x90) and second value (+0x94), 'scope_min_mag' (+0x98, record
    // default 2) [orig: WeaponDefs_ParseLineCallback @0x544F29 / @0x544F44 /
    // @0x544F7A; AdmDef_InitEntryDefaults @0x53FF73].
    {
        static const char kZoom[] =
            "weapon \"WPN_TURRET_ZOOM\"\r\nscope_max_mag 10 2\r\nend\r\n"
            "weapon \"WPN_RCWS_ZOOM\"\r\nscope_max_mag 12\r\nscope_min_mag 1\r\nend\r\n"
            "weapon \"WPN_NO_OPTIC\"\r\nend\r\n";
        DefWeaponsFile parsed{};
        CHECK(def_parse_weapons_memory(reinterpret_cast<const uint8_t *>(kZoom),
                sizeof(kZoom) - 1, &parsed) == 0);
        const world::WeaponTable table = world::build_weapon_table(parsed);
        const int expected[3][3] = {{10, 2, 2}, {12, 0, 1}, {0, 0, 2}};
        for (int row = 1; row <= 3; ++row) {
            const world::WeaponTableEntry *weapon = table.by_index(row);
            CHECK(weapon != nullptr);
            if (weapon == nullptr) continue;
            CHECK(weapon->scope_max_mag == expected[row - 1][0]);
            CHECK(weapon->scope_initial_mag == expected[row - 1][1]);
            CHECK(weapon->scope_min_mag == expected[row - 1][2]);
        }
        CHECK(table.by_index(0)->scope_min_mag == 2); // the engine-created null row
        def_free_weapons(&parsed);
    }

    // `sameas` survives the promotion as the name the powerup weapon grant
    // resolves at each use [orig: AdmDef+0x34; WeaponSlot_InitFromAvatarDef
    // @0x542779].
    {
        static const char kSameAs[] =
            "weapon \"WPN_BASE\"\r\nend\r\n"
            "weapon \"WPN_VARIANT\"\r\nsameas WPN_BASE\r\nend\r\n";
        DefWeaponsFile parsed{};
        CHECK(def_parse_weapons_memory(reinterpret_cast<const uint8_t *>(kSameAs),
                sizeof(kSameAs) - 1, &parsed) == 0);
        const world::WeaponTable table = world::build_weapon_table(parsed);
        CHECK(table.by_index(1) != nullptr && table.by_index(1)->sameas.empty());
        CHECK(table.by_index(2) != nullptr && table.by_index(2)->sameas == "WPN_BASE");
        CHECK(table.index_of(table.by_index(2)->sameas.c_str()) == 1);
        def_free_weapons(&parsed);
    }

    // An entry no `end` closed is in the table, but only `end` binds its
    // actions: its FIRE row stays the unbound default (no id stamped, no
    // delay) where the closed twin's carries the block's delay [orig:
    // WeaponDefs_ParseLineCallback,
    // the slot at the `weapon` line @0x5436E7..0x543737, `end` ->
    // Anim_InitActions @0x5437D0].
    {
        static const char kUnclosed[] =
            "weapon \"WPN_SHUT\"\r\nACTION \"FIRE\"\r\nDELAYEND 3\r\nEND\r\nend\r\n"
            "weapon \"WPN_OPEN\"\r\nclipsize 7\r\nACTION \"FIRE\"\r\nDELAYEND 3\r\nEND\r\nend";
        DefWeaponsFile parsed{};
        CHECK(def_parse_weapons_memory(reinterpret_cast<const uint8_t *>(kUnclosed),
                sizeof(kUnclosed) - 1, &parsed) == 0);
        const world::WeaponTable table = world::build_weapon_table(parsed);
        const world::WeaponTableEntry *shut = table.by_index(1);
        const world::WeaponTableEntry *open = table.by_index(2);
        CHECK(shut != nullptr && open != nullptr && open->name == "WPN_OPEN");
        if (shut != nullptr && open != nullptr) {
            CHECK(open->clipsize == 7);
            CHECK(shut->action_fsm.actions[world::weapon_action::kFire].delay_end == 3);
            CHECK(open->action_fsm.actions[world::weapon_action::kFire].delay_end == 0);
            CHECK(open->action_fsm.actions[world::weapon_action::kFire].id == -1);
        }
        def_free_weapons(&parsed);
    }

	if (const std::string install = retail::install(); !install.empty()) {
		if (live_weapon_oracle(install, std::string()) != 0) return 1;
		for (const std::string &expansion : retail::expansions())
			if (live_weapon_oracle(install, expansion) != 0) return 1;
		if (colt_sights_through_the_mount(install) != 0) return 1;
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
	      world::teamfilter_bit("green") == 0 && world::teamfilter_bit("yellow") == 0 &&
	      world::teamfilter_bit("violet") == 0); // the loadout reader's yellow / violet are not the host's

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

		CHECK(inmatch::resolve_loadout_submit_combo(slots_table, &inventory, 1, 199) == 196);
		CHECK(inmatch::resolve_loadout_submit_combo(slots_table, &inventory, 3, 199) == 196);
		CHECK(inmatch::resolve_loadout_submit_combo(slots_table, &inventory, 2, 199) == 199);
		CHECK(inmatch::resolve_loadout_submit_combo(slots_table, &inventory, 0, 199) == 199);
		CHECK(inmatch::resolve_loadout_submit_combo(slots_table, &inventory, 1, 258) == 196);
		CHECK(inmatch::resolve_loadout_submit_combo(slots_table, &inventory, 1, 260) == 260);
		CHECK(inmatch::resolve_loadout_submit_combo(slots_table, nullptr, 1, 199) == 199);
		CHECK(inmatch::resolve_loadout_submit_combo(slots_table, &inventory, 1, 900) == 900);

		world::World submit_world;
		submit_world.tables.weapons = slots_table;
		playersav::Record profile;
		world::LocalPlayerLoadout loadout;
		inmatch::JoinerConnection::LoadoutKit wire_kit;
		inmatch::build_joiner_loadout_kit(submit_world, profile, 1, loadout, 199,
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

	// --- the explosion-sound fallback: ammo def 0's static effect bank, row 5
	// dword +8 (dword_A2EB80) = the FIFTH authored tag in ascending tag order,
	// never a positional file row [orig: AmmoDef_InitEffectsTable @0x409F20
	// @0x409f62 / @0x409fe8; AmmoDef_GetExplosionRadius @0x409770 @0x40978c].
	{
		const auto row = [](DefEffectTableEntry &e, const char *tag, const char *sound) {
			std::strcpy(e.surface_type, tag);
			std::strcpy(e.hit_effect, "none");
			std::strcpy(e.impact_sound, sound);
		};
		// def 0 authors tags 1..6 contiguously (as shipped AT_NULL does): bank row 5 = tag 5 (dirt).
		DefEffectTableEntry rows0[6] = {};
		row(rows0[0], "move", "none");
		row(rows0[1], "player", "S_PLAYER");
		row(rows0[2], "zip", "S_ZIP");
		row(rows0[3], "obj", "S_OBJ");
		row(rows0[4], "dirt", "DEF0_DIRT");
		row(rows0[5], "grass", "S_GRASS");
		// def 1 authors no dirt row at all; def 2 authors dirt as 'none'.
		DefEffectTableEntry rows1[2] = {};
		row(rows1[0], "grass", "S_GRASS");
		row(rows1[1], "metal", "S_METAL");
		DefEffectTableEntry rows2[1] = {};
		row(rows2[0], "dirt", "none");
		DefAmmoDef defs[3] = {};
		std::strcpy(defs[0].name, "AT_NULL");
		defs[0].effects_table = rows0;
		defs[0].effects_table_count = 6;
		std::strcpy(defs[1].name, "AMMO_NO_DIRT");
		defs[1].effects_table = rows1;
		defs[1].effects_table_count = 2;
		std::strcpy(defs[2].name, "AMMO_DIRT_NONE");
		defs[2].effects_table = rows2;
		defs[2].effects_table_count = 1;
		DefAmmoFile af{defs, 3};
		const world::AmmoTable t = world::build_ammo_table(af);
		CHECK(t.null_bank[5].sound == "DEF0_DIRT" && t.null_bank[5].tag == 5);
		CHECK(t.by_index(0) != nullptr && t.by_index(0)->impact_effects[5].authored &&
		      t.by_index(0)->impact_effects[5].sound == "DEF0_DIRT");
		CHECK(t.by_index(1) != nullptr && !t.by_index(1)->impact_effects[5].authored &&
		      t.by_index(1)->impact_effects[5].sound.empty());
		CHECK(t.by_index(2) != nullptr && t.by_index(2)->impact_effects[5].authored &&
		      t.by_index(2)->impact_effects[5].sound.empty());
		// The bank holds def 0's rows in place: the null slot, then tags 1..6.
		CHECK(t.null_bank_rows == 7 && t.null_bank[0].tag == 0 && t.null_bank[4].tag == 4 &&
		      t.null_bank[4].sound == "S_OBJ" && t.null_bank[7].tag == 0);

		// The impact presenter's row [orig: AmmoDef_ProcessImpactEffect @0x40a1b8..0x40a1fd]: the
		// ammo's own row of the tag; none, def 0's bank at the tag's place; a tag past the table, obj.
		const world::AmmoTableEntry &no_dirt = *t.by_index(1);
		const world::ImpactRowPick grass = world::impact_effect_row(t, no_dirt, 6);
		CHECK(grass.from == world::ImpactRowFrom::Own && grass.sound == "S_GRASS");
		const world::ImpactRowPick dirt = world::impact_effect_row(t, no_dirt, 5);
		CHECK(dirt.from == world::ImpactRowFrom::NullBank && dirt.bank_tag == 5 && dirt.sound == "DEF0_DIRT");
		const world::ImpactRowPick glass = world::impact_effect_row(t, no_dirt, 19);
		CHECK(glass.from == world::ImpactRowFrom::NullBank && glass.bank_tag == 0 && glass.sound.empty() &&
		      glass.effect.empty());
		const world::ImpactRowPick past = world::impact_effect_row(t, no_dirt, 30);
		CHECK(past.tag == 4 && past.from == world::ImpactRowFrom::NullBank && past.sound == "S_OBJ");
		// An authored 'none' row is the ammo's own and plays nothing: no fallback.
		const world::ImpactRowPick none = world::impact_effect_row(t, *t.by_index(2), 5);
		CHECK(none.from == world::ImpactRowFrom::Own && none.sound.empty());
		// A direct reader (the knife, the squib) takes its own row alone.
		CHECK(world::impact_effect_own_row(no_dirt, 5).sound.empty() &&
		      world::impact_effect_own_row(no_dirt, 6).sound == "S_GRASS");
		// The drain and the impact sound present through the same pick.
		world::RoundImpact impact;
		impact.ammo_index = 1;
		impact.effect_tag = 5;
		CHECK(world::round_impact_row(t, impact).sound == "DEF0_DIRT");
		impact.own_row = true;
		CHECK(world::round_impact_row(t, impact).sound.empty());

		// A sparse def 0 (tags 1, 4, 5, 6, 7, 8): bank row 5 is the fifth authored
		// tag = snow (7), not dirt — the bank is compacted, not tag-positional.
		DefEffectTableEntry sparse[6] = {};
		row(sparse[0], "move", "none");
		row(sparse[1], "obj", "S_OBJ");
		row(sparse[2], "dirt", "S_DIRT");
		row(sparse[3], "grass", "S_GRASS");
		row(sparse[4], "snow", "S_SNOW");
		row(sparse[5], "cement", "S_CEMENT");
		DefAmmoDef sparse_def{};
		std::strcpy(sparse_def.name, "AT_NULL");
		sparse_def.effects_table = sparse;
		sparse_def.effects_table_count = 6;
		DefAmmoFile sparse_file{&sparse_def, 1};
		CHECK(world::build_ammo_table(sparse_file).null_bank[5].sound == "S_SNOW");
		// The presenter's fallback reads the same place: an ammo with no dirt row plays def 0's
		// fifth authored row there, snow's, not a dirt row def 0 has at another place.
		{
			DefEffectTableEntry only_grass[1] = {};
			row(only_grass[0], "grass", "S_OWN_GRASS");
			DefAmmoDef two[2] = {sparse_def, DefAmmoDef{}};
			std::strcpy(two[1].name, "AMMO_GRASS");
			two[1].effects_table = only_grass;
			two[1].effects_table_count = 1;
			DefAmmoFile two_file{two, 2};
			const world::AmmoTable sparse_table = world::build_ammo_table(two_file);
			const world::ImpactRowPick pick = world::impact_effect_row(sparse_table, *sparse_table.by_index(1), 5);
			CHECK(pick.from == world::ImpactRowFrom::NullBank && pick.bank_tag == 7 && pick.sound == "S_SNOW");
		}

		// Fewer than five authored tags: the static bank's row 5 stays zero.
		sparse_def.effects_table_count = 4;
		CHECK(world::build_ammo_table(sparse_file).null_bank[5].sound.empty());
	}

	// --- by-name reuse: a re-parsed name keeps its index and takes the new fields
	//     [orig: WeaponDefs_ParseLineCallback @0x5436e1 AvatarDef_FindIndexByName leg].
	{
		static const char kMini[] =
				"weapon \"WPN_A\"\r\n\tcategory 1\r\nend\r\n"
				"weapon \"WPN_B\"\r\n\tcategory 2\r\nend\r\n"
				"weapon \"WPN_A\"\r\n\tcategory 3\r\nend\r\n";
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
				"weapon \"WPN_AUTO_FIXTURE\"\r\n"
				"\tanimadm soldier\r\n"
				"\taction \"fire\"\r\n"
				"\t\tdelaystart auto\r\n"
				"\t\tdelayend auto\r\n"
				"\t\tanim anim_idle\r\n"
				"\t\tfunction wpn_std_fire\r\n"
				"\tend\r\n"
				"end\r\n";
		DefWeaponsFile automatic{};
		CHECK(def_parse_weapons_memory(
				reinterpret_cast<const uint8_t *>(kAutomatic),
				sizeof(kAutomatic) - 1, &automatic) == 0);

		opennova::ResourceIndex index;
		opennova::assets::AssetStore index_assets{&index};
		CHECK(index.scan(std::string(test_paths_repo_root(__FILE__)) + "/fixtures/anim"));
		const world::WeaponTable unresolved = world::build_weapon_table(automatic);
		const world::WeaponTable resolved = world::build_weapon_table(automatic, &index_assets);
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
				"weapon \"WPN_AUTO_NOADM\"\r\n"
				"\taction \"fire\"\r\n"
				"\t\tdelaystart auto\r\n"
				"\t\tdelayend auto\r\n"
				"\t\tanim anim_idle\r\n"
				"\t\tfunction wpn_std_fire\r\n"
				"\tend\r\n"
				"end\r\n";
		DefWeaponsFile no_adm{};
		CHECK(def_parse_weapons_memory(
				reinterpret_cast<const uint8_t *>(kNoAdm),
				sizeof(kNoAdm) - 1, &no_adm) == 0);
		const world::WeaponTable no_adm_table = world::build_weapon_table(no_adm, &index_assets);
		const world::WeaponFsmAction &no_adm_fire =
				no_adm_table.by_index(1)->action_fsm.actions[world::weapon_action::kFire];
		CHECK(no_adm_fire.delay_start == 0 && no_adm_fire.delay_end == 0);
		def_free_weapons(&no_adm);
	}

	// The gfx3 model loads with its def and the launch userpoint resolves on it
	// once every def has parsed: the first case-insensitive name match,
	// 1-based; an unmatched or unnamed point stays 0, a def without gfx3 (or a
	// build without a resource source) loads no model.
	// [orig: WeaponDefs_ParseLineCallback load @0x544FCE, store @0x545092,
	//  launchuserpoint @0x544479..0x5444AC; WeaponDef_ResolveAllReferences
	//  @0x5402C2..0x540316 -> ModelGPM_FindUserpointByName @0x5B2170]
	{
		static const char kLaunch[] =
				"weapon \"WPN_GFX3_CAMERA\"\r\n"
				"\tgfx3 mount\r\n"
				"\tlaunchuserpoint CAMERA\r\n"
				"end\r\n"
				"weapon \"WPN_GFX3_FLASH\"\r\n"
				"\tgfx3 mount\r\n"
				"\tlaunchuserpoint mflash01\r\n"
				"end\r\n"
				"weapon \"WPN_GFX3_MISSING\"\r\n"
				"\tgfx3 mount\r\n"
				"\tlaunchuserpoint missing\r\n"
				"end\r\n"
				"weapon \"WPN_GFX3_UNNAMED\"\r\n"
				"\tgfx3 mount\r\n"
				"end\r\n"
				"weapon \"WPN_NO_GFX3\"\r\n"
				"\tlaunchuserpoint camera\r\n"
				"end\r\n";
		DefWeaponsFile launch{};
		CHECK(def_parse_weapons_memory(
				reinterpret_cast<const uint8_t *>(kLaunch),
				sizeof(kLaunch) - 1, &launch) == 0);
		opennova::ResourceIndex index;
		opennova::assets::AssetStore models{&index};
		CHECK(index.scan(std::string(test_paths_repo_root(__FILE__)) + "/fixtures/threedi/synth"));
		const world::WeaponTable table = world::build_weapon_table(launch, &models);
		// fixtures/threedi/synth/mount.3di: BCasing, Bullet, Camera, heat,
		// MFlash01, Usegun.
		const world::WeaponTableEntry *camera = table.by_index(1);
		const world::WeaponTableEntry *flash = table.by_index(2);
		const world::WeaponTableEntry *missing = table.by_index(3);
		const world::WeaponTableEntry *unnamed = table.by_index(4);
		const world::WeaponTableEntry *no_gfx3 = table.by_index(5);
		CHECK(camera != nullptr && camera->third_person_model_asset != nullptr &&
				camera->launch_userpoint == 3);
		CHECK(flash != nullptr && flash->launch_userpoint == 5);
		CHECK(missing != nullptr && missing->third_person_model_asset != nullptr &&
				missing->launch_userpoint == 0);
		CHECK(unnamed != nullptr && unnamed->third_person_model_asset != nullptr &&
				unnamed->launch_userpoint == 0);
		CHECK(no_gfx3 != nullptr && no_gfx3->third_person_model_asset == nullptr &&
				no_gfx3->launch_userpoint == 0);
		const world::WeaponTable bare = world::build_weapon_table(launch);
		CHECK(bare.by_index(1) != nullptr && bare.by_index(1)->third_person_model_asset == nullptr &&
				bare.by_index(1)->launch_userpoint == 0);
		def_free_weapons(&launch);
	}

	std::printf("weapon_table: %s\n", failures == 0 ? "OK" : "FAILED");
	return failures == 0 ? 0 : 1;
}
