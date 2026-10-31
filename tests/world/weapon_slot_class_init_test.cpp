// The class inits' MountSlot leg (L4-s37). Retail binds and seeds an ewep's embedded slot, and
// an EWeap CHel / cpln's, in the entity's def+0x148 class init: at the mission start, a
// deployable's spawn and its addeweap children, a joiner's 0x0D / 0x18 rows, an aircraft's
// respawn. The
// ewep init splits the weapon's startrounds once its model is loaded; the aircraft's splits with
// no model test. No mount, dismount or remount binds, seeds or refills the slot: the gun keeps
// the rounds it was left with. A PlayerControl carrier's death refills its EWeap group's slots
// from the weapon their init bound.
// [orig: Entity_InitBoneReferences @0x441470 (WeaponSlot_InitFromEntityDef called @0x441482,
//  WeaponSlot_SplitAmmoIntoClipAndReserve called @0x4415D8); Entity_InitAircraftWeaponBones
//  @0x490160 (the calls @0x49017B / @0x490192); WeaponSlot_InitFromEntityDef @0x5466C0;
//  WeaponSlot_SplitAmmoIntoClipAndReserve @0x546370; Entity_AttachToUseGunSlot @0x546B80;
//  Entity_DetachFromVehicle @0x4355F0; Entity_RespawnVehicle @0x45FF40 (the class init call
//  @0x45FF68); Vehicle_ReleaseEWeapGroupOnDestruction @0x547040 (the split @0x54710E);
//  Entity_InitAllFromModels @0x40E460]

#include <runtime/mission/mission_kernel.h>
#include <runtime/world/ai.h>
#include <runtime/world/destruction.h>
#include <runtime/world/entity.h>
#include <runtime/world/mount_controls.h>
#include <runtime/world/vehicle_part_anim.h>
#include <runtime/world/vehicle_system.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <map>
#include <memory>
#include <string>

#include "common/boot_file_source.h"
#include "../common/title_fixture.h"

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

constexpr int32_t kGunItem = 40;   // an ewep row
constexpr int32_t kHeloItem = 41;  // a CHel row
constexpr int32_t kCrateItem = 42; // a row whose init binds no slot

constexpr uint8_t kShortAdm = 1;    // more clip than rounds
constexpr uint8_t kPlainAdm = 2;
constexpr uint8_t kCliplessAdm = 3;

void class_rows(w::World &world) {
	w::ItemDeathTraits gun;
	gun.death_class = w::ItemDeathClass::kEwep;
	world.tables.item_death_traits.set(kGunItem, gun);
	w::ItemDeathTraits helo;
	helo.death_class = w::ItemDeathClass::kBrainMachine;
	helo.air_class_init = true;
	world.tables.item_death_traits.set(kHeloItem, helo);
	w::ItemDeathTraits crate;
	crate.death_class = w::ItemDeathClass::kNull;
	world.tables.item_death_traits.set(kCrateItem, crate);
}

void add_weapon(w::World &world, uint8_t adm, const char *name, int16_t clip, int16_t start) {
	if (world.tables.weapons.entries.size() <= adm) world.tables.weapons.entries.resize(adm + 1u);
	w::WeaponTableEntry &weapon = world.tables.weapons.entries[adm];
	weapon.name = name;
	weapon.valid = true;
	weapon.clipsize = clip;
	weapon.startrounds = start;
}

struct Rig {
	std::unique_ptr<w::World> wp = std::make_unique<w::World>();
	bool wp_jo = test_titles::jo_world(*wp);
	w::World &world = *wp;

	Rig() {
		world.registry.configure_pool(0, 4);
		world.registry.configure_pool(1, 8);
		add_weapon(world, kShortAdm, "WPN_SHORT", 30, 10);
		add_weapon(world, kPlainAdm, "WPN_PLAIN", 12, 41);
		add_weapon(world, kCliplessAdm, "WPN_CLIPLESS", -1, 500);
		class_rows(world);
	}

	w::EntityHandle item(int32_t item_id, const char *weapon, uint32_t attrib, bool model,
			uint8_t item_type = 6) {
		w::Entity e;
		e.kind = w::EntityKind::Item;
		e.has_item_def = true;
		e.has_graphic_model = model;
		e.item_id = item_id;
		e.item_type = item_type;
		e.item_attrib = attrib;
		e.item_type_index = 2;
		e.health = e.health_max = 100;
		e.alive = true;
		e.primary_weapon = weapon;
		return world.registry.spawn(1, e);
	}
	w::Entity &at(w::EntityHandle h) { return *world.registry.get(h); }
};

