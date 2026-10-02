// The powerup family (world/powerup.h): the mission-start bind, the pickup's
// health / class-1 / per-class / allammo arms and its consume outcomes, the
// authority-only respawn countdown, and the remote-player grant.
// [orig: sub_442D00 @0x442D00; PowerupAction_Pickup @0x4428A0;
//  Entity_TickFireTimer @0x442850; PowerupAction_Respawn @0x442B40]
#include <runtime/world/powerup.h>

#include <formats/def/def.h>
#include <runtime/world/local_player.h>
#include <runtime/world/weapon_fsm.h>
#include <runtime/world/weapon_inventory.h>
#include <runtime/world/weapon_table.h>
#include <runtime/world/world.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_tick.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

using namespace opennova::world;
namespace def = opennova::def;
namespace im = opennova::inmatch;

static int failures = 0;

#define CHECK(c)                                                                     \
    do {                                                                             \
        if (!(c)) {                                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                 \
            ++failures;                                                              \
        }                                                                            \
    } while (0)

namespace {

constexpr int32_t kFillAmount = 0x7FFF;

// A weapon table with the built-in class ids 0..10 and three granted classes
// (11 "5.56mm" cap 120, 12 "7.62mm" cap 90, 13 "grenade" cap 4); class 1 caps
// at 6 (the entity+288 word).
WeaponTable weapons() {
    WeaponTable t;
    const char *names[] = {"null", "class1", "c2", "c3", "c4", "c5", "c6",
                           "c7", "c8", "c9", "c10", "5.56mm", "7.62mm", "grenade"};
    for (const char *n : names) t.ammo_class_names.emplace_back(n);
    t.ammo_class_caps.assign(t.ammo_class_names.size(), 0);
    t.ammo_class_caps[1] = 6;
    t.ammo_class_caps[11] = 120;
    t.ammo_class_caps[12] = 90;
    t.ammo_class_caps[13] = 4;
    // One populated weapon def at index 4: a 30-round magazine on class 11 with
    // 90 start rounds, so the `allammo` re-seed and clip redraw have a slot.
    t.entries.resize(8);
    WeaponTableEntry &rifle = t.entries[4];
    rifle.valid = true;
    rifle.name = "WPN_TEST";
    rifle.clipsize = 30;
    rifle.startrounds = 90;
    rifle.ammo_class_id = 11;
    rifle.ammo_class_count = 1;
    return t;
}

constexpr int32_t kRifleCombo = 5 * 65 + 1;

def::DefPowerupFile shipped_and_synthetic() {
    static const char kText[] =
            "powerup \"FULLHP\"\r\n hp -1\r\n respawn_time 30\r\n max_respawns 1\r\n"
            " action \"respawn\"\r\n  function powerup_respawn\r\n end\r\n"
            " action \"pickup\"\r\n  soundset HEALTH_UP\r\n  function powerup_pickup\r\n end\r\nend\r\n"
            "powerup \"FULLHP_INF\"\r\n hp -1\r\n respawn_time 30\r\n"
            " action \"respawn\"\r\n  function powerup_respawn\r\n end\r\n"
            " action \"pickup\"\r\n  soundset HEALTH_UP\r\n  function powerup_pickup\r\n end\r\nend\r\n"
            "powerup \"AmmoFull\"\r\n respawn_time 100\r\n max_respawns 1\r\n allammo\r\n"
            " action \"respawn\"\r\n  function powerup_respawn\r\n end\r\n"
            " action \"pickup\"\r\n  soundset PU_AMMO\r\n  function powerup_pickup\r\n end\r\nend\r\n"
            // Synthetic rows for the arms no shipped row reaches.
            "powerup \"HP25\"\r\n hp 25\r\n respawn_time 0\r\nend\r\n"
            "powerup \"CLASSES\"\r\n mana -1\r\n ammo 5.56mm 30\r\n ammo grenade -1\r\n"
            " ammo 7.62mm 0\r\n ammo unknown_class 9\r\nend\r\n"
            "powerup \"NULLFN\"\r\n hp -1\r\n action \"pickup\"\r\n  function null\r\n end\r\nend\r\n"
            "powerup \"WPN\"\r\n hp 10\r\n weapon all\r\nend\r\n"
            "powerup \"MEDAMMO\"\r\n hp -1\r\n allammo\r\n respawn_time 5\r\nend\r\n";
    def::DefPowerupFile f;
    if (def::def_parse_powerup_memory(reinterpret_cast<const uint8_t *>(kText),
                                      sizeof(kText) - 1, &f) != 0)
        std::abort();
    return f;
}

struct Rig {
    World world;
    std::unique_ptr<LocalPlayer> local;
    def::DefItemsFile items{};
    EntityHandle picker;

