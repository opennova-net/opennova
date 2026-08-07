// The local player's loadout ORCHESTRATION (world/player_loadout.h, S7b): the
// mission-rules promotion with the witnessed SP-vs-net gate, the ACCEPT apply's
// requested-ammo overlay + banned validation, and the spawn rebuild's class
// latch — over the same small armory the weapon_inventory unit tests use.
// [orig: Mission_LoadBMSFile @ 0x40F4E0; WeaponLoadout_ApplyFromBuffer
//  @ 0x565cd0; Player_InitPlayer @ 0x4e15f0]
#include <cstdio>

#include "world/player_loadout.h"
#include "world/world.h"

using namespace opennova::world;

static int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

int add_entry(WeaponTable &t, const char *name, int cat, int rank, int clipsize,
              int startrounds, const char *ammo_class, int wclass,
              int subclasses = 0) {
    WeaponTableEntry e;
    e.name = name;
    e.valid = true;
    e.category = static_cast<uint8_t>(cat);
    e.rank = static_cast<uint8_t>(rank);
    e.clipsize = static_cast<int16_t>(clipsize);
    e.startrounds = static_cast<int16_t>(startrounds);
    e.maxclips = 10;
    e.ammo_class = ammo_class;
    e.ammo_class_count = 1;
    e.weapon_class_slot = wclass;
    e.loadout_subclasses = static_cast<uint8_t>(subclasses);
    int id = t.ammo_class_id_of(ammo_class);
    if (id < 0) {
        t.ammo_class_names.emplace_back(ammo_class);
        t.ammo_class_caps.push_back(600);
        id = static_cast<int>(t.ammo_class_names.size()) - 1;
    }
    e.ammo_class_id = static_cast<int16_t>(id);
    t.entries.push_back(std::move(e));
    return static_cast<int>(t.entries.size()) - 1;
}

struct Rig {
    World world;
    LocalPlayerLoadout loadout;
    LocalPlayerWeapon weapon;
    WeaponInventory inventory;
    bool inventory_valid = false;
    int m4 = -1;

    Rig() {
        WeaponTable &t = world.weapons;
        t.ammo_class_names.emplace_back("");
        t.ammo_class_caps.push_back(0);
        WeaponTableEntry null_e;
        null_e.name = "null";
        null_e.valid = true;
        null_e.ammo_class_id = 0;
        t.entries.push_back(std::move(null_e));
        const int knife = add_entry(t, "WPN_KNIFE", 1, 0, -1, -1, "", 0);
        t.entries[static_cast<size_t>(knife)].ammo_class_id = 0;
        m4 = add_entry(t, "WPN_M4AUTO", 3, 0, 30, 210, "AMMO_556", 1);
        add_entry(t, "WPN_AK47AUTO", 3, 1, 30, 210, "AMMO_762", 1);
    }
};

std::vector<WeaponKitEntry> kit_of(const char *name, int32_t pri = -1,
                                   int32_t sec = -1, int32_t flags = -1) {
    return {WeaponKitEntry{name, pri, sec, flags}};
}

// The gate: a live session — listen host or joiner alike — never promotes the
// mission chunks [orig: @ 0x40f694 -> the fseek pair @ 0x40f6b2/@ 0x40f6e1].
void test_promotion_gate_skips_in_session() {
    Rig r;
    r.world.mp_session = true;
    CHECK(!local_loadout_promote_mission_rules(r.world, r.loadout,
            {{"WPN_AK47AUTO", 0}}, kit_of("WPN_M4AUTO", 3)));
    CHECK(!r.loadout.spawn_kit_set);
    CHECK(r.loadout.availability.value_for(3) ==
            weapon_availability_value::kAllowed);
}

// Offline: availability applies first, then the kit filters through it with
// the knife fallback [orig: the filter @ 0x40f834, the fallback @ 0x40f899].
void test_promotion_applies_availability_then_filters_kit() {
    Rig r;
    CHECK(local_loadout_promote_mission_rules(r.world, r.loadout,
            {{"WPN_M4AUTO", 0}}, kit_of("WPN_M4AUTO", 3)));
    CHECK(r.loadout.spawn_kit_set);
    CHECK(r.loadout.spawn_kit.size() == 1);
    if (r.loadout.spawn_kit.size() == 1)
        CHECK(r.loadout.spawn_kit[0].name == "WPN_KNIFE");
}

// The ACCEPT overlay: requested clips expand to min(req, maxclips) * clipsize;
// the banned validation drops entries only when requested
// [orig: @ 0x566166; the 0x2F gate @ 0x515a3f].
void test_accept_requested_ammo_and_banned_validation() {
    Rig r;
    CHECK(local_loadout_apply_accept(r.world, r.loadout, r.weapon, r.inventory,
            r.inventory_valid, kit_of("WPN_M4AUTO", 3), 8,
            /*validate_banned=*/true));
    CHECK(r.inventory_valid);
    const WeaponTableEntry *def =
            r.world.weapons.by_index(static_cast<uint8_t>(r.m4));
    CHECK(def != nullptr &&
            r.inventory.pools[static_cast<size_t>(def->ammo_class_id)] == 90);

    // Ban the M4 and re-accept with validation: the entry drops.
    local_loadout_apply_availability_pairs(r.world, r.loadout,
            {{"WPN_M4AUTO", 0}});
    CHECK(local_loadout_apply_accept(r.world, r.loadout, r.weapon, r.inventory,
            r.inventory_valid, kit_of("WPN_M4AUTO", 3), 8,
            /*validate_banned=*/true));
    CHECK(r.loadout.spawn_kit.empty());
    // The S2C 0x5A grant apply skips the validation and keeps it.
    CHECK(local_loadout_apply_accept(r.world, r.loadout, r.weapon, r.inventory,
            r.inventory_valid, kit_of("WPN_M4AUTO", 3), 8,
            /*validate_banned=*/false));
    CHECK(r.loadout.spawn_kit.size() == 1);
}

// The pre-spawn class latch survives resets and seeds the rebuild's pools
// until L exists [orig: the clamp default 8 @ 0x51d102].
void test_class_latch_survives_kit_reset() {
    Rig r;
    r.loadout.pending_player_class = 6;
    r.loadout.spawn_kit = kit_of("WPN_M4AUTO");
    r.loadout.spawn_kit_set = true;
    r.loadout.reset();
    CHECK(!r.loadout.spawn_kit_set);
    CHECK(r.loadout.pending_player_class == 6);
    local_loadout_rebuild(r.world, r.loadout, r.weapon, r.inventory,
            r.inventory_valid, /*select_spawn_default=*/true);
    CHECK(r.inventory_valid);
}

} // namespace

int main() {
    test_promotion_gate_skips_in_session();
    test_promotion_applies_availability_then_filters_kit();
    test_accept_requested_ammo_and_banned_validation();
    test_class_latch_survives_kit_reset();
    if (failures == 0) std::printf("player_loadout_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