// The ewep init binds the slot and, its model loaded, splits the weapon's startrounds: the clip
// capped at the rounds and the rest in reserve. Without a model the init's own words stand (the
// clipsize, and the unclamped 16-bit difference). A clipless weapon keeps the init's words
// (the split's -1 tests). The ewep init reads no attrib. The CHel init splits with no model
// test, under its def's EWeap bit alone. Another row's init touches no slot.
// [orig: Entity_InitBoneReferences @0x441482, the model / slot Def tests @0x44148D / @0x441495,
//  the split @0x4415D8; WeaponSlot_InitFromEntityDef @0x54670F..0x546733;
//  WeaponSlot_SplitAmmoIntoClipAndReserve @0x54637F..0x5463B5; Entity_InitAircraftWeaponBones
//  the attrib test @0x490169, the split @0x490192]
void test_the_class_inits_bind_and_split() {
	Rig r;
	const w::EntityHandle split = r.item(kGunItem, "WPN_SHORT", w::kItemAttribEweap, true);
	r.world.vehicles.run_weapon_slot_class_init(r.at(split));
	CHECK(r.at(split).primary_weapon_slot_adm == kShortAdm);
	CHECK(r.at(split).primary_weapon_slot.clip == 10 && r.at(split).primary_weapon_slot.reserve == 0);

	const w::EntityHandle bare = r.item(kGunItem, "WPN_SHORT", w::kItemAttribEweap, false);
	r.world.vehicles.run_weapon_slot_class_init(r.at(bare));
	CHECK(r.at(bare).primary_weapon_slot_adm == kShortAdm);
	CHECK(r.at(bare).primary_weapon_slot.clip == 30 && r.at(bare).primary_weapon_slot.reserve == -20);

	const w::EntityHandle clipless = r.item(kGunItem, "WPN_CLIPLESS", w::kItemAttribEweap, true);
	r.world.vehicles.run_weapon_slot_class_init(r.at(clipless));
	CHECK(r.at(clipless).primary_weapon_slot.clip == -1 &&
			r.at(clipless).primary_weapon_slot.reserve == 500);

	const w::EntityHandle no_attrib = r.item(kGunItem, "WPN_PLAIN", 0, true);
	r.world.vehicles.run_weapon_slot_class_init(r.at(no_attrib));
	CHECK(r.at(no_attrib).primary_weapon_slot_adm == kPlainAdm);
	CHECK(r.at(no_attrib).primary_weapon_slot.clip == 12 &&
			r.at(no_attrib).primary_weapon_slot.reserve == 29);

	const w::EntityHandle helo = r.item(kHeloItem, "WPN_SHORT", w::kItemAttribEweap, false, 1);
	r.world.vehicles.run_weapon_slot_class_init(r.at(helo));
	CHECK(r.at(helo).primary_weapon_slot_adm == kShortAdm);
	CHECK(r.at(helo).primary_weapon_slot.clip == 10 && r.at(helo).primary_weapon_slot.reserve == 0);

	const w::EntityHandle unarmed_helo = r.item(kHeloItem, "WPN_SHORT", 0, true, 1);
	r.world.vehicles.run_weapon_slot_class_init(r.at(unarmed_helo));
	CHECK(r.at(unarmed_helo).primary_weapon_slot_adm == w::kAdmSlotNone);
	CHECK(r.at(unarmed_helo).primary_weapon_slot.clip == 0);

	const w::EntityHandle crate = r.item(kCrateItem, "WPN_PLAIN", w::kItemAttribEweap, true);
	r.world.vehicles.run_weapon_slot_class_init(r.at(crate));
	CHECK(r.at(crate).primary_weapon_slot_adm == w::kAdmSlotNone);
	CHECK(r.at(crate).primary_weapon_slot.clip == 0);
}