    Rig() {
        world.registry.configure_pool(0, 4);
        world.registry.configure_pool(1, 8);
        world.registry.configure_pool(2, 4);
        world.tables.weapons = weapons();
        // A presenting peer stamps its listener; without one the one-shot
        // queue readies nothing (the dedicated-host gate).
        world.out.fire_sounds.set_listener({0.0f, 0.0f, 0.0f});
        def::DefPowerupFile file = shipped_and_synthetic();
        world.tables.powerups = build_powerup_table(file, world.tables.weapons);
        def::def_free_powerup(&file);
        // items.def rows: ordinal 1 = the med pack (FULLHP), 2 = the infinite med
        // pack, 3 = the ammo pack, 4 = HP25, 5 = CLASSES, 6 = NULLFN, 7 = an
        // unknown powerupdef name, 8 = WPN, 9 = MEDAMMO.
        items.count = 10;
        items.entries = static_cast<def::DefItemDef *>(std::calloc(items.count, sizeof(def::DefItemDef)));
        const char *names[] = {"", "fullhp", "fullhp_Inf", "AmmoFull", "HP25", "CLASSES",
                               "NULLFN", "NOT_A_ROW", "WPN", "MEDAMMO"};
        for (size_t i = 0; i < items.count; ++i)
            std::snprintf(items.entries[i].powerup_def, sizeof(items.entries[i].powerup_def),
                          "%s", names[i]);
        picker = spawn_player(true);
    }
    ~Rig() { std::free(items.entries); }

    EntityHandle spawn_player(bool local_player) {
        Entity e;
        e.kind = EntityKind::Organic;
        e.item_id = 5305;
        e.has_item_def = true;
        e.player_class = 8;
        e.health = 60;
        e.health_max = 100;
        e.mana = 2;
        e.alive = true;
        e.flags = kEntityFlagPlayer;
        e.engine_flags = kEntityFlagPlayer;
        e.position = {5.0f, 5.0f, 0.0f};
        const EntityHandle h = world.registry.spawn(0, e);
        if (local_player) {
            world.cached.local_player = h;
            local = std::make_unique<LocalPlayer>(world);
            local->inventory.reset(world.tables.weapons);
            local->inventory.pools[11] = 10;
            local->inventory_valid = true;
            world.local_player_state = local.get();
        }
        return h;
    }

    EntityHandle spawn_powerup(int32_t item_type_index, int pool = 1) {
        Entity e;
        e.kind = EntityKind::Item;
        e.item_id = 2041;
        e.has_item_def = true;
        e.item_type_index = item_type_index;
        e.item_attrib = kItemAttribPowerup;
        e.item_type = def::DEF_ITEM_TYPE_POWERUP;
        e.alive = true;
        e.position = {5.0f, 5.0f, 0.0f};
        return world.registry.spawn(pool, e);
    }

    TickContext ctx(bool authority) {
        TickContext c;
        c.world = &world;
        c.is_authority = authority;
        c.logic_tick = 100;
        return c;
    }