// The mission start's pass binds an unmanned gun, whose slot then reads its Def and its rounds
// before anyone mounts it. A UseGun attach stores the slot's owner alone; the detach and a
// remount leave the clip and reserve where the last gunner left them, never refilled; and the
// gun's Def is the bound one whatever its name says afterwards.
// [orig: Entity_InitAllFromModels @0x40E460 (the pool-1 class init @0x40E5D3);
//  Entity_AttachToUseGunSlot @0x546B80 (the owner @0x546C73, no slot seed);
//  Entity_DetachFromVehicle @0x4355F0; WeaponSlot_InitFromEntityDef the slot Def
//  @0x546709..0x54670A]
void test_the_spawn_state_survives_mount_and_remount() {
	Rig r;
	w::Entity gun_seed;
	gun_seed.kind = w::EntityKind::Item;
	gun_seed.has_item_def = true;
	gun_seed.has_graphic_model = true;
	gun_seed.item_id = kGunItem;
	gun_seed.item_type = 6;
	gun_seed.item_type_index = 2;
	gun_seed.item_attrib = w::kItemAttribEweap;
	gun_seed.health = 100;
	gun_seed.alive = true;
	gun_seed.primary_weapon = "WPN_PLAIN";
	w::Seat usegun;
	usegun.type = w::SeatType::Gunner;
	usegun.bone_index = 6;
	usegun.source_name = "UseGun";
	gun_seed.seats.push_back(usegun);
	const w::EntityHandle gun_h = r.world.registry.spawn(1, gun_seed);
	r.world.vehicles.run_weapon_slot_class_inits();

	w::Entity &gun = r.at(gun_h);
	CHECK(gun.primary_weapon_slot_adm == kPlainAdm);
	CHECK(gun.primary_weapon_slot.clip == 12 && gun.primary_weapon_slot.reserve == 29);
	CHECK(w::emplaced_slot_def(r.world, gun) == r.world.tables.weapons.by_index(kPlainAdm));

	w::Entity gunner;
	gunner.kind = w::EntityKind::Organic;
	gunner.item_type_index = 3;
	gunner.player_class = 8;
	gunner.health = 100;
	gunner.alive = true;
	const w::EntityHandle gunner_h = r.world.registry.spawn(0, gunner);
	w::AiEntity *body = r.world.ai.at(r.world.ai.attach(gunner_h));
	body->health = 100;
	body->inf.active = true;

	CHECK(r.world.vehicles.process_attach(gunner_h, gun_h, 6));
	CHECK(r.at(gun_h).primary_weapon_owner == gunner_h);
	CHECK(r.at(gun_h).primary_weapon_slot_adm == kPlainAdm);
	CHECK(r.at(gun_h).primary_weapon_slot.clip == 12 && r.at(gun_h).primary_weapon_slot.reserve == 29);

	// The gunner spends: 5 loaded, 3 carried.
	r.at(gun_h).primary_weapon_slot.clip = 5;
	r.at(gun_h).primary_weapon_slot.reserve = 3;
	CHECK(r.world.vehicles.detach(gunner_h));
	CHECK(r.at(gun_h).primary_weapon_slot.clip == 5 && r.at(gun_h).primary_weapon_slot.reserve == 3);

	CHECK(r.world.vehicles.process_attach(gunner_h, gun_h, 6));
	CHECK(r.at(gun_h).primary_weapon_owner == gunner_h);
	CHECK(r.at(gun_h).primary_weapon_slot_adm == kPlainAdm);
	CHECK(r.at(gun_h).primary_weapon_slot.clip == 5 && r.at(gun_h).primary_weapon_slot.reserve == 3);

	r.at(gun_h).primary_weapon = "WPN_SHORT";
	CHECK(w::emplaced_slot_def(r.world, r.at(gun_h)) == r.world.tables.weapons.by_index(kPlainAdm));
}

// An EWeap aircraft's respawn re-runs its class init: the slot is cleared and seeded afresh,
// its owner with it. [orig: Entity_RespawnVehicle @0x45FF53..0x45FF68 -> the CHel init's slot
//  leg; WeaponSlot_InitFromEntityDef the memset @0x5466D3]
void test_the_aircraft_respawn_reseeds_its_slot() {
	Rig r;
	const w::EntityHandle helo_h = r.item(kHeloItem, "WPN_PLAIN",
			w::kItemAttribEweap | w::kItemAttribPlayerControl, true, 1);
	r.world.vehicles.run_weapon_slot_class_init(r.at(helo_h));
	CHECK(r.at(helo_h).primary_weapon_slot.clip == 12 && r.at(helo_h).primary_weapon_slot.reserve == 29);
	r.at(helo_h).primary_weapon_slot.clip = 0;
	r.at(helo_h).primary_weapon_slot.reserve = 0;
	r.at(helo_h).primary_weapon_owner = w::EntityHandle::make(0, 1);
	r.world.vehicles.respawn(r.at(helo_h));
	CHECK(r.at(helo_h).primary_weapon_slot_adm == kPlainAdm);
	CHECK(r.at(helo_h).primary_weapon_slot.clip == 12 && r.at(helo_h).primary_weapon_slot.reserve == 29);
	CHECK(!r.at(helo_h).primary_weapon_owner.valid());
}

// A PlayerControl carrier's death refills its never-mounted EWeap child from the weapon the
// child's spawn bound: the split of its startrounds.
// [orig: Vehicle_ReleaseEWeapGroupOnDestruction @0x547040 - the slot Def @0x5470F1, the split
//  @0x54710E]
void test_the_carrier_death_refills_the_spawn_bound_child() {
	Rig r;
	const w::EntityHandle carrier_h = r.item(kHeloItem, "", w::kItemAttribPlayerControl, true, 1);
	r.at(carrier_h).ref_num = 7;
	const w::EntityHandle gun_h = r.item(kGunItem, "WPN_SHORT", w::kItemAttribEweap, true);
	r.at(gun_h).ref_num = 7;
	r.at(gun_h).ref_group_member = true;
	r.world.vehicles.run_weapon_slot_class_inits();
	CHECK(r.at(carrier_h).primary_weapon_slot_adm == w::kAdmSlotNone);
	CHECK(r.at(gun_h).primary_weapon_slot.clip == 10 && r.at(gun_h).primary_weapon_slot.reserve == 0);
	r.at(gun_h).primary_weapon_slot.clip = 1;
	r.world.vehicles.cleanup_destroyed_ref_group(r.at(carrier_h));
	CHECK(r.at(gun_h).primary_weapon_slot.clip == 10 && r.at(gun_h).primary_weapon_slot.reserve == 0);
}

// Ours promotes the mission ahead of the armory, so the slot leg runs as the armory lands: a gun
// already in the registry is bound and seeded by the table load, before anything mounts.
// [orig: Game_StartMission - Entity_InitAllFromModels @0x52567F, after the weapon.def load]
void test_the_armory_load_binds_the_promoted_guns() {
	auto kernel = std::make_unique<opennova::mission::MissionKernel>();
	test_titles::jo_world(kernel->world);
	w::World &world = kernel->world;
	world.registry.configure_pool(1, 4);
	class_rows(world);
	w::Entity gun;
	gun.kind = w::EntityKind::Item;
	gun.has_item_def = true;
	gun.has_graphic_model = true;
	gun.item_id = kGunItem;
	gun.item_type = 6;
	gun.item_attrib = w::kItemAttribEweap;
	gun.primary_weapon = "WPN_LATE";
	const w::EntityHandle gun_h = world.registry.spawn(1, gun);
	CHECK(world.registry.get(gun_h)->primary_weapon_slot_adm == w::kAdmSlotNone);
	const std::map<std::string, std::string> files = {
			{"weapon.def", "weapon \"WPN_LATE\"\r\n\tclipsize 7\r\n\tstartrounds 21\r\nend\r\n"}};
	CHECK(kernel->load_weapon_table(test_boot::source_over(&files), nullptr));
	const w::Entity &live = *world.registry.get(gun_h);
	const int late = world.tables.weapons.index_of("WPN_LATE");
	CHECK(late > 0 && live.primary_weapon_slot_adm == late);
	CHECK(live.primary_weapon_slot.clip == 7 && live.primary_weapon_slot.reserve == 14);
}