    Entity *get(EntityHandle h) { return world.registry.get(h); }
};

// The table build: rows, first-wins lookup, the action bind and the
// weapon/ammo name resolution.
void test_table_build() {
    Rig rig;
    const PowerupTable &t = rig.world.tables.powerups;
    CHECK(t.loaded);
    CHECK(t.rows.size() == 8);
    CHECK(t.index_of("fullhp") == 0);
    CHECK(t.index_of("FULLHP_INF") == 1);
    CHECK(t.index_of("nope") == -1);
    CHECK(t.index_of(nullptr) == -1);
    const PowerupDef &fullhp = t.rows[0];
    CHECK(fullhp.pickup.authored && fullhp.pickup.handler == weapon_handler::kPowerupPickup);
    CHECK(fullhp.pickup.soundset == "HEALTH_UP");
    CHECK(fullhp.respawn.authored && fullhp.respawn.handler == weapon_handler::kPowerupRespawn);
    // An unauthored block binds the default handler with no sound.
    const PowerupDef &hp25 = t.rows[3];
    CHECK(!hp25.pickup.authored && hp25.pickup.handler == weapon_handler::kPowerupPickup);
    CHECK(!hp25.respawn.authored && hp25.respawn.handler == weapon_handler::kPowerupRespawn);
    CHECK(hp25.pickup.soundset.empty());
    // The class rows resolve by name; an unknown class is dropped; 0 stays 0.
    const PowerupDef &classes = t.rows[4];
    CHECK(classes.mana == -1);
    CHECK(classes.ammo[11] == 30);
    CHECK(classes.ammo[13] == -1);
    CHECK(classes.ammo[12] == 0);
    // `function null` keeps the placeholder: the row runs nothing.
    CHECK(t.rows[5].pickup.authored && t.rows[5].pickup.handler == weapon_handler::kPlaceholder);
    CHECK(t.rows[6].weapon == -1);
}

// The bind: each Powerup row takes its def, the respawn seeds, unknown names
// destroy the row, non-Powerup rows and rows without an ordinal are skipped.
void test_bind() {
    Rig rig;
    const EntityHandle med = rig.spawn_powerup(1);
    const EntityHandle med_inf = rig.spawn_powerup(2, 2);
    const EntityHandle ammo = rig.spawn_powerup(3);
    const EntityHandle unknown = rig.spawn_powerup(7);
    const EntityHandle no_ordinal = rig.spawn_powerup(0);
    Entity crate;
    crate.kind = EntityKind::Item;
    crate.has_item_def = true;
    crate.item_type_index = 1;
    crate.item_attrib = 0;
    const EntityHandle plain = rig.world.registry.spawn(1, crate);

    powerup_bind_entities(rig.world, rig.items);

    const Entity *m = rig.get(med);
    CHECK(m != nullptr && m->powerup_def_index == 0);
    CHECK(m != nullptr && m->powerup_respawns_left == 0); // max_respawns 1 -> one-shot
    CHECK(m != nullptr && m->powerup_respawn_timer == -1);
    const Entity *mi = rig.get(med_inf);
    CHECK(mi != nullptr && mi->powerup_def_index == 1);
    CHECK(mi != nullptr && mi->powerup_respawns_left == -1); // unset -> unlimited
    const Entity *a = rig.get(ammo);
    CHECK(a != nullptr && a->powerup_def_index == 2 && a->powerup_respawns_left == 0);
    CHECK(rig.get(unknown) == nullptr); // Entity_Destroy on the lookup miss
    const Entity *n = rig.get(no_ordinal);
    CHECK(n != nullptr && n->powerup_def_index == -1);
    const Entity *p = rig.get(plain);
    CHECK(p != nullptr && p->powerup_def_index == -1);
}

// The bind with no table loaded destroys every Powerup row (retail's
// "Unable to load powerup.def" outcome).
void test_bind_without_table() {
    Rig rig;
    rig.world.tables.powerups = PowerupTable{};
    const EntityHandle med = rig.spawn_powerup(1);
    powerup_bind_entities(rig.world, rig.items);
    CHECK(rig.get(med) == nullptr);
}

// FULLHP on a hurt local player: health to the ceiling, the sound at the
// picker, the one-shot row destroyed.
void test_med_pack_one_shot() {
    Rig rig;
    const EntityHandle med = rig.spawn_powerup(1);
    powerup_bind_entities(rig.world, rig.items);
    powerup_pickup(rig.world, med, rig.picker, rig.ctx(true));
    const Entity *p = rig.get(rig.picker);
    CHECK(p != nullptr && p->health == 100);
    CHECK(rig.get(med) == nullptr);
    const auto sounds = rig.world.out.fire_sounds.drain();
    CHECK(sounds.size() == 1);
    if (!sounds.empty()) CHECK(std::string(sounds[0].set_name) == "HEALTH_UP");
    CHECK(rig.world.out.powerup_grants.empty());
}

// A client's copy of a REMOTE player's pickup (the joiner's wire-replica
// body, a transient picker with no registry entity): the pack's sound plays
// at the remote body and this peer's copy of the row is consumed; the local
// player's inventory and health are untouched and no grant is staged.
// [orig: PowerupAction_Pickup @0x4428A0 (the soundset at the picker
//  @0x442AA6, the consume @0x442AC0..0x442B35), reached from a client's
//  resolver for a remote body]
void test_remote_body_pickup_on_a_client() {
    Rig rig;
    const EntityHandle med = rig.spawn_powerup(1);
    powerup_bind_entities(rig.world, rig.items);
    Entity remote;
    remote.has_item_def = true;
    remote.health_max = 100;
    remote.health = 87; // a tier-2 class byte's midpoint
    remote.position = {9.0f, 5.0f, 0.0f};
    const int32_t local_pool = rig.local->inventory.pools[11];
    powerup_pickup_by(rig.world, med, remote, rig.ctx(false));
    CHECK(rig.get(med) == nullptr); // FULLHP is a one-shot: the copy is destroyed
    const auto sounds = rig.world.out.fire_sounds.drain();
    CHECK(sounds.size() == 1);
    if (!sounds.empty()) {
        CHECK(std::string(sounds[0].set_name) == "HEALTH_UP");
        CHECK(sounds[0].pos.x == 9.0f);
    }
    CHECK(rig.get(rig.picker)->health == 60);
    CHECK(rig.local->inventory.pools[11] == local_pool);
    CHECK(rig.world.out.powerup_grants.empty());
}

// FULLHP at full health: nothing happens, the row stays, no sound.
void test_med_pack_refuses_when_full() {
    Rig rig;
    rig.get(rig.picker)->health = 100;
    const EntityHandle med = rig.spawn_powerup(1);
    powerup_bind_entities(rig.world, rig.items);
    powerup_pickup(rig.world, med, rig.picker, rig.ctx(true));
    const Entity *m = rig.get(med);
    CHECK(m != nullptr && !m->hidden && m->powerup_respawn_timer == -1);
    CHECK(rig.get(rig.picker)->health == 100);
    CHECK(rig.world.out.fire_sounds.drain().empty());
}

// FULLHP_INF: the row hides and arms 30 s; the authority's countdown respawns
// it and spends nothing of an unlimited count; a non-authority never counts.
void test_infinite_med_pack_respawns() {
    Rig rig;
    const EntityHandle med = rig.spawn_powerup(2);
    powerup_bind_entities(rig.world, rig.items);
    powerup_pickup(rig.world, med, rig.picker, rig.ctx(false));
    Entity *m = rig.get(med);
    CHECK(m != nullptr && m->hidden && m->powerup_respawn_timer == 30 * 62);
    CHECK(rig.get(rig.picker)->health == 100);
    (void)rig.world.out.fire_sounds.drain();
    // A second touch while consumed refuses.
    rig.get(rig.picker)->health = 10;
    powerup_pickup(rig.world, med, rig.picker, rig.ctx(false));
    CHECK(rig.get(rig.picker)->health == 10);
    CHECK(rig.world.out.fire_sounds.drain().empty());
    // No countdown off the authority.
    powerup_tick(rig.world, rig.ctx(false));
    CHECK(rig.get(med)->powerup_respawn_timer == 30 * 62);
    for (int i = 0; i < 30 * 62; ++i) powerup_tick(rig.world, rig.ctx(true));
    m = rig.get(med);
    CHECK(m != nullptr && m->hidden && m->powerup_respawn_timer == 0);
    powerup_tick(rig.world, rig.ctx(true)); // zero fires the respawn
    m = rig.get(med);
    CHECK(m != nullptr && !m->hidden && m->powerup_respawn_timer == -1);
    CHECK(m != nullptr && m->powerup_respawns_left == -2); // unlimited counts down past -1
    CHECK(rig.world.out.fire_sounds.drain().empty()); // the shipped respawn row has no sound
    // Idle rows stay idle.
    powerup_tick(rig.world, rig.ctx(true));
    CHECK(rig.get(med)->powerup_respawn_timer == -1);
    // And it can be taken again.
    powerup_pickup(rig.world, med, rig.picker, rig.ctx(true));
    CHECK(rig.get(rig.picker)->health == 100);
}

// A counted respawn: max_respawns 2 -> one respawn, then the next pickup destroys.
void test_counted_respawns() {
    Rig rig;
    rig.world.tables.powerups.rows[0].max_respawns = 2;
    const EntityHandle med = rig.spawn_powerup(1);
    powerup_bind_entities(rig.world, rig.items);
    CHECK(rig.get(med)->powerup_respawns_left == 1);
    powerup_pickup(rig.world, med, rig.picker, rig.ctx(true));
    CHECK(rig.get(med) != nullptr && rig.get(med)->hidden);
    for (int i = 0; i <= 30 * 62; ++i) powerup_tick(rig.world, rig.ctx(true));
    CHECK(rig.get(med) != nullptr && !rig.get(med)->hidden);
    CHECK(rig.get(med)->powerup_respawns_left == 0);
    rig.get(rig.picker)->health = 1;
    powerup_pickup(rig.world, med, rig.picker, rig.ctx(true));
    CHECK(rig.get(med) == nullptr);
}

// hp 25: the raise probes the ceiling, the grant is old + 25 with no clamp.
void test_hp_add() {
    Rig rig;
    const EntityHandle row = rig.spawn_powerup(4);
    powerup_bind_entities(rig.world, rig.items);
    rig.get(rig.picker)->health = 90;
    powerup_pickup(rig.world, row, rig.picker, rig.ctx(true));
    CHECK(rig.get(rig.picker)->health == 115);
    CHECK(rig.get(row) == nullptr); // respawn_time 0 destroys
    // At the ceiling the add refuses.
    const EntityHandle again = rig.spawn_powerup(4);
    powerup_bind_entities(rig.world, rig.items);
    rig.get(rig.picker)->health = 100;
    powerup_pickup(rig.world, again, rig.picker, rig.ctx(true));
    CHECK(rig.get(rig.picker)->health == 100);
    CHECK(rig.get(again) != nullptr);
}

// The class arms on the local inventory: mana -1 zeroes then fills class 1 to
// its cap; 5.56mm adds 30 onto the pool of 10; grenade -1 fills to the cap;
// a 0 row leaves the class alone.
void test_class_grants_local() {
    Rig rig;
    const EntityHandle row = rig.spawn_powerup(5);
    powerup_bind_entities(rig.world, rig.items);
    rig.get(rig.picker)->mana = 2;
    rig.local->inventory.pools[12] = 7;
    powerup_pickup(rig.world, row, rig.picker, rig.ctx(true));
    CHECK(rig.get(rig.picker)->mana == 6);
    CHECK(rig.local->inventory.pools[11] == 40);
    CHECK(rig.local->inventory.pools[13] == 4);
    CHECK(rig.local->inventory.pools[12] == 7);
    CHECK(rig.world.out.powerup_grants.empty());
    // A negative class-1 word skips the mana arm.
    const EntityHandle again = rig.spawn_powerup(5);
    powerup_bind_entities(rig.world, rig.items);
    rig.get(rig.picker)->mana = -1;
    powerup_pickup(rig.world, again, rig.picker, rig.ctx(true));
    CHECK(rig.get(rig.picker)->mana == -1);
}

// A remote player on the authority: health lands on the entity, the class
// adds and the allammo re-seed go out as one grant.
void test_remote_player_grant() {
    Rig rig;
    const EntityHandle remote = rig.spawn_player(false);
    const EntityHandle classes = rig.spawn_powerup(5);
    const EntityHandle ammo = rig.spawn_powerup(3);
    powerup_bind_entities(rig.world, rig.items);
    powerup_pickup(rig.world, classes, remote, rig.ctx(true));
    CHECK(rig.get(remote)->mana == 6);
    CHECK(rig.world.out.powerup_grants.size() == 1);
    if (rig.world.out.powerup_grants.size() == 1) {
        const PowerupGrant &g = rig.world.out.powerup_grants[0];
        CHECK(g.picker == remote);
        CHECK(!g.allammo);
        CHECK(g.ammo_adds.size() == 2);
        if (g.ammo_adds.size() == 2) {
            CHECK(g.ammo_adds[0].first == 11 && g.ammo_adds[0].second == 30);
            CHECK(g.ammo_adds[1].first == 13 && g.ammo_adds[1].second == kFillAmount);
        }
    }
    rig.world.out.powerup_grants.clear();
    powerup_pickup(rig.world, ammo, remote, rig.ctx(true));
    CHECK(rig.world.out.powerup_grants.size() == 1);
    if (rig.world.out.powerup_grants.size() == 1)
        CHECK(rig.world.out.powerup_grants[0].allammo);
    CHECK(rig.get(ammo) == nullptr); // one-shot
    // The local pools are untouched by a remote pickup.
    CHECK(rig.local->inventory.pools[11] == 10);
}

// `allammo` on the local player re-seeds the pools from the defs (an empty
// kit seeds zero) and keeps the health arm out of it.
void test_allammo_local() {
    Rig rig;
    const EntityHandle ammo = rig.spawn_powerup(3);
    powerup_bind_entities(rig.world, rig.items);
    rig.get(rig.picker)->health = 30;
    powerup_pickup(rig.world, ammo, rig.picker, rig.ctx(true));
    CHECK(rig.get(rig.picker)->health == 30);
    CHECK(rig.get(ammo) == nullptr);
    const auto sounds = rig.world.out.fire_sounds.drain();
    CHECK(sounds.size() == 1);
    if (!sounds.empty()) CHECK(std::string(sounds[0].set_name) == "PU_AMMO");
}

// `allammo` on a populated local slot: the class pool re-seeds from the def,
// the magazine returns and redraws, and the held weapon's FSM takes the redrawn
// clip now (the next pump mirrors the FSM clip back into the inventory).
void test_allammo_refills_populated_slot() {
    Rig rig;
    const EntityHandle ammo = rig.spawn_powerup(3);
    powerup_bind_entities(rig.world, rig.items);
    WeaponInventory &inv = rig.local->inventory;
    inv.slots[kRifleCombo].adm_index = 4;
    inv.slots[kRifleCombo].clip = 3;
    inv.equipped_combo = kRifleCombo;
    inv.pools[11] = 0;
    rig.local->weapon.slot.clip = 3;
    powerup_pickup(rig.world, ammo, rig.picker, rig.ctx(true));
    // seed 90, the 3 loaded rounds return (93), a 30-round clip is drawn (63)
    CHECK(inv.pools[11] == 63);
    CHECK(inv.slots[kRifleCombo].clip == 30);
    CHECK(rig.local->weapon.slot.clip == 30);
    CHECK(rig.world.out.powerup_grants.empty());
}

// A row that refills AND heals, touched at full health by a remote player: the
// refill ran before the health arm (retail's arm order), so its grant survives
// the silent unconsumed return.
void test_full_health_keeps_staged_grant() {
    Rig rig;
    const EntityHandle remote = rig.spawn_player(false);
    rig.get(remote)->health = 100;
    const EntityHandle row = rig.spawn_powerup(9);
    powerup_bind_entities(rig.world, rig.items);
    powerup_pickup(rig.world, row, remote, rig.ctx(true));
    CHECK(rig.world.out.powerup_grants.size() == 1);
    if (!rig.world.out.powerup_grants.empty())
        CHECK(rig.world.out.powerup_grants[0].allammo);
    const Entity *r = rig.get(row);
    CHECK(r != nullptr && !r->hidden && r->powerup_respawn_timer == -1);
    CHECK(rig.world.out.fire_sounds.drain().empty());
}

// The host drains a remote player's grants onto ITS connection: the seed and
// clip redraw over the connection's slot rows, the class adds onto its pools;
// another player's connection and an unowned grant are left alone.
void test_server_applies_remote_grants() {
    Rig rig;
    const EntityHandle remote = rig.spawn_player(false);
    const EntityHandle other = rig.spawn_player(false);
    const EntityHandle ammo = rig.spawn_powerup(3);
    const EntityHandle classes = rig.spawn_powerup(5);
    powerup_bind_entities(rig.world, rig.items);

    im::NapiNPServerCtx server;
    server.np_protocol.connection_list.emplace_back();
    server.np_protocol.connection_list.emplace_back();
    im::NapiNPConnection &conn = server.np_protocol.connection_list[0];
    conn.phase = im::ConnectionPhase::PlayerAdded;
    conn.link.owned_entity = remote;
    conn.weapon_slots[static_cast<uint16_t>(kRifleCombo)].adm_index = 4;
    conn.weapon_slots[static_cast<uint16_t>(kRifleCombo)].clip = 3;
    conn.reply.ammo_pools[11] = 0;
    im::NapiNPConnection &bystander = server.np_protocol.connection_list[1];
    bystander.phase = im::ConnectionPhase::PlayerAdded;
    bystander.link.owned_entity = other;
    bystander.reply.ammo_pools[11] = 7;

    powerup_pickup(rig.world, ammo, remote, rig.ctx(true));
    powerup_pickup(rig.world, classes, remote, rig.ctx(true));
    CHECK(rig.world.out.powerup_grants.size() == 2);
    // A grant no connection owns is dropped with the rest of the drain.
    PowerupGrant orphan;
    orphan.picker = EntityHandle::make(0, 3);
    orphan.ammo_adds.emplace_back(11, 5);
    rig.world.out.powerup_grants.push_back(orphan);

    im::Server_ApplyPowerupGrants(server.np_protocol.connection_list, rig.world);
    CHECK(rig.world.out.powerup_grants.empty());
    // allammo: seed 90 + the 3 returned rounds - the 30-round redraw = 63,
    // then the class grant adds 30 (cap 120)
    CHECK(conn.reply.ammo_pools[11] == 93);
    CHECK(conn.weapon_slots[static_cast<uint16_t>(kRifleCombo)].clip == 30);
    CHECK(conn.reply.ammo_pools[13] == 4);
    CHECK(bystander.reply.ammo_pools[11] == 7);
    // The listen host's own player never routes through a grant.
    CHECK(rig.local->inventory.pools[11] == 10);
}

// A `function null` pickup row runs nothing: no health, no consume.
void test_placeholder_row_runs_nothing() {
    Rig rig;
    const EntityHandle row = rig.spawn_powerup(6);
    powerup_bind_entities(rig.world, rig.items);
    powerup_pickup(rig.world, row, rig.picker, rig.ctx(true));
    CHECK(rig.get(rig.picker)->health == 60);
    CHECK(rig.get(row) != nullptr && !rig.get(row)->hidden);
}

// The `weapon` arm is the recorded residual: the rest of the row still runs.
void test_weapon_arm_residual() {
    Rig rig;
    const EntityHandle row = rig.spawn_powerup(8);
    powerup_bind_entities(rig.world, rig.items);
    powerup_pickup(rig.world, row, rig.picker, rig.ctx(true));
    CHECK(rig.get(rig.picker)->health == 70);
}

// The contact drain runs the pickups in resolver order and nothing without a
// collision world.
void test_contact_drain_without_collision_world() {
    Rig rig;
    rig.world.collision = nullptr;
    powerup_process_contacts(rig.world, rig.ctx(true));
    CHECK(rig.get(rig.picker)->health == 60);
}

} // namespace

int main() {
    test_table_build();
    test_bind();
    test_bind_without_table();
    test_med_pack_one_shot();
    test_med_pack_refuses_when_full();
    test_remote_body_pickup_on_a_client();
    test_infinite_med_pack_respawns();
    test_counted_respawns();
    test_hp_add();
    test_class_grants_local();
    test_remote_player_grant();
    test_allammo_local();
    test_allammo_refills_populated_slot();
    test_full_health_keeps_staged_grant();
    test_server_applies_remote_grants();
    test_placeholder_row_runs_nothing();
    test_weapon_arm_residual();
    test_contact_drain_without_collision_world();
    if (failures == 0) std::printf("powerup: OK\n");
    return failures == 0 ? 0 : 1;
}