// A joiner's CHel / cpln class init takes back the row's +0xB0, the 0x0D's copy of the host
// brain's latch byte: the brain's latch byte becomes its low two bits (the other six cleared),
// and with bit 7 the rotor starts at the title's full rate and at full speed. A brainless row
// seeds its rotor and writes no latch; the authority, a single player and another class's row
// run no such tail; dfx2.exe's full rate is 0x5B05B.
// [orig: Entity_InitAircraftWeaponBones @0x490160 - the authority / session tests
//  @0x4902BB..0x4902CB, +0x318 = +0xB0 & 3 @0x4902D5..0x4902DE, the bit-7 test @0x4902E4,
//  +0x468 / +0x460 @0x4902ED / @0x4902F7]
// [orig: sub_490170 @0x490170 - +0x468 = 0x5B05B @0x4902FD (dfx2.exe)]
void test_a_joiners_aircraft_init_takes_back_its_latch_byte() {
	const auto latched = [](const w::Entity &e) {
		const w::Entity::VehicleMotorState &m = e.veh;
		return (m.engine_sound_latched ? 0x01u : 0u) | (m.reverse_sound_latched ? 0x02u : 0u) |
				(m.light_sound_latched ? 0x04u : 0u) | (m.flare_latched ? 0x04u : 0u) |
				(m.skid_sound_latched ? 0x08u : 0u) | (m.collision_sound_latched ? 0x10u : 0u) |
				(m.pivot_sound_latched ? 0x20u : 0u) | (m.tumble_hard_latched ? 0x40u : 0u) |
				(m.tumble_med_latched ? 0x80u : 0u);
	};
	const auto set_latched = [](w::Entity &e) {
		w::Entity::VehicleMotorState &m = e.veh;
		m.light_sound_latched = m.flare_latched = m.skid_sound_latched = true;
		m.collision_sound_latched = m.pivot_sound_latched = true;
		m.tumble_hard_latched = m.tumble_med_latched = true;
	};
	const auto row = [&](Rig &r, int32_t item_id, uint8_t byte, bool brain) {
		const w::EntityHandle h = r.item(item_id, "", 0, true, 1);
		r.at(h).veh.spawn_sound_latch = static_cast<int8_t>(byte);
		set_latched(r.at(h));
		if (brain) r.world.ai.attach(h);
		r.world.vehicles.run_weapon_slot_class_init(r.at(h));
		return h;
	};
	const w::PartSpinRates jo{};
	{
		Rig r;
		r.world.rules.mp_session = true;
		r.world.rules.projectile_authority = false;
		const w::EntityHandle turning = row(r, kHeloItem, 0x83, true);
		CHECK(latched(r.at(turning)) == 0x03u);
		CHECK(r.at(turning).veh.part_spin.rate == jo.full && jo.full == 0x2D82D);
		CHECK(r.at(turning).veh.part_spin.speed == w::kRotorSpeedMax);
		const w::EntityHandle parked = row(r, kHeloItem, 0x01, true);
		CHECK(latched(r.at(parked)) == 0x01u);
		CHECK(r.at(parked).veh.part_spin.rate == 0 && r.at(parked).veh.part_spin.speed == 0);
		const w::EntityHandle brainless = row(r, kHeloItem, 0x80, false);
		CHECK(latched(r.at(brainless)) == 0xFCu);
		CHECK(r.at(brainless).veh.part_spin.rate == jo.full &&
				r.at(brainless).veh.part_spin.speed == w::kRotorSpeedMax);
		const w::EntityHandle crate = row(r, kCrateItem, 0x83, true);
		CHECK(latched(r.at(crate)) == 0xFCu && r.at(crate).veh.part_spin.rate == 0);
		r.world.rules.title_rules = &w::title_rules("dfx2");
		const w::EntityHandle dfx2 = row(r, kHeloItem, 0x80, true);
		CHECK(r.at(dfx2).veh.part_spin.rate == 0x5B05B &&
				r.at(dfx2).veh.part_spin.speed == w::kRotorSpeedMax);
	}
	for (const bool session : {true, false}) {
		Rig r;
		r.world.rules.mp_session = session;
		r.world.rules.projectile_authority = true;
		const w::EntityHandle host = row(r, kHeloItem, 0x83, true);
		CHECK(latched(r.at(host)) == 0xFCu);
		CHECK(r.at(host).veh.part_spin.rate == 0 && r.at(host).veh.part_spin.speed == 0);
	}
}

} // namespace

int main() {
	test_the_class_inits_bind_and_split();
	test_the_spawn_state_survives_mount_and_remount();
	test_the_aircraft_respawn_reseeds_its_slot();
	test_the_carrier_death_refills_the_spawn_bound_child();
	test_the_armory_load_binds_the_promoted_guns();
	test_a_joiners_aircraft_init_takes_back_its_latch_byte();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("weapon_slot_class_init: all checks passed\n");
	return 0;
}
